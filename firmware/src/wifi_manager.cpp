#include <ArduinoJson.h>
// ============================================================================
//  WifiManager 实现
// ============================================================================

#include "wifi_manager.h"

#include <WiFi.h>
#include <esp_wifi.h>

#include "logger.h"

namespace {

const char* const kTag = "wifi_manager";

/// @brief 退避上限。
constexpr uint32_t kMaxBackoffMs = 60000;

/// @brief 单次连接尝试的超时。
constexpr uint32_t kConnectTimeoutMs = 15000;

}  // namespace

Result WifiManager::begin(AppConfig& config) {
  m_config = &config;
  WiFi.persistent(false);       // 不把凭据写入 NVS（我们自己在 AppConfig 里管）
  WiFi.mode(WIFI_STA);
  // 显式设置国家码 CN：默认全球安全集可能只扫 1-11 信道，
  // 若路由器 2.4G 落在 12/13 信道，扫描结果里会完全看不到该 AP（NO_AP_FOUND）。
  {
    wifi_country_t country = {};
    memcpy(country.cc, "CN", 2);
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = 84;  // 20 dBm
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    const esp_err_t err = esp_wifi_set_country(&country);
    LOG_I(kTag, "wifi country set to CN (ch1-13), err=%d", static_cast<int>(err));
  }
  // 注意：BLE 共存时不能关闭 modem sleep（ESP-IDF 会 abort），因此保持默认省电模式。
  WiFi.setAutoReconnect(false); // 重连策略由本模块统一管理
  WiFi.disconnect(true, false);
  setState(WifiState::Idle, "idle");
  LOG_I(kTag, "wifi manager ready, configured=%d", config.wifi().isConfigured() ? 1 : 0);
  return Result::Ok;
}

void WifiManager::setState(WifiState state, const String& info) {
  if (m_state == state && state != WifiState::Connected) {
    return;
  }
  m_state = state;
  if (m_callback) {
    m_callback(state, info);
  }
}

void WifiManager::startConnect() {
  if (m_config == nullptr || !m_config->wifi().isConfigured()) {
    setState(WifiState::Failed, "wifi not configured");
    return;
  }
  const WifiConfig& wifi = m_config->wifi();

  // 隐藏 SSID 时，扫描结果里没有名字，ESP-IDF 的扫描-连接流程会返回 NO_AP_FOUND。
  // 已知 BSSID + 信道时直接指定目标，跳过扫描（这是隐藏 AP 最可靠的连法）。
  uint8_t bssidBytes[6] = {0};
  bool haveBssid = false;
  if (wifi.hasBssid()) {
    unsigned int v[6] = {0};
    if (sscanf(wifi.bssid.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5]) == 6) {
      for (int i = 0; i < 6; i++) bssidBytes[i] = static_cast<uint8_t>(v[i]);
      haveBssid = true;
    }
  }

  if (haveBssid) {
    LOG_I(kTag, "connecting to %s (bssid=%s ch=%u) ...", wifi.ssid.c_str(), wifi.bssid.c_str(),
          static_cast<unsigned>(wifi.channel));
    WiFi.begin(wifi.ssid.c_str(), wifi.password.c_str(), wifi.channel, bssidBytes);
  } else {
    WiFi.begin(wifi.ssid.c_str(), wifi.password.c_str());
    LOG_I(kTag, "connecting to %s ...", wifi.ssid.c_str());
  }
  m_connecting = true;
  m_connectDeadlineMs = millis() + kConnectTimeoutMs;
  setState(WifiState::Connecting, wifi.ssid);
}

Result WifiManager::connect(uint32_t timeoutMs) {
  if (m_config == nullptr) return Result::NotReady;
  if (!m_config->wifi().isConfigured()) return Result::NotFound;
  if (WiFi.status() == WL_CONNECTED) {
    setState(WifiState::Connected, ip());
    return Result::Ok;
  }

  startConnect();
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (WiFi.status() == WL_CONNECTED) {
      m_connecting = false;
      m_retryCount = 0;
      m_nextRetryMs = 0;
      setState(WifiState::Connected, ip());
      LOG_I(kTag, "connected: ip=%s rssi=%d", ip().c_str(), static_cast<int>(rssi()));
      return Result::Ok;
    }
    delay(100);  // 让出 CPU，避免看门狗
  }

  m_connecting = false;
  m_retryCount++;
  m_nextRetryMs = millis() + 1000;
  setState(WifiState::Failed, "connect timeout");
  LOG_W(kTag, "connect timeout after %u ms", static_cast<unsigned>(timeoutMs));
  return Result::NetworkError;
}

void WifiManager::disconnect() {
  WiFi.disconnect(true, false);
  m_connecting = false;
  m_nextRetryMs = 0;
  m_retryCount = 0;
  setState(WifiState::Idle, "disconnected");
}

void WifiManager::poll() {
  if (m_config == nullptr) return;

  // 已连接：清空重连状态
  if (WiFi.status() == WL_CONNECTED) {
    if (m_state != WifiState::Connected) {
      LOG_I(kTag, "connected: ip=%s", ip().c_str());
      setState(WifiState::Connected, ip());
    }
    m_connecting = false;
    m_retryCount = 0;
    m_nextRetryMs = 0;
    return;
  }

  // 掉线：记录一次状态变化
  if (m_state == WifiState::Connected) {
    LOG_W(kTag, "link lost, will reconnect");
    setState(WifiState::Connecting, "link lost");
  }

  // 连接中且未超时：继续等待
  if (m_connecting) {
    if (static_cast<int32_t>(millis() - m_connectDeadlineMs) < 0) return;
    m_connecting = false;
    LOG_W(kTag, "connect attempt timed out");
  }

  // 退避等待中
  if (m_nextRetryMs != 0 && static_cast<int32_t>(millis() - m_nextRetryMs) < 0) return;

  // 指数退避：1s, 2s, 4s, 8s, 16s, 32s, 60s 封顶
  uint32_t backoff = 1000UL << (m_retryCount > 5 ? 5 : m_retryCount);
  if (backoff > kMaxBackoffMs) backoff = kMaxBackoffMs;
  m_retryCount++;
  m_nextRetryMs = millis() + backoff;
  startConnect();
}

String WifiManager::ip() const {
  if (WiFi.status() != WL_CONNECTED) return String("0.0.0.0");
  return WiFi.localIP().toString();
}

int32_t WifiManager::rssi() const { return WiFi.RSSI(); }

Result WifiManager::scan(String& outJson) {
  // show_hidden=true：隐藏 SSID 也要列出来，否则像 "202" 这种隐藏网络会完全消失
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    outJson = "[]";
    return Result::NetworkError;
  }
  JsonDocument doc;
  JsonArray aps=doc.to<JsonArray>();
  for (int i=0;i<count;++i) {
    JsonObject ap=aps.add<JsonObject>();
    ap["ssid"]=WiFi.SSID(i); ap["rssi"]=WiFi.RSSI(i);
    ap["channel"]=WiFi.channel(i); ap["enc"]=static_cast<int>(WiFi.encryptionType(i));
    ap["bssid"]=WiFi.BSSIDstr(i);
  }
  if (doc.overflowed()) { WiFi.scanDelete(); return Result::InternalError; }
  serializeJson(doc,outJson);
  WiFi.scanDelete();
  LOG_I(kTag, "scan done: %d ap", count);
  return Result::Ok;
}
