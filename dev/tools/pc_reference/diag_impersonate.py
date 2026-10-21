#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""传输层判定：伪装 Chrome 的 TLS/HTTP 指纹登录，能否拿到被授权的会话？

这是"分层排除"的最后一层。前面已证明：请求内容（URL/字段/头/Referer/门户 XHR）
与浏览器完全一致时仍被拒 ⇒ 嫌疑落在传输层。本脚本用 curl_cffi（curl-impersonate）
以 **Chrome 的 TLS ClientHello + HTTP/2 帧特征** 走完整登录链，并通过开关做二分：

    --impersonate chrome   默认：Chrome 全套指纹（TLS + h2 + 头顺序）
    --http11               保留 Chrome TLS 指纹，但强制 HTTP/1.1
    --plain                不伪装（等价于普通 requests）——对照组

判定表：
    伪装+H2 ✅ 且 伪装+H1.1 ✅   → 门槛是 **TLS 握手指纹**（设备侧需评估能否定制 ClientHello）
    伪装+H2 ✅ 但 --http11 ❌    → 门槛是 **HTTP/2 帧特征**（ESP32 无 h2 栈，基本不可行）
    全部 ❌（含 --plain）        → 门槛不是传输层，而是**登录前的浏览器状态/JS 行为**

依赖：python -m pip install curl_cffi
用法：python diag_impersonate.py [--http11] [--plain]
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_cas          # noqa: E402  复用已验证的 AES 加密实现
from wsc_http import DEFAULT_UA  # noqa: E402

CONFIG = os.path.join(HERE, 'config.json')
CAS = 'https://idas.uestc.edu.cn/authserver'
JZSZ = 'https://jzsz.uestc.edu.cn'
SERVICE = 'https%3A%2F%2Fjzsz.uestc.edu.cn%2F'
LOGIN_URL = '%s/login?service=%s' % (CAS, SERVICE)
CHECK_LOGIN = JZSZ + '/daily-checkin/checkLogin?appId=294'
EP_TODAY = JZSZ + '/daily-checkin/api/student-checkin-record/today'
H5_HEADERS = {
    'Platform': 'h5',
    'Accept': '*/*',
    'Origin': JZSZ,
    'Referer': JZSZ + '/daily-checkin-h5/index.html?appId=294',
    'Sec-Fetch-Site': 'same-origin',
    'Sec-Fetch-Mode': 'cors',
    'Sec-Fetch-Dest': 'empty',
}
DOC_ACCEPT = ('text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,image/avif,'
              'image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7')


def main() -> int:
    parser = argparse.ArgumentParser(description='Chrome 指纹登录判定')
    parser.add_argument('--http11', action='store_true', help='强制 HTTP/1.1（只保留 TLS 指纹伪装）')
    parser.add_argument('--plain', action='store_true', help='不伪装（对照组）')
    args = parser.parse_args()

    try:
        from curl_cffi import requests as creq
    except ImportError:
        print('缺少 curl_cffi：python -m pip install curl_cffi')
        return 3

    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    fingerprint = cfg.get('browser_fingerprint') or ''
    ua = cfg.get('user_agent') or DEFAULT_UA

    kwargs = {} if args.plain else {'impersonate': 'chrome'}
    if args.http11:
        kwargs['http_version'] = 3  # CURL_HTTP_VERSION_1_1
    session = creq.Session(headers={'User-Agent': ua}, timeout=25, **kwargs)
    if fingerprint and not fingerprint.startswith('PUT_'):
        session.cookies.set('MULTIFACTOR_BROWSER_FINGERPRINT', fingerprint,
                            domain='idas.uestc.edu.cn', path='/')
    print('模式: %s   http11=%s' % ('不伪装' if args.plain else 'Chrome 指纹', args.http11))

    print('[1] 匿名访问平台首页 ...')
    resp = session.get(JZSZ + '/', allow_redirects=False)
    print('    GET / -> %s' % resp.status_code)

    print('[2] 取登录页 ...')
    page = session.get(LOGIN_URL)
    execution = re.search(r'name="execution" value="([^"]+)"', page.text)
    salt = re.search(r'id="pwdEncryptSalt" value="([^"]+)"', page.text)
    if not execution or not salt:
        print('    解析失败（页面结构变化）')
        return 1
    print('    execution=%s' % execution.group(1))

    print('[3] 提交登录（AES 加密与固件/浏览器一致）...')
    encrypted = wsc_cas.encrypt_password(cfg['password'], salt.group(1))
    resp = session.post(LOGIN_URL, data={
        'username': str(cfg['student_id']), 'password': encrypted, 'captcha': '',
        '_eventId': 'submit', 'cllt': 'userNameLogin', 'dllt': 'generalLogin', 'lt': '',
        'execution': execution.group(1),
    }, headers={'Origin': 'https://idas.uestc.edu.cn', 'Referer': LOGIN_URL,
                'Content-Type': 'application/x-www-form-urlencoded',
                'Upgrade-Insecure-Requests': '1', 'Accept': DOC_ACCEPT,
                'Sec-Fetch-Site': 'same-origin', 'Sec-Fetch-Mode': 'navigate',
                'Sec-Fetch-User': '?1', 'Sec-Fetch-Dest': 'document'},
        allow_redirects=False)
    location = resp.headers.get('Location', '')
    if 'ticket=' not in location:
        print('    未取得票据：HTTP %s Location=%s' % (resp.status_code, location[:90]))
        return 1
    print('    票据已取得 -> 换会话')
    home = session.get(location, allow_redirects=True)
    print('    GET ?ticket=... -> %s（最终 %s）' % (home.status_code, home.url[:60]))

    print('[4] 关键判定：checkLogin 去哪？')
    resp = session.get(CHECK_LOGIN, headers={'Accept': DOC_ACCEPT,
                                            'Upgrade-Insecure-Requests': '1'},
                       allow_redirects=False)
    redirect = resp.headers.get('Location', '')
    reached = '/daily-checkin-h5/' in redirect
    print('    HTTP %s  Location=%s  进入 H5=%s' % (resp.status_code, redirect[:70], reached))

    today = session.get(EP_TODAY, headers=H5_HEADERS)
    try:
        code = today.json().get('code')
    except Exception:
        code = '?'
    print('    today code=%s' % code)

    print('=' * 70)
    if reached and code == 0:
        print('结论：**伪装 Chrome 传输层指纹即可拿到授权会话** ✅')
        print('      → 门槛在传输层；接下来用 --http11 二分"TLS 指纹 vs HTTP/2 帧特征"，')
        print('        再决定 ESP32 侧能否复刻（mbedTLS 可定制密码套件顺序/ALPN，但扩展顺序受限）。')
        return 0
    print('结论：Chrome 指纹也拿不到授权 → 门槛不是传输层，')
    print('      而是"登录前的浏览器状态"（JS 生成的指纹登记、CAS SSO 历史 Cookie、行为特征等）。')
    return 2


if __name__ == '__main__':
    sys.exit(main())
