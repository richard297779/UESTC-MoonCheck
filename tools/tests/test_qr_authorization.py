import http.cookiejar
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qr_authorization import CALLBACK, QrError, QrSession, RestrictedRedirect, cookie_fields, parse_poll, parse_qr_page, trusted


def cookie(name, value, domain='yybadaccess.3g.qq.com'):
    return http.cookiejar.Cookie(0, name, value, None, False, domain, True, domain.startswith('.'), '/', True, True, None, True, None, None, {})


class QrProtocolTests(unittest.TestCase):
    def test_complete_cookies_on_302_finish_without_requesting_landing_page(self):
        import io
        import urllib.request
        import urllib.response
        from email.message import Message
        from urllib.parse import urlsplit
        calls = []
        class FakeHttps(urllib.request.HTTPSHandler):
            def https_open(self, request):
                calls.append(urlsplit(request.full_url).hostname)
                headers = Message()
                if calls[-1] == 'long.open.weixin.qq.com':
                    response = urllib.response.addinfourl(io.BytesIO(b"window.wx_errcode=405;window.wx_code='fixture_code';"), headers, request.full_url, 200)
                else:
                    self_outer.assertEqual(calls[-1], 'yybadaccess.3g.qq.com')
                    for value in ('openid=fixture-id; Path=/; Secure; HttpOnly', 'accesstoken=fixture-access; Path=/; Secure; HttpOnly', 'refreshtoken=fixture-refresh; Path=/; Secure; HttpOnly'):
                        headers.add_header('Set-Cookie', value)
                    headers['Location'] = 'https://landing.invalid/unused?code=do-not-follow'
                    response = urllib.response.addinfourl(io.BytesIO(b''), headers, request.full_url, 302)
                response.msg = 'fixture'
                return response
        self_outer = self
        client = QrSession()
        client.uuid = 'fixture_uuid'
        client.opener = urllib.request.build_opener(FakeHttps(), RestrictedRedirect(client.cookies), urllib.request.HTTPCookieProcessor(client.cookies))
        status, fields = client.poll()
        self.assertEqual(status, 405)
        self.assertEqual(fields['access'], 'fixture-access')
        self.assertEqual(calls, ['long.open.weixin.qq.com', 'yybadaccess.3g.qq.com'])
        self.assertFalse(list(client.cookies))

    def test_partial_cookies_do_not_bypass_redirect_allowlist(self):
        import urllib.request
        jar = http.cookiejar.CookieJar()
        jar.set_cookie(cookie('openid', 'fixture-id'))
        with self.assertRaises(QrError):
            RestrictedRedirect(jar).redirect_request(urllib.request.Request(CALLBACK), None, 302, '', {}, 'https://landing.invalid/')

    def test_buttons_fit_requested_layout_at_different_tk_scales(self):
        import tkinter as tk
        from qr_dialog import QrDialog
        root = tk.Tk()
        root.geometry('100x80')
        root.update()
        try:
            for scale in (1.0, 1.5, 2.0):
                root.tk.call('tk', 'scaling', scale)
                with patch('qr_dialog.threading.Thread'):
                    dialog = QrDialog(root, '0', lambda _f: None)
                    dialog.window.update()
                    for button in (dialog.refresh_button, dialog.cancel_button):
                        bottom = button.winfo_rooty() - dialog.window.winfo_rooty() + button.winfo_height()
                        self.assertLessEqual(bottom, dialog.window.winfo_height() - 10)
                        self.assertGreaterEqual(button.winfo_height(), button.winfo_reqheight())
                    dialog.status.set('授权 Cookie 尚不完整且回跳地址未获允许，请刷新或手动导入')
                    dialog.window.update_idletasks()
                    bottom = dialog.cancel_button.winfo_rooty() - dialog.window.winfo_rooty() + dialog.cancel_button.winfo_height()
                    self.assertLessEqual(bottom, dialog.window.winfo_height() - 10)
                    dialog.close()
        finally:
            root.destroy()

    def test_dialog_refresh_discards_old_generation_and_fills_only_current(self):
        import tkinter as tk
        from qr_dialog import QrDialog
        root = tk.Tk()
        root.withdraw()
        received = []
        try:
            with patch('qr_dialog.threading.Thread'):
                dialog = QrDialog(root, '2', received.append)
                first_session, first_generation = dialog.session, dialog.generation
                dialog.refresh()
                self.assertTrue(first_session.cancelled.is_set())
                dialog.events.put((first_generation, 'fields', {'access': 'stale-value'}))
                dialog.poll()
                self.assertFalse(received)
                fields = dict(openid='fixture-id', access='fixture-access', refresh='fixture-refresh', expiresAt='0')
                dialog.events.put((dialog.generation, 'fields', fields))
                dialog.poll()
                self.assertEqual(received, [fields])
                self.assertTrue(dialog.closed)
                self.assertTrue(dialog.session.cancelled.is_set())
        finally:
            root.destroy()

    def test_page_and_poll_parse_without_executing_remote_script(self):
        self.assertEqual(parse_qr_page('<img src="/connect/qrcode/fixture_uuid_123">'), 'fixture_uuid_123')
        self.assertEqual(parse_poll("window.wx_errcode=405;window.wx_code='fixture_code';"), (405, 'fixture_code'))
        self.assertEqual(parse_poll('window.wx_errcode=408;'), (408, None))
        self.assertEqual(parse_poll('window.wx_errcode=404;'), (404, None))
        with self.assertRaises(QrError):
            parse_poll("window.wx_errcode=405;window.wx_code='bad;code';")
        with self.assertRaises(QrError):
            parse_poll('window.wx_errcode=500;')

    def test_cookie_fields_require_complete_trusted_credentials(self):
        values = [cookie('openid', 'fixture-id'), cookie('accesstoken', 'fixture-access', '.3g.qq.com'), cookie('refreshtoken', 'fixture-refresh')]
        self.assertEqual(cookie_fields(values), dict(openid='fixture-id', access='fixture-access', refresh='fixture-refresh', expiresAt='0'))
        with self.assertRaises(QrError):
            cookie_fields(values[:2])
        with self.assertRaises(QrError):
            cookie_fields(values + [cookie('openid', 'conflict-id')])
        with self.assertRaises(QrError):
            cookie_fields([cookie('openid', 'fixture-id', 'evil.invalid')] + values[1:])

    def test_only_exact_https_hosts_and_no_userinfo(self):
        for url in ('http://open.weixin.qq.com/', 'https://open.weixin.qq.com.evil.invalid/',
                    'https://user@yybadaccess.3g.qq.com/', 'https://yybadaccess.3g.qq.com:8443/'):
            self.assertFalse(trusted(url))
        self.assertTrue(trusted(CALLBACK))

    def test_callback_redirect_cannot_leave_callback_host(self):
        import urllib.request
        request = urllib.request.Request(CALLBACK)
        with self.assertRaises(QrError):
            RestrictedRedirect().redirect_request(request, None, 302, '', {}, 'https://open.weixin.qq.com/')

    def test_confirmed_code_exchanges_in_isolated_session_and_clears(self):
        client = QrSession()
        client.uuid = 'fixture_uuid'
        calls = []
        def get(url, *_args, **_kwargs):
            calls.append(url)
            if 'long.open' in url:
                return b"window.wx_errcode=405;window.wx_code='fixture_code';"
            for name, value in [('openid', 'fixture-id'), ('accesstoken', 'fixture-access'), ('refreshtoken', 'fixture-refresh')]:
                client.cookies.set_cookie(cookie(name, value))
            return b'{"code":0}'
        with patch.object(client, 'get', get):
            status, fields = client.poll()
        self.assertEqual(status, 405)
        self.assertEqual(fields['access'], 'fixture-access')
        self.assertIn('code=fixture_code', calls[1])
        self.assertFalse(list(client.cookies))
        self.assertIsNone(client.uuid)
        self.assertTrue(client.cancelled.is_set())

    def test_incomplete_callback_is_not_a_success_and_is_cleared(self):
        client = QrSession()
        client.uuid = 'fixture_uuid'
        client.cookies.set_cookie(cookie('openid', 'old-session'))
        with patch.object(client, 'get', side_effect=[b"window.wx_errcode=405;window.wx_code='fixture_code';", b'{}']):
            with self.assertRaises(QrError):
                client.poll()
        self.assertFalse(list(client.cookies))

    def test_cancel_never_opens_a_request(self):
        client = QrSession()
        client.close()
        with patch.object(client.opener, 'open') as open_request:
            with self.assertRaises(QrError):
                client.start()
        open_request.assert_not_called()
