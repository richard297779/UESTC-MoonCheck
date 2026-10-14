#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""二分定位：门户 17 个 XHR 里，到底是哪一个（或哪几个）让微应用开始授权？

已证实（diag_portal_replay.py 阶段1）：全新登录后回放门户 XHR 序列 → checkLogin 302→H5、today code=0。
本脚本用**一次登录**做最小化：每步只调一个候选接口，立刻复测 checkLogin；第一次成功即锁定该接口。

设备侧只需要那一个（或两个）轻量调用，不能拉 30 KB 的新闻/100 KB 的图片。

用法: python diag_minimal_warmup.py
"""
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
H5_MARK = '/daily-checkin-h5/'

# 候选：按"响应体积从小到大"排列（设备侧越轻越好），并覆盖最可能的两个猜测
CANDIDATES = [
    ('isLoginFailure', 'GET', '/api/isLoginFailure', None),
    ('getNowUserInfo', 'GET', '/information-center/xd/backUser/getNowUserInfo', None),
    ('getWhitelistFlag', 'GET', '/information-center/xd/notice/getWhitelistFlag', None),
    ('getNewStudentWelcome', 'GET', '/information-center/xd/user/getNewStudentWelcome', None),
    ('getWorkStatistics', 'GET', '/message-center/api/index/getWorkStatistics', None),
    ('getNoticeStatistics', 'GET', '/message-center/api/index/getNoticeStatistics', None),
    ('systemGetInfo', 'GET', '/information-center/xd/config/system/getInfo', None),
    ('getAppById1', 'GET', '/information-center/xd/app/getAppById/1', None),
    ('getDisplays', 'GET', '/information-center/xd/config/getDisplays', None),
    ('getAppList', 'POST', '/information-center/xd/app/getAppList',
     {'categoryId': None, 'selectFlg': 1}),
    ('weatherCurrent', 'GET', '/information-center/xd/weather/current', None),
]

XHR_HEADERS = {
    'Accept': 'application/json, text/plain, */*',
    'Content-Type': 'application/json;charset=UTF-8',
    'Origin': JZSZ,
    'Referer': JZSZ + '/',
    'Cache-Control': 'no-cache',
    'Pragma': 'no-cache',
    'Sec-Fetch-Site': 'same-origin',
    'Sec-Fetch-Mode': 'cors',
    'Sec-Fetch-Dest': 'empty',
}


def measure(client, label):
    resp = client.request('GET', wsc_checkin.CHECK_LOGIN, headers={
        'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
        'Upgrade-Insecure-Requests': '1'})
    location = resp.header('Location') or ''
    reached = H5_MARK in location
    resp2 = client.request('GET', wsc_checkin.EP_TODAY, headers=wsc_checkin.H5_HEADERS)
    try:
        code = json.loads(resp2.body.decode('utf-8', 'replace')).get('code')
    except Exception:
        code = '?'
    print('  %-22s checkLogin=%s  today code=%s'
          % (label, 'H5 ✅' if reached else '首页 ❌', code))
    return reached, code == 0


def main() -> int:
    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    fingerprint = cfg.get('browser_fingerprint') or ''
    if fingerprint and not fingerprint.startswith('PUT_'):
        wsc_cas.set_fingerprint_cookie(client, fingerprint)

    print('[登录] 全新会话（消耗一次登录）...')
    client.request('GET', JZSZ + '/')
    page = wsc_cas.fetch_login_page(client)
    resp = wsc_cas.submit_login(client, cfg['student_id'], cfg['password'], page)
    verdict, detail = wsc_cas.classify_login(resp)
    if verdict != 'OK':
        print('  登录失败: %s' % str(detail)[:140])
        return 1
    client.follow(detail)
    print('  SESSION=%s…' % ((client.jar.get('SESSION') or '')[:8]))

    ok, _ = measure(client, '基线（未预热）')
    if ok:
        print('  意外：基线就通过（可能平台状态有缓存），本次二分无效，请隔一会儿重试')
        return 2

    unlocked = []
    for name, method, path, body in CANDIDATES:
        url = '%s%s?_t=%d' % (JZSZ, path, int(time.time() * 1000))
        try:
            if method == 'POST':
                resp = client.request('POST', url, headers=XHR_HEADERS, json_body=body)
            else:
                resp = client.request('GET', url, headers=XHR_HEADERS)
            size = len(resp.body or b'')
            print('  → %-20s %s  %6d B' % (name, resp.status, size))
        except Exception as exc:
            print('  → %-20s 异常 %s' % (name, exc))
        reached, api_ok = measure(client, '调用 %s 之后' % name)
        if reached and api_ok:
            unlocked.append(name)
            break

    print('=' * 72)
    if unlocked:
        print('★ 最小触发集：**%s**' % unlocked[0])
        print('  设备侧只需在登录成功后调用这一个接口，再进 checkLogin 即可。')
    else:
        print('单个接口都不够 → 需要"组合"（下一步二分两两组合，或干脆回放轻量前 5 个）。')
    return 0 if unlocked else 2


if __name__ == '__main__':
    sys.exit(main())
