// ============================================================================
//  NotifyService 实现（见 notify_service.h 的设计约束与线程约束）
//  0.4.0 起支持多通道：默认通道（notify.set）+ 最多 5 条额外通道（notify.add），
//  逐账号事件按 NotifyTarget 分发；去重与每日配额按通道独立计数。
// ============================================================================

#include "notify_service.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <string.h>
#include <time.h>

#include "app_version.h"
#include "http_client.h"
#include "logger.h"
#include "public_ca_pem.h"
#include "time_service.h"

namespace {

const char* const kTag = "notify";

/// @brief 单次 HTTP 请求超时（比平台的 15 s 短得多：通知绝不能拖慢打卡主流程）。
constexpr uint32_t kNotifyTimeoutMs = 6000;

/// @brief 发送失败后的退避间隔（第 N 次失败后等待 kBackoffSeconds[N] 秒）。
constexpr uint32_t kBackoffSeconds[4] = {5, 30, 300, 600};

/// @brief 同一条消息最多尝试次数。
constexpr uint8_t kMaxAttempts = 4;

/// @brief 对端返回 2xx 但业务上失败。
/// @note Server酱 / 钉钉 / 企业微信 的失败都是 **HTTP 200 + JSON 里的错误码**，只看状态码
///       会把「被拒收」误报成「已发送」。只在响应是 JSON 对象且显式带 `code`/`errcode`
///       时判定；Bark 用 `code:200` 表示成功（HTTP 语义），所以 2xx 数值算通过。
bool businessError(const HttpResponse& response, String& outError) {
  if (response.body.length() == 0 || response.body[0] != '{') return false;
  JsonDocument doc;
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) return false;

  long code = 0;
  bool hasCode = false;
  if (doc["code"].is<int>()) {
    code = doc["code"].as<int>();
    hasCode = true;
  } else if (doc["errcode"].is<int>()) {
    code = doc["errcode"].as<int>();
    hasCode = true;
  }
  if (!hasCode || code == 0 || (code >= 200 && code < 300)) return false;

  outError = "code " + String(static_cast<int>(code));
  const char* msg = nullptr;
  if (doc["message"].is<const char*>()) {
    msg = doc["message"].as<const char*>();
  } else if (doc["error"].is<const char*>()) {
    msg = doc["error"].as<const char*>();
  }
  if (msg != nullptr && msg[0] != '\0') {
    outError += " ";
    outError += String(msg).substring(0, 80);
  }
  return true;
}

/// @brief 把标题/正文里的占位符替换为实际值（GET 模板用，值做 URL 编码）。
String fillTemplate(const String& tmpl, const char* title, const char* body) {
  const String encodedTitle = HttpClient::urlEncode(String(title));
  const String encodedBody = HttpClient::urlEncode(String(body));
  String out = tmpl;
  out.replace("{title}", encodedTitle);
  out.replace("{body}", encodedBody);
  return out;
}

/// @brief JSON 字符串转义（POST JSON 模板里手工替换时用）。
String jsonEscape(const String& text) {
  String out;
  out.reserve(text.length() + 8);
  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

const char* NotifyService::kindName(NotifyKind kind) {
  switch (kind) {
    case NotifyKind::StatusDigest: return "status_digest";
    case NotifyKind::CheckinOk: return "checkin_ok";
    case NotifyKind::CheckinFail: return "checkin_fail";
    case NotifyKind::CheckinRetry: return "checkin_retry";
    case NotifyKind::DeadlineMissed: return "deadline_missed";
    case NotifyKind::NeedMfa: return "need_mfa";
    case NotifyKind::NetDown: return "net_down";
    default: return "info";
  }
}

const char* NotifyService::kindTitle(NotifyKind kind) {
  switch (kind) {
    case NotifyKind::StatusDigest: return "21:00 状态摘要";
    case NotifyKind::CheckinOk: return "打卡成功";
    case NotifyKind::CheckinFail: return "打卡失败";
    case NotifyKind::CheckinRetry: return "打卡重试";
    case NotifyKind::DeadlineMissed: return "窗口已过仍未成功";
    case NotifyKind::NeedMfa: return "需要验证码";
    case NotifyKind::NetDown: return "网络不可用";
    default: return "通知";
  }
}

Result NotifyService::begin(AppConfig& config, HttpClient& http, TimeService& time) {
  m_config = &config;
  m_http = &http;
  m_time = &time;
  for (uint8_t i = 0; i < kQueueCapacity; i++) m_queue[i].used = false;
  m_count = 0;
  for (uint8_t i = 0; i < kDedupSlots; i++) {
    m_dedup[i].key[0] = '\0';
    m_dedup[i].lastSec = 0;
  }
  for (uint8_t i = 0; i < kChannelSlots; i++) m_sentTodayCh[i] = 0;
  LOG_I(kTag, "notify ready (enabled=%d, style=%u, host=%s, gap=%us, quota=%u, extra=%u)",
        m_config->notifyEnabled() ? 1 : 0, m_config->notifyStyle(),
        m_config->notifyUrlMasked().c_str(), m_config->notifyGapSeconds(),
        m_config->notifyDailyQuota(), static_cast<unsigned>(m_config->notifyChannelCount()));
  return Result::Ok;
}

bool NotifyService::active() const {
  if (m_config == nullptr) return false;
  if (m_config->notifyEnabled() && m_config->notifyUrl().length() > 0) return true;
  return m_config->hasActiveExtraChannel();
}

bool NotifyService::dedupAllow(const char* key, uint32_t nowSec) {
  // key 为空表示不去重（手动/测试消息）
  if (key == nullptr || key[0] == '\0') return true;
  for (uint8_t i = 0; i < kDedupSlots; i++) {
    if (m_dedup[i].key[0] != '\0' && strcmp(m_dedup[i].key, key) == 0) {
      const uint32_t gap = m_config ? m_config->notifyGapSeconds() : 60;
      if (m_dedup[i].lastSec != 0 && nowSec >= m_dedup[i].lastSec &&
          (nowSec - m_dedup[i].lastSec) < gap) {
        return false;  // 太近了，丢弃
      }
      m_dedup[i].lastSec = nowSec;
      return true;
    }
  }
  // 新键：找一个空位或最旧的槽登记
  uint8_t victim = 0;
  uint32_t oldest = 0xFFFFFFFFu;
  for (uint8_t i = 0; i < kDedupSlots; i++) {
    if (m_dedup[i].key[0] == '\0') {
      victim = i;
      break;
    }
    if (m_dedup[i].lastSec <= oldest) {
      oldest = m_dedup[i].lastSec;
      victim = i;
    }
  }
  strncpy(m_dedup[victim].key, key, sizeof(m_dedup[victim].key) - 1);
  m_dedup[victim].key[sizeof(m_dedup[victim].key) - 1] = '\0';
  m_dedup[victim].lastSec = nowSec;
  return true;
}

void NotifyService::rollDay() {
  if (m_time == nullptr || !m_time->isSynced()) return;
  const String today = m_time->todayString();
  if (strcmp(m_quotaDate, today.c_str()) != 0) {
    strncpy(m_quotaDate, today.c_str(), sizeof(m_quotaDate) - 1);
    m_quotaDate[sizeof(m_quotaDate) - 1] = '\0';
    for (uint8_t i = 0; i < kChannelSlots; i++) m_sentTodayCh[i] = 0;
    m_retryCount = 0;   // 「重试 #N」也按天重新编号
  }
}

bool NotifyService::quotaAllow(uint8_t channel) {
  if (m_config == nullptr || channel >= kChannelSlots) return false;
  const uint8_t quota = m_config->notifyDailyQuota();
  if (quota == 0) return true;  // 0 = 不限
  return m_sentTodayCh[channel] < quota;
}

void NotifyService::noteSent(uint8_t channel) {
  if (channel < kChannelSlots && m_sentTodayCh[channel] < 250) m_sentTodayCh[channel]++;
}

uint8_t NotifyService::totalSentToday() const {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < kChannelSlots; i++) sum += m_sentTodayCh[i];
  return sum;
}

uint8_t NotifyService::bumpAttempt() {
  if (m_time != nullptr) rollDay();  // 跨天归零，编号永远是"今天的第几轮"
  if (m_retryCount < 250) m_retryCount++;
  return m_retryCount;
}

/// @brief 该通道是否接收这条事件（含开关与账号匹配规则）。
bool NotifyService::channelAccepts(uint8_t channel, const String& account, NotifyTarget target) const {
  if (channel >= kChannelSlots) return false;
  const bool wantDefault = (target != NotifyTarget::AccountOnly);
  const bool wantExtra = (target != NotifyTarget::DefaultOnly);
  if (channel == 0) {
    if (!wantDefault) return false;
    return m_config->notifyEnabled() && m_config->notifyUrl().length() > 0;
  }
  if (!wantExtra) return false;
  const size_t idx = static_cast<size_t>(channel - 1);
  if (idx >= m_config->notifyChannelCount()) return false;
  const NotifyChannel& ch = m_config->notifyChannelAt(idx);
  if (!ch.enabled || ch.url.length() == 0) return false;
  return m_config->notifyChannelMatchesAccount(idx, account);
}

void NotifyService::post(NotifyKind kind, const String& body, const String& key,
                         const String& account, NotifyTarget target) {
  if (!active()) return;  // 未启用 / 未配置站点：静默（不打扰打卡主流程）
  rollDay();

  const uint32_t nowSec = (m_time != nullptr && m_time->isSynced()) ? static_cast<uint32_t>(m_time->now()) : 0;
  const char* baseKey = key.length() > 0 ? key.c_str() : kindName(kind);

  // 标题：[设备名] 或 [设备名·账号]；重试事件自动编号 #N
  char titleBase[64];
  if (account.length() > 0) {
    snprintf(titleBase, sizeof(titleBase), "[%s·%s]", m_config->bleDeviceName().c_str(),
             account.c_str());
  } else {
    snprintf(titleBase, sizeof(titleBase), "[%s]", m_config->bleDeviceName().c_str());
  }
  char title[64];
  if (kind == NotifyKind::CheckinRetry) {
    // 轮次编号由调用方 bumpAttempt() 维护：被限流挡掉的那些轮也要计入"第 N 轮"
    snprintf(title, sizeof(title), "%s %s #%u", titleBase, kindTitle(kind),
             static_cast<unsigned>(m_retryCount));
  } else {
    snprintf(title, sizeof(title), "%s %s", titleBase, kindTitle(kind));
  }

  bool mirrored = false;
  for (uint8_t ch = 0; ch < kChannelSlots; ch++) {
    if (!channelAccepts(ch, account, target)) continue;

    // 去重键按通道隔离："cN|<key>"，某通道被限流不影响其它通道
    char dedupKey[32];
    snprintf(dedupKey, sizeof(dedupKey), "c%u|%s", static_cast<unsigned>(ch), baseKey);
    if (!dedupAllow(dedupKey, nowSec)) {
      LOG_I(kTag, "dedup c%u: %s within %us", static_cast<unsigned>(ch), dedupKey,
            m_config->notifyGapSeconds());
      continue;
    }
    if (!quotaAllow(ch)) {
      LOG_W(kTag, "daily quota (%u) reached on channel %u, drop %s",
            m_config->notifyDailyQuota(), static_cast<unsigned>(ch), kindName(kind));
      continue;
    }

    // 入队（环形，满了挤掉最旧的一条）
    uint8_t slot = kQueueCapacity;
    for (uint8_t i = 0; i < kQueueCapacity; i++) {
      if (!m_queue[i].used) {
        slot = i;
        break;
      }
    }
    if (slot >= kQueueCapacity) {
      // 满：覆盖「产生时间最早」的一条（不覆盖刚要写入的这条自己）
      slot = 0;
      uint32_t oldest = 0xFFFFFFFFu;
      for (uint8_t i = 0; i < kQueueCapacity; i++) {
        if (m_queue[i].createdAtSec <= oldest) {
          oldest = m_queue[i].createdAtSec;
          slot = i;
        }
      }
      LOG_W(kTag, "queue full, replace oldest (%s)", kindName(m_queue[slot].kind));
    } else {
      m_count++;
    }
    NotifyItem& item = m_queue[slot];
    item.used = true;
    item.kind = kind;
    item.channel = ch;
    item.attempts = 0;
    item.nextTryMs = 0;  // 立即可发
    item.createdAtSec = nowSec;
    strncpy(item.title, title, sizeof(item.title) - 1);
    item.title[sizeof(item.title) - 1] = '\0';
    strncpy(item.body, body.c_str(), sizeof(item.body) - 1);
    item.body[sizeof(item.body) - 1] = '\0';
    strncpy(item.key, dedupKey, sizeof(item.key) - 1);
    item.key[sizeof(item.key) - 1] = '\0';
    LOG_I(kTag, "queued %s -> c%u (%u in queue)", kindName(kind), static_cast<unsigned>(ch),
          static_cast<unsigned>(m_count));

    // BLE 镜像一份（同一事件只镜像一次）：控制台在线时能立刻看到，不在线也不影响推送
    if (!mirrored && m_sink) {
      mirrored = true;
      String payload = String("\"kind\":\"notify\",\"event\":\"") + kindName(kind) +
                       "\",\"title\":\"" + title + "\",\"body\":\"" + body + "\"";
      if (account.length() > 0) payload += String(",\"account\":\"") + account + "\"";
      m_sink("event", payload);
    }
  }
}

bool NotifyService::render(const char* title, const char* body, uint8_t channel, String& outUrl,
                           String& outMethod, String& outBody, String& outContentType) const {
  if (m_config == nullptr) return false;
  String url;
  uint8_t style;
  if (channel == 0) {
    url = m_config->notifyUrl();  // 可本地修改：JSON 模板模式下要把 "{...}" 从 URL 上切下来
    style = m_config->notifyStyle();
  } else {
    const size_t idx = static_cast<size_t>(channel - 1);
    if (idx >= m_config->notifyChannelCount()) return false;
    const NotifyChannel& ch = m_config->notifyChannelAt(idx);
    url = ch.url;
    style = ch.style;
  }
  outUrl = url;
  outBody = "";
  outContentType = "text/plain; charset=utf-8";

  if (style == 0) {
    // GET 模板：Bark `https://api.day.app/KEY/{title}/{body}`、
    //           Server酱 `https://sctapi.ftqq.com/KEY.send?title={title}&desp={body}`
    outMethod = "GET";
    outUrl = fillTemplate(url, title, body);
    if (url.indexOf('{') < 0) {
      // 没有占位符 -> 退化成追加参数
      outUrl = url + (url.indexOf('?') < 0 ? "?" : "&") + "title=" +
               HttpClient::urlEncode(String(title)) + "&" +
               (isServerChan(url) ? "desp" : "body") + "=" + HttpClient::urlEncode(String(body));
    }
    return true;
  }
  if (style == 1) {
    // POST 纯文本：ntfy —— 首行即标题
    outMethod = "POST";
    if (isServerChan(url)) {
      // Server酱 不认纯文本 body（实测 HTTP 400 `title 不能为空`）：自动降级成表单 POST
      LOG_D(kTag, "serverchan rejects post_text: downgraded to form-encoded");
      outContentType = "application/x-www-form-urlencoded";
      outBody = "title=" + HttpClient::urlEncode(String(title)) +
                "&desp=" + HttpClient::urlEncode(String(body));
      return true;
    }
    outBody = String(title) + "\n" + body + "\n";
    return true;
  }
  // POST JSON：钉钉/企业微信/自建中转
  outMethod = "POST";
  outContentType = "application/json";
  if (url.indexOf('{') >= 0) {
    // 用户自带 JSON 模板：手工替换 {title} / {body}（值做 JSON 转义）
    String tmpl = url.substring(url.indexOf('{'));
    url.remove(url.indexOf('{'));
    tmpl.replace("{title}", jsonEscape(String(title)));
    tmpl.replace("{body}", jsonEscape(String(body)));
    outUrl = url;  // 模板前的部分即真正的 URL
    outBody = tmpl;
    return true;
  }
  JsonDocument doc;
  doc["title"] = title;
  doc[isServerChan(url) ? "desp" : "body"] = body;  // Server酱 只认 desp，发 body 会是空正文
  doc["device"] = m_config->bleDeviceName();
  serializeJson(doc, outBody);
  return true;
}

bool NotifyService::isServerChan(const String& url) {
  return url.indexOf("ftqq.com") >= 0 ||    // Server酱·Turbo  sctapi.ftqq.com/<KEY>.send
         url.indexOf("ft07.com") >= 0 ||    // Server酱³       sc3.ft07.com
         url.indexOf(".send") >= 0;         // 两种版本的推送路径后缀
}

bool NotifyService::deliver(const char* title, const char* body, uint8_t channel, int* outStatus,
                            String& outError) {
  if (m_http == nullptr || m_config == nullptr) {
    outError = "notify not begun";
    return false;
  }
  String url, method, reqBody, contentType;
  if (!render(title, body, channel, url, method, reqBody, contentType)) {
    outError = "render failed";
    return false;
  }

  HttpRequest request;
  request.method = method;
  request.url = url;
  request.body = reqBody;
  request.timeoutMs = kNotifyTimeoutMs;
  request.followRedirects = true;  // 中转服务常用 302 跳转
  request.publicCa = true;         // 公网站点：用公共根证书包，不用校园 CA
  request.insecure = false;        // 严格校验（要放宽请改全局 tls_mode）
  request.addHeader("Accept", "application/json, text/plain, */*");
  request.addHeader("Content-Type", contentType);

  HttpResponse response;
  const Result result = m_http->request(request, response);
  if (outStatus != nullptr) *outStatus = response.status;
  if (result != Result::Ok) {
    outError = (response.errorDetail.length() > 0) ? response.errorDetail : resultName(result);
    return false;
  }
  if (response.status < 200 || response.status >= 400) {
    outError = "http " + String(response.status);
    if (response.body.length() > 0) {
      outError += " ";
      outError += response.body.substring(0, 120);  // 回显对端错误，便于排障
    }
    return false;
  }
  // Server酱 / 钉钉 的失败也藏在 200 里，光看状态码会把拒收当成功
  if (businessError(response, outError)) {
    LOG_W(kTag, "response rejected (%s)", outError.c_str());
    return false;
  }
  outError = "";
  return true;
}

bool NotifyService::sendNow(NotifyKind kind, const String& body, int* outStatus, String& outError,
                            uint8_t channel, const String& account) {
  if (m_config == nullptr) {
    outError = "no config";
    return false;
  }
  char title[64];
  if (account.length() > 0) {
    snprintf(title, sizeof(title), "[%s·%s] %s", m_config->bleDeviceName().c_str(),
             account.c_str(), kindTitle(kind));
  } else {
    snprintf(title, sizeof(title), "[%s] %s", m_config->bleDeviceName().c_str(), kindTitle(kind));
  }
  const bool ok = deliver(title, body.c_str(), channel, outStatus, outError);
  if (ok) noteSent(channel);
  LOG_I(kTag, "sendNow %s c%u -> %s (status=%d)", kindName(kind),
        static_cast<unsigned>(channel), ok ? "ok" : outError.c_str(),
        outStatus != nullptr ? *outStatus : 0);
  return ok;
}

void NotifyService::pump() {
  if (!active() || m_count == 0) return;
  // 链路没起来时纯粹等待：既不消耗发送次数也不推进退避，这样断网期间入队的
  // 通知在恢复后仍能补发，而不是被 4 次失败白白丢掉（队列容量 8 条）。
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t nowMs = millis();

  // 找队首：最早到期的一条
  int8_t chosen = -1;
  uint32_t best = 0xFFFFFFFFu;
  for (uint8_t i = 0; i < kQueueCapacity; i++) {
    if (!m_queue[i].used) continue;
    if (m_queue[i].nextTryMs > nowMs) continue;
    if (m_queue[i].nextTryMs <= best) {
      best = m_queue[i].nextTryMs;
      chosen = static_cast<int8_t>(i);
    }
  }
  if (chosen < 0) return;

  NotifyItem& item = m_queue[static_cast<uint8_t>(chosen)];
  int status = 0;
  String error;
  const bool ok = deliver(item.title, item.body, item.channel, &status, error);
  if (ok) {
    LOG_I(kTag, "sent %s c%u (status=%d, chTotal=%u)", kindName(item.kind),
          static_cast<unsigned>(item.channel), status,
          static_cast<unsigned>(m_sentTodayCh[item.channel < kChannelSlots ? item.channel : 0]));
    item.used = false;
    if (m_count > 0) m_count--;
    noteSent(item.channel);
    return;
  }

  item.attempts++;
  if (item.attempts >= kMaxAttempts) {
    LOG_W(kTag, "drop %s c%u after %u attempts: %s", kindName(item.kind),
          static_cast<unsigned>(item.channel), static_cast<unsigned>(item.attempts),
          error.c_str());
    item.used = false;
    if (m_count > 0) m_count--;
    return;
  }
  const uint32_t waitSec = kBackoffSeconds[item.attempts < 4 ? item.attempts : 3];
  item.nextTryMs = nowMs + waitSec * 1000UL;
  LOG_W(kTag, "send %s c%u failed (try %u): %s -> retry in %us", kindName(item.kind),
        static_cast<unsigned>(item.channel), static_cast<unsigned>(item.attempts), error.c_str(),
        static_cast<unsigned>(waitSec));
}

String NotifyService::statusJson() const {
  String out = "{\"on\":";
  out += (m_config != nullptr && m_config->notifyEnabled()) ? "true" : "false";
  out += ",\"style\":\"";
  const uint8_t style = m_config ? m_config->notifyStyle() : 2;
  out += (style == 0 ? "get" : (style == 1 ? "post_text" : "post_json"));
  out += "\",\"url\":\"";
  out += m_config ? m_config->notifyUrlMasked() : String("<unset>");
  out += "\",\"gap\":";
  out += String(m_config ? m_config->notifyGapSeconds() : 60);
  out += ",\"quota\":";
  out += String(m_config ? m_config->notifyDailyQuota() : 40);
  out += ",\"sentToday\":";
  out += String(static_cast<unsigned>(totalSentToday()));
  out += ",\"queued\":";
  out += String(static_cast<unsigned>(m_count));
  out += ",\"retryCount\":";
  out += String(static_cast<unsigned>(m_retryCount));
  // 默认正文键名（URL 里写了 {title}/{body} 模板时以模板为准，这里只是排障提示）
  out += ",\"contentField\":\"";
  out += (m_config != nullptr && isServerChan(m_config->notifyUrl())) ? "desp" : "body";
  out += "\"";
  // 额外通道列表（URL 已脱敏；sentToday 为该通道独立计数）
  out += ",\"channels\":[";
  if (m_config != nullptr) {
    for (size_t i = 0; i < m_config->notifyChannelCount(); i++) {
      if (i > 0) out += ",";
      const NotifyChannel& ch = m_config->notifyChannelAt(i);
      const uint8_t chSlot = static_cast<uint8_t>(i + 1);
      out += "{\"label\":\"";
      out += ch.label;
      out += "\",\"on\":";
      out += ch.enabled ? "true" : "false";
      out += ",\"style\":\"";
      out += (ch.style == 0 ? "get" : (ch.style == 1 ? "post_text" : "post_json"));
      out += "\",\"url\":\"";
      out += AppConfig::maskUrl(ch.url);
      out += "\",\"accounts\":\"";
      out += ch.accountsCsv;
      out += "\",\"sentToday\":";
      out += String(static_cast<unsigned>(chSlot < kChannelSlots ? m_sentTodayCh[chSlot] : 0));
      out += "}";
    }
  }
  out += "]}";
  return out;
}
