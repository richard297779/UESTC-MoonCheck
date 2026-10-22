# -*- coding: utf-8 -*-
"""离线测试：不联网、不写真实配置。

运行（在 desktop/ 目录）：
    python -m unittest discover -s tests -v          # 需要 PYTHONPATH=src
或：
    python tests/test_offline.py
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), 'src')
sys.path.insert(0, SRC)

from mooncheck_app import aes, cas_auth, checkin_api, config  # noqa: E402


class TestAes(unittest.TestCase):
    def test_self_test(self):
        ok, detail = aes.self_test()
        self.assertTrue(ok, detail)

    def test_cas_encrypt_shape(self):
        import base64
        ct = cas_auth.encrypt_password('P@ssw0rd!', '0123456789abcdef')
        raw = base64.b64decode(ct)
        self.assertEqual(len(raw) % 16, 0)
        self.assertGreaterEqual(len(raw), 80)  # 64 随机前缀 + 9 密码 -> 73 字符 -> 补齐 80 字节

    def test_pycryptodome_crosscheck(self):
        try:
            from Crypto.Cipher import AES
            from Crypto.Util.Padding import pad
        except ImportError:
            self.skipTest('pycryptodome 未安装，跳过交叉验证')
        import random
        rnd = random.Random(42)
        for _ in range(8):
            key = bytes(rnd.randrange(256) for _ in range(16))
            iv = bytes(rnd.randrange(256) for _ in range(16))
            n = rnd.randrange(1, 200)
            pt = bytes(rnd.randrange(256) for _ in range(n))
            ref = AES.new(key, AES.MODE_CBC, iv).encrypt(pad(pt, 16))
            got = aes.encrypt_cbc(key, iv, pt)
            self.assertEqual(ref, got)


class TestConfig(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mooncheck-test-')
        self.path = os.path.join(self.tmp, 'config.json')
        os.environ['MOONCHECK_CONFIG'] = self.path

    def tearDown(self):
        os.environ.pop('MOONCHECK_CONFIG', None)

    def test_create_and_roundtrip(self):
        cfg = config.load()
        self.assertTrue(os.path.exists(self.path))
        cfg['student_id'] = '2023000000000'
        config.save(cfg)
        again = config.load()
        self.assertEqual(again['student_id'], '2023000000000')

    def test_unknown_keys_preserved(self):
        with open(self.path, 'w', encoding='utf-8') as f:
            json.dump({'note': 'hello', 'timeout': 30}, f)
        cfg = config.load()
        self.assertEqual(cfg.get('note'), 'hello')
        self.assertEqual(cfg['timeout'], 30)

    def test_set_value_coercion(self):
        cfg = config.load()
        config.set_value(cfg, 'location.latitude', '30.123')
        self.assertIsInstance(cfg['location']['latitude'], float)
        config.set_value(cfg, 'timeout', '45')
        self.assertEqual(cfg['timeout'], 45)
        with self.assertRaises(ValueError):
            config.set_value(cfg, 'no.such.key', 'x')

    def test_validate_rules(self):
        cfg = config.default_config()
        errs = [p for p in config.validate(cfg) if p[0] == 'error']
        self.assertTrue(errs)  # 空学号密码必须报错
        cfg['student_id'] = '2023000000000'
        cfg['password'] = 'x'
        cfg['browser_fingerprint'] = '0123456789ABCDEF0123456789ABCDEF'
        errs = [p for p in config.validate(cfg) if p[0] == 'error']
        self.assertEqual(errs, [])
        warns = [p for p in config.validate(cfg) if p[0] == 'warn']
        self.assertTrue(any(w[1] == 'location' for w in warns))  # 示例坐标提醒


class TestPayload(unittest.TestCase):
    def test_fields(self):
        api = checkin_api.CheckinApi(None, 294)
        loc = config.DEFAULTS['location']
        p = api.build_payload(loc, 'UA/1.0')
        for k in ('latitude', 'longitude', 'reverseGeocode', 'clientTime', 'userAgent',
                  'coordinateSystemCode', 'accuracy', 'locationDurationMs',
                  'networkTypeCode', 'locationEnabled', 'locationAuthorized',
                  'locationAccuracy', 'errorMessage'):
            self.assertIn(k, p)
        self.assertEqual(p['coordinateSystemCode'], 'gcj02')
        self.assertIs(p['locationEnabled'], False)
        self.assertRegex(p['clientTime'], r'^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$')

    def test_endpoints(self):
        api = checkin_api.CheckinApi(None, 294)
        self.assertIn('/daily-checkin/checkLogin?appId=294', api.check_login)
        self.assertTrue(api.ep_checkin.startswith('https://jzsz.uestc.edu.cn'))


class TestCliOffline(unittest.TestCase):
    """通过子进程跑真实入口，验证 exe/源码一致的 CLI 行为。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mooncheck-cli-')
        self.cfgpath = os.path.join(self.tmp, 'config.json')

    def _run(self, args):
        env = dict(os.environ)
        env['MOONCHECK_CONFIG'] = self.cfgpath
        proc = subprocess.run(
            [sys.executable, os.path.join(os.path.dirname(HERE), 'mooncheck.py')] + args,
            capture_output=True, text=True, encoding='utf-8', errors='replace',
            env=env, timeout=60)
        return proc

    def test_selftest_exit(self):
        proc = self._run(['selftest', '-v'])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)  # 默认配置缺学号->warn
        self.assertIn('AES 加密自检', proc.stdout)

    def test_config_init_and_set(self):
        proc = self._run(['config', 'init'])
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertTrue(os.path.exists(self.cfgpath))
        proc = self._run(['config', 'set', 'student_id', '2023000000000'])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        with open(self.cfgpath, encoding='utf-8') as f:
            self.assertEqual(json.load(f)['student_id'], '2023000000000')

    def test_checkin_dry_run_offline(self):
        self._run(['config', 'init'])
        proc = self._run(['checkin', '--dry-run'])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('latitude', proc.stdout)

    def test_help(self):
        proc = self._run(['help'])
        self.assertEqual(proc.returncode, 0)
        self.assertIn('config', proc.stdout)


class TestMultiAccountConfig(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mooncheck-mc-')
        self.path = os.path.join(self.tmp, 'config.json')
        os.environ['MOONCHECK_CONFIG'] = self.path

    def tearDown(self):
        os.environ.pop('MOONCHECK_CONFIG', None)

    def test_effective_accounts_single_mode(self):
        cfg = config.default_config()
        cfg['student_id'] = '2023000000000'
        cfg['password'] = 'x'
        accts = config.effective_accounts(cfg)
        self.assertEqual(len(accts), 1)
        self.assertEqual(accts[0]['label'], 'self')
        self.assertTrue(accts[0]['_virtual'])
        config.save_account_session(cfg, accts[0], 'SES')
        with open(self.path, encoding='utf-8') as f:
            self.assertEqual(json.load(f)['session_cookie'], 'SES')

    def test_accounts_validate_and_location_override(self):
        cfg = config.default_config()
        cfg['accounts'] = [
            dict(config.ACCOUNT_TEMPLATE, label='A01', student_id='2023000000001',
                 password='p1', browser_fingerprint='0123456789ABCDEF0123456789ABCDEF'),
            dict(config.ACCOUNT_TEMPLATE, label='A01', student_id='x', password='p'),
        ]
        errs = [p for p in config.validate(cfg) if p[0] == 'error']
        self.assertTrue(any('重复' in p[2] for p in errs))
        cfg['accounts'][1]['label'] = 'A02'
        warns = [p for p in config.validate(cfg) if p[0] == 'warn']
        self.assertTrue(any('指纹' in p[2] for p in warns))  # A02 缺指纹=提醒
        cfg['accounts'][1]['browser_fingerprint'] = '0123456789ABCDEF0123456789ABCDEF'
        cfg['accounts'][1]['location'] = dict(config.LOCATION_DEFAULTS,
                                              latitude=30.1, longitude=103.2)
        errs = [p for p in config.validate(cfg) if p[0] == 'error']
        self.assertEqual(errs, [])
        loc = config.account_location(cfg, cfg['accounts'][1])
        self.assertEqual(loc['latitude'], 30.1)
        loc2 = config.account_location(cfg, cfg['accounts'][0])
        self.assertEqual(loc2['latitude'], config.EXAMPLE_LAT)  # 沿用顶层
        # 会话/结果按账号写回
        config.save(cfg)
        cfg2 = config.load()
        real = config.find_account(cfg2, 'A01')
        config.save_account_session(cfg2, real, 'SES-A01')
        cfg3 = config.load()
        self.assertEqual(cfg3['accounts'][0]['session_cookie'], 'SES-A01')

    def test_channel_matching_rules(self):
        cfg = config.default_config()
        cfg['accounts'] = [dict(config.ACCOUNT_TEMPLATE, label='A01', student_id='s',
                                password='p',
                                browser_fingerprint='0123456789ABCDEF0123456789ABCDEF')]
        cfg['notify'] = dict(config.DEFAULTS['notify'], on=True, url='https://x/send')
        cfg['notify']['channels'] = [
            dict(config.CHANNEL_TEMPLATE, label='mine', url='https://a.send',
                 accounts=['A01']),
            dict(config.CHANNEL_TEMPLATE, label='all', url='https://b.send',
                 accounts=['all']),
        ]
        chs = config.iter_channels(cfg)
        self.assertEqual(len(chs), 3)
        self.assertTrue(config.channel_matches(chs[1], 'A01'))
        self.assertFalse(config.channel_matches(chs[1], 'A02'))
        self.assertFalse(config.channel_matches(chs[1], ''))  # 全局事件不收
        self.assertTrue(config.channel_matches(chs[2], 'A99'))
        self.assertTrue(config.channel_matches(chs[2], ''))
        errs = [p for p in config.validate(cfg) if p[0] == 'error']
        self.assertEqual(errs, [])
        # 关联不存在的账号 -> warn
        cfg['notify']['channels'].append(
            dict(config.CHANNEL_TEMPLATE, label='bad', url='https://c.send',
                 accounts=['A99']))
        warns = [p for p in config.validate(cfg) if p[0] == 'warn']
        self.assertTrue(any('不存在' in p[2] for p in warns))

    def test_window_helper(self):
        cfg = config.default_config()
        import datetime
        noon = datetime.datetime(2026, 1, 1, 12, 0)
        self.assertFalse(config.in_window(cfg, noon))       # 22:00-23:30
        night = datetime.datetime(2026, 1, 1, 22, 15)
        self.assertTrue(config.in_window(cfg, night))
        edge = datetime.datetime(2026, 1, 1, 23, 30)
        self.assertTrue(config.in_window(cfg, edge))


class TestPushFanOut(unittest.TestCase):
    """离线 fake HTTP：验证渲染 fan-out 与默认/额外通道语义。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mooncheck-push-')
        os.environ['MOONCHECK_CONFIG'] = os.path.join(self.tmp, 'config.json')

    def tearDown(self):
        os.environ.pop('MOONCHECK_CONFIG', None)

    def _cfg(self):
        cfg = config.default_config()
        cfg['notify'] = dict(config.DEFAULTS['notify'], on=True,
                             url='https://sctapi.ftqq.com/DEF.send')
        cfg['notify']['channels'] = [
            dict(config.CHANNEL_TEMPLATE, label='a01', url='https://sctapi.ftqq.com/A1.send',
                 accounts=['A01']),
        ]
        return cfg

    def test_fan_out_routing(self):
        from mooncheck_app import push as push_mod
        sent = []

        class FakeResp(object):
            status = 200
            text = '{"code":0}'

            def json(self):
                return {'code': 0}

        class FakeClient(object):
            def __init__(self, **kw):
                pass

            def post(self, url, body, headers=None):
                sent.append(('POST', url, body))
                return FakeResp()

            def get(self, url, headers=None):
                sent.append(('GET', url, None))
                return FakeResp()

        orig = push_mod.http_client.HttpClient
        push_mod.http_client.HttpClient = FakeClient
        try:
            cfg = self._cfg()
            # 多人语义：逐账号事件 skip_default，汇总 only_default
            push_mod.fan_out(cfg, [('A01', '打卡成功', 'A01：好'),
                                   ('A02', '打卡失败', 'A02：坏')], skip_default=True)
            push_mod.fan_out(cfg, [('', '今晚汇总', '1/2')], only_default=True)
        finally:
            push_mod.http_client.HttpClient = orig
        urls = [s[1] for s in sent]
        self.assertEqual(len(sent), 2)  # A01 事件->a01 通道；A02 不匹配被过滤；汇总->默认
        self.assertIn('A1.send', urls[0])
        self.assertIn('DEF.send', urls[1])
        self.assertIn('今晚汇总', sent[1][2])

    def test_serverchan_desp_field(self):
        from mooncheck_app import push as push_mod
        method, url, body, ctype = push_mod.render(
            'https://sctapi.ftqq.com/KEY.send', 2, '标题', '正文')
        self.assertEqual(method, 'POST')
        doc = json.loads(body)
        self.assertEqual(doc['desp'], '正文')
        self.assertNotIn('body', doc)
        method, url, body, ctype = push_mod.render(
            'https://ntfy.sh/topic', 2, '标题', '正文')
        self.assertEqual(json.loads(body)['body'], '正文')


class TestWindowGate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mooncheck-gate-')
        os.environ['MOONCHECK_CONFIG'] = os.path.join(self.tmp, 'config.json')

    def tearDown(self):
        os.environ.pop('MOONCHECK_CONFIG', None)

    def test_checkin_and_auto_gated(self):
        from mooncheck_app import service as svc
        cfg = config.default_config()
        cfg['student_id'] = '2023000000000'
        cfg['password'] = 'x'
        cfg['browser_fingerprint'] = '0123456789ABCDEF0123456789ABCDEF'
        orig = config.in_window
        config.in_window = lambda c, now=None: False
        try:
            res = svc.do_checkin(cfg, lambda m: None)
            self.assertEqual(res.kind, 'warn')
            self.assertIn('--force', res.text)
            res2 = svc.do_auto(cfg, lambda m: None)
            self.assertEqual(res2.kind, 'warn')
            self.assertIn('跳过', res2.text)
        finally:
            config.in_window = orig

    def test_dry_run_bypasses_gate(self):
        from mooncheck_app import service as svc
        cfg = config.default_config()
        orig = config.in_window
        config.in_window = lambda c, now=None: False
        try:
            res = svc.do_checkin(cfg, lambda m: None, dry_run=True)
            self.assertEqual(res.kind, 'ok')
        finally:
            config.in_window = orig


class TestCliMultiOffline(unittest.TestCase):
    """CLI 管理层（account/notify/sched）离线可测。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mooncheck-cli-mc-')
        self.cfgpath = os.path.join(self.tmp, 'config.json')

    def _run(self, args):
        env = dict(os.environ)
        env['MOONCHECK_CONFIG'] = self.cfgpath
        return subprocess.run(
            [sys.executable, os.path.join(os.path.dirname(HERE), 'mooncheck.py')] + args,
            capture_output=True, text=True, encoding='utf-8', errors='replace',
            env=env, timeout=60)

    def test_account_notify_sched_flow(self):
        self.assertEqual(self._run(['config', 'init']).returncode, 0)
        fp = '0123456789ABCDEF0123456789ABCDEF'
        p = self._run(['account', 'add', 'A01', '2023000000001', 'pw1', fp])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        p = self._run(['account', 'add', 'A02', '2023000000002', 'pw2', fp,
                       'lat=30.5', 'lng=103.5', 'addr=测试宿舍'])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        p = self._run(['account', 'list'])
        self.assertIn('A01', p.stdout)
        self.assertIn('A02', p.stdout)
        p = self._run(['account', 'off', 'A02'])
        self.assertEqual(p.returncode, 0, p.stdout)
        with open(self.cfgpath, encoding='utf-8') as f:
            cfg = json.load(f)
        self.assertFalse(cfg['accounts'][1]['enabled'])
        self.assertEqual(cfg['accounts'][1]['location']['latitude'], 30.5)

        p = self._run(['notify', 'set', 'url=https://sctapi.ftqq.com/AAA.send', 'style=2',
                       'on=true'])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        p = self._run(['notify', 'add', 'a02push',
                       'url=https://sctapi.ftqq.com/BBB.send', 'accounts=A02'])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        p = self._run(['notify', 'list'])
        self.assertIn('a02push', p.stdout)
        with open(self.cfgpath, encoding='utf-8') as f:
            cfg = json.load(f)
        self.assertEqual(cfg['notify']['channels'][0]['accounts'], ['A02'])

        p = self._run(['sched', 'set', '00:00', '00:01'])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        # 不在 00:00-00:01 窗口内跑 auto 应当被拦下（warn=1），不联网
        import datetime
        inside = datetime.datetime.now().strftime('%H:%M') in ('00:00', '00:01')
        p = self._run(['auto'])
        if not inside:
            self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
            self.assertIn('不在打卡窗口', p.stdout)
        p = self._run(['notify', 'link', 'a02push', 'all'])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        p = self._run(['account', 'rm', 'A02'])
        self.assertEqual(p.returncode, 0, p.stdout)

    def test_selftest_reports_accounts_and_window(self):
        self._run(['config', 'init'])
        p = self._run(['selftest'])
        self.assertIn('打卡窗口', p.stdout)
        self.assertIn('推送', p.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
