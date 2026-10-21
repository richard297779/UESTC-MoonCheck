# -*- coding: utf-8 -*-
"""最小 HTTP 客户端：手工 Cookie 罐 + 手工重定向 + 明文日志（仅标准库）。

设计目标：与 ESP32 固件的 http_cli 行为一一对应——不用 requests 的隐式魔法，
所有 302 都显式跟随，所有 Cookie 都显式管理，便于把逻辑原样搬到固件。
"""
import gzip
import json
import re
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

DEFAULT_UA = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
              '(KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36')

_FIRST_PAIR = re.compile(r'^\s*([^=;\s]+)=([^;]*)')


class Cookie(object):
    __slots__ = ('name', 'value', 'domain', 'path', 'expires')

    def __init__(self, name, value, domain, path='/', expires=None):
        self.name = name
        self.value = value
        self.domain = (domain or '').lower()
        self.path = path or '/'
        self.expires = expires


class CookieJar(object):
    """按「域 + 路径」匹配的最小 Cookie 罐（支持多域，对应固件 cfg_store）。"""

    def __init__(self):
        self._c = {}

    def set(self, name, value, domain, path='/', expires=None):
        self._c[(domain.lower(), name)] = Cookie(name, value, domain, path, expires)

    def get(self, name):
        for (_d, n), c in self._c.items():
            if n == name:
                return c.value
        return None

    def names(self):
        return sorted(n for (_d, n) in self._c)

    def update(self, url, headers):
        """从响应头解析 Set-Cookie 并入库（含 Max-Age 过期与删除）。"""
        host = (urllib.parse.urlsplit(url).hostname or '').lower()
        for raw in (headers.get_all('Set-Cookie') or []):
            parts = raw.split(';')
            m = _FIRST_PAIR.match(parts[0])
            if not m:
                continue
            name, value = m.group(1), m.group(2)
            path, domain, expires = '/', host, None
            for attr in parts[1:]:
                a = attr.strip()
                low = a.lower()
                if low.startswith('path='):
                    path = a.split('=', 1)[1].strip() or '/'
                elif low.startswith('domain='):
                    domain = a.split('=', 1)[1].strip().lstrip('.')
                elif low.startswith('max-age='):
                    try:
                        expires = time.time() + int(a.split('=', 1)[1])
                    except ValueError:
                        expires = None
            if value == '' and expires is not None and expires <= time.time():
                self._c.pop((domain.lower(), name), None)
            else:
                self.set(name, value, domain, path, expires)

    def header(self, url):
        u = urllib.parse.urlsplit(url)
        host = (u.hostname or '').lower()
        path = u.path or '/'
        now = time.time()
        out = []
        for (_dom, _n), c in self._c.items():
            if not (host == c.domain or host.endswith('.' + c.domain)):
                continue
            if not path.startswith(c.path):
                continue
            if c.expires is not None and c.expires < now:
                continue
            out.append('%s=%s' % (c.name, c.value))
        return '; '.join(out)


class Resp(object):
    def __init__(self, status, headers, body, url):
        self.status = status
        self.headers = headers
        self.body = body
        self.url = url
        self.history = []

    @property
    def text(self):
        return self.body.decode('utf-8', 'replace')

    def json(self):
        return json.loads(self.body.decode('utf-8'))

    def header(self, name, default=''):
        return self.headers.get(name, default)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """关闭自动重定向：302 交回调用者处理（与固件一致）。"""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class HttpClient(object):
    def __init__(self, user_agent=DEFAULT_UA, timeout=20, verbose=True):
        self.ua = user_agent or DEFAULT_UA
        self.timeout = timeout
        self.verbose = verbose
        self.jar = CookieJar()
        ctx = ssl.create_default_context()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=ctx), _NoRedirect())

    def request(self, method, url, headers=None, data=None, json_body=None):
        h = {
            'User-Agent': self.ua,
            'Accept': '*/*',
            'Accept-Language': 'zh-CN,zh;q=0.9',
            'Accept-Encoding': 'gzip, deflate',
            'Connection': 'close',
        }
        if headers:
            h.update(headers)
        cookie = self.jar.header(url)
        if cookie:
            h['Cookie'] = cookie

        body = None
        if json_body is not None:
            body = json.dumps(json_body, ensure_ascii=False).encode('utf-8')
            h['Content-Type'] = 'application/json'
        elif data is not None:
            body = urllib.parse.urlencode(data).encode('utf-8')
            h['Content-Type'] = 'application/x-www-form-urlencoded'

        req = urllib.request.Request(url, data=body, headers=h, method=method)
        try:
            raw = self.opener.open(req, timeout=self.timeout)
            status, hdrs, payload = raw.status, raw.headers, raw.read()
        except urllib.error.HTTPError as e:
            status, hdrs, payload = e.code, e.headers, e.read()

        self.jar.update(url, hdrs)
        enc = (hdrs.get('Content-Encoding') or '').lower()
        if payload:
            try:
                if 'gzip' in enc:
                    payload = gzip.decompress(payload)
                elif 'deflate' in enc:
                    payload = zlib.decompress(payload, -zlib.MAX_WBITS)
            except Exception:
                pass

        resp = Resp(status, hdrs, payload, url)
        if self.verbose:
            loc = resp.header('Location')
            print('    %-4s %s -> %s%s%s' % (
                method, url[:120], status,
                ('  Location: ' + loc) if loc else '',
                ('  [%d B]' % len(payload)) if payload else ''))
        return resp

    def follow(self, url, headers=None, max_hops=10):
        """手工跟随 302（返回最终响应，历史记录在 resp.history）。"""
        hops = []
        for _ in range(max_hops):
            r = self.request('GET', url, headers=headers)
            if r.status in (301, 302, 303, 307, 308) and r.header('Location'):
                url = urllib.parse.urljoin(url, r.header('Location'))
                hops.append(url)
                continue
            r.history = hops
            return r
        raise RuntimeError('重定向次数过多: %s' % hops)
