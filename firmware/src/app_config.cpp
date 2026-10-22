// ============================================================================
//  AppConfig 实现
// ============================================================================

#include "app_config.h"

#include <esp_mac.h>

#include <ArduinoJson.h>

#include "account_store.h"
#include "app_version.h"
#include "logger.h"

namespace {

const char* const kTag = "app_config";

/// @brief NVS 键名（与 docs/firmware-design.md §8 一致）。
constexpr const char* kNvsWifiSsid = "wifi_ssid";
constexpr const char* kNvsWifiPass = "wifi_pass";
constexpr const char* kNvsAuthUser = "auth_user";
constexpr const char* kNvsAuthPass = "auth_pass";
constexpr const char* kNvsAuthBfp = "auth_bfp";
constexpr const char* kNvsLocLat = "loc_lat";
constexpr const char* kNvsLocLng = "loc_lng";
constexpr const char* kNvsLocAddr = "loc_addr";
constexpr const char* kNvsSchAt = "sch_at";
constexpr const char* kNvsSchStart = "sch_start";
constexpr const char* kNvsSchEnd = "sch_end";
constexpr const char* kNvsSchJitter = "sch_jitter";
constexpr const char* kNvsSchAuto = "sch_auto";
constexpr const char* kNvsTlsMode = "tls_mode";
constexpr const char* kNvsLastDate = "last_date";
constexpr const char* kNvsLastOk = "last_ok";
constexpr const char* kNvsLastMsg = "last_msg";
constexpr const char* kNvsLastTs = "last_ts";
constexpr const char* kNvsDevLabel = "dev_label";
// 旧键 multi_on（单人/多人开关）已废弃：账号列表是唯一事实来源，不再读写。
constexpr const char* kNvsMultiGap = "multi_gap";
constexpr const char* kNvsNotifyOn = "n_on";
constexpr const char* kNvsNotifyStyle = "n_style";
constexpr const char* kNvsNotifyUrl = "n_url";
constexpr const char* kNvsNotifyGap = "n_gap";
constexpr const char* kNvsNotifyQuota = "n_quota";
constexpr const char* kNvsNotifyChannels = "n_chans";

/// @brief 默认坐标：平台公布的打卡点中心（半径 130 m，距离 0 m，天然在范围内）。
/// @note 仅作兜底的默认值。请在蓝牙配网时用 loc.set 写入**你自己宿舍的实测坐标**
///       （实测坐标属于个人信息，不要写进任何公开文档或代码默认值）。
constexpr double kDefaultLatitude = 30.7549800;
constexpr double kDefaultLongitude = 103.9338830;
constexpr const char* kDefaultAddress = "四川省成都市郫都区";

/// @brief 校验自定义标签：只允许 A-Z a-z 0-9 - _ ，最长 12 字符（空串=清除）。
bool deviceLabelOk(const String& text) {
  if (text.length() > 12) return false;
  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

/// @brief 通道标签：同账号标签字符集，但不允许空。
bool channelLabelOk(const String& text) {
  return text.length() > 0 && deviceLabelOk(text);
}

/// @brief 规范化通道关联账号："all"（或空）= 全部；否则逗号分隔的合法标签列表。非法返回 false。
bool normalizeAccountsCsv(const String& in, String& out) {
  String s = in;
  s.trim();
  if (s.length() == 0 || s.equalsIgnoreCase("all")) {
    out = "all";
    return true;
  }
  out = "";
  int start = 0;
  while (true) {
    const int comma = s.indexOf(',', start);
    String token = comma < 0 ? s.substring(start) : s.substring(start, comma);
    token.trim();
    if (token.length() == 0 || !deviceLabelOk(token)) return false;
    if (out.length() > 0) out += ",";
    out += token;
    if (comma < 0) break;
    start = comma + 1;
  }
  return out.length() > 0;
}

/// @brief 把 "HH:MM" 解析为自 00:00 起的分钟数；非法返回 -1。
int parseHhmm(const String& text) {
  const int colon = text.indexOf(':');
  if (colon <= 0) return -1;
  const int hour = text.substring(0, colon).toInt();
  const int minute = text.substring(colon + 1).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
  return hour * 60 + minute;
}

/// @brief 读取字符串；键不存在时直接返回默认值（避免 Preferences 打印 NOT_FOUND 噪音）。
String getStringSafe(Preferences& prefs, const char* key, const char* fallback) {
  if (!prefs.isKey(key)) return String(fallback);
  return prefs.getString(key, fallback);
}

}  // namespace

Result AppConfig::begin(const char* nvsNamespace) {
  if (m_mutex == nullptr) {
    m_mutex = xSemaphoreCreateMutex();
  }
  if (!m_prefs.begin(nvsNamespace, false)) {
    LOG_E(kTag, "NVS open failed: %s", nvsNamespace);
    return Result::StorageError;
  }
  m_opened = true;
  const Result result = loadAll();
  LOG_I(kTag, "config loaded: ready=%d user=%s bfp=%d",
        isReady() ? 1 : 0, m_auth.studentId.length() ? "set" : "unset",
        m_auth.hasFingerprint() ? 1 : 0);
  return result;
}

Result AppConfig::loadAll() {
  // ---- WiFi ----
  m_wifi.ssid = getStringSafe(m_prefs, kNvsWifiSsid, "");
  m_wifi.password = getStringSafe(m_prefs, kNvsWifiPass, "");

  // ---- 设备标签（BLE 名）----
  m_deviceLabel = getStringSafe(m_prefs, kNvsDevLabel, "");

  // ---- 账号之间的间隔（打卡顺序由账号表决定，不再有单人/多人开关）----
  m_multiGapSeconds = static_cast<uint16_t>(m_prefs.getUShort(kNvsMultiGap, 20));

  // ---- 手机通知（默认关闭：没配 URL 之前不会有任何出站请求）----
  m_notifyEnabled = m_prefs.getBool(kNvsNotifyOn, false);
  m_notifyStyle = static_cast<uint8_t>(m_prefs.getUChar(kNvsNotifyStyle, 2));
  m_notifyUrl = getStringSafe(m_prefs, kNvsNotifyUrl, "");
  m_notifyGapSeconds = static_cast<uint16_t>(m_prefs.getUShort(kNvsNotifyGap, 60));
  m_notifyDailyQuota = static_cast<uint8_t>(m_prefs.getUChar(kNvsNotifyQuota, 40));

  // ---- 额外通知通道（n_chans：JSON [{l,u,s,en,a}]；loadAll 只在持锁上下文调用）----
  m_notifyChannels.clear();
  const String chansJson = getStringSafe(m_prefs, kNvsNotifyChannels, "");
  if (chansJson.length() > 0) {
    JsonDocument doc;
    if (!deserializeJson(doc, chansJson)) {
      for (JsonObject o : doc.as<JsonArray>()) {
        NotifyChannel ch;
        ch.label = o["l"] | "";
        ch.url = o["u"] | "";
        ch.style = static_cast<uint8_t>(o["s"] | 0);
        ch.enabled = o["en"] | true;
        ch.accountsCsv = o["a"] | "all";
        if (!channelLabelOk(ch.label) || ch.url.length() <= 6 || ch.style >= 3) continue;
        if (m_notifyChannels.size() >= AppConfig::kMaxExtraChannels) break;
        m_notifyChannels.push_back(ch);
      }
    } else {
      LOG_W(kTag, "n_chans json broken, channels ignored");
    }
  }

  // ---- 账号 ----
  m_auth.studentId = getStringSafe(m_prefs, kNvsAuthUser, "");
  m_auth.password = getStringSafe(m_prefs, kNvsAuthPass, "");
  m_auth.fingerprint = getStringSafe(m_prefs, kNvsAuthBfp, "");

  // ---- 定位（默认写入实测宿舍坐标）----
  m_location.latitude = m_prefs.getDouble(kNvsLocLat, kDefaultLatitude);
  m_location.longitude = m_prefs.getDouble(kNvsLocLng, kDefaultLongitude);
  m_location.address = getStringSafe(m_prefs, kNvsLocAddr, kDefaultAddress);

  // ---- 时段 ----
  m_schedule.windowStart = getStringSafe(m_prefs, kNvsSchStart, "21:00");
  m_schedule.checkinAt = getStringSafe(m_prefs, kNvsSchAt, "22:00");
  m_schedule.windowEnd = getStringSafe(m_prefs, kNvsSchEnd, "23:30");
  m_schedule.jitterSeconds = static_cast<uint16_t>(m_prefs.getUShort(kNvsSchJitter, 30));
  m_schedule.autoDailyEnabled = m_prefs.getBool(kNvsSchAuto, true);
  m_schedule.windowStartMinutes = parseHhmm(m_schedule.windowStart);
  m_schedule.checkinAtMinutes = parseHhmm(m_schedule.checkinAt);
  m_schedule.windowEndMinutes = parseHhmm(m_schedule.windowEnd);
  if (m_schedule.windowStartMinutes < 0) {
    m_schedule.windowStart = "21:00";
    m_schedule.windowStartMinutes = 21 * 60;
  }
  if (m_schedule.checkinAtMinutes < 0) {
    m_schedule.checkinAt = "22:00";
    m_schedule.checkinAtMinutes = 22 * 60;
  }
  if (m_schedule.windowEndMinutes < 0) {
    m_schedule.windowEnd = "23:30";
    m_schedule.windowEndMinutes = 23 * 60 + 30;
  }

  // 首次运行把默认定位写回 NVS，便于手机看到实际生效值
  if (!m_prefs.isKey(kNvsLocLat)) {
    m_prefs.putDouble(kNvsLocLat, m_location.latitude);
    m_prefs.putDouble(kNvsLocLng, m_location.longitude);
    m_prefs.putString(kNvsLocAddr, m_location.address);
  }
  return Result::Ok;
}

bool AppConfig::isReady() const {
  return m_wifi.isConfigured() && m_auth.isConfigured();
}

Result AppConfig::persistString(const char* key, const String& value) {
  if (!m_opened) return Result::NotReady;
  return m_prefs.putString(key, value) > 0 ? Result::Ok : Result::StorageError;
}

Result AppConfig::setWifi(const String& ssid, const String& password) {
  if (ssid.length() == 0) return Result::InvalidParam;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_wifi.ssid = ssid;
  m_wifi.password = password;
  Result result = persistString(kNvsWifiSsid, m_wifi.ssid);
  if (result == Result::Ok) result = persistString(kNvsWifiPass, m_wifi.password);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "wifi saved: ssid=%s", ssid.c_str());
  return result;
}

Result AppConfig::setAuth(const String& studentId, const String& password,
                          const String& fingerprint) {
  if (studentId.length() == 0 || password.length() == 0) return Result::InvalidParam;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_auth.studentId = studentId;
  m_auth.password = password;
  if (fingerprint.length() > 0) m_auth.fingerprint = fingerprint;
  Result result = persistString(kNvsAuthUser, m_auth.studentId);
  if (result == Result::Ok) result = persistString(kNvsAuthPass, m_auth.password);
  if (result == Result::Ok) result = persistString(kNvsAuthBfp, m_auth.fingerprint);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "auth saved: user=%s bfp=%s", studentId.c_str(),
        m_auth.hasFingerprint() ? "set" : "none");
  return result;
}

Result AppConfig::setLocation(double latitude, double longitude, const String& address) {
  if (latitude == 0.0 || longitude == 0.0) return Result::InvalidParam;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_location.latitude = latitude;
  m_location.longitude = longitude;
  if (address.length() > 0) m_location.address = address;
  m_prefs.putDouble(kNvsLocLat, latitude);
  m_prefs.putDouble(kNvsLocLng, longitude);
  Result result = persistString(kNvsLocAddr, m_location.address);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "location saved: %.6f,%.6f", latitude, longitude);
  return result;
}

Result AppConfig::setSchedule(const String& checkinAt, const String& windowEnd,
                              uint16_t jitterSeconds, const String& windowStart) {
  const int atMinutes = parseHhmm(checkinAt);
  const int endMinutes = parseHhmm(windowEnd);
  if (atMinutes < 0 || endMinutes < 0) return Result::InvalidParam;
  const bool hasStart = windowStart.length() > 0;
  const int startMinutes = hasStart ? parseHhmm(windowStart) : m_schedule.windowStartMinutes;
  if (hasStart && startMinutes < 0) return Result::InvalidParam;

  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_schedule.checkinAt = checkinAt;
  m_schedule.windowEnd = windowEnd;
  m_schedule.checkinAtMinutes = atMinutes;
  m_schedule.windowEndMinutes = endMinutes;
  m_schedule.windowStartMinutes = startMinutes;
  if (hasStart) m_schedule.windowStart = windowStart;
  m_schedule.jitterSeconds = jitterSeconds;
  Result result = persistString(kNvsSchAt, checkinAt);
  if (result == Result::Ok) result = persistString(kNvsSchEnd, windowEnd);
  if (result == Result::Ok && hasStart) result = persistString(kNvsSchStart, windowStart);
  if (result == Result::Ok) {
    m_prefs.putUShort(kNvsSchJitter, jitterSeconds);
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "schedule saved: window %s - %s, checkin %s, jitter %us",
        m_schedule.windowStart.c_str(), windowEnd.c_str(), checkinAt.c_str(), jitterSeconds);
  return result;
}

Result AppConfig::setAutoDaily(bool enabled) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_schedule.autoDailyEnabled = enabled;
  m_prefs.putBool(kNvsSchAuto, enabled);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "auto daily = %d", enabled ? 1 : 0);
  return Result::Ok;
}

Result AppConfig::reload() {
  if (!m_opened) return Result::NotReady;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const Result result = loadAll();
  m_activeLabel = "device";  // 回到 NVS 里那份"设备自己的"凭证
  if (m_mutex) xSemaphoreGive(m_mutex);
  return result;
}

void AppConfig::applyProfile(const CheckinProfile& profile) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_auth.studentId = profile.studentId;
  m_auth.password = profile.password;
  m_auth.fingerprint = profile.fingerprint;
  if (profile.hasLocation()) {
    m_location.latitude = profile.latitude;
    m_location.longitude = profile.longitude;
    if (profile.address.length() > 0) m_location.address = profile.address;
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  // 注意：**不写 NVS** —— 全局配置只是"没有账号表时"的兜底，执行器按账号运行期切换。
  m_activeLabel = profile.label.length() > 0 ? profile.label : String("device");
}

CheckinProfile AppConfig::deviceProfile() const {
  // 账号表为空时的兜底：用设备自身的 auth/loc 配置合成一条，
  // 语义上等价于"列表里的第一个用户"（老设备只配过 auth 也能直接跑）。
  CheckinProfile profile;
  profile.enabled = true;
  profile.label = m_deviceLabel.length() > 0 ? m_deviceLabel : String("self");
  profile.studentId = m_auth.studentId;
  profile.password = m_auth.password;
  profile.fingerprint = m_auth.fingerprint;
  profile.latitude = m_location.latitude;
  profile.longitude = m_location.longitude;
  profile.address = m_location.address;
  return profile;
}

Result AppConfig::setMultiGapSeconds(uint16_t seconds) {
  if (seconds > 600) return Result::InvalidParam;  // 上限 10 分钟，避免拖过打卡窗口
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_multiGapSeconds = seconds;
  m_prefs.putUShort(kNvsMultiGap, seconds);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "multi-user gap = %u s", seconds);
  return Result::Ok;
}

String AppConfig::bleDeviceName() const {
  char buffer[32];
  if (m_deviceLabel.length() > 0) {
    snprintf(buffer, sizeof(buffer), "%s-%s", FW_DEVICE_NAME_PREFIX, m_deviceLabel.c_str());
    return String(buffer);
  }
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
  }
  snprintf(buffer, sizeof(buffer), "%s-%02X%02X", FW_DEVICE_NAME_PREFIX, mac[4], mac[5]);
  return String(buffer);
}

Result AppConfig::setDeviceLabel(const String& label) {
  if (!deviceLabelOk(label)) {
    LOG_W(kTag, "invalid device label (only A-Z a-z 0-9 - _, max 12 chars)");
    return Result::InvalidParam;
  }
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_deviceLabel = label;
  const Result result = persistString(kNvsDevLabel, label);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "device label = '%s' -> ble name %s", label.c_str(), bleDeviceName().c_str());
  return result;
}

String AppConfig::toJsonSummary() const {
  // 学号只保留前 4 位；密码与指纹仅报是否存在
  String maskedUser = m_auth.studentId;
  if (maskedUser.length() > 4) {
    maskedUser = maskedUser.substring(0, 4) + "******";
  }
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  char buffer[704];
  snprintf(buffer, sizeof(buffer),
           "{\"device\":\"%s\",\"account\":\"%s\",\"label\":\"%s\",\"wifiSsid\":\"%s\",\"wifiConfigured\":%s,\"user\":\"%s\","
           "\"passwordSet\":%s,\"fingerprintSet\":%s,"
           "\"location\":{\"lat\":%.6f,\"lng\":%.6f,\"address\":\"%s\"},"
           "\"schedule\":{\"checkinAt\":\"%s\",\"windowEnd\":\"%s\",\"jitterSeconds\":%u,"
           "\"autoDaily\":%s},\"accountGap\":%u,"
           "\"notifyOn\":%s,\"notifyStyle\":\"%s\",\"notifyUrl\":\"%s\"}",
           bleDeviceName().c_str(), m_activeLabel.c_str(), m_deviceLabel.c_str(),
           m_wifi.ssid.c_str(), m_wifi.isConfigured() ? "true" : "false", maskedUser.c_str(),
           m_auth.password.length() ? "true" : "false", m_auth.hasFingerprint() ? "true" : "false",
           m_location.latitude, m_location.longitude, m_location.address.c_str(),
           m_schedule.checkinAt.c_str(), m_schedule.windowEnd.c_str(), m_schedule.jitterSeconds,
           m_schedule.autoDailyEnabled ? "true" : "false",
           m_multiGapSeconds,
            m_notifyEnabled ? "true" : "false",
            m_notifyStyle == 0 ? "get" : (m_notifyStyle == 1 ? "post_text" : "post_json"),
            notifyUrlMasked().c_str());
  if (m_mutex) xSemaphoreGive(m_mutex);
  return String(buffer);
}

String AppConfig::lastResultJson() {
  if (!m_opened) return String("{}");
  const String date = getStringSafe(m_prefs, kNvsLastDate, "");
  const bool success = m_prefs.getBool(kNvsLastOk, false);
  String message = getStringSafe(m_prefs, kNvsLastMsg, "");
  message.replace("\"", "'");  // 防止 JSON 被平台返回文案里的引号破坏
  const uint32_t timestamp = m_prefs.getULong(kNvsLastTs, 0);
  char buffer[320];
  snprintf(buffer, sizeof(buffer), "{\"date\":\"%s\",\"ok\":%s,\"message\":\"%s\",\"ts\":%lu}",
           date.c_str(), success ? "true" : "false", message.c_str(),
           static_cast<unsigned long>(timestamp));
  return String(buffer);
}

bool AppConfig::isLastResultOkFor(const String& date) {
  if (!m_opened || date.length() == 0) return false;
  const String storedDate = getStringSafe(m_prefs, kNvsLastDate, "");
  if (storedDate != date) return false;
  return m_prefs.getBool(kNvsLastOk, false);
}

Result AppConfig::setNotify(int enabled, int style, const String& url, int gap, int quota) {
  if (style >= 3 || gap > 3600 || quota > 200) return Result::InvalidParam;
  if (url.length() > 0 && url.length() <= 6) return Result::InvalidParam;  // 明显不是完整 URL
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  Result result = Result::Ok;
  if (enabled >= 0) {
    m_notifyEnabled = (enabled != 0);
    m_prefs.putBool(kNvsNotifyOn, m_notifyEnabled);
  }
  if (style >= 0) {
    m_notifyStyle = static_cast<uint8_t>(style);
    m_prefs.putUChar(kNvsNotifyStyle, m_notifyStyle);
  }
  if (url.length() > 0) {
    m_notifyUrl = url;
    result = persistString(kNvsNotifyUrl, m_notifyUrl);
  }
  if (gap >= 0) {
    m_notifyGapSeconds = static_cast<uint16_t>(gap);
    m_prefs.putUShort(kNvsNotifyGap, m_notifyGapSeconds);
  }
  if (quota >= 0) {
    m_notifyDailyQuota = static_cast<uint8_t>(quota);
    m_prefs.putUChar(kNvsNotifyQuota, m_notifyDailyQuota);
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  // 只报脱敏后的站点，绝不打印整条 URL（里面带着 token）
  LOG_I(kTag, "notify set: on=%d style=%u gap=%us quota=%u url=%s", m_notifyEnabled ? 1 : 0,
        m_notifyStyle, m_notifyGapSeconds, m_notifyDailyQuota, notifyUrlMasked().c_str());
  return result;
}

String AppConfig::notifyUrlMasked() const {
  return maskUrl(m_notifyUrl);
}

String AppConfig::maskUrl(const String& url) {
  if (url.length() == 0) return String("<unset>");
  const int schemeEnd = url.indexOf("://");
  const int hostStart = (schemeEnd < 0) ? 0 : schemeEnd + 3;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) pathStart = url.indexOf('?', hostStart);
  if (pathStart < 0) pathStart = url.length();
  const String head = url.substring(0, pathStart);  // scheme + host(+port)，不含路径与 token
  return head + "/***(len=" + String(url.length()) + ")";
}

int AppConfig::notifyChannelIndexByLabel(const String& label) const {
  for (size_t i = 0; i < m_notifyChannels.size(); i++) {
    if (m_notifyChannels[i].label == label) return static_cast<int>(i);
  }
  return -1;
}

bool AppConfig::notifyChannelMatchesAccount(size_t idx, const String& account) const {
  if (idx >= m_notifyChannels.size()) return false;
  const NotifyChannel& ch = m_notifyChannels[idx];
  if (ch.accountsCsv == "all") return true;
  if (account.length() == 0) return false;  // 全局事件只进 "all" 通道
  int start = 0;
  while (true) {
    const int comma = ch.accountsCsv.indexOf(',', start);
    const String token = comma < 0 ? ch.accountsCsv.substring(start) : ch.accountsCsv.substring(start, comma);
    if (token == account) return true;
    if (comma < 0) break;
    start = comma + 1;
  }
  return false;
}

bool AppConfig::hasActiveExtraChannel() const {
  for (size_t i = 0; i < m_notifyChannels.size(); i++) {
    if (m_notifyChannels[i].enabled && m_notifyChannels[i].url.length() > 0) return true;
  }
  return false;
}

Result AppConfig::persistNotifyChannels() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < m_notifyChannels.size(); i++) {
    const NotifyChannel& ch = m_notifyChannels[i];
    JsonObject o = arr.add<JsonObject>();
    o["l"] = ch.label;
    o["u"] = ch.url;
    o["s"] = ch.style;
    o["en"] = ch.enabled;
    o["a"] = ch.accountsCsv;
  }
  String json;
  serializeJson(doc, json);
  return persistString(kNvsNotifyChannels, json);
}

Result AppConfig::addOrUpdateNotifyChannel(const String& label, const String& url, int style,
                                           int on, const String& accountsCsv) {
  if (!channelLabelOk(label)) return Result::InvalidParam;
  if (style < 0 || style >= 3) return Result::InvalidParam;
  String normalized;
  if (!normalizeAccountsCsv(accountsCsv, normalized)) return Result::InvalidParam;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  Result result = Result::Ok;
  const int existing = notifyChannelIndexByLabel(label);
  if (existing >= 0) {
    NotifyChannel& ch = m_notifyChannels[existing];
    if (url.length() > 0) {
      if (url.length() <= 6) {
        result = Result::InvalidParam;  // 明显不是完整 URL
      } else {
        ch.url = url;
      }
    }
    if (result == Result::Ok) {
      ch.style = static_cast<uint8_t>(style);
      if (on >= 0) ch.enabled = (on != 0);
      ch.accountsCsv = normalized;
      result = persistNotifyChannels();
    }
  } else {
    if (url.length() <= 6) result = Result::InvalidParam;
    if (result == Result::Ok && m_notifyChannels.size() >= kMaxExtraChannels) {
      result = Result::InvalidParam;  // 通道已满（最多 5 条额外通道）
    }
    if (result == Result::Ok) {
      NotifyChannel ch;
      ch.label = label;
      ch.url = url;
      ch.style = static_cast<uint8_t>(style);
      ch.enabled = (on < 0) ? true : (on != 0);
      ch.accountsCsv = normalized;
      m_notifyChannels.push_back(ch);
      result = persistNotifyChannels();
      if (result != Result::Ok) m_notifyChannels.pop_back();
    }
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "notify chan '%s': style=%d on=%d accounts=%s url=%s", label.c_str(), style, on,
        normalized.c_str(), maskUrl(url).c_str());
  return result;
}

Result AppConfig::removeNotifyChannel(const String& label) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const int idx = notifyChannelIndexByLabel(label);
  Result result;
  if (idx < 0) {
    result = Result::InvalidParam;
  } else {
    m_notifyChannels.erase(m_notifyChannels.begin() + idx);
    result = persistNotifyChannels();
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  if (result == Result::Ok) LOG_I(kTag, "notify chan '%s' removed", label.c_str());
  return result;
}

Result AppConfig::setNotifyChannelEnabled(const String& label, bool on) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const int idx = notifyChannelIndexByLabel(label);
  Result result;
  if (idx < 0) {
    result = Result::InvalidParam;
  } else {
    m_notifyChannels[idx].enabled = on;
    result = persistNotifyChannels();
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  if (result == Result::Ok) LOG_I(kTag, "notify chan '%s' = %s", label.c_str(), on ? "on" : "off");
  return result;
}

Result AppConfig::linkNotifyChannel(const String& label, const String& accountsCsv) {
  String normalized;
  if (!normalizeAccountsCsv(accountsCsv, normalized)) return Result::InvalidParam;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const int idx = notifyChannelIndexByLabel(label);
  Result result;
  if (idx < 0) {
    result = Result::InvalidParam;
  } else {
    m_notifyChannels[idx].accountsCsv = normalized;
    result = persistNotifyChannels();
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  if (result == Result::Ok) {
    LOG_I(kTag, "notify chan '%s' linked to %s", label.c_str(), normalized.c_str());
  }
  return result;
}

String AppConfig::notifyChannelsJson() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < m_notifyChannels.size(); i++) {
    const NotifyChannel& ch = m_notifyChannels[i];
    JsonObject o = arr.add<JsonObject>();
    o["label"] = ch.label;
    o["on"] = ch.enabled;
    o["style"] = ch.style == 0 ? "get" : (ch.style == 1 ? "post_text" : "post_json");
    o["url"] = maskUrl(ch.url);
    o["accounts"] = ch.accountsCsv;
  }
  String json;
  serializeJson(doc, json);
  return json;
}

Result AppConfig::setLastResult(const String& date, bool success, const String& message) {
  if (!m_opened) return Result::NotReady;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_prefs.putString(kNvsLastDate, date);
  m_prefs.putBool(kNvsLastOk, success);
  m_prefs.putString(kNvsLastMsg, message);
  m_prefs.putULong(kNvsLastTs, static_cast<uint32_t>(time(nullptr)));
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "last result saved: date=%s ok=%d", date.c_str(), success ? 1 : 0);
  return Result::Ok;
}

Result AppConfig::factoryReset() {
  if (!m_opened) return Result::NotReady;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_prefs.clear();
  if (m_mutex) xSemaphoreGive(m_mutex);
  const Result result = loadAll();
  LOG_W(kTag, "factory reset done");
  return result;
}
