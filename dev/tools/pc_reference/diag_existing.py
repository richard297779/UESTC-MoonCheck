#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用"已保存的会话"直接测试微应用入口（不重新登录，零登录次数）。

用途：判定"窗口内建立的会话是否需要浏览器全套请求头才能进入微应用"。

用法: python diag_existing.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_cas          # noqa: E402
import wsc_checkin      # noqa: E402
from wsc_http import DEFAULT_UA, HttpClient  # noqa: E402

CONFIG = os.path.join(HERE, 'config.json')
JZSZ = 'https://jzsz.uestc.edu.cn'

MIN_HEADERS = {
    'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
    'Upgrade-Insecure-Requests': '1',
}

# 与抓包中浏览器请求逐字段对齐（含 Sec-Fetch / Sec-Ch-Ua）
BROWSER_HEADERS = {
    'Accept': ('text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,image/avif,'
               'image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7'),
    'Upgrade-Insecure-Requests': '1',
    'Pragma': 'no-cache',
    'Cache-Control': 'no-cache',
    'Sec-Fetch-Site': 'same-origin',
    'Sec-Fetch-Mode': 'navigate',
    'Sec-Fetch-User': '?1',
    'Sec-Fetch-Dest': 'document',
    'Sec-Ch-Ua': '"Google Chrome";v="155", "Chromium";v="155", "Not(A:Brand";v="24"',
    'Sec-Ch-Ua-Mobile': '?0',
    'Sec-Ch-Ua-Platform': '"Windows"',
    'Priority': 'u=0, i',
}


def short(value: str) -> str:
    return (value[:8] + '…') if value else '(none)'


def try_entry(client, label: str, headers: dict) -> bool:
    """尝试进入微应用并查状态，返回是否成功。"""
    resp = client.follow(wsc_checkin.CHECK_LOGIN, headers=headers)
    reached_h5 = '/daily-checkin-h5/' in resp.url
    print(f'  [{label}] checkLogin -> 最终 {resp.url[:76]}')
    print(f'  [{label}] 进入 H5 页: {reached_h5}')

    resp = wsc_checkin.today(client)
    try:
        payload = json.loads(resp.body.decode('utf-8', 'replace'))
    except json.JSONDecodeError:
        payload = {}
    code = payload.get('code')
    print(f'  [{label}] today -> HTTP {resp.status} code={code} msg={payload.get("msg")}')
    data = payload.get('data') or {}
    if isinstance(data, dict) and data:
        print(f'  [{label}]   recordStatusCode={data.get("recordStatusCode")} '
              f'checkinTime={data.get("checkinTime")}')
    return reached_h5 and code == 0


def main() -> int:
    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)

    session = cfg.get('session_cookie') or ''
    if not session or session.startswith('PUT_'):
        print('config.json 里没有可用的 session_cookie（请从浏览器 Application → Cookies 复制）')
        return 2
    print(f'使用已保存的 SESSION = {short(session)}（不重新登录）')

    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    client.jar.set('SESSION', session, 'jzsz.uestc.edu.cn', '/')
    fingerprint = cfg.get('browser_fingerprint') or ''
    if fingerprint and not fingerprint.startswith('PUT_'):
        wsc_cas.set_fingerprint_cookie(client, fingerprint)

    resp = client.request('GET', JZSZ + '/')
    print(f'0) GET / -> HTTP {resp.status}'
          f'{"（302 = 该会话已失效，请重新从浏览器复制）" if resp.status == 302 else "（200 = 会话有效）"}')
    if resp.status == 302:
        location = resp.header('Location')
        print(f'   Location: {location[:90]}')
        return 0

    print('--- 变体①：极简请求头（固件当前行为）---')
    ok_min = try_entry(client, 'minimal', MIN_HEADERS)
    print('--- 变体②：浏览器全套请求头 ---')
    ok_full = try_entry(client, 'browser', BROWSER_HEADERS)

    print('=' * 70)
    print(f'结论：极简头={"成功" if ok_min else "失败"}  全套头={"成功" if ok_full else "失败"}')
    if ok_min:
        print('  → 请求头不是关键；说明"窗口内建立的会话"本身可用（固件在窗口起点 21:00 预登录即可）')
    elif ok_full:
        print('  → 请求头是关键：需要给 enterApp 补齐 Sec-Fetch/Sec-Ch-Ua 等浏览器头')
    else:
        print('  → 该会话也无法进入微应用，需要重新在浏览器里确认会话是否仍然可用')
    return 0


if __name__ == '__main__':
    sys.exit(main())
