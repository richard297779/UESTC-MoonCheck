# -*- coding: utf-8 -*-
"""推送通道：渲染 + fan-out + 发送。

规则与固件 notify_service.cpp 对齐：
  style 0  GET 模板：URL 含 {title}/{body} 占位符则填充；否则按查询参数追加
         （Server酱正文键用 desp）；模板缺 {title} 时 title 另附一次
  style 1  POST 纯文本：Server酱自动降级为表单 title=&desp=
  style 2  POST JSON：URL 内嵌 {字段:{占位}} 模板则按模板构造，否则
         {title, desp|body, device}
Server酱判定：主机含 ftqq.com / ft07.com / .send。
桌面单次运行不做每日配额/去重间隔（一晚最多一轮汇总 + 每人一条），
失败只记日志——推送坏了不能拖累打卡本身。"""
import json
import re
import urllib.parse

from . import http_client

_TITLE_MAX = 60      # 固件 title[64]，含设备前缀，这里掐正文标题
_BODY_MAX = 240      # 固件 body[256]


def is_server_chan(url):
    host = urllib.parse.urlparse(url or '').netloc.lower()
    return ('ftqq.com' in host) or ('ft07.com' in host) or host.endswith('.send')


def _clamp(text, limit):
    text = text or ''
    return text if len(text) <= limit else text[:limit - 1] + '…'


def _fill_template(template, mapping):
    """替换 {key}；编码规则由调用方决定（URL 参数 / JSON 字符串）。"""
    def sub(m):
        key = m.group(1)
        return mapping.get(key)
    # 只替换已知键，未知 {x} 原样保留
    out, pos = [], 0
    for m in re.finditer(r'\{(\w+)\}', template):
        val = mapping.get(m.group(1))
        if val is None:
            continue
        out.append(template[pos:m.start()])
        out.append(val)
        pos = m.end()
    out.append(template[pos:])
    return ''.join(out)


def render(url, style, title, body):
    """返回 (method, final_url, post_body, content_type)。"""
    title = _clamp(title, _TITLE_MAX)
    body = _clamp(body, _BODY_MAX)
    if style == 0:
        has_tpl = ('{title}' in url) or ('{body}' in url)
        mapping = {'title': urllib.parse.quote(title.encode('utf-8')),
                   'body': urllib.parse.quote(body.encode('utf-8'))}
        final = _fill_template(url, mapping) if has_tpl else url
        if not has_tpl:
            field = 'desp' if is_server_chan(url) else 'body'
            sep = '&' if ('?' in final) else '?'
            final += sep + 'title=' + mapping['title'] + '&' + field + '=' + mapping['body']
        elif '{title}' not in url:
            field = 'desp' if is_server_chan(url) else 'message'
            sep = '&' if ('?' in final) else '?'
            final += sep + field + '=' + mapping['title']
        return 'GET', final, None, None
    if style == 1:
        if is_server_chan(url):
            form = urllib.parse.urlencode({'title': title, 'desp': body})
            return 'POST', url, form, 'application/x-www-form-urlencoded'
        return 'POST', url, title + '\n\n' + body, 'text/plain; charset=utf-8'
    # style == 2 JSON：URL 内嵌 {"字段":"{title|body}"} 模板段则按模板构造
    tpl_m = re.search(r'(\{[^{}]*"[^"]+"\s*:\s*"[^"]*\{(?:title|body)\}[^"]*"[^{}]*\})', url)
    if tpl_m:
        base = url[:tpl_m.start()] + url[tpl_m.end():]
        base = re.sub(r'\?[&]', '?', base)
        base = base.rstrip('?&')
        mapping = {'title': _json_escape(title), 'body': _json_escape(body)}
        doc_text = _fill_template(tpl_m.group(1), mapping)
        doc = json.loads(doc_text)
        return 'POST', base, json.dumps(doc, ensure_ascii=False), 'application/json; charset=utf-8'
    field = 'desp' if is_server_chan(url) else 'body'
    doc = {'title': title, field: body, 'device': 'MoonCheck-Desktop'}
    return 'POST', url, json.dumps(doc, ensure_ascii=False), 'application/json; charset=utf-8'


def _json_escape(text):
    # 把字符串转成 JSON 串再剥掉外层引号，得到可嵌模板的安全值
    return json.dumps(text or '', ensure_ascii=False)[1:-1]


def _business_error(status, text):
    """HTTP 2xx 但响应体是失败 JSON（Server酱/企业微信常见）。"""
    if status < 200 or status >= 300:
        return None
    try:
        doc = json.loads(text)
    except Exception:
        return None
    if not isinstance(doc, dict):
        return None
    for k in ('code', 'errcode'):
        v = doc.get(k)
        if v in (None, 0, '0'):
            continue
        if isinstance(v, int) and 200 <= v < 300:
            continue
        return '%s=%s %s' % (k, v, str(doc.get('message') or doc.get('msg') or '')[:60])
    return None


def send(channel, title, body, log=None, timeout=15):
    """向单个通道发送。channel = {label,on,style,url,accounts}（config.iter_channels 的形状）。
    返回 (ok, note)。"""
    log = log or (lambda s: None)
    url = (channel.get('url') or '').strip()
    if not url:
        return False, '通道 %s 未配置 URL' % (channel.get('label') or '默认')
    try:
        style = int(channel.get('style', 2))
    except (TypeError, ValueError):
        style = 2
    method, final_url, post_body, ctype = render(url, style, title, body)
    client = http_client.HttpClient(timeout=timeout)
    headers = {}
    if ctype:
        headers['Content-Type'] = ctype
    try:
        if method == 'GET':
            resp = client.get(final_url, headers=headers)
        else:
            resp = client.post(final_url, post_body or '', headers=headers)
    except http_client.NetworkError as e:
        return False, '%s: %s' % (channel.get('label') or '默认', e)
    text = resp.text or ''
    err = _business_error(resp.status, text)
    if err:
        return False, '%s: HTTP %d 但业务失败（%s）' % (channel.get('label') or '默认', resp.status, err)
    if not (200 <= resp.status < 300):
        return False, '%s: HTTP %d' % (channel.get('label') or '默认', resp.status)
    return True, '%s: HTTP %d' % (channel.get('label') or '默认', resp.status)


def fan_out(cfg, events, log=None, only_default=False, skip_default=False):
    """events = [(account_label_or_'', title, body)]，与固件 post() 的通道匹配规则一致：
      - 默认通道（顶层 notify.url）恒收所有事件（on+url 才发）
      - 额外通道 accounts 含 'all' 或为空 -> 收全部；否则只收列出的账号事件
      - 纯账号事件（account 非 ''）默认通道也收（与固件一致：默认通道无过滤）
    only_default=True 时跳过额外通道（多人「汇总」的用法）；
    skip_default=True 时跳过默认通道（多人「逐账号结果」的用法）。
    返回 [(通道标签, ok, note)]。"""
    from . import config as _config
    log = log or (lambda s: None)
    results = []
    for channel in _config.iter_channels(cfg):
        if not channel.get('on') or not (channel.get('url') or '').strip():
            continue
        is_default = channel.get('label') == ''
        if only_default and not is_default:
            continue
        if skip_default and is_default:
            continue
        for account, title, body in events:
            if not is_default and not _config.channel_matches(channel, account):
                continue
            display_title = '[%s] %s' % (account, title) if account else title
            ok, note = send(channel, display_title, body, log=log)
            results.append((channel.get('label') or '默认', ok, note))
            log('%s推送 -> %s：%s' % ('[OK] ' if ok else '[FAIL] ',
                                     channel.get('label') or '默认', note))
    return results
