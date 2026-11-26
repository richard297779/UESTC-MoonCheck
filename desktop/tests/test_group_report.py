"""Offline regression tests for the web safety-officer confirmation flow."""
import datetime
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from mooncheck_app import config, group_report, service
from mooncheck_app.checkin_api import BASE
from mooncheck_app.http_client import NetworkError, Resp

TODAY = '2026-01-02'
OWNER = 'fixture-officer'
NOW = datetime.datetime.fromisoformat(TODAY + 'T22:15:00+08:00')


def pending_row(number, *, date=TODAY, abnormal=False):
    return {
        'record': {
            'id': 'record-%d' % number, 'businessDate': date,
            'recordStatusCode': 'PENDING', 'groupReportingFlag': True,
            'pendingAbnormalFlag': abnormal, 'abnormalFlag': False,
            'instructorConfirmedFlag': False
        },
        'displayStatus': 'UNFILLED', 'reportingWay': 'GROUP',
        'reportSubmitted': False, 'safetyConfirmResult': None
    }


class FixturePortal:
    def __init__(self, rows, *, role='SAFETY_OFFICER', owner=OWNER):
        self.rows = rows
        self.role = role
        self.owner = owner
        self.posts = []
        self.pages = []
        self.fail_submit = False
        self.keep_pending = False
        self.inconsistent_count = False

    def follow(self, url, headers):
        if '/daily-checkin/checkLogin?' not in url:
            raise AssertionError(url)
        return Resp(200, {}, b'', BASE + '/daily-checkin-h5/index.html?appId=294')

    def request(self, method, url, headers, json_body=None):
        path = urlsplit(url).path
        result = {'ok': True, 'code': 0}
        if path != '/api/isLoginFailure':
            assert headers.get('Platform') == 'h5'
            assert 'X-Checkin-Ticket' not in headers
        if path == '/api/isLoginFailure':
            return Resp(200, {}, b'', url)
        if path.endswith('/auth/user-info'):
            result['data'] = {'userData': {'username': self.owner, 'userType': self.role,
                                           'invalid': False, 'canGroupReport': True}}
        elif path.endswith('/pending-confirmation-count'):
            result['data'] = str(len(self.rows) + int(self.inconsistent_count))
        elif path.endswith('/group-situation/list'):
            args = parse_qs(urlsplit(url).query)
            page = int(args['pageIndex'][0])
            assert args['confirmed'] == ['false'] and args['pageSize'] == ['10']
            self.pages.append(page)
            result.update(data=self.rows[(page - 1) * 10:page * 10], page={
                'pageIndex': page, 'pageSize': 10, 'confirmed': False,
                'totalCount': str(len(self.rows)), 'totalPage': (len(self.rows) + 9) // 10})
        elif path.endswith('/group-situation/report'):
            assert method == 'POST'
            self.posts.append(json_body)
            if self.fail_submit:
                raise NetworkError('fixture timeout')
            if not self.keep_pending:
                sent = {item['recordId'] for item in json_body}
                self.rows = [row for row in self.rows if row['record']['id'] not in sent]
            result['data'] = True
        else:
            raise AssertionError('Unexpected portal request: ' + path)
        return Resp(200, {}, json.dumps(result).encode(), url)


class GroupReportTests(unittest.TestCase):
    def setUp(self):
        self.cfg = config.default_config()
        self.cfg.update(student_id=OWNER, password='fixture-password')
        config.set_value(self.cfg, 'schedule.group_report', 'true')

    def run_auto(self, portal):
        with patch.object(service, 'build_client', return_value=portal), \
             patch.object(service, '_login_chain') as login, \
             patch.object(config, 'datetime_now', return_value=NOW), \
             patch.object(config, 'save_account_result') as saved, \
             patch.object(service.wechat, 'ensure_ticket') as ticket:
            result = service.do_auto(self.cfg, lambda _msg: None)
            login.assert_not_called()
            ticket.assert_not_called()
            if saved.called:
                self.assertEqual(result.kind == 'ok', saved.call_args.args[3])
            return result

    def test_scheduled_batch_success_and_rerun_reads_without_posting(self):
        portal = FixturePortal([pending_row(1), pending_row(2)])
        self.assertTrue(self.run_auto(portal).ok)
        self.assertEqual(portal.posts, [[{'recordId': 'record-1', 'action': 'NORMAL'},
                                         {'recordId': 'record-2', 'action': 'NORMAL'}]])
        self.assertEqual(portal.pages, [1, 1])
        self.assertTrue(self.run_auto(portal).ok)
        self.assertEqual(len(portal.posts), 1)

    def test_pagination_and_exception_exclusion(self):
        portal = FixturePortal([pending_row(i) for i in range(10)]
                               + [pending_row(10, abnormal=True), pending_row(11)])
        portal.rows[0]['reportSubmitted'] = True
        result = self.run_auto(portal)
        self.assertEqual(result.kind, 'warn')
        self.assertEqual(portal.pages, [1, 2, 1])
        self.assertEqual([row['recordId'] for row in portal.posts[0]],
                         ['record-%d' % i for i in list(range(1, 10)) + [11]])
        self.assertEqual(len(portal.rows), 2)
        self.assertEqual(self.run_auto(portal).kind, 'warn')
        self.assertEqual(len(portal.posts), 1)

    def test_duplicate_stale_or_incomplete_list_prevents_write(self):
        for rows, bad_count in (
            ([pending_row(1), pending_row(1)], False),
            ([pending_row(1, date='2026-01-01')], False),
            ([pending_row(1)], True)
        ):
            with self.subTest(rows=rows, bad_count=bad_count):
                portal = FixturePortal(rows)
                portal.inconsistent_count = bad_count
                self.assertFalse(self.run_auto(portal).ok)
                self.assertEqual(portal.posts, [])

    def test_wrong_role_or_identity_blocks_even_when_server_lists_records(self):
        for role, owner in (('STUDENT', OWNER), ('SAFETY_OFFICER', 'fixture-other')):
            portal = FixturePortal([pending_row(1)], role=role, owner=owner)
            self.assertEqual(self.run_auto(portal).kind, 'fail')
            self.assertEqual(portal.pages, [])
            self.assertEqual(portal.posts, [])

    def test_timeout_does_not_replay_and_ack_alone_is_insufficient(self):
        portal = FixturePortal([pending_row(1)])
        portal.fail_submit = True
        self.assertEqual(self.run_auto(portal).kind, 'fail')
        self.assertEqual(len(portal.posts), 1)
        portal.fail_submit = False
        portal.keep_pending = True
        self.assertEqual(self.run_auto(portal).kind, 'warn')
        self.assertEqual(len(portal.posts), 2)

    def test_window_closing_before_submission_blocks_post(self):
        portal = FixturePortal([pending_row(1)])
        with patch.object(service, 'build_client', return_value=portal), \
             patch.object(config, 'datetime_now', return_value=NOW), \
             patch.object(config, 'in_window', side_effect=[True, False]), \
             patch.object(config, 'save_account_result'):
            result = service.do_auto(self.cfg, lambda _msg: None)
        self.assertEqual(result.kind, 'warn')
        self.assertEqual(portal.posts, [])

    def test_status_and_dry_run_use_group_mode(self):
        portal = FixturePortal([pending_row(1)])
        with patch.object(service, 'build_client', return_value=portal):
            result = service.do_status(self.cfg, lambda _msg: None)
        self.assertTrue(result.ok)
        self.assertEqual(result.data, {'pending_count': 1})
        self.assertEqual(portal.posts, [])
        with patch.object(service, 'build_client') as client:
            result = service.do_checkin(self.cfg, lambda _msg: None, dry_run=True)
        client.assert_not_called()
        self.assertIn(('POST', group_report.REPORT), result.rows)
