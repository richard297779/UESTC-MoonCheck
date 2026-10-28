# -*- coding: utf-8 -*-
"""命令行界面。

    MoonCheck status [login|checkin|auto] [-a 账号]
    MoonCheck config show|init|set|path
    MoonCheck account list|add|rm|on|off
    MoonCheck notify list|set|add|rm|on|off|link|test
    MoonCheck sched [set <开始> <截止>]
    MoonCheck selftest
    不带参数 = 打开图形界面。

退出码：0 成功 / 1 部分成功或警告 / 2 失败 / 3 需要二次验证。
"""
import argparse
import getpass
import os
import sys

from . import config, paths, push, service, privacy


def _log_factory(verbose):
    def log(msg):
        if verbose:
            print(privacy.sanitize(msg, config.load(create=False)))
    return log


def _print_result(res):
    stream = sys.stdout
    mark = {'ok': '[OK]  ', 'warn': '[WARN]', 'fail': '[FAIL]',
            'mfa': '[MFA] ', 'not_configured': '[NEED]'}
    cfg = config.load(create=False)
    stream.write('%s %s\n' % (mark.get(res.kind, '[..]'), privacy.sanitize(res.text, cfg)))
    for name, value in res.rows:
        stream.write('  - %s: %s\n' % (name, privacy.sanitize(value, cfg)))


# ------------------------------------------------------------------ config

def cmd_config(rest):
    parser = argparse.ArgumentParser(prog='MoonCheck config', description='配置管理')
    parser.add_argument('action', choices=['show', 'init', 'set', 'path'])
    parser.add_argument('key', nargs='?')
    parser.add_argument('value', nargs='?')
    parser.add_argument('--all', action='store_true', help='兼容参数；敏感字段仍隐藏')
    parser.add_argument('--force', action='store_true', help='init 时覆盖已有文件')
    ns = parser.parse_args(rest)

    if ns.action == 'path':
        print(paths.config_path())
        return 0

    if ns.action == 'init':
        path = paths.config_path()
        if os.path.exists(path) and not ns.force:
            print('已存在：%s（覆盖请加 --force）' % path)
            return 1
        config.save(config.default_config())
        print('已生成默认配置：%s' % path)
        print('用 config set <字段> <值> 填写，或直接编辑该 JSON 文件。')
        return 0

    cfg = config.load()

    if ns.action == 'show':
        print('# %s' % paths.config_path())
        for key in sorted(config.EDITABLE):
            value = config.masked_value(cfg, key)
            print('  %-28s %s   # %s' % (key, value, config.describe_field(key)))
        _print_accounts(cfg)
        _print_channels(cfg)
        problems = config.validate(cfg)
        for level, field, msg in problems:
            print('[%s] %s: %s' % (level.upper(), field, msg))
        return 0 if not [p for p in problems if p[0] == 'error'] else 1

    if ns.action == 'set':
        if not ns.key:
            print('用法：config set <字段> [值]。可设置：\n  '
                  + '\n  '.join('%s（%s）' % (k, config.EDITABLE[k]) for k in sorted(config.EDITABLE)))
            return 2
        if ns.key == 'password' and ns.value is None:
            if not sys.stdin or not sys.stdin.isatty():
                print('隐藏输入需要交互终端；非交互请传值，或直接编辑 config.json')
                return 2
            ns.value = getpass.getpass('输入密码（不回显）: ')
        if ns.value is None:
            print('请提供值（或使用 --help 查看）')
            return 2
        try:
            old, new = config.set_value(cfg, ns.key, ns.value)
        except ValueError as e:
            print(str(e))
            return 2
        if ns.key == 'session_cookie' and new and new.startswith('SESSION='):
            cfg['session_cookie'] = new.split('=', 1)[1].strip()
        path = config.save(cfg)
        shown_old = '******' if ns.key == 'password' else old
        shown_new = '******' if ns.key == 'password' else (
            new[:6] + '…' if ns.key == 'session_cookie' else new)
        print('%s: %s -> %s（已保存到 %s）' % (ns.key, shown_old, shown_new, path))
        return 0


def _print_accounts(cfg):
    accounts = cfg.get('accounts') or []
    if not accounts:
        print('# 账号表：空（单人模式，凭证取顶层 student_id/password）')
        return
    print('# 账号表（%d 个）：' % len(accounts))
    for a in accounts:
        loc = a.get('location')
        loc_txt = ('%s,%s' % (loc.get('latitude'), loc.get('longitude'))) \
            if isinstance(loc, dict) else '沿用顶层'
        print('  %-8s %-6s 学号=%s 指纹=%s 会话=%s 坐标=%s 最近=%s' % (
            a.get('label'), '启用' if a.get('enabled', True) else '禁用',
            str(a.get('student_id') or '')[:4] + '****',
            '有' if a.get('browser_fingerprint') else '无',
            '有' if a.get('session_cookie') else '无',
            loc_txt, a.get('last_msg') or '-'))


def _print_channels(cfg):
    channels = config.iter_channels(cfg)
    print('# 推送通道：')
    for c in channels:
        label = c.get('label') or '默认'
        url = c.get('url') or ''
        print('  %-8s %-6s style=%s accounts=%-12s url=%s' % (
            label, '启用' if c.get('on') and url else '关闭',
            c.get('style', 2), ','.join(c.get('accounts') or ['all']),
            config.mask_url(url) if url else '(未设置)'))


# ------------------------------------------------------------------ account

def cmd_account(rest):
    parser = argparse.ArgumentParser(
        prog='MoonCheck account',
        description='多账号管理（最多 %d 个；账号表为空=单人模式）' % config.MAX_ACCOUNTS)
    sub = parser.add_subparsers(dest='action')
    sub.add_parser('list')
    p_add = sub.add_parser('add', help='新增/更新账号')
    p_add.add_argument('label')
    p_add.add_argument('student_id', nargs='?')
    p_add.add_argument('password', nargs='?')
    p_add.add_argument('fingerprint', nargs='?')
    p_add.add_argument('extra', nargs='*',
                       help='key=value：lat= lng= addr= enabled=true|false')
    p_rm = sub.add_parser('rm', help='删除账号')
    p_rm.add_argument('label')
    for name in ('on', 'off'):
        p = sub.add_parser(name)
        p.add_argument('label')
    ns = parser.parse_args(rest)

    cfg = config.load()
    accounts = cfg.setdefault('accounts', [])

    if ns.action in (None, 'list'):
        _print_accounts(cfg)
        return 0

    if ns.action == 'add':
        if len(accounts) >= config.MAX_ACCOUNTS and not config.find_account(cfg, ns.label):
            print('最多 %d 个账号（与固件一致）。先 account rm 一个。' % config.MAX_ACCOUNTS)
            return 2
        import re
        if not re.match(config._LABEL, ns.label):
            print('标签只允许 A-Za-z0-9-_，≤%d 字符' % 12)
            return 2
        acct = config.find_account(cfg, ns.label) or dict(config.ACCOUNT_TEMPLATE)
        acct['label'] = ns.label
        if ns.student_id:
            config.reset_identity(acct,ns.student_id)
            acct['student_id'] = ns.student_id
        if ns.password:
            acct['password'] = ns.password
        if not ns.password and not acct.get('password') and sys.stdin and sys.stdin.isatty():
            acct['password'] = getpass.getpass('账号 %s 的密码（不回显）: ' % ns.label)
        elif not ns.password and not acct.get('password'):
            print('需要密码（第三个参数或隐藏输入；非交互请直接编辑 config.json）')
            return 2
        if ns.fingerprint:
            acct['browser_fingerprint'] = ns.fingerprint
        for kv in ns.extra or []:
            if '=' not in kv:
                print('忽略无法解析的参数：%s（应为 key=value）' % kv)
                continue
            key, value = kv.split('=', 1)
            if key == 'lat':
                if not isinstance(acct.get('location'), dict):
                    acct['location'] = dict(config.LOCATION_DEFAULTS)
                acct['location']['latitude'] = float(value)
            elif key == 'lng':
                if not isinstance(acct.get('location'), dict):
                    acct['location'] = dict(config.LOCATION_DEFAULTS)
                acct['location']['longitude'] = float(value)
            elif key == 'addr':
                if not isinstance(acct.get('location'), dict):
                    acct['location'] = dict(config.LOCATION_DEFAULTS)
                acct['location']['reverse_geocode'] = value
            elif key == 'enabled':
                acct['enabled'] = value.lower() in ('1', 'true', 'yes', 'on', '开')
            else:
                print('忽略未知参数：%s' % key)
        existing = config.find_account(cfg, ns.label)
        if existing is None:
            accounts.append(acct)
        path = config.save(cfg)
        print('账号 %s 已保存（%s）' % (ns.label, path))
        _print_accounts(cfg)
        return 0

    if ns.action == 'rm':
        acct = config.find_account(cfg, ns.label)
        if acct is None:
            print('找不到账号 %s' % ns.label)
            return 2
        accounts.remove(acct)
        config.save(cfg)
        print('已删除账号 %s' % ns.label)
        return 0

    if ns.action in ('on', 'off'):
        acct = config.find_account(cfg, ns.label)
        if acct is None:
            print('找不到账号 %s' % ns.label)
            return 2
        acct['enabled'] = ns.action == 'on'
        config.save(cfg)
        print('账号 %s 已%s' % (ns.label, '启用' if acct['enabled'] else '禁用'))
        return 0


# ------------------------------------------------------------------ notify

def _parse_kv(pairs, int_keys=()):
    out = {}
    for kv in pairs or []:
        if '=' not in kv:
            continue
        key, value = kv.split('=', 1)
        if key in int_keys:
            out[key] = int(value)
        elif key == 'on':
            out[key] = value.lower() in ('1', 'true', 'yes', 'on', '开')
        else:
            out[key] = value
    return out


def cmd_notify(rest):
    parser = argparse.ArgumentParser(
        prog='MoonCheck notify',
        description='推送通道：默认通道（Server酱/Bark/ntfy…）+ 最多 %d 个额外通道，'
                    '额外通道可关联具体账号' % config.MAX_CHANNELS)
    sub = parser.add_subparsers(dest='action')
    sub.add_parser('list')
    p_set = sub.add_parser('set', help='配置默认通道')
    p_set.add_argument('pairs', nargs='*', help='url=… style=0|1|2 on=true|false')
    p_add = sub.add_parser('add', help='新增/更新额外通道')
    p_add.add_argument('label')
    p_add.add_argument('pairs', nargs='*',
                       help='url=… [style=…] [accounts=A01,A02|all] [on=…]')
    for name in ('rm', 'on', 'off'):
        p = sub.add_parser(name)
        p.add_argument('label')
    p_link = sub.add_parser('link', help='设置通道关联账号')
    p_link.add_argument('label')
    p_link.add_argument('accounts', help='逗号分隔账号标签，或 all')
    p_test = sub.add_parser('test', help='立即发一条测试消息')
    p_test.add_argument('args', nargs='*', help='[chan=标签] [文本]')
    ns = parser.parse_args(rest)

    cfg = config.load()
    notify = config.notify_conf(cfg)
    channels = notify.setdefault('channels', [])

    def find_channel(label):
        for c in channels:
            if c.get('label') == label:
                return c
        return None

    if ns.action in (None, 'list'):
        _print_channels(cfg)
        return 0

    if ns.action == 'set':
        kv = _parse_kv(ns.pairs, int_keys=('style', 'gap', 'quota'))
        if not kv:
            print('用法：notify set url=… [style=0|1|2] [on=true|false]')
            return 2
        for key in ('url', 'style', 'on', 'gap', 'quota'):
            if key in kv:
                notify[key] = kv[key]
        config.save(cfg)
        print('默认通道已更新')
        _print_channels(cfg)
        return 0

    if ns.action == 'add':
        kv = _parse_kv(ns.pairs, int_keys=('style',))
        if 'url' not in kv and find_channel(ns.label) is None:
            print('新增通道需要 url=…')
            return 2
        import re
        if not re.match(config._LABEL, ns.label):
            print('通道标签只允许 A-Za-z0-9-_，≤12 字符')
            return 2
        ch = find_channel(ns.label) or dict(config.CHANNEL_TEMPLATE)
        ch['label'] = ns.label
        if 'url' in kv:
            ch['url'] = kv['url']
        if 'style' in kv:
            ch['style'] = kv['style']
        if 'on' in kv:
            ch['on'] = kv['on']
        if 'accounts' in kv:
            ch['accounts'] = [t.strip() for t in kv['accounts'].split(',') if t.strip()]
        if 'accounts' not in kv and not ch.get('accounts'):
            ch['accounts'] = ['all']
        if find_channel(ns.label) is None:
            if len(channels) >= config.MAX_CHANNELS:
                print('额外通道最多 %d 条' % config.MAX_CHANNELS)
                return 2
            channels.append(ch)
        config.save(cfg)
        print('通道 %s 已保存' % ns.label)
        _print_channels(cfg)
        return 0

    if ns.action == 'rm':
        ch = find_channel(ns.label)
        if ch is None:
            print('找不到通道 %s' % ns.label)
            return 2
        channels.remove(ch)
        config.save(cfg)
        print('已删除通道 %s' % ns.label)
        return 0

    if ns.action in ('on', 'off'):
        ch = find_channel(ns.label)
        if ch is None:
            print('找不到通道 %s' % ns.label)
            return 2
        ch['on'] = ns.action == 'on'
        config.save(cfg)
        print('通道 %s 已%s' % (ns.label, '启用' if ch['on'] else '关闭'))
        return 0

    if ns.action == 'link':
        ch = find_channel(ns.label)
        if ch is None:
            print('找不到通道 %s' % ns.label)
            return 2
        ch['accounts'] = [t.strip() for t in ns.accounts.split(',') if t.strip()] or ['all']
        config.save(cfg)
        print('通道 %s 关联：%s' % (ns.label, ','.join(ch['accounts'])))
        _known = config.account_labels(cfg)
        missing = [a for a in ch['accounts'] if a not in _known and a != 'all']
        for m in missing:
            print('[提示] 账号 %s 尚不存在（account add %s …）' % (m, m))
        return 0

    if ns.action == 'test':
        chan_label, body = '', 'MoonCheck 桌面端测试消息 %s' % \
            config.datetime_now().strftime('%H:%M:%S')
        for tok in ns.args or []:
            if tok.startswith('chan='):
                chan_label = tok.split('=', 1)[1]
            else:
                body = tok
        target = config.default_channel(cfg)
        if chan_label:
            ch = find_channel(chan_label)
            if ch is None:
                print('找不到通道 %s' % chan_label)
                return 2
            target = ch
        if not (target.get('url') or '').strip():
            print('通道 %s 未配置 URL' % (chan_label or '默认'))
            return 2
        ok, note = push.send(target, '推送测试', body, timeout=cfg.get('timeout') or 15)
        print('%s %s' % ('[OK]  ' if ok else '[FAIL]', note))
        return 0 if ok else 1


# ------------------------------------------------------------------ sched

def cmd_sched(rest):
    parser = argparse.ArgumentParser(prog='MoonCheck sched',
                                     description='打卡窗口（auto 只在窗口内执行，--force 越窗）')
    parser.add_argument('action', nargs='?', default='show', choices=['show', 'set'])
    parser.add_argument('at', nargs='?')
    parser.add_argument('end', nargs='?')
    ns = parser.parse_args(rest)

    cfg = config.load()
    sched = cfg.setdefault('schedule', dict(config.DEFAULTS['schedule']))
    if ns.action == 'show':
        print('打卡窗口：%s – %s（现在%s窗口内）' % (
            sched.get('checkin_at'), sched.get('window_end'),
            '在' if config.in_window(cfg) else '不在'))
        return 0
    if not ns.at:
        print('用法：sched set <开始 HH:MM> [截止 HH:MM]')
        return 2
    try:
        config.set_value(cfg, 'schedule.checkin_at', ns.at)
        if ns.end:
            config.set_value(cfg, 'schedule.window_end', ns.end)
    except ValueError as e:
        print(str(e))
        return 2
    problems = [p for p in config.validate(cfg) if p[0] == 'error'
                and p[1].startswith('schedule')]
    if problems:
        print('; '.join('%s: %s' % (p[1], p[2]) for p in problems))
        return 2
    config.save(cfg)
    print('打卡窗口：%s – %s' % (sched.get('checkin_at'), sched.get('window_end')))
    return 0


def cmd_wechat(rest, verbose=False):
    import json
    from . import wechat
    parser = argparse.ArgumentParser(prog='MoonCheck wx', description='微信授权与取票（测试不提交打卡）')
    parser.add_argument('action', choices=['import','status','test','clear'])
    parser.add_argument('-a','--account')
    parser.add_argument('--file', help='从本人授权 JSON 导入，避免把凭据放到命令参数')
    ns = parser.parse_args(rest)
    cfg = config.load()
    accounts = config.effective_accounts(cfg, only_enabled=False)
    if ns.action == 'status':
        for acct in accounts:
            if ns.account and ns.account != acct['label']: continue
            value = acct.get('wechat') or {}
            print('%s: 授权%s；到期 Unix 秒=%s' % (acct['label'], '已保存' if value.get('refresh') else '未配置',value.get('expires_at',0)))
        return 0
    acct = service.resolve_account(cfg,ns.account) if ns.account else (accounts[0] if not cfg.get('accounts') else None)
    if acct is None:
        print('多人模式必须使用 -a 指定已有账号'); return 2
    try:
        if ns.action == 'import':
            if ns.file:
                with open(ns.file,'r',encoding='utf-8-sig') as stream:
                    text = stream.read(65537)
                if len(text)>65536: raise ValueError('授权文件过大')
                value = json.loads(text)
            else:
                if not sys.stdin.isatty(): raise ValueError('请使用交互终端或 --file 授权文件')
                value = {key:getpass.getpass(key+'（不回显）: ') for key in ('openid','access','refresh')}
                value['expires_at'] = input('绝对到期 Unix 秒（回车=0，首次刷新）: ') or 0
            value = wechat.validate_credentials(value)
            config.save_account_wechat(cfg,acct,value)
            print('授权已保存到指定账号，不显示凭据'); return 0
        if ns.action == 'clear':
            config.save_account_wechat(cfg,acct,{})
            print('该账号微信授权已清除'); return 0
        result = service.do_wechat_test(cfg,_log_factory(verbose),label=acct['label'])
        _print_result(result); return result.exit_code()
    except (ValueError,OSError,TypeError):
        print('微信授权操作失败，请检查文件格式、输入或文件权限（不显示敏感内容）'); return 2


# ------------------------------------------------------------------ 主入口

def _main_cli(argv):
    parser = argparse.ArgumentParser(
        prog='MoonCheck',
        description='电子科技大学「晚安成电」晚间打卡桌面工具（单文件版）',
        epilog='不带参数运行 = 打开图形界面。文档见 desktop/README.md。')
    parser.add_argument('command', nargs='?', default='gui',
                        help='gui|selftest|status|login|checkin|auto|config|account|notify|sched|wx|help')
    parser.add_argument('-v', '--verbose', action='store_true', help='打印每步 HTTP 日志')
    parser.add_argument('--config', help='指定 config.json 路径（默认为程序同目录）')
    from . import __version__
    parser.add_argument('--version', action='version',
                        version='MoonCheck Desktop %s' % __version__)
    args, rest = parser.parse_known_args(argv)

    if args.config:
        os.environ['MOONCHECK_CONFIG'] = os.path.abspath(args.config)

    if args.command in (None, 'gui', '', 'help', '--help', '-h'):
        if args.command in ('help', '--help', '-h'):
            parser.print_help()
            return 0
        from .gui import run_gui
        return run_gui()

    if args.command == 'wx':
        return cmd_wechat(rest,args.verbose)
    if args.command == 'config':
        return cmd_config(rest)
    if args.command == 'account':
        return cmd_account(rest)
    if args.command == 'notify':
        return cmd_notify(rest)
    if args.command == 'sched':
        return cmd_sched(rest)

    cfg = config.load()
    log = _log_factory(args.verbose)

    if args.command == 'selftest':
        res = service.selftest(cfg, log)
    elif args.command == 'login':
        sub = argparse.ArgumentParser(prog='MoonCheck login')
        sub.add_argument('-a', '--account', help='仅登录该账号（省略=所有启用账号）')
        ns = sub.parse_args(rest)
        res = service.do_login(cfg, log, label=ns.account)
    elif args.command == 'status':
        sub = argparse.ArgumentParser(prog='MoonCheck status')
        sub.add_argument('-a', '--account', help='仅查该账号（省略=逐个启用账号）')
        ns = sub.parse_args(rest)
        res = service.do_status(cfg, log, label=ns.account)
    elif args.command == 'checkin':
        sub = argparse.ArgumentParser(prog='MoonCheck checkin')
        sub.add_argument('label', nargs='?', help='账号标签；省略=单人模式或全部账号')
        sub.add_argument('--dry-run', action='store_true', help='只打印将要提交的载荷，不联网')
        sub.add_argument('--force', action='store_true',
                         help='忽略打卡窗口/已打卡状态强制发送')
        sub.add_argument('--reverse', action='store_true', help='提交前用平台逆地理编码校准地址')
        ns = sub.parse_args(rest)
        res = service.do_checkin(cfg, log, label=ns.label, force=ns.force,
                                 dry_run=ns.dry_run, use_reverse=ns.reverse)
    elif args.command == 'auto':
        sub = argparse.ArgumentParser(prog='MoonCheck auto')
        sub.add_argument('--dry-run', action='store_true')
        sub.add_argument('--force', action='store_true', help='越窗立即执行（调试用）')
        ns = sub.parse_args(rest)
        res = service.do_auto(cfg, log, dry_run=ns.dry_run, force=ns.force)
    else:
        print('未知命令：%s（MoonCheck help 查看全部）' % args.command)
        return 2

    _print_result(res)
    return res.exit_code()


def main_cli(argv):
    from .operation_lock import operation_lock, OperationBusy
    locator = argparse.ArgumentParser(add_help=False)
    locator.add_argument('--config'); locator.add_argument('-v','--verbose',action='store_true')
    locator.add_argument('command',nargs='?',default='gui')
    ns,_ = locator.parse_known_args(argv)
    if ns.config: os.environ['MOONCHECK_CONFIG'] = os.path.abspath(ns.config)
    if ns.command in ('gui','help',''):
        return _main_cli(argv)
    try:
        with operation_lock(paths.config_path()): return _main_cli(argv)
    except OperationBusy as error:
        print(str(error)); return 1
    except OSError:
        print('配置文件不可访问，请检查路径与权限'); return 2
