# -*- coding: utf-8 -*-
"""晚安成电打卡接口封装。

端点、请求头、载荷字段全部来自 2026-10-11 实测抓包：
见 docs/python-verification-plan.md §1.1 / §1.3 / §1.4。
"""
import datetime

BASE = 'https://jzsz.uestc.edu.cn'
APP_ID = 294

CHECK_LOGIN = '%s/daily-checkin/checkLogin?appId=%d' % (BASE, APP_ID)
H5_URL = '%s/daily-checkin-h5/index.html?appId=%d' % (BASE, APP_ID)

EP_TODAY = BASE + '/daily-checkin/api/student-checkin-record/today'
EP_CHECKIN = BASE + '/daily-checkin/api/student-checkin-record/checkin'
EP_LOCATIONS = BASE + '/daily-checkin/api/checkin-location/available-locations'
EP_SCHEDULE = BASE + '/daily-checkin/api/checkin-schedule/effective'
EP_REVERSE = BASE + '/daily-checkin/api/map/reverse'

# 微应用接口统一要求的头（缺 Platform/Referer 可能被拒）
H5_HEADERS = {
    'Platform': 'h5',
    'Accept': '*/*',
    'Origin': BASE,
    'Referer': H5_URL,
    'Sec-Fetch-Site': 'same-origin',
    'Sec-Fetch-Mode': 'cors',
    'Sec-Fetch-Dest': 'empty',
}

# 只允许打印的安全字段（响应体含姓名/手机号/辅导员联系方式，禁止整包输出）
SAFE_FIELDS = (
    'businessDate', 'recordStatusCode', 'checkinTime', 'distanceMeter', 'abnormalFlag',
    'checkinLocationName', 'checkinLocationLatitude', 'checkinLocationLongitude',
    'checkinRadiusMeter',
)


def build_payload(loc, user_agent, now=None, reverse_geocode=None):
    """按实测成功载荷逐字段构造（不做字段裁剪：裁剪有产生异常记录的风险）。"""
    now = now or datetime.datetime.now()
    return {
        'latitude': loc['latitude'],
        'longitude': loc['longitude'],
        'reverseGeocode': (loc.get('reverse_geocode', '') if reverse_geocode is None
                           else reverse_geocode),
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


def enter_app(client):
    """EP05：进入微应用（302 → H5 页，无一次性 token）。"""
    return client.follow(CHECK_LOGIN)


def today(client):
    """EP06：查询当日打卡状态。"""
    return client.request('GET', EP_TODAY, headers=H5_HEADERS)


def locations(client):
    """EP08：可用打卡点与半径（设备应动态读取，勿硬编码）。"""
    return client.request('GET', EP_LOCATIONS, headers=H5_HEADERS)


def schedule(client):
    """EP07：有效时段（分钟偏移，自 00:00 起）。"""
    return client.request('GET', EP_SCHEDULE, headers=H5_HEADERS)


def reverse_geocode(client, lat, lng):
    """EP09：逆地理编码。"""
    return client.request('GET', '%s?lat=%s&lng=%s' % (EP_REVERSE, lat, lng), headers=H5_HEADERS)


def submit(client, payload):
    """EP10：提交打卡。"""
    return client.request('POST', EP_CHECKIN, headers=H5_HEADERS, json_body=payload)


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
