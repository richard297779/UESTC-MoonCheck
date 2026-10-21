#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对照实验：补上浏览器独有的 bfp/info 预登记步骤后，新会话能否被微应用授权？

背景（2026-10-13 实测）：
  - 浏览器会话：`checkLogin` 302 → H5，`today` code=0（可用）。
  - 我们程序化登录的新会话：窗口内也一样被拒（302 → 平台首页，today=4001）。
  - 抓包显示浏览器在"取登录页"与"提交登录"之间多做了一步：
        GET /authserver/bfp/info?bfp=<32位指纹>&_=<时间戳>   (X-Requested-With: XMLHttpRequest)
    而固件/Python 都是"只塞指纹 Cookie、不预登记"。

本脚本只改这一处：登录前先按浏览器的方式调一次 bfp/info，然后照原样登录，
最后用 checkLogin + today 判定新会话是否被授权。

用法: python diag_bfp.py            # 补 bfp/info
      python diag_bfp.py --skip-bfp # 不补（对照组，等价于固件现状）
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
IDAS = 'https://idas.uestc.edu.cn'

BFP_HEADERS = {
    'Accept': 'application/json, text/javascript, */*; q=0.01',
    'X-Requested-With': 'XMLHttpRequest',
    'Pragma': 'no-cache',
    'Cache-Control': 'no-cache',
    'Sec-Fetch-Site': 'same-origin',
    'Sec-Fetch-Mode': 'cors',
    'Sec-Fetch-Dest': 'empty',
    'Referer': IDAS + '/authserver/login?service=https%3A%2F%2Fjzsz.uestc.edu.cn%2F',
}


def short(value: str) -> str:
    return (value[:8] + '…') if value else '(none)'


def main() -> int:
    parser = argparse.ArgumentParser(description='bfp/info 预登记对照实验')
    parser.add_argument('--skip-bfp', action='store_true', help='不调用 bfp/info（对照组）')
    args = parser.parse_args()

    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    fingerprint = cfg.get('browser_fingerprint') or ''
    if not fingerprint or fingerprint.startswith('PUT_'):
        print('config.json 缺少 browser_fingerprint')
        return 2

    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    wsc_cas.set_fingerprint_cookie(client, fingerprint)
    print(f'指纹 = {short(fingerprint)}   bfp/info 预登记 = {not args.skip_bfp}')

    # 0) 预访问平台首页（与固件一致）
    client.request('GET', JZSZ + '/')
    # 1) 取登录页
    page = wsc_cas.fetch_login_page(client)
    print(f'登录页: execution={page.execution} salt={page.salt}')

    # 2) 关键变量：bfp/info 预登记
    if not args.skip_bfp:
        url = '%s/authserver/bfp/info?bfp=%s&_=%d' % (IDAS, fingerprint, int(time.time() * 1000))
        resp = client.request('GET', url, headers=BFP_HEADERS)
        body = resp.body.decode('utf-8', 'replace')
        print(f'bfp/info -> HTTP {resp.status}  {len(body)} B  set-cookie={bool(resp.header("Set-Cookie"))}')
        try:
            payload = json.loads(body)
            print('  返回字段: %s' % list(payload.keys()))
        except Exception:
            print('  返回（前 120 字符）: %s' % body[:120].replace('\n', ' '))

    # 3) 提交登录 + 换票据
    before = client.jar.get('SESSION')
    resp = wsc_cas.submit_login(client, cfg['student_id'], cfg['password'], page)
    verdict, detail = wsc_cas.classify_login(resp)
    print(f'登录 POST -> HTTP {resp.status} 判定={verdict}')
    if verdict != 'OK':
        print('  详情: %s' % str(detail)[:160])
        return 1
    client.follow(detail)
    after = client.jar.get('SESSION')
    print(f'票据换会话 -> SESSION {short(before)} => {short(after)}  变化={before != after}')

    # 4) 判定：能否进 H5 / 业务接口是否可用
    entry = client.follow(wsc_checkin.CHECK_LOGIN)
    reached = '/daily-checkin-h5/' in entry.url
    print(f'checkLogin -> 最终 {entry.url[:80]}   进入 H5: {reached}')
    resp = wsc_checkin.today(client)
    try:
        payload = json.loads(resp.body.decode('utf-8', 'replace'))
    except json.JSONDecodeError:
        payload = {}
    print(f'today -> HTTP {resp.status} code={payload.get("code")} msg={payload.get("msg")}')

    print('=' * 70)
    if reached and payload.get('code') == 0:
        print('结论：新会话**可用** → bfp/info 预登记就是缺的那一步 ✅')
        print('      → 固件应在提交登录前补一次 bfp/info（并保存服务端下发的 Cookie）')
    else:
        print('结论：补了 bfp/info 仍被拒 → 该步骤不是原因，继续查下一处差异')
    return 0


if __name__ == '__main__':
    sys.exit(main())
