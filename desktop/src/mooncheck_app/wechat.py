"""Account-scoped OAuth and operation tickets. No credentials in logs/errors."""
import hashlib
import json
import re
import secrets
import time
import urllib.error
import urllib.request

from . import config
from .http_client import HttpClient
from .wechat_wire import ProtocolError

APP_ID = 'wxaa78bf2905f01cee'
BASE = 'https://yybadaccess.3g.qq.com/pc_yyb_auth/'
ACCESS_KEY = 'wgrdg373hy26ww2'  # Public YYB web application access key, also in firmware.


class AuthorizationRejected(ProtocolError):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def validate_credentials(value):
    if not isinstance(value,dict): raise ProtocolError('请先导入本人微信授权')
    for key in ('openid','access','refresh'):
        token = value.get(key)
        if not isinstance(token,str) or not token.strip() or len(token)>16384 or any(c in token for c in '\r\n;'):
            raise ProtocolError('微信授权字段不完整或格式无效')
    try: expiry = int(value.get('expires_at',0))
    except (ValueError,TypeError): raise ProtocolError('授权到期时间应为绝对 Unix 秒') from None
    if expiry < 0 or expiry > 2**63-1: raise ProtocolError('授权到期时间无效')
    return dict(openid=value['openid'],access=value['access'],refresh=value['refresh'],expires_at=expiry)


def signed_headers(body, timestamp=None, nonce=None):
    timestamp = str(int(time.time()*1000)) if timestamp is None else str(timestamp)
    nonce = '%04d'%secrets.randbelow(10000) if nonce is None else str(nonce)
    signature = hashlib.md5((body+timestamp+ACCESS_KEY+nonce).encode()).hexdigest()
    return {'Ual-Access-Businessid':'pc_yyb_auth','Ual-Access-Timestamp':timestamp,
            'Ual-Access-Nonce':nonce,'Ual-Access-Signature':signature,'Content-Type':'application/json','Accept':'application/json'}


def oauth_post(path, payload, credentials=None, timeout=20):
    body = json.dumps(payload,ensure_ascii=False,separators=(',',':'))
    headers = signed_headers(body)
    if credentials:
        headers['Cookie'] = 'openid=%s; accesstoken=%s; refreshtoken=%s'%(credentials['openid'],credentials['access'],credentials['refresh'])
    request = urllib.request.Request(BASE+path,data=body.encode(),headers=headers,method='POST')
    opener = urllib.request.build_opener(NoRedirect())
    try:
        with opener.open(request,timeout=timeout) as response:
            data = response.read(65537)
            if len(data)>65536: raise ProtocolError('微信响应超过长度限制')
            parsed = json.loads(data)
    except urllib.error.HTTPError as error:
        if error.code == 403: raise AuthorizationRejected('微信授权被拒绝，请重新扫码') from None
        raise ProtocolError('微信授权服务 HTTP 请求失败') from None
    except (OSError,urllib.error.URLError,ValueError):
        raise ProtocolError('微信授权网络或响应异常') from None
    if not isinstance(parsed,dict) or 'code' not in parsed:
        raise ProtocolError('微信授权响应结构无效')
    if str(parsed['code']) != '0':
        raise AuthorizationRejected('微信授权被拒绝，请重新扫码')
    return parsed


def refresh(credentials, timeout):
    response = oauth_post('pcyyb_refresh_token_auth',{'userInfo':{'openId':credentials['openid'],
        'refreshToken':credentials['refresh'],'accessToken':credentials['access'],'loginType':'WX'}},timeout=timeout)
    info = response.get('user_info')
    if not isinstance(info,dict): raise ProtocolError('微信刷新响应缺少用户信息')
    try: ttl = int(info.get('expires_in') or 7200)
    except (ValueError,TypeError): raise ProtocolError('微信刷新到期时间无效') from None
    if ttl <= 0: ttl = 7200
    if info.get('openid',credentials['openid']) != credentials['openid']: raise ProtocolError('刷新返回不同微信身份')
    return validate_credentials(dict(openid=credentials['openid'],access=info.get('access_token'),
        refresh=info.get('refresh_token') or credentials['refresh'],expires_at=int(time.time())+ttl))


def login_buffer(credentials, timeout):
    response = oauth_post('pcyyb_get_wx_login_buffer_auth',{'extInfo':{'listS':{
        'unionid':{'value':[credentials['openid']]},'user_id':{'value':[credentials['openid']]},
        'access_token':{'value':[credentials['access']]}},'listI':{'user_type':{'value':[0]}}}},credentials,timeout)
    try: value = response['ext_info']['list_s']['login_buffer']['value'][0]
    except (KeyError,IndexError,TypeError): raise ProtocolError('微信响应缺少 login_buffer') from None
    if not isinstance(value,str) or not value or len(value)>6000: raise ProtocolError('微信 login_buffer 无效')
    return value


def platform_post(path, payload, timeout, session=None, client_factory=HttpClient):
    # A fresh client has NO CAS jar and cannot override the explicit MP cookie.
    client = client_factory(timeout=timeout)
    headers = {'platform':'mp-weixin'}
    if session: headers['Cookie'] = 'SESSION='+session
    response = client.request('POST','https://jzsz.uestc.edu.cn/platform-app/api/auth/'+path,headers=headers,json_body=payload)
    if response.status != 200: raise ProtocolError('小程序授权接口 HTTP 失败')
    try: result = response.json()
    except ValueError: raise ProtocolError('小程序授权响应无效') from None
    if not isinstance(result,dict) or result.get('ok') is not True or not isinstance(result.get('data'),dict): raise ProtocolError('小程序授权未通过')
    return result['data']


def ensure_ticket(cfg, account, log=lambda _:None):
    from .wechat_transport import get_code
    credentials = validate_credentials(account.get('wechat'))
    timeout = min(120,max(5,int(cfg.get('timeout') or 20)))
    def rotate():
        nonlocal credentials
        credentials = refresh(credentials,timeout)
        config.save_account_wechat(cfg,account,credentials)
        log('微信授权已刷新并保存（不显示凭据）')
    if credentials['expires_at'] <= int(time.time())+120: rotate()
    def code():
        try: buffer = login_buffer(credentials,timeout)
        except AuthorizationRejected:
            rotate()
            buffer = login_buffer(credentials,timeout)
        return get_code(buffer,APP_ID,timeout=max(30,timeout))
    bind = platform_post('check-bind',{'code':code(),'sn':''},timeout)
    session = bind.get('sessionId')
    if bind.get('bind') is not True or not isinstance(session,str) or not session or any(c in session for c in '\r\n;'):
        raise ProtocolError('微信尚未绑定当前平台账号，请先在官方小程序绑定')
    result = platform_post('checkin-ticket',{'code':code()},timeout,session)
    ticket = result.get('ticket')
    if not isinstance(ticket,str) or not re.fullmatch(r'[A-Za-z0-9_-]{43}',ticket): raise ProtocolError('平台未返回有效操作票据')
    return ticket
