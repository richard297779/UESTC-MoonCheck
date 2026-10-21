#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对照实验：checkLogin 带不带 Referer（来源页）是否决定微应用放行。

依据（2026-10-13）：
  - 用户实测：**从平台首页点"晚安成电"图标能进**；直接输网址只有**登录信息未过期**时能进。
  - 我们此前所有程序化实验（极简头 / 浏览器全套头 / 补 bfp/info / 等 14 分钟）都**没有带 Referer**，全部被拒。
  - 抓包里那条成功的 checkLogin **没有** Referer —— 但那次用户是"直接输网址"且会话已授权。

本脚本：全新登录一次，然后**同一个会话**依次换请求头试 checkLogin，
判断"首次进入是否需要门户来源页"；若某一变体成功，再回测"不带 Referer"，
确认该次成功是否把会话**标记为已授权**（之后裸请求也能进）。

用法: python diag_referer.py
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
H5 = JZSZ + '/daily-checkin-h5/index.html?appId=294'

DOC_ACCEPT = ('text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,image/avif,'
              'image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7')

BASE = {
    'Accept': DOC_ACCEPT,
    'Upgrade-Insecure-Requests': '1',
    'Sec-Fetch-Site': 'same-origin',
    'Sec-Fetch-Mode': 'navigate',
    'Sec-Fetch-User': '?1',
    'Sec-Fetch-Dest': 'document',
    'Sec-Ch-Ua': '"Google Chrome";v="155", "Chromium";v="155", "Not(A:Brand";v="24"',
    'Sec-Ch-Ua-Mobile': '?0',
    'Sec-Ch-Ua-Platform': '"Windows"',
    'Accept-Language': 'zh-CN,zh;q=0.9',
}

VARIANTS = [
    ('V1 无 Referer（此前所有实验的形态）', dict(BASE)),
    ('V2 Referer = 平台首页', dict(BASE, Referer=JZSZ + '/')),
    ('V3 Referer = H5 页', dict(BASE, Referer=H5)),
    ('V4 Referer 首页 + 无 Sec-Fetch-*', {k: v for k, v in dict(BASE, Referer=JZSZ + '/').items()
                                          if not k.startswith('Sec-Fetch')}),
]


def try_entry(client, label, headers):
    resp = client.request('GET', wsc_checkin.CHECK_LOGIN, headers=headers)
    location = resp.header('Location') or ''
    reached = '/daily-checkin-h5/' in location
    print('  [%s] HTTP %s  Location=%s  进入H5=%s' % (label, resp.status, location[:66] or '(none)', reached))
    return reached


def today_code(client):
    resp = wsc_checkin.today(client)
    try:
        payload = json.loads(resp.body.decode('utf-8', 'replace'))
    except json.JSONDecodeError:
        payload = {}
    return payload.get('code'), payload.get('msg')


def main() -> int:
    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    fingerprint = cfg.get('browser_fingerprint') or ''

    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    if fingerprint and not fingerprint.startswith('PUT_'):
        wsc_cas.set_fingerprint_cookie(client, fingerprint)

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
    print('票据换会话: SESSION %s => %s  新下发=%s'
          % ((before[:8] + '…') if before else '(none)', (after[:8] + '…') if after else '(none)',
             before != after))
    print('=' * 72)

    success_label = None
    for label, headers in VARIANTS:
        if try_entry(client, label, headers):
            success_label = label
            code, msg = today_code(client)
            print('      today -> code=%s msg=%s' % (code, msg))
            break
        # 每次失败后顺手看一眼业务接口（4001 表示会话仍未被接受）
        code, msg = today_code(client)
        print('      today -> code=%s msg=%s' % (code, msg))

    print('=' * 72)
    if success_label is None:
        print('结论：带 Referer 也进不去 → 门户来源页不是原因，继续找差异')
        return 2

    print('结论：%s 成功进入 H5 → **首次进入需要门户来源页**' % success_label)
    print('      回测：不带 Referer 再来一次...')
    if try_entry(client, 'V5 回测（无 Referer，会话已被放行后）', dict(BASE)):
        print('      → 一旦被放行，会话即长期有效（裸请求也能进）')
    else:
        print('      → 每次都需带 Referer（说明是每请求校验，而非一次性放行）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
