# -*- coding: utf-8 -*-
"""业务编排层：CLI 与 GUI 共用同一套动作。

所有函数签名统一：action(cfg, log, **opts) -> Result。
Result.kind：ok / warn / fail / mfa / not_configured。
日志一律走 log 回调；返回值里带 rows 供 GUI 展示结构化信息。

对齐固件 0.4.0：
  - 多账号（config.accounts）：每个账号独立的 指纹/会话/坐标/最近结果；
    账号表为空 = 单人模式（凭证取顶层，视图 label='self'）。
  - 打卡窗口（config.schedule）：auto 默认只在 [checkin_at, window_end] 内执行，
    --force 越窗；手动 checkin 同样受闸门约束（提示用 --force，不拦 dry-run）。
  - 推送（push.py）：与固件同规则 fan-out——单人结果同时进默认通道与关联的
    额外通道；多人 auto 时默认通道收「汇总」、额外通道收各自账号的结果。
"""
import datetime
import json

from . import aes, cas_auth, checkin_api, config, push
from .http_client import HttpClient, NetworkError, DEFAULT_UA


class Result(object):
    def __init__(self, kind, text, rows=None, data=None):
        self.kind = kind          # ok / warn / fail / mfa / not_configured
        self.text = text
        self.rows = rows or []    # [(name, value)]
        self.data = data

    @property
    def ok(self):
        return self.kind == 'ok'

    def exit_code(self):
        return {'ok': 0, 'warn': 1, 'mfa': 3}.get(self.kind, 2)


# --------------------------------------------------------------- 基础件

def build_client(cfg, log, acct=None):
    """会话/指纹一律取自账号视图（单人模式=顶层合成的 'self' 视图）。"""
    acct = acct or {}
    ua = cfg.get('user_agent') or DEFAULT_UA
    client = HttpClient(user_agent=ua, timeout=cfg.get('timeout') or 20, log=log)
    session = acct.get('session_cookie') or ''
    if session and not session.startswith('PUT_'):
        client.jar.set('SESSION', session, 'jzsz.uestc.edu.cn', '/')
    bfp = acct.get('browser_fingerprint') or ''
    if bfp and not bfp.startswith('PUT_'):
        cas_auth.set_fingerprint_cookie(client, bfp)
    return client


def _api(cfg, client):
    return checkin_api.CheckinApi(client, cfg.get('app_id') or checkin_api.DEFAULT_APP_ID)


def resolve_account(cfg, label=None):
    """按标签取账号视图；label 为空且账号表存在 -> None（调用方决定语义）。"""
    if not label:
        return None
    real = config.find_account(cfg, label)
    if real is not None:
        return real
    for a in config.effective_accounts(cfg, only_enabled=False):
        if a.get('label') == label:
            return a
    return None


def probe(client, api, log):
    """预热 + 进微应用 + 查当日状态。返回 (ok, data, note)。"""
    try:
        api.warmup_portal()
        api.enter_app()
    except NetworkError as e:
        return False, None, '网络不可达：%s' % e
    except Exception as e:
        return False, None, '进入微应用失败：%s' % e
    try:
        r = api.today()
    except NetworkError as e:
        return False, None, '网络不可达：%s' % e
    if r.status in (301, 302):
        return False, None, '会话已失效（被重定向到登录页）'
    if r.status != 200:
        return False, None, 'HTTP %s' % r.status
    try:
        j = r.json()
    except Exception:
        return False, None, '响应不是 JSON：%s' % r.text[:120]
    if j.get('code') != 0:
        return False, None, 'code=%s msg=%s' % (j.get('code'), j.get('msg'))
    return True, (j.get('data') or {}), ''


def _login_chain(cfg, log, acct):
    """对一个账号执行 CAS 登录。返回 (client, session) 或 (None, Result)。"""
    sid = str(acct.get('student_id') or '')
    pwd = str(acct.get('password') or '')
    if not config.account_credentials_set(acct):
        return None, Result('not_configured',
                            '账号 %s 的学号/密码未填' % acct.get('label'))
    client = build_client(cfg, log, acct)
    bfp = acct.get('browser_fingerprint') or ''
    if bfp.startswith('PUT_'):
        bfp = ''
    try:
        verdict, detail = cas_auth.login(client, sid, pwd, bfp_value=bfp, log=log)
    except NetworkError as e:
        return None, Result('fail', '网络不可达：%s（校园网站点仅校园网可达）' % e)
    except Exception as e:
        return None, Result('fail', '登录链异常：%s' % e)
    if verdict == 'MFA':
        return None, Result('mfa',
                            '账号 %s：服务端要求二次验证，该指纹未被信任。'
                            '请在其已信任的浏览器里抄 MULTIFACTOR_BROWSER_FINGERPRINT 填入。'
                            % acct.get('label'))
    if verdict != 'OK':
        return None, Result('fail', '登录失败：%s（%s）' %
                            (verdict, cas_auth.mask_ticket(str(detail))[:120]))
    session = client.jar.get('SESSION') or ''
    if session:
        config.save_account_session(cfg, acct, session)
        log('账号 %s：新会话已保存到 config.json' % acct.get('label'))
    return client, session


def _ensure_session(cfg, log, acct):
    """复用已有会话，失效则登录。返回 (client, api, ok, note)。"""
    client = build_client(cfg, log, acct)
    api = _api(cfg, client)
    ok, data, note = probe(client, api, log)
    if ok:
        return client, api, ok, note, data
    if not config.account_credentials_set(acct):
        return client, api, False, note, None
    log('账号 %s：会话不可用（%s），尝试登录 ...' % (acct.get('label'), note))
    client2, res = _login_chain(cfg, log, acct)
    if isinstance(res, Result) and not res.ok:
        return client, api, False, '登录失败：%s' % res.text, None
    client = client2 or client
    api = _api(cfg, client)
    ok, data, note = probe(client, api, log)
    return client, api, ok, note, data


# --------------------------------------------------------------- 推送

def _notify_active(cfg):
    return any((c.get('on') and (c.get('url') or '').strip())
               for c in config.iter_channels(cfg))


def _push(cfg, log, account_events, summary=None):
    """account_events=[(label,title,body)] 走 All 语义（默认+关联额外通道）；
    summary=(title,body) 只进默认通道。单人结果用 account_events 不带 summary；
    多人 auto 反之：逐账号事件跳过默认通道，默认通道收汇总。"""
    if not _notify_active(cfg):
        return
    try:
        if account_events:
            push.fan_out(cfg, account_events, log=log,
                         skip_default=bool(summary))
        if summary:
            push.fan_out(cfg, [('', summary[0], summary[1])], log=log, only_default=True)
    except Exception as e:
        log('[WARN] 推送异常（不影响打卡结果）：%s' % e)


# --------------------------------------------------------------- 动作

def do_login(cfg, log, label=None):
    """登录并保存会话。label 省略：单人=顶层；多人=所有启用账号逐个登录。"""
    accounts = ([resolve_account(cfg, label)] if label
                else config.effective_accounts(cfg))
    if label and accounts[0] is None:
        return Result('fail', '找不到账号 %s（account list 查看）' % label)
    rows, kinds = [], []
    for acct in accounts:
        client, res = _login_chain(cfg, log, acct)
        if isinstance(res, Result):
            kinds.append(res.kind)
            rows.append((acct.get('label'), res.text))
            if res.kind == 'mfa' and len(accounts) == 1:
                return res
            continue
        api = _api(cfg, client)
        ok, data, note = probe(client, api, log)
        kinds.append('ok' if ok else 'warn')
        rows.append((acct.get('label'),
                     '登录成功，打卡接口可用' if ok else '登录成功但状态复查失败：%s' % note))
    kind = 'fail' if 'fail' in kinds else ('mfa' if 'mfa' in kinds and 'ok' not in kinds
                                           else ('warn' if 'warn' in kinds else 'ok'))
    if len(accounts) == 1 and kinds and kinds[0] in ('not_configured', 'fail', 'mfa'):
        return Result(kinds[0] if kinds[0] != 'not_configured' else 'not_configured',
                      rows[0][1], rows=[])
    return Result(kind, '登录完成：%s' % '，'.join('%s=%s' % (a, k) for a, k in
                                                  zip([x.get('label') for x in accounts], kinds)),
                  rows=rows)


def do_status(cfg, log, label=None):
    """查状态。label 省略：单人模式查顶层；多人模式逐个启用账号查（只读复用会话）。"""
    accounts = ([resolve_account(cfg, label)] if label
                else config.effective_accounts(cfg))
    if label and accounts[0] is None:
        return Result('fail', '找不到账号 %s' % label)
    rows = []
    worst = 'ok'
    order = {'ok': 0, 'warn': 1, 'mfa': 2, 'fail': 3, 'not_configured': 3}
    last_data = None
    for acct in accounts:
        client, api, ok, note, data = _ensure_session(cfg, log, acct)
        if not ok:
            worst = 'fail'
            rows.append(('%s' % acct.get('label'), '会话不可用：%s' % note))
            continue
        last_data = data
        summary = checkin_api.safe_summary(data)
        rows.append(('%s 今日状态' % acct.get('label'), summary.get('recordStatusCode')))
        if summary.get('abnormalFlag'):
            rows.append(('%s 异常标记' % acct.get('label'), 'abnormalFlag=true'))
            worst = 'warn' if order[worst] < order['warn'] else worst
        if len(accounts) == 1:
            for k, v in summary.items():
                rows.append((k, str(v)))
            try:
                sched = api.schedule()
                if sched.status == 200:
                    for k, v in checkin_api.schedule_summary(sched.json().get('data') or {}).items():
                        rows.append(('时段.%s' % k, str(v)))
            except Exception:
                pass
            try:
                locs = api.locations()
                if locs.status == 200:
                    for one in checkin_api.locations_summary(locs.json().get('data')):
                        rows.append(('打卡点', '%s (%s, %s) r=%s m' % (
                            one['locationName'], one['latitude'], one['longitude'],
                            one['radiusMeter'])))
            except Exception:
                pass
    status = (last_data or {}).get('recordStatusCode')
    text = ('会话有效，今日状态：%s' % status) if len(accounts) == 1 and last_data \
        else '%d 个账号状态已查询' % len(accounts)
    return Result(worst, text, rows=rows, data=last_data)


def _checkin_one(cfg, log, acct, force, use_reverse):
    """单账号打卡全链。返回 (kind, label, text, rows)。不抛异常。"""
    label = acct.get('label') or 'self'
    client, api, ok, note, data = _ensure_session(cfg, log, acct)
    if not ok:
        return ('fail', label, '前置检查失败：%s' % note, [])
    status = data.get('recordStatusCode')
    if status == 'CHECKED_IN' and not force:
        return ('ok', label, '今日已打卡（%s），无需重复' % data.get('checkinTime'), [])
    loc = config.account_location(cfg, acct)
    if 'latitude' not in loc or 'longitude' not in loc:
        return ('not_configured', label, '缺少坐标（location）', [])
    addr = loc.get('reverse_geocode')
    if use_reverse:
        try:
            r = api.reverse_geocode(loc['latitude'], loc['longitude'])
            d = r.json().get('data')
            if isinstance(d, str) and d:
                addr = d
            log('%s：逆地理编码：%s' % (label, addr))
        except Exception as e:
            log('%s：逆地理编码失败，沿用配置地址：%s' % (label, e))
    payload = api.build_payload(loc, client.ua, reverse_geocode=addr)
    log('%s：提交打卡 ...' % label)
    try:
        r = api.submit(payload)
    except NetworkError as e:
        return ('fail', label, '网络不可达：%s' % e, [])
    if r.status != 200:
        return ('fail', label, '请求失败：HTTP %s %s' % (r.status, r.text[:160]), [])
    try:
        j = r.json()
    except Exception:
        return ('fail', label, '响应不是 JSON：%s' % r.text[:160], [])
    if j.get('code') != 0:
        msg = 'code=%s msg=%s' % (j.get('code'), j.get('msg'))
        kind = 'mfa' if '证' in str(j.get('msg') or '') else 'fail'
        return (kind, label, msg, [])
    ok2, data2, note2 = probe(client, api, log)
    summary = checkin_api.safe_summary(data2) if ok2 else {}
    today = datetime.date.today().isoformat()
    if summary.get('recordStatusCode') == 'CHECKED_IN':
        if summary.get('abnormalFlag'):
            config.save_account_result(cfg, acct, today, True, '已打卡但标记异常')
            return ('warn', label, '已打卡但被标记异常（abnormalFlag=true），'
                    '建议今天改用人工打卡并检查坐标',
                    [('checkinTime', summary.get('checkinTime')),
                     ('distanceMeter', summary.get('distanceMeter'))])
        text = '打卡成功（%s，%.0f m，%s）' % (summary.get('checkinTime'),
              float(summary.get('distanceMeter') or 0), summary.get('checkinLocationName') or '')
        config.save_account_result(cfg, acct, today, True, text)
        return ('ok', label, text,
                [('checkinTime', summary.get('checkinTime')),
                 ('distanceMeter', summary.get('distanceMeter')),
                 ('打卡点', summary.get('checkinLocationName'))])
    if not ok2:
        config.save_account_result(cfg, acct, today, False, '提交返回成功但复查失败')
        return ('warn', label, '接口返回成功，但复查失败（%s），请稍后人工确认' % note2, [])
    config.save_account_result(cfg, acct, today, False, '复查状态 %s' % summary.get('recordStatusCode'))
    return ('warn', label, '接口返回成功，复查仍为 %s，请稍后人工确认'
            % summary.get('recordStatusCode'), [])


def do_checkin(cfg, log, label=None, force=False, dry_run=False, use_reverse=False):
    if dry_run:
        acct = resolve_account(cfg, label) if label else (
            config.effective_accounts(cfg)[0] if cfg.get('accounts') else
            {'label': 'self', 'location': None})
        if acct is None:
            return Result('fail', '找不到账号 %s' % label)
        loc = config.account_location(cfg, acct)
        if 'latitude' not in loc or 'longitude' not in loc:
            return Result('not_configured', 'config.json 缺少 location 坐标')
        client = build_client(cfg, log, acct)
        api = _api(cfg, client)
        payload = api.build_payload(loc, client.ua)
        return Result('ok', 'DRY-RUN：将要提交的载荷（未联网）',
                      rows=[('账号', acct.get('label')),
                            ('POST', api.ep_checkin),
                            ('Body', json.dumps(payload, ensure_ascii=False, indent=2))])

    # 窗口闸门：--force 越窗；不在窗口时 dry-run 之外一律拦下
    if not force and not config.in_window(cfg):
        sched = cfg.get('schedule') or {}
        return Result('warn', '当前不在打卡窗口 %s–%s。确认要发用 --force（平台会拒窗外观景）'
                      % (sched.get('checkin_at'), sched.get('window_end')))

    if label or not cfg.get('accounts'):
        acct = resolve_account(cfg, label) if label else config.effective_accounts(cfg)[0]
        if acct is None:
            return Result('fail', '找不到账号 %s（account list 查看）' % label)
        kind, _label, text, rows = _checkin_one(cfg, log, acct, force, use_reverse)
        if _notify_active(cfg):
            titles = {'ok': '打卡成功', 'warn': '打卡异常', 'fail': '打卡失败',
                      'mfa': '需要验证码', 'not_configured': '未配置'}
            _push(cfg, log, [(acct.get('label'), titles.get(kind, '打卡结果'),
                              '%s：%s' % (acct.get('label'), text))])
        return Result(kind, text, rows=rows)

    # 多人且未指定账号 = auto 语义
    return do_auto(cfg, log, force=force, dry_run=False, use_reverse=use_reverse,
                   gate=False)


def do_auto(cfg, log, force=False, dry_run=False, use_reverse=False, gate=True):
    """日常一键：窗口内逐账号确保会话->未打卡就打卡->推送。"""
    if dry_run:
        return do_checkin(cfg, log, dry_run=True)
    if gate and not force and not config.in_window(cfg):
        sched = cfg.get('schedule') or {}
        return Result('warn', '不在打卡窗口 %s–%s，跳过（计划任务可放心挂着；'
                              '立刻执行用 auto --force）'
                      % (sched.get('checkin_at'), sched.get('window_end')))
    accounts = config.effective_accounts(cfg)
    results = []
    for acct in accounts:
        kind, label, text, rows = _checkin_one(cfg, log, acct, force, use_reverse)
        results.append((kind, label, text, rows))
    n = len(results)
    oks = [r for r in results if r[0] == 'ok']
    kinds = [r[0] for r in results]
    order = {'ok': 0, 'warn': 1, 'mfa': 2, 'not_configured': 3, 'fail': 4}
    worst = max(kinds, key=lambda k: order.get(k, 4)) if kinds else 'ok'
    rows = [('%s' % r[1], r[2]) for r in results]
    digest = '今晚汇总：%d/%d 成功 ｜ %s' % (len(oks), n,
                                           '；'.join('%s %s' % (r[1], r[2][:40]) for r in results))
    if _notify_active(cfg):
        if n == 1:
            titles = {'ok': '打卡成功', 'warn': '打卡异常', 'fail': '打卡失败',
                      'mfa': '需要验证码', 'not_configured': '未配置'}
            _push(cfg, log, [(results[0][1], titles.get(worst, '打卡结果'),
                              '%s：%s' % (results[0][1], results[0][2]))])
        else:
            account_events = []
            for kind, label, text, _rows in results:
                if kind == 'ok' and text.startswith('今日已打卡'):
                    title, body_txt = '今日已打卡', '%s：%s' % (label, text)
                else:
                    titles = {'ok': '打卡成功', 'warn': '打卡异常', 'fail': '打卡失败',
                              'mfa': '需要验证码', 'not_configured': '未配置'}
                    title, body_txt = titles.get(kind, '打卡结果'), '%s：%s' % (label, text)
                account_events.append((label, title, body_txt))
            _push(cfg, log, account_events, summary=('今晚汇总', digest))
    text = ('全部 %d 个账号打卡成功' % n if worst == 'ok'
            else '打卡完成，部分账号见明细（worst=%s）' % worst)
    return Result(worst if n > 1 else (kinds[0] if kinds else 'ok'), text, rows=rows)


def selftest(cfg, log):
    """离线自检：加密、配置、载荷、推送渲染。不联网。"""
    rows = []
    ok_all = True

    ok, detail = aes.self_test()
    rows.append(('AES 加密自检', 'PASS' if ok else 'FAIL: %s' % detail))
    ok_all = ok_all and ok

    problems = config.validate(cfg)
    errs = [p for p in problems if p[0] == 'error']
    warns = [p for p in problems if p[0] == 'warn']
    rows.append(('配置完整性', 'PASS' if not errs else
                 '; '.join('%s: %s' % (e[1], e[2]) for e in errs)))
    ok_all = ok_all and not errs
    for w in warns:
        rows.append(('提示', '%s: %s' % (w[1], w[2])))

    accounts = config.effective_accounts(cfg, only_enabled=False)
    rows.append(('打卡窗口', '%s–%s（当前%s）' % (
        config.get(cfg, 'schedule.checkin_at'), config.get(cfg, 'schedule.window_end'),
        '在内' if config.in_window(cfg) else '不在')))
    sched = cfg.get('schedule') or {}
    if _minutes_bad(sched):
        rows.append(('时间格式', 'FAIL: schedule 应为 HH:MM'))
        ok_all = False

    try:
        api = checkin_api.CheckinApi(None, cfg.get('app_id') or 294)
        for acct in accounts:
            loc = config.account_location(cfg, acct)
            payload = api.build_payload(loc, DEFAULT_UA)
            missing = [k for k in ('latitude', 'longitude', 'clientTime', 'userAgent')
                       if k not in payload]
            rows.append(('载荷字段[%s]' % acct.get('label'),
                         'PASS' if not missing else '缺: %s' % missing))
            ok_all = ok_all and not missing
    except Exception as e:
        rows.append(('打卡载荷字段', 'FAIL: %s' % e))
        ok_all = False

    channels = [c for c in config.iter_channels(cfg)
                if c.get('on') and (c.get('url') or '').strip()]
    if channels:
        for c in channels:
            try:
                method, url, body, ctype = push.render(
                    c['url'], int(c.get('style', 2)), '自检标题', '自检正文')
                shown = config.mask_url(url)
                rows.append(('推送渲染[%s]' % (c.get('label') or '默认'),
                             '%s %s%s' % (method, shown,
                                          '' if body is None else ' body=%d字符' % len(body))))
            except Exception as e:
                rows.append(('推送渲染[%s]' % (c.get('label') or '默认'), 'FAIL: %s' % e))
                ok_all = False
    else:
        rows.append(('推送', '未启用（仅提示）'))

    rows.append(('结果', 'PASS' if ok_all else 'FAIL'))
    return Result('ok' if ok_all else 'warn',
                  '自检通过' if ok_all else '自检发现问题，见明细', rows=rows)


def _minutes_bad(sched):
    import re as _re
    for key in ('checkin_at', 'window_end'):
        if not _re.match(r'^([01]?\d|2[0-3]):[0-5]\d$', str(sched.get(key) or '')):
            return True
    return False
