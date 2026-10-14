#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""会话绑定诊断：定位"新登录的会话为何无法用于 daily-checkin 微应用"。

固件现象：CAS 登录成功、拿到 1 年期 SESSION，但
  - /daily-checkin/checkLogin?appId=294 被 302 退回平台首页（应为 H5 页）
  - /daily-checkin/api/student-checkin-record/today 返回 4001 登录无效或已过期

本脚本在 PC 上复现并做对照，每次登录内测试三个变体：
  变体① 直接进微应用
  变体② 先访问平台首页"预热"再进微应用
  变体③ 进微应用时带 Referer: https://jzsz.uestc.edu.cn/

用法: python diag_session.py
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


def short(value: str) -> str:
    return (value[:8] + '…') if value else '(none)'


def jar_summary(client) -> str:
    parts = [f'{name}@{domain}={short(cookie.value)}'
             for (domain, name), cookie in client.jar._c.items()]
    return ', '.join(sorted(parts)) or '(empty)'


def call_today(client) -> str:
    """查今日状态并返回可读结论。"""
    resp = wsc_checkin.today(client)
    try:
        payload = json.loads(resp.body.decode('utf-8', 'replace'))
    except json.JSONDecodeError:
        payload = {}
    return f'HTTP {resp.status} code={payload.get("code")} msg={payload.get("msg")}'


def enter_app(client, referer: str | None = None) -> str:
    """进入微应用，返回"最终落在哪个 URL"。"""
    headers = {'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8'}
    if referer:
        headers['Referer'] = referer
    resp = client.follow(wsc_checkin.CHECK_LOGIN, headers=headers)
    return resp.url[:80]


def run_case(label: str, injected_session: str, cfg: dict) -> None:
    print('=' * 78)
    print(f'### {label}')
    client = HttpClient(user_agent=cfg.get('user_agent') or DEFAULT_UA)

    if injected_session:
        client.jar.set('SESSION', injected_session, 'jzsz.uestc.edu.cn', '/')
        print(f'  预注入 SESSION = {short(injected_session)}')

    # 指纹是免二次验证的关键（固件同样会注入）
    fingerprint = cfg.get('browser_fingerprint') or ''
    if fingerprint and not fingerprint.startswith('PUT_'):
        wsc_cas.set_fingerprint_cookie(client, fingerprint)
        print(f'  注入指纹 = {short(fingerprint)}')

    resp = client.request('GET', JZSZ + '/')
    print(f'  匿名 GET /           -> HTTP {resp.status}  jar: {jar_summary(client)}')

    page = wsc_cas.fetch_login_page(client)
    print(f'  取登录页            -> salt={page.salt}')

    resp = wsc_cas.submit_login(client, cfg['student_id'], cfg['password'], page)
    verdict, detail = wsc_cas.classify_login(resp)
    print(f'  POST 登录           -> HTTP {resp.status} 判定={verdict}')
    if verdict != 'OK':
        print('  非 OK，终止本组（若为 MFA 说明指纹未生效）')
        return

    before = client.jar.get('SESSION')
    resp = client.follow(detail)
    after = client.jar.get('SESSION')
    print(f'  票据换会话          -> HTTP {resp.status}  SESSION {short(before)} => {short(after)}')
    body = resp.text
    print(f'  首页含学号: {cfg["student_id"] in body}  页面长度={len(body)}')

    print('  --- 变体① 直接进微应用 ---')
    print(f'    enter_app -> {enter_app(client)}')
    print(f'    today     -> {call_today(client)}')
    print(f'    jar: {jar_summary(client)}')

    print('  --- 变体② 先预热平台首页，再进微应用 ---')
    client.follow(JZSZ + '/')
    print(f'    enter_app -> {enter_app(client)}')
    print(f'    today     -> {call_today(client)}')
    print(f'    jar: {jar_summary(client)}')

    print('  --- 变体③ 进微应用时带 Referer ---')
    print(f'    enter_app -> {enter_app(client, referer=JZSZ + "/")}')
    print(f'    today     -> {call_today(client)}')
    print(f'    jar: {jar_summary(client)}')


def main() -> int:
    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)
    captured = cfg.get('session_cookie') or ''
    if captured.startswith('PUT_'):
        captured = ''

    # A 组：完全从零开始（复刻固件）
    run_case('A 组：登录前无任何 SESSION（复刻固件）', '', cfg)
    # C 组：先注入抓包旧 SESSION 再登录（复刻此前"成功"的 Python V2）
    run_case('C 组：登录前注入旧 SESSION（复刻 Python V2）', captured, cfg)
    return 0


if __name__ == '__main__':
    sys.exit(main())
