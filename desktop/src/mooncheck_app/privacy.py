"""Conservative filtering for diagnostic output; never used on requests."""
import re
from urllib.parse import urlsplit

SECRET_KEYS = {
    'pass', 'password', 'pwd', 'student_id', 'user', 'browser_fingerprint', 'bfp',
    'session', 'session_cookie', 'cookie', 'authorization', 'openid', 'access',
    'refresh', 'access_token', 'refresh_token', 'accesstoken', 'refreshtoken',
    'token', 'ticket', 'x-checkin-ticket', 'code', 'url', 'latitude', 'longitude',
    'lat', 'lng', 'address', 'reverse_geocode', 'last_msg',
}


def diagnostic_url(value):
    try:
        parsed = urlsplit(value)
        if parsed.scheme not in ('http', 'https') or not parsed.hostname:
            return '<已隐藏地址>'
        return '%s://%s/[已隐藏路径和参数]' % (parsed.scheme, parsed.hostname)
    except ValueError:
        return '<已隐藏地址>'


def sanitize(text, configuration=None):
    text = str(text)
    secrets = set()
    def walk(value):
        if isinstance(value, dict):
            for key, item in value.items():
                if key.lower() in SECRET_KEYS and isinstance(item, (str, int, float)) and len(str(item)) >= 3:
                    secrets.add(str(item))
                else:
                    walk(item)
        elif isinstance(value, list):
            for item in value:
                walk(item)
    walk(configuration)
    for secret in sorted(secrets, key=len, reverse=True):
        text = text.replace(secret, '<已隐藏>')
    text = re.sub(r'https?://[^\s<>"\']+', lambda m: diagnostic_url(m.group()), text)
    text = re.sub(r'(?i)\b(SESSION|Cookie|Authorization|access_token|refresh_token|openid|ticket|code|password|token|bfp)\s*[:=]\s*[^\s;,<>]+',
                  lambda m: m.group(1) + '=<已隐藏>', text)
    return text
