// ============================================================================
//  AppConfig 实现
// ============================================================================

#include "app_config.h"

#include <esp_mac.h>

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
constexpr const char* kNvsMultiOn = "multi_on";
constexpr const char* kNvsMultiGap = "multi_gap";

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

  // ---- 多人顺序打卡（多人版构建默认开启）----
#ifdef WANAN_MULTIUSER
  constexpr bool kMultiDefault = true;
#else
  constexpr bool kMultiDefault = false;
#endif
  m_multiEnabled = m_prefs.getBool(kNvsMultiOn, kMultiDefault);
  m_multiGapSeconds = static_cast<uint16_t>(m_prefs.getUShort(kNvsMultiGap, 20));

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
  // 注意：**不写 NVS** —— 全局配置仍保持"这台设备自己的账号"，多人只是运行期切换。
}

Result AppConfig::setMultiUserEnabled(bool enabled) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_multiEnabled = enabled;
  m_prefs.putBool(kNvsMultiOn, enabled);
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "multi-user checkin = %d", enabled ? 1 : 0);
  return Result::Ok;
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
  char buffer[512];
  snprintf(buffer, sizeof(buffer),
           "{\"device\":\"%s\",\"label\":\"%s\",\"wifiSsid\":\"%s\",\"wifiConfigured\":%s,\"user\":\"%s\","
           "\"passwordSet\":%s,\"fingerprintSet\":%s,"
           "\"location\":{\"lat\":%.6f,\"lng\":%.6f,\"address\":\"%s\"},"
           "\"schedule\":{\"checkinAt\":\"%s\",\"windowEnd\":\"%s\",\"jitterSeconds\":%u,"
           "\"autoDaily\":%s},\"multiUser\":%s,\"multiGap\":%u}",
           bleDeviceName().c_str(), m_deviceLabel.c_str(),
           m_wifi.ssid.c_str(), m_wifi.isConfigured() ? "true" : "false", maskedUser.c_str(),
           m_auth.password.length() ? "true" : "false", m_auth.hasFingerprint() ? "true" : "false",
           m_location.latitude, m_location.longitude, m_location.address.c_str(),
           m_schedule.checkinAt.c_str(), m_schedule.windowEnd.c_str(), m_schedule.jitterSeconds,
           m_schedule.autoDailyEnabled ? "true" : "false", m_multiEnabled ? "true" : "false",
           m_multiGapSeconds);
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
