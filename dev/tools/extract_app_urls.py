#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 HAR 里挖"晚安成电"的真实启动地址（不打印任何 Cookie/PII 值）。

用法: python tools/extract_app_urls.py jzsz.uestc.edu.cn.har
"""
import json
import re
import sys

NEEDLE = re.compile(r'daily-checkin|checkLogin|appId=294')


def body_of(entry):
    content = (entry.get('response') or {}).get('content') or {}
    text = content.get('text')
    return text if isinstance(text, str) else ''


def brief(text, start, end, limit=220):
    frag = text[start:end].replace('\n', ' ')
    frag = re.sub(r'[0-9a-fA-F]{16,}', '<hex>', frag)
    return frag[:limit]


def walk(node, path, hits, depth=0):
    """在 JSON 里找"名字像应用名/地址像链接"的键值对（只收集字符串，不收集个人信息字段）。"""
    if depth > 8:
        return
    if isinstance(node, dict):
        name_val = None
        link_vals = {}
        for key, value in node.items():
            lk = key.lower()
            if isinstance(value, str):
                if lk in ('name', 'appname', 'title') and ('晚安' in value or '打卡' in value):
                    name_val = value
                if any(t in lk for t in ('url', 'link', 'path', 'address', 'href', 'addres')):
                    link_vals[key] = value[:180]
                if NEEDLE.search(value):
                    hits.append((path + '.' + key, value[:200]))
        if name_val or link_vals:
            hits.append((path, {'name': name_val, 'links': link_vals}))
        for key, value in node.items():
            if isinstance(value, (dict, list)):
                walk(value, path + '.' + key, hits, depth + 1)
    elif isinstance(node, list):
        for index, item in enumerate(node[:60]):
            walk(item, '%s[%d]' % (path, index), hits, depth + 1)


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else 'jzsz.uestc.edu.cn.har'
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        har = json.load(handle)
    entries = har['log']['entries']
    print('HAR 请求数: %d' % len(entries))

    with_body = sum(1 for e in entries if body_of(e))
    print('含响应体的条目: %d' % with_body)
    print('=' * 96)

    print('--- ① 任何响应文本里出现 daily-checkin / checkLogin / appId=294 的位置 ---')
    found = 0
    for index, entry in enumerate(entries):
        text = body_of(entry)
        if not text:
            continue
        for match in NEEDLE.finditer(text):
            found += 1
            if found > 25:
                break
            print('[%03d] %s' % (index, entry['request']['url'][:95]))
            print('      …%s…' % brief(text, max(0, match.start() - 90), match.end() + 130))
            break
        if found > 25:
            break
    if not found:
        print('      （无）—— 说明 HAR 里完全没有微应用相关流量')
    print('=' * 96)

    print('--- ② 首页应用列表（getAppList / getAppById）的结构与应用地址字段 ---')
    for index, entry in enumerate(entries):
        url = entry['request']['url']
        if 'getAppList' not in url and 'getAppById' not in url:
            continue
        text = body_of(entry)
        print('[%03d] %s  (%d B)' % (index, url[:88], len(text)))
        if not text:
            print('      响应体未导出（HAR 需要"Save with content"才有）')
            continue
        try:
            data = json.loads(text)
        except Exception:
            print('      非 JSON：%s' % brief(text, 0, 120))
            continue
        hits = []
        walk(data, '$', hits)
        shown = 0
        for where, value in hits:
            if shown > 18:
                print('      …（截断）')
                break
            print('      %s = %s' % (where[:52], value))
            shown += 1
        if not hits:
            print('      未匹配到应用名/地址字段；顶层键: %s' % list(data)[:8])
    print('=' * 96)

    print('--- ③ 请求头对照：登录 POST 与首页 XHR 是否带了特殊头 ---')
    for index, entry in enumerate(entries):
        url = entry['request']['url']
        if 'authserver/login' not in url and 'getAppList' not in url:
            continue
        names = [h.get('name', '') for h in entry['request'].get('headers', [])]
        print('[%03d] %s' % (index, url[:88]))
        print('      头名: %s' % ', '.join(n for n in names if n.lower() != 'cookie'))
        cookie_header = [h.get('value', '') for h in entry['request'].get('headers', [])
                         if h.get('name', '').lower() == 'cookie']
        if cookie_header:
            print('      Cookie 名: %s' % sorted(
                {p.split('=')[0].strip() for p in cookie_header[0].split(';') if '=' in p}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
