#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T3：自动化浏览器（Playwright Chromium）登录 → 新会话是否被微应用授权？

这是"伴生程序架构"的可行性判定：
  - 若 Playwright 建立的会话能被授权 → 伴生脚本可全自动（每天定时登录+供会话）；
  - 若被拒 → 说明平台识别自动化，伴生程序必须改为挂在你真实 Chrome 上"只读 Cookie"。

用法:
    python -m pip install playwright
    python -m playwright install chromium
    python companion_probe.py              # 有头模式（更接近真人）
    python companion_probe.py --headless   # 无头模式（对照组）

依赖 config.json 的 student_id / password / user_agent。每次运行消耗一次登录。
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

CONFIG = os.path.join(HERE, 'config.json')
JZSZ = 'https://jzsz.uestc.edu.cn'
CHECK_LOGIN = JZSZ + '/daily-checkin/checkLogin?appId=294'
H5_MARK = '/daily-checkin-h5/'


def main() -> int:
    parser = argparse.ArgumentParser(description='Playwright 登录 → 授权判定')
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--timeout', type=float, default=45.0, help='每步等待上限（秒）')
    args = parser.parse_args()

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print('缺少 playwright：python -m pip install playwright && python -m playwright install chromium')
        return 2

    with open(CONFIG, 'r', encoding='utf-8') as handle:
        cfg = json.load(handle)

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=args.headless)
        ctx = browser.new_context(user_agent=cfg.get('user_agent') or
                                  'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
                                  '(KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36')
        page = ctx.new_page()
        page.set_default_timeout(args.timeout * 1000)

        print('[1] 打开平台入口（会跳 CAS 登录页）...')
        page.goto(JZSZ + '/', wait_until='domcontentloaded')
        print('    当前 URL: %s' % page.url[:90])

        if 'authserver' not in page.url:
            print('    已是登录态（CAS 有 SSO 会话）—— 本次测的是"复用旧会话"，不算数；')
            print('    请先在浏览器里退出登录/清 Cookie 再跑。')
            browser.close()
            return 1

        print('[2] 填写并提交登录表单 ...')
        page.wait_for_selector('input[name="username"]', state='visible')
        page.fill('input[name="username"]', str(cfg['student_id']))
        # 金智 CAS 的可见密码框是 readonly（前端 JS 负责 AES 加密），
        # 直接 evaluate 写 value 后提交，与真人路径一致（JS 在 submit 时读取该值）。
        page.evaluate(
            """(pwd) => {
                const el = document.querySelector('#password') ||
                           document.querySelector('input[name="password"]');
                el.removeAttribute('readonly');
                el.value = pwd;
                el.dispatchEvent(new Event('input', { bubbles: true }));
            }""",
            cfg['password'])
        submit = page.query_selector('#submitLogin') or page.query_selector('button[type="submit"]') \
            or page.query_selector('input[type="submit"]')
        if submit is None:
            print('    找不到提交按钮，请把登录页截图发我')
            browser.close()
            return 1
        submit.click()

        print('[3] 等待回到平台首页 ...')
        try:
            page.wait_for_url('**jzsz.uestc.edu.cn**', timeout=args.timeout * 1000)
        except Exception:
            print('    未跳回平台，当前 URL: %s' % page.url[:120])
        print('    登录后 URL: %s' % page.url[:90])

        print('[4] 直接访问 checkLogin（关键判定）...')
        page.goto(CHECK_LOGIN, wait_until='domcontentloaded')
        reached = H5_MARK in page.url
        print('    最终 URL: %s' % page.url[:110])
        print('    进入 H5 打卡页: %s' % reached)

        session = ''
        for cookie in ctx.cookies():
            if cookie['name'] == 'SESSION':
                session = cookie['value']
        print('    SESSION=%s…  expires=%s' % (session[:8] if session else '(none)',
                                               cookie.get('expires', '?') if session else '?'))

        print('[5] 用该会话再查一次业务接口（双重确认）...')
        try:
            resp = page.request.get(
                JZSZ + '/daily-checkin/api/student-checkin-record/today',
                headers={'Platform': 'h5', 'Referer': JZSZ + '/daily-checkin-h5/index.html?appId=294'})
            body = resp.json()
            print('    today -> code=%s msg=%s status=%s'
                  % (body.get('code'), body.get('msg'),
                     (body.get('data') or {}).get('recordStatusCode')))
            api_ok = body.get('code') == 0
        except Exception as exc:
            print('    today 查询失败: %s' % exc)
            api_ok = False

        browser.close()

    print('=' * 70)
    if reached and api_ok:
        print('结论：Playwright 会话**被授权** → 伴生程序可全自动（架构 B1）✅')
        return 0
    print('结论：Playwright 会话**未被授权** → 自动化被识别或另有门槛 → 需走 B2（真实 Chrome 读 Cookie）')
    return 3


if __name__ == '__main__':
    sys.exit(main())
