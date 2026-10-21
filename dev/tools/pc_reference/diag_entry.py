#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""微应用入口（EP05）逐跳诊断 —— 定位"设备能登录、却进不了晚安成电界面"。

背景（2026-10-12/13 实测）：
  - 浏览器：任意时间都能进入 `晚安成电`（`checkLogin` 302 → `/daily-checkin-h5/...`）。
  - 设备：CAS 登录成功（拿到 SESSION），但 `checkLogin` 最终没有落在 H5 页
          （固件判据 = 最终 URL 是否包含 `/daily-checkin-h5/`）。

本脚本**逐跳打印**（不自动跟随），把"302 去哪了"彻底看清，并对比几种请求头，
回答两个问题：
  Q1 平台是否真的按"会话建立时间是否在打卡窗口内"决定放行？→ 看 A/B 组结果
  Q2 若浏览器头能进、极简头不能进 → 缺的是哪些头？

用法:
    python diag_entry.py                # 重新登录（消耗一次登录）+ 全套对照
    python diag_entry.py --no-login     # 用 config.json 里保存的 session_cookie（零登录）
    python diag_entry.py --no-previsit  # 登录前不预访问平台首页（对照固件差异）
"""
import argparse
import json
import os
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_cas          # noqa: E402
import wsc_checkin      # noqa: E402
from wsc_http import DEFAULT_UA, HttpClient  # noqa: E402

CONFIG = os.path.join(HERE, 'config.json')
JZSZ = 'https://jzsz.uestc.edu.cn'

# 固件当前发送的极简头（enterApp）
MIN_HEADERS = {
    'Accept': ('text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,image/avif,'
               'image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7'),
    'Upgrade-Insecure-Requests': '1',
    'Pragma': 'no-cache',
    'Cache-Control': 'no-cache',
    'Sec-Fetch-Site': 'same-origin',
    'Sec-Fetch-Mode': 'navigate',
    'Sec-Fetch-User': '?1',
    'Sec-Fetch-Dest': 'document',
    'Priority': 'u=0, i',
}

# 抓包中浏览器实际发送的头（#99 / #100）
BROWSER_HEADERS = dict(MIN_HEADERS)
BROWSER_HEADERS.update({
    'Accept-Language': 'zh-CN,zh;q=0.9',
    'Sec-Ch-Ua': '"Google Chrome";v="155", "Chromium";v="155", "Not(A:Brand";v="24"',
    'Sec-Ch-Ua-Mobile': '?0',
    'Sec-Ch-Ua-Platform': '"Windows"',
})


def jar_names(client) -> str:
    """只打印 Cookie 名称（绝不打印值）。"""
    names = sorted({name for (_domain, name) in client.jar._c.keys()})
    return ','.join(names) or '(empty)'


def trace(client, url: str, headers: dict, label: str, max_hops: int = 6) -> str:
    """逐跳请求并打印，返回最终 URL。"""
    print(f'  [{label}] GET {url}')
    for hop in range(max_hops + 1):
        resp = client.request('GET', url, headers=headers)
        location = resp.header('Location')
        set_cookie = resp.header('Set-Cookie')
        cookie_names = ''
        if set_cookie:
            cookie_names = ','.join(sorted({c.split('=')[0].strip() for c in set_cookie.split(';')
                                            if '=' in c}))
        print(f'    hop{hop}: HTTP {resp.status}  len={len(resp.body)}'
              f'{"  set-cookie: " + cookie_names if cookie_names else ""}')
        if location:
            print(f'           Location: {location[:110]}')
        if resp.status not in (301, 302, 303, 307, 308) or not location:
            return url
        url = urllib.parse.urljoin(url, location)
    return url


def today_code(client) -> str:
    resp = wsc_checkin.today(client)
    try:
        payload = json.loads(resp.body.decode('utf-8', 'replace'))
    except json.JSONDecodeError:
        payload = {}
    data = payload.get('data') or {}
    status = data.get('recordStatusCode') if isinstance(data, dict) else None
    return f'HTTP {resp.status} code={payload.get("code")} msg={payload.get("msg")} status={status}'


def main() -> int:
    parser = argparse.ArgumentParser(description='EP05 入口逐跳诊断')
    parser.add_argument('--no-login', action='store_true', help='复用 config.json 的 session_cookie')
    parser.add_argument('--no-previsit', action='store_true', help='登录前不预访问平台首页')
    parser.add_argument('--header-set', default='both', choices=['min', 'browser', 'both'])
    args = parser.parse_args()

    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)

    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    fingerprint = cfg.get('browser_fingerprint') or ''
    if fingerprint and not fingerprint.startswith('PUT_'):
        wsc_cas.set_fingerprint_cookie(client, fingerprint)

    if args.no_login:
        session = cfg.get('session_cookie') or ''
        if not session or session.startswith('PUT_'):
            print('config.json 里没有 session_cookie，去掉 --no-login 重新登录')
            return 2
        client.jar.set('SESSION', session, 'jzsz.uestc.edu.cn', '/')
        print('使用 config.json 中保存的 SESSION（零登录）')
    else:
        if not args.no_previsit:
            warm = client.request('GET', JZSZ + '/')
            print(f'登录前预访问 GET / -> HTTP {warm.status}   jar: {jar_names(client)}')
        verdict, detail = wsc_cas.login(client, cfg['student_id'], cfg['password'],
                                       bfp_value=fingerprint)
        print(f'登录判定 = {verdict}')
        if verdict != 'OK':
            print(f'  详情: {detail[:160]}')
            return 1
        print(f'  登录后 jar: {jar_names(client)}')

    print('-' * 78)
    results = {}
    if args.header_set in ('min', 'both'):
        final = trace(client, wsc_checkin.CHECK_LOGIN, MIN_HEADERS, 'A 极简头+checkLogin')
        results['A 极简头'] = '/daily-checkin-h5/' in final
        print(f'     A 落到 H5: {results["A 极简头"]}    today: {today_code(client)}')
    if args.header_set in ('browser', 'both'):
        final = trace(client, wsc_checkin.CHECK_LOGIN, BROWSER_HEADERS, 'B 浏览器头+checkLogin')
        results['B 浏览器头'] = '/daily-checkin-h5/' in final
        print(f'     B 落到 H5: {results["B 浏览器头"]}    today: {today_code(client)}')

    print('-' * 78)
    final = trace(client, wsc_checkin.H5_URL, BROWSER_HEADERS, 'C 直接 GET H5 页')
    print(f'     C 直接进 H5: {"/daily-checkin-h5/" in final and "index.html" in final}')

    print('-' * 78)
    print(f'  jar（jzsz 域）: {jar_names(client)}')
    print('  结论：')
    for key, ok in results.items():
        print(f'    {key}: {"可进入界面 ✅" if ok else "被拒 ❌"}')
    if results and not any(results.values()):
        print('    → 新会话此刻进不了微应用（checkLogin 被退回平台首页、today 回 4001）。')
        print('      2026-10-13 结论更新：这不是"窗口内外"的问题，而是**新会话需要预热/激活**：')
        print('        · 浏览器实测：新登录后只能进平台首页；登录信息未过期时可直接进打卡页；')
        print('        · 另一台机器用**已授权会话**重放（极简头）→ checkLogin 302 到 H5、today code=0；')
        print('        · 补 bfp/info、换浏览器全套头均无效；客户端 IP 也无关（校园网内设备同样被拒）。')
        print('      → 用 diag_activation.py 量化"新会话多久才被授权"。')
    elif results.get('A 极简头') and results.get('B 浏览器头'):
        print('    → 请求头不是关键；设备侧失败应查会话本身（是否为登录态会话、是否已激活）。')
    elif results.get('B 浏览器头') and not results.get('A 极简头'):
        print('    → 缺的是浏览器头（把差异补进 CheckinApi::addH5Headers / enterApp）。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
