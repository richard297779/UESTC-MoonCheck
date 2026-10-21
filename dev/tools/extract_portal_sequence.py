#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 HAR 提取"登录后 → 点图标前"浏览器在 jzsz 上的请求序列，生成可回放清单。

用法: python tools/extract_portal_sequence.py jzsz.uestc.edu.cn.har tools/pc_reference/portal_sequence.json
输出清单只含 method / path / query / 请求头（不含 Cookie）/ POST 体，供 PC 与固件对照回放。
"""
import json
import re
import sys
from urllib.parse import urlsplit

SKIP_EXT = ('.js', '.css', '.png', '.jpg', '.jpeg', '.gif', '.svg', '.ico', '.woff', '.woff2',
            '.apng', '.webp', '.ttf')


def main() -> int:
    har_path = sys.argv[1] if len(sys.argv) > 1 else 'jzsz.uestc.edu.cn.har'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'tools/pc_reference/portal_sequence.json'
    with open(har_path, 'r', encoding='utf-8', errors='replace') as handle:
        har = json.load(handle)

    items = []
    for entry in har['log']['entries']:
        request = entry['request']
        url = request['url']
        parts = urlsplit(url)
        if parts.netloc != 'jzsz.uestc.edu.cn':
            continue
        if parts.path.endswith(SKIP_EXT):
            continue
        if parts.path in ('/', '') or '/static/' in parts.path:
            continue
        headers = {}
        for header in request.get('headers', []):
            name = header.get('name', '')
            if name.lower() in ('cookie', 'host', 'content-length'):
                continue
            headers[name] = header.get('value', '')
        post = request.get('postData') or {}
        body = post.get('text') or ''
        items.append({
            'method': request.get('method'),
            'path': parts.path,
            'query': parts.query,
            'headers': headers,
            'body': body[:400],
            'status': entry.get('response', {}).get('status'),
        })

    # 去重（同 path 的重复请求只保留第一次，保持顺序）
    seen = set()
    unique = []
    for item in items:
        key = (item['method'], item['path'])
        if key in seen:
            continue
        seen.add(key)
        unique.append(item)

    with open(out_path, 'w', encoding='utf-8') as handle:
        json.dump(unique, handle, ensure_ascii=False, indent=2)

    print('提取请求数: %d（去重后 %d）→ %s' % (len(items), len(unique), out_path))
    print()
    for index, item in enumerate(unique):
        print('[%02d] %-5s %s%s' % (index, item['method'], item['path'],
                                     ('?' + item['query'][:44]) if item['query'] else ''))
        if item['body']:
            print('      body: %s' % re.sub(r'[0-9a-fA-F]{16,}', '<hex>', item['body'])[:120])
        interesting = {k: v for k, v in item['headers'].items()
                       if k.lower() in ('platform', 'content-type', 'referer', 'origin',
                                        'accept-encoding', 'sec-fetch-mode', 'sec-fetch-site',
                                        'x-requested-with', 'cache-control')}
        if interesting:
            print('      headers: %s' % json.dumps(interesting, ensure_ascii=False)[:200])
    return 0


if __name__ == '__main__':
    sys.exit(main())
