// Host-only Arduino/ESP fixtures. Never linked into the device firmware.
#pragma once
#include <cstdint>
#include <string>
#include <type_traits>
#include <ArduinoJson.h>
#include "app_version.h"

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  template <class T, typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
  explicit String(T value) : std::string(std::to_string(value)) {}
  String& append(const char* value) { std::string::append(value); return *this; }
  String& append(const char* value, size_t size) { std::string::append(value, size); return *this; }
};

enum class WifiState { Connected, Connecting, Failed, Idle };
enum class AuthState { Idle, Trusted, NeedMfa, Failed, InProgress };
namespace BoardProfile { inline const char* kId = "s3-n16r8"; }
namespace AppConst { inline int kNetTaskCore = 1; }
inline int64_t esp_timer_get_time() { return 90000000000LL; }
inline unsigned uxTaskGetStackHighWaterMark(void*) { return 6144; }
struct EspFixture {
  unsigned getFlashChipSize() const { return 16 * 1024 * 1024; }
  unsigned getPsramSize() const { return psram ? 8 * 1024 * 1024 : 0; }
  unsigned getFreeHeap() const { return 150000; }
  unsigned getHeapSize() const { return 300000; }
  unsigned getMinFreeHeap() const { return 80000; }
  unsigned getMaxAllocHeap() const { return 100000; }
  unsigned getFreePsram() const { return psram ? 8000000 : 0; }
  unsigned getMinFreePsram() const { return psram ? 7000000 : 0; }
  const char* getChipModel() const { return "ESP32-S3"; }
  bool psram = true;
} inline ESP;
struct WifiFixture {
  bool isConnected() const { return connected; }
  String SSID() const { return "宿舍 \\\" WiFi"; }
  bool connected = true;
} inline WiFi;
struct TimeFixture { bool isSynced() const { return true; } String nowString() const { return "2026-10-27 22:01:02"; } };
struct NetworkFixture {
  WifiState state() const { return WiFi.connected ? WifiState::Connected : WifiState::Idle; }
  String ip() const { return "192.0.2.10"; }
  int rssi() const { return -48; }
};
struct ConfigFixture {
  String toJsonSummary() const { return "{\"schedule\":{\"autoDaily\":false},\"wifiSsid\":\"fixture\"}"; }
  String lastResultJson() const { return "{\"date\":\"2026-10-26\",\"ok\":true}"; }
};
struct CookieFixture { String toJsonSummary() const { return "[]"; } };
struct AuthFixture { AuthState state() const { return AuthState::Trusted; } };
struct SchedulerFixture { String nextRunString() const { return "2026-10-27 22:00:00"; } };

inline String escapeJson(const String& text) {
  JsonDocument doc; doc.set(std::string(text));
  std::string encoded; serializeJson(doc, encoded);
  return encoded.substr(1, encoded.size() - 2);
}

class CommandRouter {
 public:
  String buildStatusJson() const;
  String legacyStatusJson() const;
  TimeFixture* m_time = nullptr;
  NetworkFixture* m_wifi = nullptr;
  ConfigFixture* m_config = nullptr;
  CookieFixture* m_cookies = nullptr;
  AuthFixture* m_auth = nullptr;
  SchedulerFixture* m_scheduler = nullptr;
};
