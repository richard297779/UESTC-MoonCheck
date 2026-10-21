#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脱敏分析 HAR：按时间序列出请求，只暴露结构，绝不打印 Cookie / token 值。

用法: python tools/analyze_har.py jzsz.uestc.edu.cn.har
"""
import json
import re
import sys

HEXISH = re.compile(r'[0-9a-fA-F]{16,}')
SECRET_HEADERS = {'cookie', 'set-cookie', 'authorization'}
INTEREST = ('daily-checkin', 'checkLogin', 'authserver', 'login', 'ticket', 'session', 'token',
            'user-info', 'userInfo', 'sso', 'app')


def redact(name, value):
    low = name.lower()
    if low == 'cookie':
        names = re.findall(r'(?:^|;\s*)([A-Za-z0-9_\-\.]+)=', value)
        return '<%s>' % ','.join(dict.fromkeys(names))
    if low == 'set-cookie':
        key = value.split(';', 1)[0]
        cname = key.split('=', 1)[0]
        return '<%s=***>' % cname
    if low in SECRET_HEADERS or 'token' in low or 'auth' in low:
        return '<redacted>'
    return HEXISH.sub('<hex>', value)[:140]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'jzsz.uestc.edu.cn.har'
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        har = json.load(handle)
    entries = har.get('log', {}).get('entries', [])
    print('总请求数: %d' % len(entries))
    print('=' * 100)
    for idx, entry in enumerate(entries):
        request = entry.get('request', {})
        response = entry.get('response', {})
        method = request.get('method', '?')
        url = request.get('url', '')
        status = response.get('status', 0)
        headers = {h.get('name', ''): h.get('value', '') for h in request.get('headers', [])}
        rheaders = {h.get('name', ''): h.get('value', '') for h in response.get('headers', [])}
        resource_type = entry.get('_resourceType') or ''
        interesting = any(token.lower() in url.lower() for token in INTEREST) or \
            resource_type in ('document', 'xhr', 'fetch')
        if not interesting:
            continue
        print('[%03d] %s %s -> %s  (%s)' % (idx, method, url[:130], status, resource_type))
        if headers.get('Referer'):
            print('      Referer: %s' % redact('referer', headers['Referer']))
        if headers.get('Cookie'):
            print('      Cookie:  %s' % redact('cookie', headers['Cookie']))
        for name, value in headers.items():
            if name.lower() in ('referer', 'cookie', 'user-agent', 'host'):
                continue
            if name.lower().startswith(('sec-', 'accept', 'content-type', 'platform', 'pragma',
                                        'cache-control', 'priority', 'upgrade', 'x-')):
                print('      %s: %s' % (name, redact(name, value)))
        location = rheaders.get('Location')
        if location:
            print('      ⤷ Location: %s' % redact('location', location))
        for name, value in rheaders.items():
            if name.lower() == 'set-cookie':
                print('      ⤷ Set-Cookie: %s' % redact('set-cookie', value))
        content = (response.get('content') or {})
        text = content.get('text') or ''
        if text and status >= 400:
            print('      响应片段: %s' % HEXISH.sub('<hex>', text)[:160].replace('\n', ' '))
        print('-' * 100)
    return 0


if __name__ == '__main__':
    sys.exit(main())
