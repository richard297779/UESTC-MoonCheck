#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性侦察脚本：用随机指纹触发二次验证，测绘 MFA 页面结构。

目的：为固件 F2（蓝牙回传验证码）提供实现依据——验证码从哪发、往哪提交、信任参数叫什么。
**不提交任何验证码**，不改变账号状态。调查结果写入 capture/raw/mfa_page.html。

用法: python recon_mfa.py
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_cas                      # noqa: E402
from wsc_http import HttpClient    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
KEYWORDS = ('sendCode', 'dynamicCode', 'getCode', 'verifyCode', 'trustBrowser',
            'isTrust', 'reAuth', 'multifactor', 'reAuthCheck', 'code')


def structure(html, label):
    print('  --- %s ---' % label)
    title = re.search(r'<title>(.*?)</title>', html, re.S | re.I)
    print('  title      : %s' % (title.group(1).strip() if title else '(none)'))
    forms = re.findall(r'<form[^>]*>', html, re.I)
    for f in forms[:6]:
        print('  form       : %s' % re.sub(r'\s+', ' ', f)[:180])
    inputs = re.findall(r'<input[^>]*>', html, re.I)
    names = []
    for i in inputs:
        m = re.search(r'name="([^"]+)"', i)
        t = re.search(r'type="([^"]+)"', i)
        if m:
            names.append('%s(%s)' % (m.group(1), t.group(1) if t else '?'))
    print('  inputs     : %s' % ', '.join(names[:20]) if names else '  inputs     : (none)')
    scripts = re.findall(r'<script[^>]*src="([^"]+)"', html, re.I)
    print('  scripts    : %s' % ', '.join(scripts[:10]))
    for kw in KEYWORDS:
        m = re.search(kw, html, re.I)
        if m:
            s = max(0, m.start() - 70)
            snippet = re.sub(r'\s+', ' ', html[s:m.start() + 90])
            print('  hit[%-13s]: …%s…' % (kw, snippet))
    return scripts


def main():
    cfg = json.load(open(os.path.join(HERE, 'config.json'), encoding='utf-8'))
    c = HttpClient(user_agent=cfg.get('user_agent'))

    print('[1] 取登录页')
    page = wsc_cas.fetch_login_page(c)
    print('    execution=%s salt=%s' % (page.execution, page.salt))

    print('[2] 上报随机指纹（故意制造"未信任设备"）')
    fp = wsc_cas.random_fingerprint()
    wsc_cas.report_fingerprint(c, fp)

    print('[3] 提交登录')
    r = wsc_cas.submit_login(c, cfg.get('student_id'), cfg.get('password'), page)
    verdict, detail = wsc_cas.classify_login(r)
    print('    判定=%s' % verdict)
    if verdict != 'MFA':
        print('    未触发二次验证，侦察结束（%s）' % wsc_cas.mask_ticket(str(detail))[:200])
        return 0

    print('[4] 跟随到 MFA 页面')
    resp = c.follow(detail)
    html = resp.text
    print('    最终 URL=%s  HTTP %s  [%d B]' % (wsc_cas.mask_ticket(resp.url), resp.status, len(html)))
    scripts = structure(html, 'MFA 主页面')

    out = os.path.join(ROOT, 'capture', 'raw', 'mfa_page.html')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        f.write(html)
    print('    页面已保存 → %s' % out)

    print('[5] 抓取页面 JS 中的验证码相关接口')
    base = 'https://idas.uestc.edu.cn'
    for src in scripts:
        low = src.lower()
        if not any(k in low for k in ('reauth', 'multifactor', 'login', 'mfa', 'auth')):
            continue
        url = src if src.startswith('http') else base + ('' if src.startswith('/') else '/authserver/') + src
        try:
            js = c.request('GET', url, headers={'Referer': resp.url})
            body = js.text
            print('    [%s] %d B' % (url.split('/')[-1][:60], len(body)))
            for kw in ('sendCode', 'dynamicCode', 'getDynamicCode', 'trustBrowser', 'reAuthCheck', 'verifyCode'):
                for m in re.finditer(kw, body, re.I):
                    s = max(0, m.start() - 90)
                    print('       hit[%s]: …%s…' % (kw, re.sub(r'\s+', ' ', body[s:m.start() + 110])))
                    break
        except Exception as e:
            print('    [warn] 取 %s 失败：%s' % (url, e))
    return 0


if __name__ == '__main__':
    sys.exit(main())
