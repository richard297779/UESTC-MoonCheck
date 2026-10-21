#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T5：测量"已授权会话"的生命周期（绝对 TTL / 是否滑动续期）。

用法（浏览器登录一次，把 SESSION 填进 config.json 后运行）：
    python diag_ttl.py --interval 300             # 只观测：每 5 分钟测一次，直到被拒
    python diag_ttl.py --interval 300 --activity  # 对照组：每轮顺带 GET 首页（模拟正常使用）

输出是一条时间线；最后一行给出存活时长。
两组对比即可回答：伴生程序需要多久刷新一次会话。
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_checkin      # noqa: E402
from wsc_http import DEFAULT_UA, HttpClient  # noqa: E402

CONFIG = os.path.join(HERE, 'config.json')
JZSZ = 'https://jzsz.uestc.edu.cn'
H5_MARK = '/daily-checkin-h5/'


def main() -> int:
    parser = argparse.ArgumentParser(description='SESSION 生命周期测量')
    parser.add_argument('--interval', type=float, default=300.0, help='探测间隔秒（默认 300）')
    parser.add_argument('--max-hours', type=float, default=6.0)
    parser.add_argument('--activity', action='store_true', help='每轮访问首页，模拟正常使用（测滑动）')
    args = parser.parse_args()

    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    session = cfg.get('session_cookie') or ''
    if not session or session.startswith('PUT_'):
        print('config.json 缺少 session_cookie（浏览器登录后复制）')
        return 2

    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    client.jar.set('SESSION', session, 'jzsz.uestc.edu.cn', '/')
    fingerprint = cfg.get('browser_fingerprint') or ''
    if fingerprint and not fingerprint.startswith('PUT_'):
        import wsc_cas  # noqa: WPS433
        wsc_cas.set_fingerprint_cookie(client, fingerprint)

    anchor = time.time()
    deadline = anchor + args.max_hours * 3600
    while time.time() < deadline:
        elapsed = time.time() - anchor
        stamp = time.strftime('%H:%M:%S')
        try:
            resp = client.request('GET', wsc_checkin.CHECK_LOGIN)
            location = resp.header('Location') or ''
            reached = H5_MARK in location
            line = '[%s +%4.0fm] checkLogin=%s' % (
                stamp, elapsed / 60, 'H5✅' if reached else ('首页❌' if resp.status == 302 else str(resp.status)))
            if not reached:
                print(line)
                print('=' * 70)
                print('结论：会话在 +%.0f 分钟（约 %.1f 小时）后失去授权%s'
                      % (elapsed / 60, elapsed / 3600,
                         '（含每轮访问首页的"活动"）' if args.activity else '（纯观测，无活动）'))
                return 0
            resp = client.request('GET', wsc_checkin.EP_TODAY, headers=wsc_checkin.H5_HEADERS)
            try:
                code = json.loads(resp.body.decode('utf-8', 'replace')).get('code')
            except json.JSONDecodeError:
                code = '?'
            print('%s today=code%s' % (line, code))
            if code != 0:
                print('=' * 70)
                print('结论：checkLogin 仍放行但业务接口已拒绝（+%.0f 分钟）→ 以先失效者为准' % (elapsed / 60))
                return 0
        except Exception as exc:
            print('[%s +%4.0fm] 探测异常（不计为失效）: %s' % (stamp, elapsed / 60, exc))

        if args.activity:
            try:
                client.request('GET', JZSZ + '/')
            except Exception:
                pass
        time.sleep(args.interval)

    print('结论：%.1f 小时内在%s模式下始终有效' % (args.max_hours, '有活动' if args.activity else '无活动'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
