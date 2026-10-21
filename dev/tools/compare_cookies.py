#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对比：浏览器在 jzsz / idas 两个域上到底带了哪些 Cookie（只列名字，绝不打印值）。

用法: python tools/compare_cookies.py jzsz.uestc.edu.cn.har
"""
import json
import re
import sys
from collections import OrderedDict


def cookie_names(value):
    return [p.split('=')[0].strip() for p in value.split(';') if '=' in p]


def headers(entry, wanted, section):
    out = OrderedDict()
    for header in entry.get('request' if section == 'req' else 'response', {}).get('headers', []):
        name = header.get('name', '')
        if name.lower() in wanted:
            out.setdefault(name.lower(), []).append(header.get('value', ''))
    return out


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else 'jzsz.uestc.edu.cn.har'
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        har = json.load(handle)
    entries = har['log']['entries']

    print('=== 浏览器发给各主机的 Cookie 名称（按主机去重）===')
    seen = {}
    for index, entry in enumerate(entries):
        url = entry['request']['url']
        host = re.sub(r'^https?://', '', url).split('/')[0]
        cookie = headers(entry, {'cookie'}, 'req').get('cookie')
        if not cookie:
            continue
        names = tuple(sorted(set(cookie_names(cookie[0]))))
        if seen.get(host) != names:
            seen[host] = names
            print('[%03d] %s\n      %s' % (index, host, ', '.join(names)))

    print()
    print('=== 服务端下发的 Set-Cookie（名称 + 作用域，值已隐藏）===')
    for index, entry in enumerate(entries):
        sets = headers(entry, {'set-cookie'}, 'resp').get('set-cookie') or []
        if not sets:
            continue
        host = re.sub(r'^https?://', '', entry['request']['url']).split('/')[0]
        print('[%03d] %s -> %s' % (index, host, entry['request']['url'].split('?')[0][:70]))
        for item in sets:
            name = item.split('=', 1)[0].strip()
            attrs = [a.strip().split('=')[0] for a in item.split(';')[1:]]
            domain = re.search(r'[Dd]omain=([^;]+)', item)
            path_attr = re.search(r'[Pp]ath=([^;]+)', item)
            max_age = re.search(r'[Mm]ax-Age=(-?\d+)', item)
            print('      %-34s domain=%-22s path=%-12s maxAge=%s attrs=%s'
                  % (name, (domain.group(1) if domain else '(host-only)'),
                     (path_attr.group(1) if path_attr else '/'),
                     (max_age.group(1) if max_age else '-'), ','.join(attrs)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
