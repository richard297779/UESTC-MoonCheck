# -*- coding: utf-8 -*-
"""晚安成电打卡接口封装（EP05–EP10）。

端点与请求头来自实测抓包（见 dev/docs/api-contract.md）。
与固件一致：登录后必须先门户预热（GET /api/isLoginFailure）再进微应用，
否则 checkLogin 会 302 回首页、接口报 4001。
"""
import datetime

BASE = 'https://jzsz.uestc.edu.cn'
PORTAL_WARMUP = BASE + '/api/isLoginFailure'
DEFAULT_APP_ID = 294

# 只允许展示的安全字段（响应含姓名/手机号，禁止整包输出）
SAFE_FIELDS = (
    'businessDate', 'recordStatusCode', 'checkinTime', 'distanceMeter', 'abnormalFlag',
    'checkinLocationName', 'checkinLocationLatitude', 'checkinLocationLongitude',
    'checkinRadiusMeter',
)


class CheckinApi(object):
    def __init__(self, client, app_id=DEFAULT_APP_ID):
        self.client = client
        self.app_id = int(app_id)
        self.check_login = '%s/daily-checkin/checkLogin?appId=%d' % (BASE, self.app_id)
        self.h5_url = '%s/daily-checkin-h5/index.html?appId=%d' % (BASE, self.app_id)
        self.ep_today = BASE + '/daily-checkin/api/student-checkin-record/today'
        self.ep_checkin = BASE + '/daily-checkin/api/student-checkin-record/checkin'
        self.ep_locations = BASE + '/daily-checkin/api/checkin-location/available-locations'
        self.ep_schedule = BASE + '/daily-checkin/api/checkin-schedule/effective'
        self.ep_reverse = BASE + '/daily-checkin/api/map/reverse'

    @property
    def h5_headers(self):
        return {
            'Platform': 'h5',
            'Accept': '*/*',
            'Origin': BASE,
            'Referer': self.h5_url,
            'Sec-Fetch-Site': 'same-origin',
            'Sec-Fetch-Mode': 'cors',
            'Sec-Fetch-Dest': 'empty',
        }

    def warmup_portal(self):
        """门户预热：解锁「微应用授权」会话属性。已登录时返回 204。"""
        return self.client.request('GET', PORTAL_WARMUP, headers={
            'Accept': '*/*',
            'Referer': BASE + '/',
        })

    def enter_app(self):
        """EP05：进入微应用（302 -> H5 页）。"""
        return self.client.follow(self.check_login, headers={'Accept': 'text/html'})

    def today(self):
        """EP06：查询当日打卡状态。"""
        return self.client.request('GET', self.ep_today, headers=self.h5_headers)

    def locations(self):
        """EP08：可用打卡点与半径。"""
        return self.client.request('GET', self.ep_locations, headers=self.h5_headers)

    def schedule(self):
        """EP07：有效时段（分钟偏移，自 00:00 起）。"""
        return self.client.request('GET', self.ep_schedule, headers=self.h5_headers)

    def reverse_geocode(self, lat, lng):
        """EP09：逆地理编码（用于校准地址）。"""
        return self.client.request('GET', '%s?lat=%s&lng=%s' % (self.ep_reverse, lat, lng),
                                   headers=self.h5_headers)

    def submit(self, payload):
        """EP10：提交打卡。"""
        return self.client.request('POST', self.ep_checkin,
                                   headers=self.h5_headers, json_body=payload)

    def build_payload(self, loc, user_agent, reverse_geocode=None, now=None):
        """按实测成功载荷逐字段构造（字段裁剪有产生异常记录的风险，不做裁剪）。"""
        now = now or datetime.datetime.now()
        return {
            'latitude': loc['latitude'],
            'longitude': loc['longitude'],
            'reverseGeocode': (loc.get('reverse_geocode', '')
                               if reverse_geocode is None else reverse_geocode),
            'clientTime': now.strftime('%Y-%m-%d %H:%M:%S'),
            'userAgent': user_agent,
            'coordinateSystemCode': loc.get('coordinate_system', 'gcj02'),
            'accuracy': loc.get('accuracy', 86),
            'locationDurationMs': loc.get('location_duration_ms', 1189),
            'networkTypeCode': loc.get('network_type', '4g'),
            'locationEnabled': False,
            'locationAuthorized': '',
            'locationAccuracy': '',
            'errorMessage': '',
        }


def safe_summary(data):
    if not isinstance(data, dict):
        return {}
    return dict((k, data.get(k)) for k in SAFE_FIELDS)


def hhmm(offset_minutes):
    try:
        v = int(offset_minutes)
    except (TypeError, ValueError):
        return None
    return '%02d:%02d' % (v // 60, v % 60)


def schedule_summary(data):
    if not isinstance(data, dict):
        return {}
    return {
        'scheduleName': data.get('scheduleName'),
        'checkinWindow': '%s - %s' % (hhmm(data.get('studentCheckinStartOffsetMinute')),
                                      hhmm(data.get('studentCheckinEndOffsetMinute'))),
        'effective': data.get('effective'),
    }


def locations_summary(data):
    out = []
    if isinstance(data, list):
        for x in data:
            if isinstance(x, dict):
                out.append({
                    'locationName': x.get('locationName'),
                    'latitude': x.get('latitude'),
                    'longitude': x.get('longitude'),
                    'radiusMeter': x.get('radiusMeter'),
                })
    return out
