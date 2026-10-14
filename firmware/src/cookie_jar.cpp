// ============================================================================
//  CookieJar 实现
// ============================================================================

#include "cookie_jar.h"

#include <ArduinoJson.h>

#include "logger.h"

namespace {

const char* const kTag = "cookie_jar";

/// @brief NVS 键名。
constexpr const char* kNvsJarKey = "jar";

/// @brief 从 URL 中取主机名（小写）。
String hostOf(const String& url) {
  int start = url.indexOf("://");
  start = (start < 0) ? 0 : start + 3;
  int end = url.indexOf('/', start);
  if (end < 0) end = url.length();
  const int colon = url.indexOf(':', start);
  if (colon > 0 && colon < end) end = colon;
  String host = url.substring(start, end);
  host.toLowerCase();
  return host;
}

/// @brief 从 URL 中取路径（含前导 '/'）。
String pathOf(const String& url) {
  int start = url.indexOf("://");
  start = (start < 0) ? 0 : start + 3;
  const int slash = url.indexOf('/', start);
  if (slash < 0) return String("/");
  return url.substring(slash);
}

/// @brief 解析 Expires（形如 "Fri, 29 Oct 2094 16:40:05 GMT"）为时间戳；失败返回 0。
time_t parseExpires(const String& text) {
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  char monthText[4] = {0};
  if (sscanf(text.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, monthText, &year, &hour, &minute,
             &second) != 6) {
    return 0;
  }
  int month = -1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(monthText, kMonths[i], 3) == 0) {
      month = i;
      break;
    }
  }
  if (month < 0 || year < 1970) return 0;

  struct tm parts = {};
  parts.tm_mday = day;
  parts.tm_mon = month;
  parts.tm_year = year - 1900;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = second;
  parts.tm_isdst = 0;
  // 服务器给的是 GMT；这里按 UTC 计算后再减去本地时区偏移，得到正确的绝对时刻
  const time_t asUtc = mktime(&parts);
  return asUtc - static_cast<time_t>(AppConst::kTimezoneOffsetSeconds);
}

}  // namespace

Result CookieJar::begin(const char* nvsNamespace) {
  if (m_mutex == nullptr) {
    m_mutex = xSemaphoreCreateMutex();
  }
  if (!m_prefs.begin(nvsNamespace, false)) {
    LOG_E(kTag, "NVS open failed: %s", nvsNamespace);
    return Result::StorageError;
  }
  m_opened = true;
  const Result result = restore();
  LOG_I(kTag, "cookie jar ready: %u entries", static_cast<unsigned>(m_cookies.size()));
  return result;
}

std::vector<Cookie>::iterator CookieJar::findCookie(const String& name, const String& domain) {
  const String lowered = domain;
  for (auto it = m_cookies.begin(); it != m_cookies.end(); ++it) {
    if (it->name == name && (lowered.length() == 0 || it->domain == lowered)) {
      return it;
    }
  }
  return m_cookies.end();
}

void CookieJar::evictIfNeeded() {
  while (m_cookies.size() > AppConst::kMaxCookies) {
    // 淘汰最久未使用者
    size_t oldest = 0;
    for (size_t i = 1; i < m_cookies.size(); i++) {
      if (m_cookies[i].lastUsedMs < m_cookies[oldest].lastUsedMs) oldest = i;
    }
    LOG_W(kTag, "evict cookie: %s@%s", m_cookies[oldest].name.c_str(),
          m_cookies[oldest].domain.c_str());
    m_cookies.erase(m_cookies.begin() + oldest);
  }
}

void CookieJar::set(const String& name, const String& value, const String& domain,
                    const String& path, time_t expires) {
  if (name.length() == 0) return;
  String lowered = domain;
  lowered.toLowerCase();

  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  auto it = findCookie(name, lowered);
  if (it != m_cookies.end()) {
    it->value = value;
    it->path = path;
    it->expires = expires;
    it->lastUsedMs = millis();
  } else {
    Cookie cookie;
    cookie.name = name;
    cookie.value = value;
    cookie.domain = lowered;
    cookie.path = path;
    cookie.expires = expires;
    cookie.lastUsedMs = millis();
    m_cookies.push_back(cookie);
  }
  evictIfNeeded();
  persist();
  if (m_mutex) xSemaphoreGive(m_mutex);
}

String CookieJar::get(const String& name, const String& domain) const {
  String lowered = domain;
  lowered.toLowerCase();
  for (const auto& cookie : m_cookies) {
    if (cookie.name == name && (lowered.length() == 0 || cookie.domain == lowered)) {
      return cookie.value;
    }
  }
  return String();
}

String CookieJar::headerFor(const String& url) const {
  const String host = hostOf(url);
  const String path = pathOf(url);
  const time_t nowSeconds = time(nullptr);
  String header;
  for (const auto& cookie : m_cookies) {
    if (cookie.value.length() == 0) continue;
    const bool domainOk =
        (host == cookie.domain) || host.endsWith("." + cookie.domain) || cookie.domain.length() == 0;
    if (!domainOk) continue;
    if (!path.startsWith(cookie.path)) continue;
    if (cookie.expires != 0 && nowSeconds > cookie.expires) continue;
    if (header.length() > 0) header += "; ";
    header += cookie.name;
    header += '=';
    header += cookie.value;
  }
  return header;
}

String CookieJar::namesFor(const String& url) const {
  const String host = hostOf(url);
  const String path = pathOf(url);
  const time_t nowSeconds = time(nullptr);
  String out;
  for (const auto& cookie : m_cookies) {
    if (cookie.value.length() == 0) continue;
    const bool domainOk =
        (host == cookie.domain) || host.endsWith("." + cookie.domain) || cookie.domain.length() == 0;
    if (!domainOk) continue;
    if (!path.startsWith(cookie.path)) continue;
    if (cookie.expires != 0 && nowSeconds > cookie.expires) continue;
    if (out.length() > 0) out += ',';
    out += cookie.name;
  }
  return out;
}

int CookieJar::applySetCookie(const String& url, const String& setCookieValue) {
  const int semicolon = setCookieValue.indexOf(';');
  const String pair = (semicolon < 0) ? setCookieValue : setCookieValue.substring(0, semicolon);
  const int equals = pair.indexOf('=');
  if (equals <= 0) return 0;

  const String name = pair.substring(0, equals);
  const String value = pair.substring(equals + 1);

  String domain = hostOf(url);
  String path = "/";
  time_t expires = 0;
  bool expired = false;

  int cursor = (semicolon < 0) ? setCookieValue.length() : semicolon + 1;
  while (cursor < static_cast<int>(setCookieValue.length())) {
    int next = setCookieValue.indexOf(';', cursor);
    if (next < 0) next = setCookieValue.length();
    String attribute = setCookieValue.substring(cursor, next);
    attribute.trim();
    const int eq = attribute.indexOf('=');
    if (eq > 0) {
      String key = attribute.substring(0, eq);
      String val = attribute.substring(eq + 1);
      key.trim();
      val.trim();
      key.toLowerCase();
      if (key == "domain") {
        String lowered = val;
        lowered.toLowerCase();
        if (lowered.startsWith(".")) lowered = lowered.substring(1);
        domain = lowered;
      } else if (key == "path") {
        path = val.length() ? val : "/";
      } else if (key == "max-age") {
        const long seconds = val.toInt();
        if (seconds <= 0) {
          expired = true;
        } else if (time(nullptr) > 1600000000) {
          expires = time(nullptr) + seconds;
        } else {
          // 尚未校时：先按会话 Cookie 保存，避免被误判为已过期而丢会话
          expires = 0;
        }
      } else if (key == "expires") {
        if (!expired && expires == 0 && time(nullptr) > 1600000000) {
          const time_t parsed = parseExpires(val);
          if (parsed != 0) {
            expires = parsed;
            if (parsed < time(nullptr)) expired = true;
          }
        }
      }
    }
    cursor = next + 1;
  }

  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  if (value.length() == 0 || expired) {
    auto it = findCookie(name, domain);
    if (it != m_cookies.end()) {
      m_cookies.erase(it);
      persist();
      if (m_mutex) xSemaphoreGive(m_mutex);
      LOG_I(kTag, "Set-Cookie %s@%s -> removed (empty/expired)", name.c_str(), domain.c_str());
      return 0;
    }
    if (m_mutex) xSemaphoreGive(m_mutex);
    return 0;
  }

  auto it = findCookie(name, domain);
  if (it != m_cookies.end()) {
    it->value = value;
    it->path = path;
    it->expires = expires;
    it->lastUsedMs = millis();
  } else {
    Cookie cookie;
    cookie.name = name;
    cookie.value = value;
    cookie.domain = domain;
    cookie.path = path;
    cookie.expires = expires;
    cookie.lastUsedMs = millis();
    m_cookies.push_back(cookie);
  }
  evictIfNeeded();
  persist();
  if (m_mutex) xSemaphoreGive(m_mutex);

  LOG_I(kTag, "Set-Cookie %s@%s path=%s ttl=%ld s", name.c_str(), domain.c_str(), path.c_str(),
        (expires != 0) ? static_cast<long>(expires - time(nullptr)) : -1L);
  return 1;
}

Result CookieJar::persist() {
  if (!m_opened) return Result::NotReady;
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (const auto& cookie : m_cookies) {
    JsonObject item = array.add<JsonObject>();
    item["n"] = cookie.name;
    item["v"] = cookie.value;
    item["d"] = cookie.domain;
    item["p"] = cookie.path;
    item["e"] = static_cast<long>(cookie.expires);
  }
  String serialized;
  serializeJson(doc, serialized);
  const size_t written = m_prefs.putString(kNvsJarKey, serialized);
  if (written == 0 && serialized.length() > 0) {
    LOG_E(kTag, "persist failed (%u bytes)", static_cast<unsigned>(serialized.length()));
    return Result::StorageError;
  }
  return Result::Ok;
}

Result CookieJar::restore() {
  if (!m_opened) return Result::NotReady;
  m_cookies.clear();
  const String serialized = m_prefs.getString(kNvsJarKey, "");
  if (serialized.length() == 0) return Result::Ok;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, serialized);
  if (error) {
    LOG_W(kTag, "restore parse failed: %s", error.c_str());
    return Result::StorageError;
  }
  for (JsonObject item : doc.as<JsonArray>()) {
    Cookie cookie;
    cookie.name = item["n"].as<const char*>();
    cookie.value = item["v"].as<const char*>();
    cookie.domain = item["d"].as<const char*>();
    cookie.path = item["p"] | "/";
    cookie.expires = item["e"].as<long>();
    m_cookies.push_back(cookie);
  }
  return Result::Ok;
}

void CookieJar::clear() {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_cookies.clear();
  if (m_opened) m_prefs.remove(kNvsJarKey);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_W(kTag, "cookie jar cleared");
}

void CookieJar::remove(const String& name, const String& domain) {
  if (name.length() == 0) return;
  String lowered = domain;
  lowered.toLowerCase();

  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  auto it = findCookie(name, lowered);
  if (it != m_cookies.end()) {
    m_cookies.erase(it);
    persist();
    LOG_I(kTag, "cookie removed: %s@%s", name.c_str(),
          lowered.length() ? lowered.c_str() : "(any)");
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
}

String CookieJar::toJsonSummary() const {
  const time_t nowSeconds = time(nullptr);
  String out = "[";
  for (size_t i = 0; i < m_cookies.size(); i++) {
    const Cookie& cookie = m_cookies[i];
    if (i > 0) out += ',';
    long remainingDays = -1;
    if (cookie.expires != 0) {
      remainingDays = static_cast<long>((cookie.expires - nowSeconds) / 86400);
    }
    out += "{\"name\":\"";
    out += cookie.name;
    out += "\",\"domain\":\"";
    out += cookie.domain;
    out += "\",\"session\":";
    out += (cookie.expires == 0) ? "true" : "false";
    out += ",\"expiresInDays\":";
    out += String(remainingDays);
    out += '}';
  }
  out += ']';
  return out;
}
