"""QR Connect in an isolated, memory-only HTTP session. Never executes remote JS."""
import http.cookiejar
import re
import threading
import urllib.error
import urllib.parse
import urllib.request

CALLBACK = 'https://yybadaccess.3g.qq.com/pc_yyb/pcyyb_oauth'
QR_URL = 'https://open.weixin.qq.com/connect/qrconnect?' + urllib.parse.urlencode({
    'appid': 'wxd44977328b36e647', 'redirect_uri': CALLBACK + '?login_type=WX',
    'response_type': 'code', 'scope': 'snsapi_login,snsapi_runtime_pcsdk',
    'state': 'web', 'fast_login': '1', 'self_redirect': 'true',
})
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131 Safari/537.36'
HOSTS = {'open.weixin.qq.com', 'long.open.weixin.qq.com', 'yybadaccess.3g.qq.com'}


class QrError(Exception):
    """Only fixed, credential-free messages reach the UI."""


class CallbackComplete(Exception):
    """Stop before navigating to a landing page; the CookieJar is complete."""


def trusted(url):
    try:
        parsed = urllib.parse.urlsplit(url)
        return parsed.scheme == 'https' and parsed.hostname in HOSTS and parsed.port in (None, 443) and not parsed.username and not parsed.password
    except ValueError:
        return False


class RestrictedRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self, cookies=None):
        super().__init__()
        self.cookies = cookies

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(req.full_url).hostname == 'yybadaccess.3g.qq.com' and self.cookies is not None:
            # HTTPCookieProcessor runs before HTTPErrorProcessor dispatches 302.
            # OAuth can set all three HttpOnly cookies and redirect to a public
            # landing page. Nothing on that page is needed for BLE import.
            try:
                cookie_fields(self.cookies)
            except QrError:
                pass
            else:
                if fp is not None:
                    fp.close()
                raise CallbackComplete()
        if not trusted(newurl) or (urllib.parse.urlsplit(req.full_url).hostname == 'yybadaccess.3g.qq.com' and
                                  urllib.parse.urlsplit(newurl).hostname != 'yybadaccess.3g.qq.com'):
            if fp is not None:
                fp.close()
            if urllib.parse.urlsplit(req.full_url).hostname == 'yybadaccess.3g.qq.com':
                present = {cookie.name for cookie in self.cookies or () if cookie.name in {'openid', 'accesstoken', 'refreshtoken'} and not cookie.is_expired()}
                raise QrError(f'回调授权不完整或无效（检测到 {len(present)}/3 项），已阻止外部跳转；请刷新或手动导入')
            raise QrError('微信授权服务返回非预期跳转，已停止；请刷新或手动导入')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def parse_qr_page(html):
    match = re.search(r'/connect/qrcode/([A-Za-z0-9_-]{8,128})', html)
    if not match:
        raise QrError('微信页面未提供二维码，请刷新或使用手动导入')
    return match.group(1)


def parse_poll(text):
    match = re.search(r'\b(?:window\.)?wx_errcode\s*=\s*(\d{3})\s*;', text)
    if not match:
        raise QrError('扫码状态格式已变化，请使用手动导入并反馈版本')
    status = int(match.group(1))
    if status == 405:
        code = re.search(r'''\b(?:window\.)?wx_code\s*=\s*["']([A-Za-z0-9_-]{1,256})["']''', text)
        if not code:
            raise QrError('授权确认未返回有效 code，请重新获取二维码')
        return status, code.group(1)
    if status not in (402, 403, 404, 408):
        raise QrError('微信返回未知扫码状态，请刷新二维码')
    return status, None


def cookie_fields(cookies):
    fields = {}
    for cookie in cookies:
        if cookie.domain.lstrip('.') not in {'yybadaccess.3g.qq.com', '3g.qq.com', 'qq.com'} or cookie.is_expired():
            continue
        key = {'openid': 'openid', 'accesstoken': 'access', 'refreshtoken': 'refresh'}.get(cookie.name)
        if key:
            value = cookie.value
            if not value or len(value) > 16384 or any(c in value for c in '\r\n;'):
                raise QrError('授权 Cookie 格式无效，未填入表单')
            if key in fields and fields[key] != value:
                raise QrError('授权 Cookie 冲突，请刷新重试')
            fields[key] = value
    if set(fields) != {'openid', 'access', 'refresh'}:
        raise QrError('回调未返回完整的三个授权 Cookie，请重新扫码或手动导入')
    return dict(fields, expiresAt='0')


class QrSession:
    def __init__(self):
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(RestrictedRedirect(self.cookies), urllib.request.HTTPCookieProcessor(self.cookies))
        self.cancelled = threading.Event()
        self.uuid = None

    def close(self):
        self.cancelled.set()
        self.uuid = None
        self.cookies.clear()

    def get(self, url, limit, timeout=20):
        if self.cancelled.is_set():
            raise QrError('授权已取消')
        if not trusted(url):
            raise QrError('已阻止未允许的授权地址')
        try:
            request = urllib.request.Request(url, headers={'User-Agent': UA, 'Referer': QR_URL})
            with self.opener.open(request, timeout=timeout) as response:
                if not trusted(response.url):
                    raise QrError('授权响应来源无效')
                data = response.read(limit + 1)
                if len(data) > limit:
                    raise QrError('授权响应过大，已停止')
        except (OSError, urllib.error.URLError):
            raise QrError('微信授权网络请求失败，请检查电脑网络后刷新') from None
        if self.cancelled.is_set():
            self.cookies.clear()
            raise QrError('授权已取消')
        return data

    def start(self):
        self.uuid = parse_qr_page(self.get(QR_URL, 262144).decode('utf-8', 'replace'))
        image = self.get('https://open.weixin.qq.com/connect/qrcode/' + self.uuid, 524288)
        return image

    def poll(self):
        if not self.uuid:
            raise QrError('请先获取二维码')
        result = self.get('https://long.open.weixin.qq.com/connect/l/qrconnect?' +
                          urllib.parse.urlencode({'uuid': self.uuid}), 65536, timeout=35)
        status, code = parse_poll(result.decode('utf-8', 'replace'))
        if status == 405:
            self.cookies.clear()
            try:
                try:
                    self.get(CALLBACK + '?' + urllib.parse.urlencode({'login_type': 'WX', 'code': code, 'state': 'web'}), 65536)
                except CallbackComplete:
                    pass
                if self.cancelled.is_set():
                    raise QrError('授权已取消')
                return status, cookie_fields(self.cookies)
            finally:
                self.close()
        return status, None
