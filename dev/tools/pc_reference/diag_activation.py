#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""测"新会话多久才被微应用授权"（激活延迟）。

依据（2026-10-13）：
  - 用户实测：校园网上"登录信息未过期"时直接输打卡页网址能进；**全新登录后只能进平台首页**。
  - 我们的程序化登录：登录成功后立刻 `checkLogin` 被 302 退回首页、`today` 回 4001。

本脚本：全新登录一次，然后每 30 秒用**同一个会话**探测 `checkLogin`，
记录它从"退回首页"变成"H5 打卡页"需要多久（同时抽测业务接口 code）。

用法: python diag_activation.py [--minutes 15] [--interval 30]
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_cas          # noqa: E402
import wsc_checkin      # noqa: E402
from wsc_http import DEFAULT_UA, HttpClient  # noqa: E402

CONFIG = os.path.join(HERE, 'config.json')
JZSZ = 'https://jzsz.uestc.edu.cn'


def short(value: str) -> str:
    return (value[:8] + '…') if value else '(none)'


def main() -> int:
    parser = argparse.ArgumentParser(description='微应用授权激活延迟测量')
    parser.add_argument('--minutes', type=float, default=15.0)
    parser.add_argument('--interval', type=float, default=30.0)
    args = parser.parse_args()

    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    fingerprint = cfg.get('browser_fingerprint') or ''

    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    if fingerprint and not fingerprint.startswith('PUT_'):
        wsc_cas.set_fingerprint_cookie(client, fingerprint)

    print('登录前 jar: %s' % ','.join(sorted(client.jar.names())) or '(empty)')
    client.request('GET', JZSZ + '/')
    page = wsc_cas.fetch_login_page(client)
    before = client.jar.get('SESSION')
    resp = wsc_cas.submit_login(client, cfg['student_id'], cfg['password'], page)
    verdict, detail = wsc_cas.classify_login(resp)
    print('登录判定 = %s' % verdict)
    if verdict != 'OK':
        print('详情: %s' % str(detail)[:160])
        return 1
    client.follow(detail)
    after = client.jar.get('SESSION')
    print('票据换会话: SESSION %s => %s  是否新下发=%s' % (short(before), short(after), before != after))

    anchor = time.time()
    deadline = anchor + args.minutes * 60
    last_code = None
    while True:
        elapsed = time.time() - anchor
        stamp = '+%4.0fs' % elapsed
        try:
            resp = client.request('GET', wsc_checkin.CHECK_LOGIN)
            location = resp.header('Location')
            reached = '/daily-checkin-h5/' in location
            print('%s checkLogin -> HTTP %s  Location=%s  进入H5=%s'
                  % (stamp, resp.status, location[:70] or '(none)', reached))
        except Exception as exc:  # 网络抖动不应终止测量
            print('%s checkLogin 异常: %s' % (stamp, exc))
            reached = False

        # 每 2 分钟抽测一次业务接口（4001 → 0 是"被接受"的另一个信号）
        if reached or int(elapsed) % 120 < args.interval:
            try:
                resp = client.request('GET', wsc_checkin.EP_TODAY, headers=wsc_checkin.H5_HEADERS)
                payload = json.loads(resp.body.decode('utf-8', 'replace'))
                last_code = payload.get('code')
                print('%s today -> code=%s msg=%s' % (stamp, last_code, payload.get('msg')))
            except Exception as exc:
                print('%s today 异常: %s' % (stamp, exc))

        if reached and last_code == 0:
            print('=' * 70)
            print('结论：新会话在 +%.0f 秒 后同时满足 checkLogin→H5 与 today code=0' % elapsed)
            return 0
        if time.time() >= deadline:
            break
        time.sleep(args.interval)

    print('=' * 70)
    print('结论：%.0f 分钟内（checkLogin=%s, today code=%s）仍未授权' %
          (args.minutes, 'H5' if reached else '首页', last_code))
    return 2


if __name__ == '__main__':
    sys.exit(main())
