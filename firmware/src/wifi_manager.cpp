// ============================================================================
//  WifiManager 实现
// ============================================================================

#include "wifi_manager.h"

#include <WiFi.h>

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
  WiFi.begin(wifi.ssid.c_str(), wifi.password.c_str());
  m_connecting = true;
  m_connectDeadlineMs = millis() + kConnectTimeoutMs;
  setState(WifiState::Connecting, wifi.ssid);
  LOG_I(kTag, "connecting to %s ...", wifi.ssid.c_str());
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
  const int count = WiFi.scanNetworks();
  if (count < 0) {
    outJson = "[]";
    return Result::NetworkError;
  }
  outJson = "[";
  for (int i = 0; i < count; i++) {
    if (i > 0) outJson += ',';
    outJson += "{\"ssid\":\"";
    outJson += WiFi.SSID(i);
    outJson += "\",\"rssi\":";
    outJson += String(WiFi.RSSI(i));
    outJson += ",\"channel\":";
    outJson += String(WiFi.channel(i));
    outJson += ",\"enc\":";
    outJson += String(static_cast<int>(WiFi.encryptionType(i)));
    outJson += '}';
  }
  outJson += ']';
  WiFi.scanDelete();
  LOG_I(kTag, "scan done: %d ap", count);
  return Result::Ok;
}
