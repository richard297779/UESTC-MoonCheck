"""Pure BLE status formatting, account targeting and payload construction."""
import math
import re


def group_report_mode(snapshot):
    config = snapshot.get("cfg")
    schedule = config.get("schedule") if isinstance(config, dict) else None
    value = schedule.get("groupReport") if isinstance(schedule, dict) else None
    return value if type(value) is bool else None


def mode_name(mode):
    if mode is None:
        return "设备未提供模式切换"
    return "安全员小组确认" if mode else "个人定位"


def group_pending_count(data):
    if not isinstance(data, dict):
        return None
    # 早期 v0.9.0 体检总是返回该数值，个人模式或会话失效时为 0。
    if data.get("sessionValid") is False or data.get("recordStatusCode", data.get("today")) not in (
            "GROUP_PENDING", "GROUP_CLEAR"):
        return None
    value = data.get("groupPendingCount")
    return value if type(value) is int and value >= 0 else None


def snapshot_accounts(snapshot):
    rows = [dict(row) for row in snapshot.get('accounts', [])]
    if not rows and snapshot.get('primary'):
        rows = [dict(snapshot['primary'], primary=True)]
    return rows


def resolve_account(rows, query, kind='auto'):
    query = str(query).strip()
    if not query:
        raise ValueError('请选择或输入目标账号')
    matches = []
    for row in rows:
        values = {'slot': str(row['slot']), 'label': row['label'], 'user': row.get('user', '')}
        if query in ([values[kind]] if kind != 'auto' else list(values.values())):
            matches.append(row)
    if len(matches) != 1:
        raise ValueError('没有唯一匹配账号，请选择查找方式并刷新账号表')
    return matches[0]


def patch_payload(snapshot, values, target=None, creating=False):
    fields = {}
    for key in ('label', 'user', 'pass', 'bfp', 'lat', 'lng', 'addr'):
        value = str(values.get(key, ''))
        if key in ('label', 'user', 'bfp', 'lat', 'lng'):
            value = value.strip()
        if not value:
            continue
        if key in ('lat', 'lng'):
            value = float(value)
            if not math.isfinite(value) or value == 0 or not (-90 <= value <= 90 if key == 'lat' else -180 <= value <= 180):
                raise ValueError('请填写合法、非零、有限的经纬度')
        fields[key] = value
    if not fields:
        raise ValueError('没有修改，不会发送空更新')
    if ('lat' in fields) != ('lng' in fields):
        raise ValueError('经纬度必须成对填写')
    if 'label' in fields and not re.fullmatch(r'[A-Za-z0-9_-]{1,12}', fields['label']):
        raise ValueError('标签仅支持 1–12 位字母、数字、- 或 _')
    if 'bfp' in fields and not re.fullmatch(r'[A-Fa-f0-9]{32}', fields['bfp']):
        raise ValueError('指纹必须为 32 位十六进制')
    if creating and not all(fields.get(k) for k in ('label', 'user', 'pass', 'bfp')):
        raise ValueError('新账号必须填写标签、学号、密码和浏览器指纹')
    if not creating and (not target or target.get('primary')):
        raise ValueError('请选择已有账号；主账号使用设备设置中的基础账号配置')
    return dict(op='account.patch', create=creating, expectedRevision=int(snapshot['revision']),
                target='' if creating else target['label'], fields=fields)


def selected_payload(snapshot, targets, operation, **options):
    if not 1 <= len(targets) <= 6 or len({a['label'] for a in targets}) != len(targets):
        raise ValueError('请选择 1–6 个不重复的目标账号')
    if any(not a.get('enabled') for a in targets):
        raise ValueError('目标包含已禁用账号，请先明确启用')
    return dict(op=operation, targets=[a['label'] for a in targets], expectedRevision=int(snapshot['revision']), **options)


def notification_labels(notification, label):
    names = []
    if notification.get('on'):
        names.append('默认通道')
    for channel in notification.get('channels', []):
        accounts = channel.get('accounts', '')
        linked = accounts if isinstance(accounts, list) else [x.strip() for x in accounts.split(',') if x.strip()]
        if not linked or 'all' in linked or label in linked:
            names.append(str(channel.get('label', '附加通道')) + ('' if channel.get('on') else '（停用）'))
    return '、'.join(names) if names else '未关联启用通道'
