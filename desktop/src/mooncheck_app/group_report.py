"""晚安成电网页端小组确认。所有请求使用当前账号的门户会话。"""
import re
from urllib.parse import urlencode

from .checkin_api import BASE
from .http_client import NetworkError

API = BASE + '/daily-checkin/api'
GROUP = API + '/group-situation'
REPORT = GROUP + '/report'
_PAGE_SIZE = 10
_MAX_PAGES = 20


class GroupReportError(Exception):
    def __init__(self, message, *, session_expired=False):
        super().__init__(message)
        self.session_expired = session_expired


def _response_json(response, *, is_identity_response=False):
    if response.status in (301, 302, 303, 307, 308, 401):
        raise GroupReportError('小组上报会话已失效', session_expired=True)
    if response.status != 200:
        raise GroupReportError('小组接口返回 HTTP %d' % response.status)
    try:
        data = response.json()
    except (ValueError, UnicodeError):
        raise GroupReportError('小组接口响应格式无效') from None
    if not isinstance(data, dict):
        raise GroupReportError('小组接口响应格式无效')
    code = data.get('code')
    if code in (4001, '4001'):
        raise GroupReportError('小组上报会话已失效', session_expired=True)
    message = data.get('msg')
    if (is_identity_response and type(code) is int and code == 5000 and data.get('ok') is False
            and 'data' in data and data['data'] is None and isinstance(message, str) and '未登录' in message):
        raise GroupReportError('小组上报会话已失效', session_expired=True)
    if type(code) is not int or code != 0 or data.get('ok') is not True:
        raise GroupReportError('平台拒绝小组上报请求')
    return data


def _can_report_normal(row, record):
    return (
        row.get('reportingWay') == 'GROUP'
        and row.get('displayStatus') == 'UNFILLED'
        and row.get('reportSubmitted') is False
        and 'safetyConfirmResult' in row and row['safetyConfirmResult'] is None
        and record.get('recordStatusCode') == 'PENDING'
        and record.get('groupReportingFlag') is True
        and record.get('pendingAbnormalFlag') is False
        and record.get('abnormalFlag') is False
        and record.get('instructorConfirmedFlag') is False
    )


class GroupReport:
    def __init__(self, api, owner):
        self.api = api
        self.owner = owner

    def _get(self, url, *, is_identity_response=False):
        response = self.api.client.request('GET', url, headers=self.api.h5_headers)
        return _response_json(response, is_identity_response=is_identity_response)

    def probe(self):
        """校验网页会话归属与小组上报角色，返回脱敏的待确认人数。"""
        self.api.warmup_portal()
        response = self.api.enter_app()
        if response.status != 200 or not response.url.startswith(BASE + '/daily-checkin-h5/'):
            response = self.api.client.request('GET', self.api.h5_url, headers={'Accept': 'text/html'})
            if response.status != 200:
                raise GroupReportError('当前会话未进入晚安成电网页', session_expired=True)
        envelope = self._get(API + '/auth/user-info', is_identity_response=True).get('data')
        user = envelope.get('userData') if isinstance(envelope, dict) else None
        if not isinstance(user, dict):
            raise GroupReportError('小组上报身份响应格式无效')
        if not self.owner or user.get('username') != self.owner:
            raise GroupReportError('网页会话与所配置账号不一致')
        if (user.get('userType') != 'SAFETY_OFFICER' or user.get('invalid') is not False
                or user.get('canGroupReport') is not True):
            raise GroupReportError('该账号没有安全员小组上报权限')
        return self.pending_count()

    def pending_count(self):
        count = self._get(GROUP + '/safety-officer/pending-confirmation-count').get('data')
        if not isinstance(count, str) or not re.fullmatch(r'[0-9]{1,6}', count):
            raise GroupReportError('待确认人数格式无效')
        return int(count)

    def pending(self, today):
        """返回当天完整待确认名单中的记录 ID 与可自动确认标记。"""
        records = {}
        page_index = 1
        total = None
        while True:
            query = urlencode({'pageIndex': page_index, 'pageSize': _PAGE_SIZE, 'confirmed': 'false'})
            envelope = self._get(GROUP + '/list?' + query)
            page = envelope.get('page')
            rows = envelope.get('data')
            if not isinstance(page, dict) or not isinstance(rows, list):
                raise GroupReportError('待确认名单响应格式无效')
            count = page.get('totalCount')
            if not isinstance(count, str) or not re.fullmatch(r'[0-9]{1,6}', count):
                raise GroupReportError('待确认名单总数格式无效')
            count = int(count)
            pages = (count + _PAGE_SIZE - 1) // _PAGE_SIZE
            if (pages > _MAX_PAGES or (total is not None and count != total)
                    or page.get('pageIndex') != page_index or page.get('pageSize') != _PAGE_SIZE
                    or page.get('totalPage') != pages or page.get('confirmed') is not False
                    or len(rows) != min(_PAGE_SIZE, count - len(records))):
                raise GroupReportError('待确认名单有变化或分页不完整，请复查')
            total = count
            for row in rows:
                record = row.get('record') if isinstance(row, dict) else None
                record_id = record.get('id') if isinstance(record, dict) else None
                if (not isinstance(record_id, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', record_id)
                        or record_id in records or record.get('businessDate') != today):
                    raise GroupReportError('待确认记录重复、格式无效或日期不符')
                records[record_id] = _can_report_normal(row, record)
            if page_index >= pages:
                return records
            page_index += 1

    def submit_normal(self, ids):
        """仅请求一次。由调用方重新读取待确认名单来核实结果。"""
        payload = [{'recordId': record_id, 'action': 'NORMAL'} for record_id in ids]
        response = self.api.client.request('POST', REPORT, headers=self.api.h5_headers, json_body=payload)
        if _response_json(response).get('data') is not True:
            raise GroupReportError('平台未确认批量提交成功')


def check_in(group, today, can_submit):
    """执行一轮已授权的小组确认，返回 (kind, text)。"""
    try:
        records = group.pending(today)
        if len(records) != group.pending_count():
            return 'warn', '待确认名单有变化，本轮未提交'
        if not records:
            return 'ok', '今日无待确认记录'
        eligible = [record_id for record_id, normal in records.items() if normal]
        if not eligible:
            return 'warn', '有 %d 条记录待人工处理' % len(records)
        if not can_submit():
            return 'warn', '时间或日期已变化，本轮未提交'
    except (GroupReportError, NetworkError) as error:
        return 'fail', str(error)

    try:
        group.submit_normal(eligible)
    except (GroupReportError, NetworkError) as error:
        return 'fail', '小组提交未获得成功回执，请查看网页结果。%s' % error
    try:
        remaining = group.pending(today)
        if any(record_id in remaining for record_id in eligible) or len(remaining) != group.pending_count():
            return 'warn', '批量提交已获回执，待确认名单尚未核实'
    except (GroupReportError, NetworkError):
        return 'warn', '批量提交已获回执，复查失败，请查看网页结果'
    if remaining:
        return 'warn', '已确认 %d 条，仍有 %d 条待处理' % (len(eligible), len(remaining))
    return 'ok', '已批量确认 %d 条小组记录，待确认人数为 0' % len(eligible)
