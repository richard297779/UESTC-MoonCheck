import sys
from pathlib import Path
import unittest
from types import SimpleNamespace
from email.message import Message

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from mooncheck_app import config, privacy


class PrivacyTests(unittest.TestCase):
    def test_http_diagnostics_never_log_query_or_redirect_ticket(self):
        from mooncheck_app.http_client import HttpClient
        logs = []
        client = HttpClient(log=logs.append)
        headers = Message()
        headers['Location'] = 'https://example.invalid/auth?code=fixture-code'
        response = SimpleNamespace(status=302, headers=headers, read=lambda: b'')
        client.opener = SimpleNamespace(open=lambda *_a, **_k: response)
        client.request('GET', 'https://example.invalid/path/fixture-key?ticket=fixture-ticket')
        for secret in ('fixture-code', 'fixture-key', 'fixture-ticket'):
            self.assertNotIn(secret, str(logs))

    def test_export_clears_nested_identifiers_locations_and_credentials(self):
        source = {'student_id': 'fixture-id', 'accounts': [{'wechat': {'access_token': 'secret-value'},
                  'location': {'latitude': 30.123, 'address': 'private-room'}, 'last_msg': 'private-result'}]}
        safe = config.redact_copy(source)
        self.assertNotIn('fixture-id', str(safe))
        self.assertNotIn('secret-value', str(safe))
        self.assertNotIn('private-room', str(safe))
        self.assertNotIn('30.123', str(safe))
        self.assertEqual(source['student_id'], 'fixture-id')

    def test_logs_filter_redirects_and_known_secrets(self):
        text = privacy.sanitize('SESSION=unrecognized https://user:pwd@example.invalid/code?ticket=private ' +
                                'fixture-password fixture-refresh', {'password': 'fixture-password', 'wechat': {'refresh': 'fixture-refresh'}})
        for secret in ('user:pwd', 'private', 'unrecognized', 'fixture-password', 'fixture-refresh'):
            self.assertNotIn(secret, text)
        self.assertIn('example.invalid', text)

    def test_config_display_and_url_do_not_reveal_prefixes(self):
        cfg = config.default_config()
        cfg.update(student_id='fixture-user', password='fixture-password', session_cookie='fixture-cookie', browser_fingerprint='fixture-fingerprint')
        for field in ('student_id', 'password', 'session_cookie', 'browser_fingerprint'):
            self.assertEqual(config.masked_value(cfg, field), '<已隐藏>')
        self.assertNotIn('private-key', config.mask_url('https://user:private-key@example.invalid/private-key?token=private-key'))
