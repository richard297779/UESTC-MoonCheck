#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""分层验证：浏览器登录后做的那些门户请求，是否就是"微应用授权"的触发条件？

一次登录，按阶段逼近，每个阶段后立刻测一次 checkLogin + today：

  阶段0  基线：登录成功后直接测            （预期：被拒 —— 已知）
  阶段1  回放门户 XHR 序列（portal_sequence.json，浏览器顺序与头，不含 Accept-Encoding）
  阶段2  打开 H5 静态页 + 调用打卡应用自身接口（user-info / available-locations / schedule）
  阶段3  再回放一次门户序列，但**带上 Accept-Encoding 并解压**（检验"会不会解压缩"是否被当作机器人特征）
  阶段4  等待并复测（排除"其实只是需要时间"）

只要某个阶段后 checkLogin 变成 302→H5 且 today code=0，就说明**设备也能做到**（架构 A 复活）。

用法: python diag_portal_replay.py
"""
import gzip
import json
import os
import re
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import wsc_cas          # noqa: E402
import wsc_checkin      # noqa: E402
from wsc_http import DEFAULT_UA, HttpClient  # noqa: E402

CONFIG = os.path.join(HERE, 'config.json')
SEQUENCE = os.path.join(HERE, 'portal_sequence.json')
JZSZ = 'https://jzsz.uestc.edu.cn'
H5_MARK = '/daily-checkin-h5/'


def decode_body(resp):
    body = resp.body or b''
    encoding = (resp.header('Content-Encoding') or '').lower()
    try:
        if 'gzip' in encoding:
            body = gzip.decompress(body)
        elif 'deflate' in encoding:
            body = zlib.decompress(body, -zlib.MAX_WBITS)
    except Exception:
        pass
    return body


def portal_headers(item, with_encoding):
    headers = {}
    for name, value in (item.get('headers') or {}).items():
        if name.lower() == 'accept-encoding' and not with_encoding:
            continue
        if name.lower() in ('connection', 'host', 'content-length'):
            continue
        headers[name] = value
    if with_encoding:
        # 只声明我们能解的算法，避免服务端选 br/zstd 后我们读不懂
        headers['Accept-Encoding'] = 'gzip, deflate'
    return headers


def replay(client, items, with_encoding, label):
    print('  ── %s（%d 个请求，Accept-Encoding=%s）' % (label, len(items), with_encoding))
    for item in items:
        query = re.sub(r'_t=\d+', '_t=%d' % int(time.time() * 1000), item.get('query') or '')
        url = JZSZ + item['path'] + (('?' + query) if query else '')
        headers = portal_headers(item, with_encoding)
        body = item.get('body') or ''
        try:
            if item['method'] == 'POST':
                payload = json.loads(body) if body else {}
                resp = client.request('POST', url, headers=headers, json_body=payload)
            else:
                resp = client.request('GET', url, headers=headers)
            decode_body(resp)
            status = resp.status
        except Exception as exc:
            print('      %-5s %-58s 异常 %s' % (item['method'], item['path'][:58], exc))
            continue
        print('      %-5s %-58s %s' % (item['method'], item['path'][:58], status))


def measure(client, label):
    resp = client.request('GET', wsc_checkin.CHECK_LOGIN, headers={
        'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
        'Upgrade-Insecure-Requests': '1'})
    location = resp.header('Location') or ''
    reached = H5_MARK in location
    resp2 = client.request('GET', wsc_checkin.EP_TODAY, headers=wsc_checkin.H5_HEADERS)
    try:
        code = json.loads(decode_body(resp2).decode('utf-8', 'replace')).get('code')
    except Exception:
        code = '?'
    print('  【%s】checkLogin=%s（%s）  today code=%s'
          % (label, 'H5 ✅ 已授权' if reached else '首页 ❌', location[:46] or '-', code))
    return reached, code == 0


def main() -> int:
    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    with open(SEQUENCE, 'r', encoding='utf-8') as handle:
        items = json.load(handle)

    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)
    fingerprint = cfg.get('browser_fingerprint') or ''
    if fingerprint and not fingerprint.startswith('PUT_'):
        wsc_cas.set_fingerprint_cookie(client, fingerprint)

    print('[登录] 全新会话（消耗一次登录）...')
    client.request('GET', JZSZ + '/')
    page = wsc_cas.fetch_login_page(client)
    resp = wsc_cas.submit_login(client, cfg['student_id'], cfg['password'], page)
    verdict, detail = wsc_cas.classify_login(resp)
    print('  判定=%s' % verdict)
    if verdict != 'OK':
        print('  %s' % str(detail)[:150])
        return 1
    client.follow(detail)
    print('  SESSION=%s…' % ((client.jar.get('SESSION') or '')[:8]))

    print('\n[阶段0] 基线')
    if all(measure(client, '阶段0')):
        print('  意外：基线就通过 —— 说明这次登录本身已被授权')
        return 0

    print('\n[阶段1] 回放门户 XHR 序列')
    replay(client, items, False, '阶段1')
    if all(measure(client, '阶段1')):
        print('  ★ 结论：**门户请求序列**就是缺失的一步（纯 HTTP，设备可复刻）→ 架构 A 复活')
        return 0

    print('\n[阶段2] 打开 H5 静态页 + 打卡应用自身接口')
    client.request('GET', wsc_checkin.H5_URL, headers={
        'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8'})
    for path in ('/daily-checkin/api/auth/user-info', wsc_checkin.EP_LOCATIONS,
                 wsc_checkin.EP_SCHEDULE):
        try:
            resp = client.request('GET', JZSZ + path, headers=wsc_checkin.H5_HEADERS)
            print('      GET %-46s %s' % (path, resp.status))
        except Exception as exc:
            print('      GET %s 异常 %s' % (path, exc))
    if all(measure(client, '阶段2')):
        print('  ★ 结论：**先访问 H5 页/应用接口**再进 checkLogin 才被授权 → 设备可复刻')
        return 0

    print('\n[阶段3] 再回放门户序列，这次带 Accept-Encoding（检验"会不会解压"是否被当机器人特征）')
    replay(client, items, True, '阶段3')
    if all(measure(client, '阶段3')):
        print('  ★ 结论：**Accept-Encoding（解压能力）**是关键特征 → 设备需支持 gzip 解压')
        return 0

    print('\n[阶段4] 等 3 分钟后复测（排除"只是需要时间"）')
    time.sleep(180)
    if all(measure(client, '阶段4')):
        print('  ★ 结论：仍然只是需要时间（激活延迟）')
        return 0

    print('\n' + '=' * 72)
    print('结论：以上 4 个阶段都未解锁 ⇒ 差异不在 HTTP 请求内容层，')
    print('      指向 **TLS 握手指纹 / HTTP 客户端特征（JA3/JA4、ALPN、头顺序）**。')
    print('      下一步：用 curl_cffi（伪装 Chrome TLS 指纹）做同样一次登录复测 —— 若解锁，')
    print('      说明只要"像 Chrome 的 TLS"就能过，设备侧要评估 mbedTLS 能否定制 ClientHello。')
    return 2


if __name__ == '__main__':
    sys.exit(main())
