# -*- coding: utf-8 -*-
"""CAS 统一身份认证：登录页解析 + 密码加密 + 提交 + 票据换会话。

密码算法（抓包双重确认，见 dev/docs/api-contract.md）：
    明文 = randomString(64) + 密码
    key  = pwdEncryptSalt（登录页隐藏域，16 字符，UTF-8）
    iv   = randomString(16)（不随请求发送）
    输出 = base64(AES-128-CBC/PKCS7(明文))
加密由本包的纯 Python AES 实现（aes.py），无第三方依赖。
"""
import re
import time
import urllib.parse

from . import aes

CAS = 'https://idas.uestc.edu.cn/authserver'
SERVICE = 'https://jzsz.uestc.edu.cn/'
LOGIN_URL = CAS + '/login?service=' + urllib.parse.quote(SERVICE, safe='')
BFP_URL = CAS + '/bfp/info'
AES_CHARS = 'ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678'

_RE_EXECUTION = re.compile(r'name="execution"\s+value="([^"]+)"')
_RE_SALT = re.compile(r'id="pwdEncryptSalt"\s+value="([^"]+)"')
_RE_ALT_SALT = re.compile(r'pwdEncryptSalt["\']?\s*[:=]\s*["\']([^"\']+)["\']')
_RE_MFA = re.compile(r'isMultifactor|authLogin|multifactor|二次验证|多因素认证')


def encrypt_password(password, salt):
    """加密密码（随机前缀 + 随机 IV，每次密文不同）。"""
    return aes.encrypt_cas_password(password, salt)


def random_fingerprint():
    import random
    return ''.join(random.choice('0123456789ABCDEF') for _ in range(32))


class LoginPage(object):
    def __init__(self, execution, salt):
        self.execution = execution
        self.salt = salt


def fetch_login_page(client):
    r = client.request('GET', LOGIN_URL, headers={
        'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
        'Upgrade-Insecure-Requests': '1',
    })
    if r.status >= 400:
        raise RuntimeError('取登录页失败：HTTP %s' % r.status)
    html = r.text
    m = _RE_EXECUTION.search(html)
    if not m:
        raise RuntimeError('未找到 execution（页面结构可能已变更）')
    salt = None
    for rx in (_RE_SALT, _RE_ALT_SALT):
        mm = rx.search(html)
        if mm:
            salt = mm.group(1)
            break
    if not salt:
        raise RuntimeError('未找到 pwdEncryptSalt（页面结构可能已变更）')
    return LoginPage(m.group(1), salt)


def set_fingerprint_cookie(client, value):
    """直接注入指纹 Cookie，等价于浏览器已上报并获批。"""
    client.jar.set('MULTIFACTOR_BROWSER_FINGERPRINT', value,
                   'idas.uestc.edu.cn', '/', time.time() + 315360000)


def report_fingerprint(client, value):
    """模拟页面 JS：GET /authserver/bfp/info?bfp=<32HEX>&_=<毫秒>。"""
    url = '%s?bfp=%s&_=%d' % (BFP_URL, value, int(time.time() * 1000))
    return client.request('GET', url, headers={
        'X-Requested-With': 'XMLHttpRequest',
        'Accept': 'application/json, text/javascript, */*; q=0.01',
        'Referer': LOGIN_URL,
    })


def submit_login(client, username, password, page):
    data = {
        'username': username,
        'password': encrypt_password(password, page.salt),
        'captcha': '',
        '_eventId': 'submit',
        'cllt': 'userNameLogin',
        'dllt': 'generalLogin',
        'lt': '',
        'execution': page.execution,
    }
    return client.request('POST', LOGIN_URL, data=data, headers={
        'Content-Type': 'application/x-www-form-urlencoded',
        'Origin': 'https://idas.uestc.edu.cn',
        'Referer': LOGIN_URL,
        'Upgrade-Insecure-Requests': '1',
        'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
    })


def mask_ticket(text):
    return re.sub(r'ticket=[^&\s]+', 'ticket=***', text or '')


def classify_login(r):
    """返回 (判定, 详情)：OK / MFA / REDIRECT / FAIL。"""
    loc = r.header('Location') or ''
    try:
        body = r.text
    except Exception:
        body = ''
    if loc and 'ticket=' in loc:
        return 'OK', loc
    if loc and ('authLogin' in loc or 'isMultifactor' in loc):
        return 'MFA', loc
    if _RE_MFA.search(body):
        return 'MFA', loc or '(页面内含多因素认证特征)'
    if loc:
        return 'REDIRECT', loc
    return 'FAIL', (body[:300] if body else '(空响应)')


def login(client, username, password, bfp_value=None, log=None):
    """完整登录链；返回 (判定, 详情)。bfp_value 为空则不上报指纹。
    判定：OK / MFA / NO_SESSION / REDIRECT / FAIL（异常上抛 NetworkError/RuntimeError）。
    """
    log = log or (lambda _m: None)
    log('取登录页 ...')
    page = fetch_login_page(client)
    log('  execution=%s  pwdEncryptSalt=%s' % (page.execution[:16], page.salt))

    if bfp_value:
        log('  注入可信指纹 %s…' % bfp_value[:8])
        set_fingerprint_cookie(client, bfp_value)
    else:
        log('  未配置指纹，直接提交')

    log('提交登录 ...')
    r = submit_login(client, username, password, page)
    verdict, detail = classify_login(r)
    log('  HTTP %s -> %s' % (r.status, mask_ticket(detail)[:160]))
    if verdict != 'OK':
        return verdict, detail

    log('票据换会话 ...')
    client.follow(detail)
    session = client.jar.get('SESSION')
    if not session:
        return 'NO_SESSION', '登录成功但未取到 SESSION'
    log('  SESSION 已建立（%s…）' % session[:8])
    return 'OK', detail
