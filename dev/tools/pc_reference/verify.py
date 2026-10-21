#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""晚安成电 · 打卡链路 Python 验证工具

执行顺序与判定标准见 docs/python-verification-plan.md（V0–V5）。
默认只打印必要信息：不输出密码、Cookie 值、姓名与手机号。

常用命令：
    python verify.py selftest               # V0 自检（不联网）
    python verify.py status                 # V1a 只读状态（需 config.session_cookie）
    python verify.py checkin --dry-run      # V1b 干跑，只看载荷
    python verify.py checkin                # V1c 真实打卡
    python verify.py login                  # V2 脱离浏览器登录
    python verify.py login --bfp random     # V3 用全新指纹（测免验证条件）
    python verify.py probe-session          # V4 会话寿命探测（建议每天一次）
    python verify.py auto                   # V5 全自动闭环
"""
import argparse
import datetime
import json
import os
import re
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_cas          # noqa: E402
import wsc_checkin      # noqa: E402
from wsc_http import HttpClient, DEFAULT_UA   # noqa: E402

CONFIG_PATH = os.path.join(HERE, 'config.json')
OUT_DIR = os.path.join(HERE, 'out')


# ---------------------------------------------------------------- 基础设施

def load_config():
    if not os.path.exists(CONFIG_PATH):
        sys.exit('未找到 %s\n请先执行：copy config.example.json config.json  然后填写。' % CONFIG_PATH)
    with open(CONFIG_PATH, 'r', encoding='utf-8') as f:
        return json.load(f)


def is_placeholder(v):
    return (not v) or str(v).startswith('PUT_')


def new_client(cfg):
    c = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    sid = cfg.get('session_cookie')
    if not is_placeholder(sid):
        c.jar.set('SESSION', sid, 'jzsz.uestc.edu.cn', '/')
    bfp = cfg.get('browser_fingerprint')
    if not is_placeholder(bfp):
        wsc_cas.set_fingerprint_cookie(c, bfp)
    return c


def save_session(client):
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, 'session.json')
    payload = {
        'saved_at': datetime.datetime.now().isoformat(timespec='seconds'),
        'cookies': dict((n, client.jar.get(n)) for n in client.jar.names()),
    }
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    print('    会话已保存 → %s（含凭据，勿外传）' % path)


def probe(client, quiet=False):
    """EP05 + EP06：返回 (ok, data, note)。"""
    try:
        wsc_checkin.enter_app(client)
    except Exception as e:
        return False, None, 'enter_app 失败: %s' % e
    r = wsc_checkin.today(client)
    if r.status in (301, 302):
        return False, None, '会话已失效（HTTP %s 被重定向）' % r.status
    if r.status != 200:
        return False, None, 'HTTP %s' % r.status
    try:
        j = r.json()
    except Exception:
        return False, None, '响应不是 JSON：%s' % r.text[:120]
    if j.get('code') != 0:
        return False, None, 'code=%s msg=%s' % (j.get('code'), j.get('msg'))
    return True, (j.get('data') or {}), ''


def print_status(data, client):
    print('[OK] 会话有效，今日状态查询成功')
    for k, v in wsc_checkin.safe_summary(data).items():
        print('      %-26s = %s' % (k, v))
    try:
        sched = wsc_checkin.schedule(client)
        if sched.status == 200:
            for k, v in wsc_checkin.schedule_summary(sched.json().get('data') or {}).items():
                print('      %-26s = %s' % ('schedule.' + k, v))
    except Exception as e:
        print('      [warn] 取时段失败: %s' % e)
    try:
        locs = wsc_checkin.locations(client)
        if locs.status == 200:
            for one in wsc_checkin.locations_summary(locs.json().get('data')):
                print('      %-26s = %s' % ('location',
                      '%s (%s, %s) r=%s m' % (one['locationName'], one['latitude'],
                                              one['longitude'], one['radiusMeter'])))
    except Exception as e:
        print('      [warn] 取打卡点失败: %s' % e)


def resolve_location(cfg, client, use_center, want_reverse):
    loc = dict(cfg.get('location') or {})
    if not loc:
        sys.exit('config.json 缺少 location')
    if use_center and cfg.get('fallback_location'):
        loc.update({'latitude': cfg['fallback_location']['latitude'],
                    'longitude': cfg['fallback_location']['longitude']})
        print('      [i] 使用打卡点中心坐标（距离 0 m）')
    addr = loc.get('reverse_geocode', '')
    if want_reverse:
        try:
            r = wsc_checkin.reverse_geocode(client, loc['latitude'], loc['longitude'])
            d = r.json().get('data')
            if isinstance(d, str) and d:
                addr = d
            elif isinstance(d, dict):
                for k in ('address', 'reverseGeocode', 'formattedAddress'):
                    if d.get(k):
                        addr = d[k]
                        break
            print('      [i] 逆地理编码结果：%s' % addr)
        except Exception as e:
            print('      [warn] 逆地理编码失败，沿用配置值：%s' % e)
    return loc, addr


def do_checkin(client, cfg, args):
    loc, addr = resolve_location(cfg, client, args.center, args.reverse)
    payload = wsc_checkin.build_payload(loc, client.ua, reverse_geocode=addr)

    if args.dry_run:
        print('[DRY-RUN] 将要发送的请求（未联网）：')
        print('      POST %s' % wsc_checkin.EP_CHECKIN)
        print('      Headers: %s' % json.dumps(wsc_checkin.H5_HEADERS, ensure_ascii=False))
        print('      Body:    %s' % json.dumps(payload, ensure_ascii=False))
        return 0

    print('[3/3] 提交打卡 ...')
    r = wsc_checkin.submit(client, payload)
    if r.status != 200:
        print('[FAIL] HTTP %s：%s' % (r.status, r.text[:200]))
        return 2
    try:
        j = r.json()
    except Exception:
        print('[FAIL] 响应不是 JSON：%s' % r.text[:200])
        return 2
    print('      响应：code=%s msg=%s data=%s ok=%s'
          % (j.get('code'), j.get('msg'), j.get('data'), j.get('ok')))
    if j.get('code') != 0:
        print('[FAIL] 业务失败，请把上面这行发我')
        return 2

    print('      复查状态 ...')
    ok, data, note = probe(client)
    if not ok:
        print('[WARN] 复查失败（%s），请手动确认' % note)
        return 1
    summary = wsc_checkin.safe_summary(data)
    if summary.get('recordStatusCode') == 'CHECKED_IN':
        print('[OK] 打卡成功并已复查确认：checkinTime=%s distanceMeter=%s abnormalFlag=%s'
              % (summary.get('checkinTime'), summary.get('distanceMeter'),
                 summary.get('abnormalFlag')))
        if summary.get('abnormalFlag'):
            print('[WARN] abnormalFlag=true（被判异常），请立即停止自动化并联系我')
            return 1
        return 0
    print('[WARN] 接口返回成功，但复查仍为 %s，请稍后人工确认' % summary.get('recordStatusCode'))
    return 1


# ---------------------------------------------------------------- 子命令

def cmd_selftest(args):
    print('Python : %s' % sys.version.split()[0])
    if sys.version_info < (3, 10):
        print('[WARN] 建议使用 Python 3.10+')
    ok = True

    try:
        import Crypto
        print('[OK] pycryptodome 可用（%s）' % getattr(Crypto, '__version__', '?'))
        from wsc_cas import encrypt_password, decrypt_like_server
        salt, pwd = 'SUcjRLLQehfjKUv7', 'Test123456'
        back = decrypt_like_server(encrypt_password(pwd, salt), salt)
        if back.endswith(pwd.encode('utf-8')):
            print('[OK] 加密算法自测通过（解出 %d 字节，尾部为原密码）' % len(back))
        else:
            print('[FAIL] 加密算法自测失败：解出的内容不匹配')
            ok = False
    except ImportError:
        print('[SKIP] pycryptodome 未安装（V1 不需要；V2/V3 需要：python -m pip install pycryptodome）')
    except Exception as e:
        print('[FAIL] 加密自测异常：%s' % e)
        ok = False

    root = os.path.abspath(os.path.join(HERE, '..', '..'))
    post_path = os.path.join(root, 'capture', 'raw', '#2-login-POST.txt')
    html_path = os.path.join(root, 'capture', 'raw', 'idas_login_page.html')
    if os.path.exists(post_path) and os.path.exists(html_path):
        try:
            from wsc_cas import decrypt_like_server
            txt = open(post_path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
            body = txt.split('\n\n')[-1]
            m = re.search(r'password=([^&]+)', body)
            h = open(html_path, encoding='utf-8', errors='replace').read()
            mm = re.search(r'id="pwdEncryptSalt"\s+value="([^"]+)"', h)
            if m and mm:
                back = decrypt_like_server(urllib.parse.unquote(m.group(1)), mm.group(1))
                tail = back[48:]
                printable = all(32 <= b < 127 for b in tail)
                print('[OK] 抓包密文复现：解出 %d 字节，密码段 %d 字节，可见字符=%s'
                      % (len(back), len(tail), printable))
        except ImportError:
            print('[SKIP] 抓包密文复现需要 pycryptodome')
        except Exception as e:
            print('[FAIL] 抓包密文复现失败：%s' % e)
            ok = False
    else:
        print('[SKIP] 未找到抓包样本 capture/raw/#2-login-POST.txt')

    if os.path.exists(CONFIG_PATH):
        cfg = load_config()
        bad = [k for k in ('student_id', 'password') if is_placeholder(cfg.get(k))]
        if bad:
            print('[WARN] config.json 仍为占位符：%s' % ', '.join(bad))
        else:
            print('[OK] config.json 基本字段已填写')
        if is_placeholder(cfg.get('session_cookie')):
            print('[WARN] session_cookie 未填 → V1 无法执行（可从浏览器再登录一次后抓取）')
        if is_placeholder(cfg.get('browser_fingerprint')):
            print('[WARN] browser_fingerprint 未填 → V2 的 captured 模式不可用')
    else:
        print('[SKIP] 未找到 config.json')

    print('[%s] 自检结束' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def cmd_status(args, cfg):
    client = new_client(cfg)
    print('[1/1] 进入微应用并查询状态 ...')
    ok, data, note = probe(client)
    if not ok:
        print('[FAIL] %s' % note)
        print('       若为会话失效：浏览器登录一次，把新的 SESSION 填回 config.json')
        return 2
    print_status(data, client)
    return 0


def cmd_checkin(args, cfg):
    client = new_client(cfg)
    print('[1/3] 进入微应用并查询状态 ...')
    ok, data, note = probe(client)
    if not ok:
        print('[FAIL] %s' % note)
        return 2
    status = data.get('recordStatusCode')
    print('      今日状态：%s' % status)
    if status == 'CHECKED_IN' and not args.force:
        print('[OK] 今天已打卡（%s），跳过。如需强制重发加 --force'
              % data.get('checkinTime'))
        return 0
    print('[2/3] 构造载荷 ...')
    return do_checkin(client, cfg, args)


def cmd_login(args, cfg):
    client = new_client(cfg)
    if args.bfp == 'captured':
        mode, value = 'captured', cfg.get('browser_fingerprint')
    elif args.bfp == 'random':
        mode, value = 'random', None
    else:
        mode, value = 'none', None
    verdict, detail = wsc_cas.login(client, cfg.get('student_id'), cfg.get('password'),
                                    bfp_mode=mode, bfp_value=value)
    if verdict == 'OK':
        save_session(client)
        print('[OK] 登录链可脱离浏览器复现（模式：%s）' % mode)
        print('      用新会话复查状态（端到端证明）...')
        ok, data, note = probe(client)
        if ok:
            print('      [OK] 新会话可用，今日状态：%s'
                  % (data or {}).get('recordStatusCode'))
            print('      [OK] 登录 → 会话 → 打卡接口 全链路打通')
        else:
            print('      [WARN] 新会话查询失败：%s' % note)
        return 0
    if verdict == 'MFA':
        print('[MFA] 服务端要求二次验证 —— 这是重要结论，别急着重试')
        print('      含义：该指纹未被服务端信任。设备侧两种应对：')
        print('        a) 固定使用抓包里的指纹（已被信任），再试 --bfp captured')
        print('        b) 保留"蓝牙回传验证码"能力作为兜底（固件 F2）')
        return 3
    print('[FAIL] 判定=%s 详情=%s' % (verdict, wsc_cas.mask_ticket(str(detail))[:300]))
    return 2


def cmd_auto(args, cfg):
    client = new_client(cfg)
    print('[1/4] 复用会话查询状态 ...')
    ok, data, note = probe(client)
    if not ok:
        print('      会话不可用（%s）→ 执行登录' % note)
        verdict, detail = wsc_cas.login(client, cfg.get('student_id'), cfg.get('password'),
                                        bfp_mode='captured',
                                        bfp_value=cfg.get('browser_fingerprint'))
        if verdict != 'OK':
            print('[FAIL] 登录失败：%s（详情 %s）' % (verdict, wsc_cas.mask_ticket(str(detail))[:200]))
            return 3 if verdict == 'MFA' else 2
        save_session(client)
        ok, data, note = probe(client)
        if not ok:
            print('[FAIL] 登录后仍无法查询状态：%s' % note)
            return 2
    status = data.get('recordStatusCode')
    print('      今日状态：%s' % status)
    if status == 'CHECKED_IN' and not args.force:
        print('[2/4] 已打卡，无需操作 [OK]')
        return 0
    print('[2/4] 构造载荷 ...')
    return do_checkin(client, cfg, args)


def cmd_probe_session(args, cfg):
    client = new_client(cfg)
    ok, data, note = probe(client)
    line = '%s\t%s\t%s\t%s' % (datetime.datetime.now().isoformat(timespec='seconds'),
                               'OK' if ok else 'FAIL', note or '',
                               (data or {}).get('recordStatusCode', ''))
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, 'session_probe.log'), 'a', encoding='utf-8') as f:
        f.write(line + '\n')
    print('[%s] %s' % ('OK' if ok else 'FAIL', line))
    return 0 if ok else 2


def main():
    p = argparse.ArgumentParser(description='晚安成电打卡链路验证工具')
    sub = p.add_subparsers(dest='cmd')

    sub.add_parser('selftest', help='V0：离线自检')

    sub.add_parser('status', help='V1a：只读查询今日状态')
    sub.add_parser('probe-session', help='V4：会话寿命探测（建议每天一次）')

    sp = sub.add_parser('checkin', help='V1b/V1c：提交打卡')
    sp.add_argument('--dry-run', action='store_true', help='只打印载荷，不发送')
    sp.add_argument('--force', action='store_true', help='即使今天已打卡也重发（默认跳过）')
    sp.add_argument('--center', action='store_true', help='改用打卡点官方中心坐标')
    sp.add_argument('--reverse', action='store_true', help='先调用逆地理编码取地址')

    sp = sub.add_parser('login', help='V2/V3：登录链')
    sp.add_argument('--bfp', choices=['captured', 'random', 'none'], default='captured',
                    help='指纹策略：抓包值/随机值/不上报')

    sp = sub.add_parser('auto', help='V5：全自动闭环')
    sp.add_argument('--dry-run', action='store_true')
    sp.add_argument('--force', action='store_true')
    sp.add_argument('--center', action='store_true')
    sp.add_argument('--reverse', action='store_true')

    args = p.parse_args()
    if not args.cmd:
        p.print_help()
        return 0
    if args.cmd == 'selftest':
        return cmd_selftest(args)

    cfg = load_config()
    table = {
        'status': cmd_status,
        'checkin': cmd_checkin,
        'login': cmd_login,
        'auto': cmd_auto,
        'probe-session': cmd_probe_session,
    }
    return table[args.cmd](args, cfg)


if __name__ == '__main__':
    sys.exit(main())
