# -*- coding: utf-8 -*-
"""config.json 读写与校验。

- 文件位置由 paths.config_path() 决定（exe 同目录）。
- 首次加载自动生成默认配置；未知键原样保留，方便手工加注释性字段。
- 保存走「临时文件 + 原子替换」，Linux/macOS 下权限收紧到 0600。
- 敏感字段始终打码，兼容参数 --all 也不会显示原文。
- 对齐固件 0.4.0 语义：
    schedule        打卡时间（到点窗口内才允许 auto/checkin，--force 可越窗）
    accounts        多账号表（≤6，与固件一致；为空=用顶层凭证的单人模式）
    notify          推送配置：默认通道 + 额外通道（≤5），通道按 accounts 关联
                    逐账号结果，"all" 表示也收全局事件（摘要/汇总/告警）
"""
import copy
import json
import os
import re
import stat
import tempfile

from . import paths

DEFAULT_USER_AGENT = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
                      '(KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36')

# 示例坐标 = 平台公布的打卡点中心（公开数据，非任何人的宿舍）
EXAMPLE_LAT = 30.75498
EXAMPLE_LNG = 103.933883

KMAX = 294  # 微应用 appId

LOCATION_DEFAULTS = {
    'latitude': EXAMPLE_LAT,
    'longitude': EXAMPLE_LNG,
    'reverse_geocode': '四川省成都市郫都区',
    'coordinate_system': 'gcj02',
    'accuracy': 86,
    'location_duration_ms': 1189,
    'network_type': '4g',
}

ACCOUNT_TEMPLATE = {
    'label': '',
    'wechat': {},
    'enabled': True,
    'student_id': '',
    'password': '',
    'browser_fingerprint': '',
    # location 留空 = 沿用顶层 location（同一宿舍的几个人只需各填凭证）
    'location': None,
    'session_cookie': '',
    'session_saved_at': '',
    'last_date': '',
    'last_ok': False,
    'last_msg': '',
}

CHANNEL_TEMPLATE = {
    'label': '',
    'on': True,
    # style 与固件一致：0=GET 模板 1=POST 纯文本 2=POST JSON（Server酱自动用 desp）
    'style': 2,
    'url': '',
    # 关联账号标签列表；["all"] 或不填 = 收所有事件
    'accounts': [],
}

DEFAULTS = {
    'wechat': {},
    'config_schema': 2,    'student_id': '',
    'password': '',
    'browser_fingerprint': '',
    'session_cookie': '',
    'session_saved_at': '',
    'timeout': 20,
    'user_agent': '',
    'location': dict(LOCATION_DEFAULTS),
    'schedule': {
        'checkin_at': '22:00',
        'window_end': '23:30',
    },
    'accounts': [],
    'notify': {
        'on': False,
        'url': '',
        'style': 2,
        'gap': 60,
        'quota': 40,
        'channels': [],
    },
}

# CLI/GUI 可直接编辑的字段（点号路径 -> 说明），类型取自 DEFAULTS 的默认值
EDITABLE = {
    'student_id': '学号（顶层=单人模式或账号表的默认凭证）',
    'password': '密码（不传值则隐藏输入）',
    'browser_fingerprint': '浏览器指纹（32 位 HEX，来自被信任的浏览器）',
    'session_cookie': '会话 Cookie（一般由 login 自动维护，可留空）',
    'timeout': 'HTTP 超时秒数',
    'location.latitude': '纬度',
    'location.longitude': '经度',
    'location.reverse_geocode': '地址文字（随经纬度一起提交）',
    'location.coordinate_system': '坐标系（默认 gcj02）',
    'location.accuracy': '定位精度（米，仅作为载荷字段）',
    'location.location_duration_ms': '定位耗时（毫秒，仅作为载荷字段）',
    'location.network_type': '网络类型（载荷字段，如 4g/wifi）',
    'schedule.checkin_at': '打卡开始时间 HH:MM（默认 22:00）',
    'schedule.window_end': '打卡截止时间 HH:MM（默认 23:30）',
    'notify.on': '推送开关（true/false）',
    'notify.url': '默认推送地址（Server酱 https://sctapi.ftqq.com/<SENDKEY>.send）',
    'notify.style': '推送方式 0=GET 模板 1=POST 文本 2=POST JSON',
    'notify.gap': '同类事件去重间隔（仅提示用；桌面单次运行不受限）',
    'notify.quota': '每日条数上限（0=不限）',
}

_HEX32 = re.compile(r'^[0-9A-Fa-f]{32}$')
_PLACEHOLDER = re.compile(r'^PUT_|^20\d{2}X+$')
_LABEL = re.compile(r'^[A-Za-z0-9_-]{1,12}$')
_HHMM = re.compile(r'^([01]?\d|2[0-3]):[0-5]\d$')

MAX_ACCOUNTS = 6
MAX_CHANNELS = 5


def default_config():
    return copy.deepcopy(DEFAULTS)


def load(create=True):
    """读取配置；缺失时按默认值生成并落盘。未知键保留，缺失键用默认值补齐。"""
    path = paths.config_path()
    if not os.path.exists(path):
        cfg = default_config()
        if create:
            save(cfg)
        return cfg
    with open(path, 'r', encoding='utf-8') as f:
        loaded = json.load(f)
    cfg = _merge(default_config(), loaded if isinstance(loaded, dict) else {})
    cfg['config_schema'] = 2
    # accounts/notify.channels 是数组：_merge 里 extra 直接覆盖，元素补齐模板键
    cfg['accounts'] = [_merge(copy.deepcopy(ACCOUNT_TEMPLATE), a)
                       for a in cfg.get('accounts') or [] if isinstance(a, dict)]
    notify = cfg.setdefault('notify', {})
    notify['channels'] = [_merge(copy.deepcopy(CHANNEL_TEMPLATE), c)
                          for c in notify.get('channels') or [] if isinstance(c, dict)]
    return cfg


def save(cfg):
    """原子写入；POSIX 下权限 0600。返回路径。"""
    path = paths.config_path()
    directory = os.path.dirname(path) or '.'
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix='.config-', suffix='.tmp')
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
            f.write('\n')
        if os.name != 'nt':
            os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return path


def _merge(base, extra):
    out = copy.deepcopy(base)
    for key, value in extra.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _merge(out[key], value)
        else:
            out[key] = value
    return out


def get(cfg, dotted):
    node = cfg
    for part in dotted.split('.'):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def set_value(cfg, dotted, raw):
    """按点号路径写入。raw 为 None 时保持原类型（bool 会失败，需用命令行传值）。
    返回 (旧值, 新值)。"""
    default = _default_for(dotted)
    value = _coerce(raw, default)
    parts = dotted.split('.')
    node = cfg
    for part in parts[:-1]:
        node = node.setdefault(part, {})
        if not isinstance(node, dict):
            raise ValueError('路径 %s 不是对象' % '.'.join(parts[:-1]))
    old = node.get(parts[-1])
    if dotted == 'student_id': reset_identity(node,value)
    node[parts[-1]] = value
    return old, value


def _default_for(dotted):
    if dotted in EDITABLE:
        node = DEFAULTS
        for part in dotted.split('.'):
            node = node[part]
        return node
    raise ValueError('未知字段：%s（可用字段：%s）' % (dotted, ', '.join(sorted(EDITABLE))))


def _coerce(raw, default):
    if raw is None:
        if isinstance(default, bool):
            raise ValueError('该字段需要显式传值（true/false）')
        return default
    text = str(raw).strip()
    if isinstance(default, bool):
        if text.lower() in ('1', 'true', 'yes', 'y', 'on', '开'):
            return True
        if text.lower() in ('0', 'false', 'no', 'n', 'off', '关'):
            return False
        raise ValueError('布尔字段需要 true/false，收到 %r' % raw)
    if isinstance(default, int) and not isinstance(default, bool):
        return int(float(text))
    if isinstance(default, float):
        return float(text)
    return text


# ------------------------------------------------------- 账号表辅助

def account_labels(cfg):
    return [str(a.get('label') or '') for a in cfg.get('accounts') or []]


def find_account(cfg, label):
    for a in cfg.get('accounts') or []:
        if str(a.get('label') or '') == label:
            return a
    return None


def effective_accounts(cfg, only_enabled=True):
    """要打卡的账号列表（dict 引用，可直接改 session/last_* 再 save）。
    账号表为空 = 单人模式：合成一个 label='self' 的视图，凭证取顶层。"""
    accounts = [a for a in cfg.get('accounts') or []
                if (not only_enabled) or a.get('enabled', True)]
    if not (cfg.get('accounts') or []):
        return [{
            'label': 'self',
            'enabled': True,
            'student_id': cfg.get('student_id', ''),
            'password': cfg.get('password', ''),
            'browser_fingerprint': cfg.get('browser_fingerprint', ''),
            'location': None,
            'session_cookie': cfg.get('session_cookie', ''),
            'session_saved_at': cfg.get('session_saved_at', ''),
            'last_date': '',
            'last_ok': False,
            'last_msg': '',
            'wechat': cfg.get('wechat') or {},
            '_virtual': True,   # 不落盘，写回时落到顶层键
        }]
    return accounts


def account_location(cfg, account):
    """账号坐标：自带优先，否则沿用顶层 location。"""
    loc = account.get('location')
    if isinstance(loc, dict) and 'latitude' in loc and 'longitude' in loc:
        merged = dict(LOCATION_DEFAULTS)
        merged.update(loc)
        return merged
    return dict(cfg.get('location') or LOCATION_DEFAULTS)


def reset_identity(account, student_id):
    if str(account.get('student_id') or '') != str(student_id or ''):
        account.update(wechat={},session_cookie='',session_saved_at='',last_date='',last_ok=False,last_msg='')


def save_account_wechat(cfg, account, credentials):
    target = cfg if account.get('_virtual') else account
    target['wechat'] = dict(credentials)
    account['wechat'] = target['wechat']
    save(cfg)


def redact_copy(value):
    """Export recursively; credentials/URLs cannot escape through new fields."""
    if isinstance(value, dict):
        from .privacy import SECRET_KEYS
        sensitive = SECRET_KEYS
        return {key: ('' if key.lower() in sensitive else redact_copy(item)) for key,item in value.items()}
    if isinstance(value, list): return [redact_copy(item) for item in value]
    return value


def save_account_session(cfg, account, session):
    """把新会话写回账号（虚拟 self 账号写回顶层），并落盘。"""
    now = _now_iso()
    if account.get('_virtual'):
        cfg['session_cookie'] = session
        cfg['session_saved_at'] = now
    else:
        account['session_cookie'] = session
        account['session_saved_at'] = now
    save(cfg)


def save_account_result(cfg, account, date, ok, msg):
    if account.get('_virtual'):
        # 单人模式没有 per-account 记录位：结果只进本次运行输出，不落盘也行，
        # 但为了 status 展示，借用顶层（新增两个键，未知键会被保留）
        cfg['last_date'] = date
        cfg['last_ok'] = ok
        cfg['last_msg'] = msg
    else:
        account['last_date'] = date
        account['last_ok'] = ok
        account['last_msg'] = msg
    save(cfg)


def account_credentials_set(account):
    sid = str(account.get('student_id') or '')
    pwd = str(account.get('password') or '')
    return bool(sid) and bool(pwd) and not pwd.startswith('PUT_') and not _PLACEHOLDER.match(sid)


# ------------------------------------------------------- 通道辅助

def notify_conf(cfg):
    return cfg.setdefault('notify', {})


def default_channel(cfg):
    """顶层 notify 合成默认通道（与固件通道 0 同语义：恒收所有事件）。"""
    n = notify_conf(cfg)
    return {'label': '', 'on': bool(n.get('on')) and bool(n.get('url')),
            'style': int(n.get('style', 2)), 'url': n.get('url') or '',
            'accounts': ['all']}


def iter_channels(cfg):
    """[默认通道, *额外通道]，元素 = {label,on,style,url,accounts(list)}。"""
    out = [default_channel(cfg)]
    for c in notify_conf(cfg).get('channels') or []:
        out.append(c)
    return out


def channel_matches(channel, account_label):
    """与固件 notifyChannelMatchesAccount 同规则：
    accounts 含 'all' 或空 -> 收所有事件；否则只收列出的账号事件。"""
    accounts = channel.get('accounts') or []
    if not accounts or 'all' in accounts:
        return True
    return account_label in accounts


def in_window(cfg, now=None):
    """当前时间是否落在打卡窗口 [checkin_at, window_end]（跨午夜按不跨处理：
    打卡场景 22:00–23:30 不跨天）。"""
    now = now or datetime_now()
    sched = cfg.get('schedule') or {}
    start = _minutes(str(sched.get('checkin_at') or '22:00'))
    end = _minutes(str(sched.get('window_end') or '23:30'))
    cur = now.hour * 60 + now.minute
    return start <= cur <= end


def datetime_now():
    import datetime
    return datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))


def _now_iso():
    return datetime_now().isoformat(timespec='seconds')


def _minutes(hhmm):
    if not _HHMM.match(hhmm):
        return -1
    h, m = hhmm.split(':')
    return int(h) * 60 + int(m)


# ------------------------------------------------------------------ 校验

def validate(cfg):
    """返回 [(级别, 字段, 说明)]，级别 in ('error', 'warn', 'info')。"""
    out = []
    accounts = cfg.get('accounts') or []
    if accounts:
        labels = set()
        for i, a in enumerate(accounts):
            where = 'accounts[%d]' % i
            label = str(a.get('label') or '')
            if not _LABEL.match(label):
                out.append(('error', where + '.label', '标签只允许 A-Za-z0-9-_，≤12 字符'))
            elif label in labels:
                out.append(('error', where + '.label', '标签重复：%s' % label))
            labels.add(label)
            if a.get('enabled', True) and not account_credentials_set(a):
                out.append(('error', label or where, '账号已启用但学号/密码未填（或仍是占位符）'))
            bfp = str(a.get('browser_fingerprint') or '')
            if a.get('enabled', True) and not bfp:
                out.append(('warn', label or where,
                            '该账号没有指纹：登录时可能要求验证码'))
            elif bfp and not _HEX32.match(bfp):
                out.append(('error', label + '.browser_fingerprint', '指纹应为 32 位十六进制'))
        enabled = [a for a in accounts if a.get('enabled', True)]
        if not enabled:
            out.append(('warn', 'accounts', '所有账号都被禁用，今晚不会打卡'))
    else:
        sid = str(get(cfg, 'student_id') or '')
        if not sid:
            out.append(('error', 'student_id', '学号未填写（多人可用 account add）'))
        elif _PLACEHOLDER.match(sid):
            out.append(('error', 'student_id', '学号仍是占位符'))
        if not str(get(cfg, 'password') or ''):
            out.append(('error', 'password', '密码未填写'))
        bfp = str(get(cfg, 'browser_fingerprint') or '')
        if not bfp:
            out.append(('warn', 'browser_fingerprint',
                        '未填指纹：无法免二次验证（可先 login 一次看服务端判定）'))
        elif not _HEX32.match(bfp):
            out.append(('error', 'browser_fingerprint', '指纹应为 32 位十六进制字符'))

    # 坐标：顶层必填合法；账号自带的也要合法
    def check_loc(where, loc):
        try:
            lat = float(loc.get('latitude'))
            lng = float(loc.get('longitude'))
        except (TypeError, ValueError, AttributeError):
            out.append(('error', where, '经纬度不是数字'))
            return
        if not (-90.0 <= lat <= 90.0 and -180.0 <= lng <= 180.0):
            out.append(('error', where, '经纬度超出合法范围'))
        elif abs(lat - EXAMPLE_LAT) < 1e-6 and abs(lng - EXAMPLE_LNG) < 1e-6:
            out.append(('warn', where,
                        '仍是示例坐标（打卡点中心）。换成你自己宿舍的实测坐标更稳妥'))

    check_loc('location', cfg.get('location') or {})
    for a in accounts:
        if isinstance(a.get('location'), dict):
            check_loc('%s.location' % (a.get('label') or '?'), a['location'])

    timeout = get(cfg, 'timeout')
    if not isinstance(timeout, int) or timeout < 3 or timeout > 120:
        out.append(('warn', 'timeout', '建议在 3–120 秒之间'))

    sched = cfg.get('schedule') or {}
    start = _minutes(str(sched.get('checkin_at') or ''))
    end = _minutes(str(sched.get('window_end') or ''))
    if start < 0:
        out.append(('error', 'schedule.checkin_at', '时间格式应为 HH:MM'))
    if end < 0:
        out.append(('error', 'schedule.window_end', '时间格式应为 HH:MM'))
    if start >= 0 and end >= 0 and start > end:
        out.append(('error', 'schedule', 'checkin_at 不能晚于 window_end'))

    n = cfg.get('notify') or {}
    if n.get('on') and not (n.get('url') or ''):
        out.append(('error', 'notify.url', '推送已开启但未填地址'))
    if (n.get('url') or '') and not str(n.get('url')).startswith('http'):
        out.append(('error', 'notify.url', '推送地址需要以 http(s):// 开头'))
    channels = n.get('channels') or []
    if len(channels) > MAX_CHANNELS:
        out.append(('error', 'notify.channels', '额外通道最多 %d 条' % MAX_CHANNELS))
    ch_labels = set()
    for c in channels:
        label = str(c.get('label') or '')
        if not _LABEL.match(label):
            out.append(('error', 'notify.channels', '通道标签不合法：%r（A-Za-z0-9-_，≤12）' % label))
        elif label in ch_labels:
            out.append(('error', 'notify.channels', '通道标签重复：%s' % label))
        ch_labels.add(label)
        if not (c.get('url') or '').startswith('http'):
            out.append(('error', label or 'channel', '通道地址需要以 http(s):// 开头'))
        if c.get('style', 2) not in (0, 1, 2):
            out.append(('error', label or 'channel', 'style 只能是 0/1/2'))
        for acc in c.get('accounts') or []:
            if acc != 'all' and acc not in account_labels(cfg):
                out.append(('warn', label or 'channel',
                            '关联了不存在的账号 %r（可先 account add，或改用 all）' % acc))

    if get(cfg, 'session_cookie'):
        out.append(('info', 'session_cookie', '已存有会话，status/checkin 可直接复用'))
    if len(accounts) > 1:
        out.append(('info', 'accounts', '多人模式：%d 个账号，auto 会逐个打卡' % len(accounts)))
    return out


def missing_required(cfg):
    return [item for item in validate(cfg) if item[0] == 'error']


def describe_field(dotted):
    return EDITABLE.get(dotted, dotted)


def masked_value(cfg, dotted):
    """敏感字段打码后的展示值。"""
    value = get(cfg, dotted)
    if value in (None, ''):
        return '(未设置)'
    text = str(value)
    from .privacy import SECRET_KEYS
    if dotted.split('.')[-1].lower() in SECRET_KEYS:
        return '<已隐藏>'
    if dotted == 'notify.url':
        return mask_url(text)
    return text


def mask_url(url):
    """与固件 AppConfig::maskUrl 同语义：只留 scheme+host，路径打码。"""
    from .privacy import diagnostic_url
    return diagnostic_url(url)
