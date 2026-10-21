#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 HAR 的 JS 响应体里挖"点击应用图标"的代码路径（窗口化上下文，脱敏）。

用法: python tools/inspect_app_js.py jzsz.uestc.edu.cn.har
"""
import json
import re
import sys

KEYWORDS = ['checkLogin', 'window.open', 'open(', 'linkUrl', 'base64Url', 'appSecret',
            'getAppList', 'handleClick', 'toApp', 'goApp', 'openApp', 'appId']


def brief(text, start, end):
    frag = text[start:end].replace('\n', ' ').replace('\r', ' ')
    frag = re.sub(r'\s+', ' ', frag)
    return re.sub(r'[0-9a-fA-F]{16,}', '<hex>', frag)[:520]


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else 'jzsz.uestc.edu.cn.har'
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        har = json.load(handle)
    entries = har['log']['entries']

    print('=== ① 应用注册项（晚安成电）完整字段（值脱敏）===')
    for index, entry in enumerate(entries):
        if 'getAppList' not in entry['request']['url']:
            continue
        text = ((entry.get('response') or {}).get('content') or {}).get('text') or ''
        pos = text.find('晚安成电')
        if pos < 0:
            continue
        start = text.rfind('{', 0, pos)
        end = text.find('}', pos)
        frag = text[start:end + 1]
        frag = re.sub(r'("appSecret"\s*:\s*")[^"]+(")', r'\1<REDACTED>\2', frag)
        frag = re.sub(r'("id"\s*:\s*")[^"]{8,}(")', r'\1<id>\2', frag)
        print('[%03d] %s' % (index, frag[:600]))
        break

    print()
    print('=== ② JS 中与"打开应用 / checkLogin"相关的代码片段 ===')
    shown = 0
    for index, entry in enumerate(entries):
        url = entry['request']['url']
        if not url.endswith('.js'):
            continue
        text = ((entry.get('response') or {}).get('content') or {}).get('text') or ''
        if not text:
            continue
        for keyword in ('checkLogin', 'window.open', 'base64Url'):
            for match in re.finditer(re.escape(keyword), text):
                if shown > 26:
                    break
                print('[%03d %s] 「%s」' % (index, url.split('/')[-1][:28], keyword))
                print('    %s' % brief(text, max(0, match.start() - 260), match.end() + 260))
                shown += 1
            if shown > 26:
                break
        if shown > 26:
            break
    if shown == 0:
        print('    （HAR 里没有 JS 响应体，或其中不含这些关键字）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
