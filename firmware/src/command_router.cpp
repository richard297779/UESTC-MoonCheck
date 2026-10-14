// ============================================================================
//  CommandRouter 实现
// ============================================================================

#include "command_router.h"

#include <esp_mac.h>
#include <esp_system.h>

#include "app_version.h"
#include "logger.h"

namespace {

const char* const kTag = "router";

/// @brief 队列长度。
constexpr UBaseType_t kQueueLength = 8;

/// @brief 转义 JSON 字符串。
String escapeJson(const String& text) {
  String out;
  out.reserve(text.length() + 16);
  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (static_cast<uint8_t>(c) < 0x20) {
      // 控制字符忽略
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- 生命周期

Result CommandRouter::begin(AppConfig& config, CookieJar& cookies, WifiManager& wifi,
                            TimeService& time, CasAuth& auth, CheckinApi& api, HttpClient& http,
                            CheckinService& checkin, BleService& ble, TaskScheduler& scheduler,
                            StatusLed& led, AccountStore& accounts, MultiCheckin& multi) {
  m_config = &config;
  m_cookies = &cookies;
  m_wifi = &wifi;
  m_time = &time;
  m_auth = &auth;
  m_api = &api;
  m_http = &http;
  m_checkin = &checkin;
  m_ble = &ble;
  m_scheduler = &scheduler;
  m_led = &led;
  m_accounts = &accounts;
  m_multi = &multi;
  // 多人编排器的事件走同一个 BLE 出口
  multi.setEventSink(
      [this](const String& type, const String& payload) { emitEvent(type, payload); });

  m_queue = xQueueCreate(kQueueLength, sizeof(WorkItem));
  if (m_queue == nullptr) {
    LOG_E(kTag, "queue create failed");
    return Result::InternalError;
  }
  LOG_I(kTag, "command router ready");
  return Result::Ok;
}

bool CommandRouter::enqueue(WorkKind kind, const String& payload, bool force, int32_t requestId) {
  if (m_queue == nullptr) return false;
  WorkItem item;
  item.kind = kind;
  item.force = force;
  item.requestId = requestId;
  const size_t copyLength =
      payload.length() < sizeof(item.payload) - 1 ? payload.length() : sizeof(item.payload) - 1;
  memcpy(item.payload, payload.c_str(), copyLength);
  item.payload[copyLength] = '\0';
  if (xQueueSend(m_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
    LOG_W(kTag, "queue full, drop kind=%u", static_cast<unsigned>(kind));
    return false;
  }
  return true;
}

void CommandRouter::processPending() {
  WorkItem item;
  if (xQueueReceive(m_queue, &item, pdMS_TO_TICKS(200)) == pdTRUE) {
    execute(item);
  }
}

// ---------------------------------------------------------------- 事件与回执

void CommandRouter::emitEvent(const String& type, const String& payloadJson) {
  String frame = "{\"evt\":\"";
  frame += type;
  frame += '"';
  if (payloadJson.length() > 0) {
    frame += ',';
    frame += payloadJson;
  }
  frame += '}';
  LOG_I(kTag, "event %s %s", type.c_str(), payloadJson.c_str());
  if (m_ble != nullptr) m_ble->notifyLine(frame);
}

void CommandRouter::replyOk(int32_t id, const String& message, const String& dataJson) {
  String frame = "{\"id\":";
  frame += String(id);
  frame += ",\"evt\":\"ok\",\"msg\":\"";
  frame += escapeJson(message);
  frame += '"';
  if (dataJson.length() > 0) {
    frame += ",\"data\":";
    frame += dataJson;
  }
  frame += '}';
  if (m_ble != nullptr) m_ble->notifyLine(frame);
}

void CommandRouter::replyError(int32_t id, Result result, const String& message) {
  String frame = "{\"id\":";
  frame += String(id);
  frame += ",\"evt\":\"error\",\"code\":\"";
  frame += resultCode(result);
  frame += "\",\"msg\":\"";
  frame += escapeJson(message);
  frame += "\"}";
  if (m_ble != nullptr) m_ble->notifyLine(frame);
}

void CommandRouter::setBusy(bool busy) {
  if (m_led == nullptr) return;
  if (busy) {
    m_led->setPattern(LedPattern::Busy);
  } else {
    const bool online = (m_wifi != nullptr) && (m_wifi->state() == WifiState::Connected);
    m_led->setPattern(online ? LedPattern::WifiOk : LedPattern::NotConfigured);
  }
}

void CommandRouter::flashResult(bool ok) {
  if (m_led == nullptr) return;
  m_led->setTemporary(ok ? LedPattern::Success : LedPattern::Failure, ok ? 3000 : 6000);
}

String CommandRouter::buildStatusJson() const {
  const bool timeSynced = (m_time != nullptr) && m_time->isSynced();
  String wifiState = "unknown";
  if (m_wifi != nullptr) {
    switch (m_wifi->state()) {
      case WifiState::Connected: wifiState = "connected"; break;
      case WifiState::Connecting: wifiState = "connecting"; break;
      case WifiState::Failed: wifiState = "failed"; break;
      case WifiState::Idle: wifiState = "idle"; break;
    }
  }

  String out = "{\"fw\":\"";
  out += FW_VERSION;
  out += "\",\"proto\":";
  out += String(FW_PROTO_VERSION);
  out += ",\"chip\":{\"flashMB\":";
  out += String(ESP.getFlashChipSize() / (1024 * 1024));
  out += ",\"psramKB\":";
  out += String(ESP.getPsramSize() / 1024);
  out += ",\"freeHeapKB\":";
  out += String(ESP.getFreeHeap() / 1024);
  out += "},\"net\":{\"state\":\"";
  out += wifiState;
  out += "\",\"ip\":\"";
  out += (m_wifi != nullptr) ? m_wifi->ip() : String("0.0.0.0");
  out += "\",\"rssi\":";
  out += String((m_wifi != nullptr) ? m_wifi->rssi() : 0);
  out += ",\"timeSynced\":";
  out += timeSynced ? "true" : "false";
  out += ",\"deviceTime\":\"";
  out += (m_time != nullptr) ? m_time->nowString() : String("-");
  out += "\"},\"cfg\":";
  out += (m_config != nullptr) ? m_config->toJsonSummary() : String("{}");
  out += ",\"cookies\":";
  out += (m_cookies != nullptr) ? m_cookies->toJsonSummary() : String("[]");
  out += ",\"auth\":{\"state\":\"";
  if (m_auth != nullptr) {
    switch (m_auth->state()) {
      case AuthState::Idle: out += "idle"; break;
      case AuthState::Trusted: out += "trusted"; break;
      case AuthState::NeedMfa: out += "need_mfa"; break;
      case AuthState::Failed: out += "failed"; break;
      default: out += "in_progress"; break;
    }
  } else {
    out += "unknown";
  }
  out += "\"},\"schedule\":{\"nextRun\":\"";
  out += (m_scheduler != nullptr) ? m_scheduler->nextRunString() : String("-");
  out += "\"},\"last\":";
  out += (m_config != nullptr) ? m_config->lastResultJson() : String("{}");
  out += '}';
  return out;
}

// ---------------------------------------------------------------- 指令解析

void CommandRouter::onBleLine(const String& line) {
  // ⚠️ 本函数运行在 NimBLE host 任务（栈约 4 KB）：
  //    只允许入队，绝不能做 JSON 解析或字符串格式化。
  //    （历史故障：status 在此同步处理，snprintf("%f") 走 _dtoa_r 触发栈溢出崩溃）
  if (!enqueue(WorkKind::Command, line)) {
    LOG_W(kTag, "queue full, dropping ble command");
  }
}

void CommandRouter::dispatchCommand(const String& line) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, line);
  if (error) {
    LOG_W(kTag, "bad json: %s", error.c_str());
    replyError(0, Result::InvalidParam, String("bad json: ") + error.c_str());
    return;
  }
  const String op = doc["op"] | "";
  const int32_t id = doc["id"] | 0;
  if (op.length() == 0) {
    replyError(id, Result::InvalidParam, "missing op");
    return;
  }
  const JsonObjectConst request = doc.as<JsonObjectConst>();

  if (op == "hello") {
    handleHello(id);
  } else if (op == "status") {
    handleStatus(id);
  } else if (op == "wifi.set") {
    const String ssid = doc["ssid"] | "";
    const String password = doc["pass"] | "";
    if (ssid.length() == 0) {
      replyError(id, Result::InvalidParam, "ssid required");
      return;
    }
    const Result result = m_config->setWifi(ssid, password);
    if (result != Result::Ok) {
      replyError(id, result, "save wifi failed");
      return;
    }
    if (!enqueue(WorkKind::WifiConnect, "", false, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "wifi.scan") {
    if (!enqueue(WorkKind::WifiScan, "", false, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "auth.set") {
    handleAuthSet(request, id);
  } else if (op == "loc.set") {
    handleLocationSet(request, id);
  } else if (op == "sched.set") {
    handleScheduleSet(request, id);
  } else if (op == "auto.set") {
    handleAutoSet(request, id);
  } else if (op == "account.add") {
    handleAccountAdd(request, id);
  } else if (op == "account.list") {
    if (m_accounts == nullptr) {
      replyError(id, Result::NotReady, "account store unavailable");
    } else {
      replyOk(id, "accounts", m_accounts->toJsonSummary());
    }
  } else if (op == "account.remove") {
    const String label = request["label"] | "";
    if (m_accounts == nullptr || label.length() == 0) {
      replyError(id, Result::InvalidParam, "label required");
    } else {
      const Result result = m_accounts->remove(label);
      if (result != Result::Ok) {
        replyError(id, result, "account not found");
      } else {
        replyOk(id, "account removed");
      }
    }
  } else if (op == "account.clear") {
    if (m_accounts != nullptr) m_accounts->clearAll();
    replyOk(id, "accounts cleared");
  } else if (op == "multi.set") {
    handleMultiSet(request, id);
  } else if (op == "test.multi") {
    const bool force = doc["force"] | false;
    const bool probeOnly = doc["probe"] | false;
    if (!enqueue(WorkKind::MultiCheckin, probeOnly ? "probe" : "", force, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "name.set") {
    // 自定义 BLE 广播名：{"op":"name.set","name":"A01"} → 广播名变成 WANAN-A01
    // name 为空串则恢复"MAC 尾号"命名。只允许 A-Z a-z 0-9 - _，最长 12 字符。
    const String label = String(doc["name"] | "");
    const Result result = m_config->setDeviceLabel(label);
    if (result != Result::Ok) {
      replyError(id, result,
                 "invalid name (only A-Z a-z 0-9 - _, max 12 chars; empty = back to MAC tail)");
      return;
    }
    const String effective = m_config->bleDeviceName();
    if (m_ble != nullptr) m_ble->applyDeviceName(effective);
    char data[192];
    snprintf(data, sizeof(data), "{\"bleName\":\"%s\",\"label\":\"%s\"}", effective.c_str(),
             label.c_str());
    replyOk(id,
            "已保存并即时生效：重新扫描就能看到新广播名（Windows 可能缓存旧名，"
            "删设备或重开蓝牙即刷新）",
            data);
  } else if (op == "time.sync") {
    if (!enqueue(WorkKind::TimeSync, "", false, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "logs") {
    handleLogs(request, id);
  } else if (op == "session.clear") {
    handleSessionClear(id);
  } else if (op == "session.set") {
    // 调试/应急用：直接把外部拿到的 SESSION 注入设备
    // （例如浏览器在打卡窗口内建立的会话，用于验证只读链路，见 docs/api-contract.md §8.4）
    const String value = doc["value"] | "";
    if (value.length() < 8) {
      replyError(id, Result::InvalidParam, "value required (SESSION uuid)");
    } else {
      m_cookies->set("SESSION", value, AppConst::kJzszHost, "/");
      replyOk(id, "session imported");
    }
  } else if (op == "factory.reset") {
    handleFactoryReset(request, id);
  } else if (op == "reboot") {
    replyOk(id, "rebooting");
    delay(200);
    ESP.restart();
  } else if (op == "test.time") {
    handleTestTime(id);
  } else if (op == "test.http") {
    const String url = doc["url"] | "";
    if (url.length() == 0) {
      replyError(id, Result::InvalidParam, "url required");
      return;
    }
    if (!enqueue(WorkKind::TestHttp, url, false, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "test.login") {
    // force=true 时才允许在打卡窗口外强行登录（默认遵守窗口策略，避免无谓登录）
    const bool force = doc["force"] | false;
    if (!enqueue(WorkKind::TestLogin, "", force, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "test.chain") {
    // 全链路自测：wait_s 秒内持续尝试"进入微应用"，用于观察平台何时放行
    // （force=true 才允许在打卡窗口外强行登录）
    const int waitSeconds = doc["wait_s"] | 0;
    const bool force = doc["force"] | false;
    const int clamped = (waitSeconds < 0) ? 0 : ((waitSeconds > 900) ? 900 : waitSeconds);
    char payload[16];
    snprintf(payload, sizeof(payload), "%d", clamped);
    if (!enqueue(WorkKind::TestChain, payload, force, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "test.probe") {
    if (!enqueue(WorkKind::TestProbe, "", false, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "test.checkin") {
    const bool force = doc["force"] | false;
    if (!enqueue(WorkKind::TestCheckin, "", force, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else {
    replyError(id, Result::InvalidParam, "unknown op: " + op);
  }
}

// ---------------------------------------------------------------- 快指令

void CommandRouter::handleHello(int32_t id) {
  // 用 esp_read_mac 按显示顺序取（ESP.getEfuseMac() 是**小端打包**，
  // 早期实现按 >>40 移位打印得到的是反过来的 MAC，多板对不上号 —— 已修正）
  uint8_t mac[6] = {0};
  uint8_t btMac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  esp_read_mac(btMac, ESP_MAC_BT);

  char data[384];
  snprintf(data, sizeof(data),
           "{\"fw\":\"%s\",\"proto\":%d,\"built\":\"%s\","
           "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
           "\"btMac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
           "\"bleName\":\"%s-%02X%02X\","
           "\"flashMB\":%u,\"psramKB\":%u,\"caps\":[\"wifi\",\"ble\",\"cas\",\"checkin\",\"test\","
           "\"cron\",\"logs\"]}",
           FW_VERSION, FW_PROTO_VERSION, FW_BUILD_TIME, mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5], btMac[0], btMac[1], btMac[2], btMac[3], btMac[4], btMac[5],
           FW_DEVICE_NAME_PREFIX, btMac[4], btMac[5],
           static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
           static_cast<unsigned>(ESP.getPsramSize() / 1024));
  LOG_I(kTag, "hello: mac=%02X:%02X:%02X:%02X:%02X:%02X bt=%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
        mac[1], mac[2], mac[3], mac[4], mac[5], btMac[0], btMac[1], btMac[2], btMac[3], btMac[4],
        btMac[5]);
  replyOk(id, "hello", data);
}

void CommandRouter::handleStatus(int32_t id) { replyOk(id, "status", buildStatusJson()); }

void CommandRouter::handleAuthSet(JsonObjectConst request, int32_t id) {
  const String user = request["user"] | "";
  const String password = request["pass"] | "";
  const String fingerprint = request["bfp"] | "";
  if (user.length() == 0 || password.length() == 0) {
    replyError(id, Result::InvalidParam, "user and pass required");
    return;
  }
  const Result result = m_config->setAuth(user, password, fingerprint);
  if (result != Result::Ok) {
    replyError(id, result, "save auth failed");
    return;
  }
  char data[96];
  snprintf(data, sizeof(data), "{\"fingerprintSet\":%s}",
           fingerprint.length() > 0 ? "true" : "false");
  replyOk(id, "auth saved", data);
}

void CommandRouter::handleLocationSet(JsonObjectConst request, int32_t id) {
  const double latitude = request["lat"] | 0.0;
  const double longitude = request["lng"] | 0.0;
  const String address = request["addr"] | "";
  if (latitude == 0.0 || longitude == 0.0) {
    replyError(id, Result::InvalidParam, "lat and lng required");
    return;
  }
  const Result result = m_config->setLocation(latitude, longitude, address);
  if (result != Result::Ok) {
    replyError(id, result, "save location failed");
    return;
  }
  replyOk(id, "location saved");
}

void CommandRouter::handleScheduleSet(JsonObjectConst request, int32_t id) {
  const String at = request["at"] | "";
  const String windowStart = request["window_start"] | "";  // 可选：窗口起点（默认 21:00）
  const String windowEnd = request["window_end"] | "23:30";
  const uint16_t jitter = request["jitter_s"] | 30;
  if (at.length() == 0) {
    replyError(id, Result::InvalidParam, "at required (HH:MM)");
    return;
  }
  const Result result = m_config->setSchedule(at, windowEnd, jitter, windowStart);
  if (result != Result::Ok) {
    replyError(id, result, "invalid time format");
    return;
  }
  char data[176];
  snprintf(data, sizeof(data),
           "{\"checkinAt\":\"%s\",\"windowStart\":\"%s\",\"windowEnd\":\"%s\",\"jitterSeconds\":%u}",
           at.c_str(), m_config->schedule().windowStart.c_str(), windowEnd.c_str(), jitter);
  replyOk(id, "schedule saved", data);
}

void CommandRouter::handleAutoSet(JsonObjectConst request, int32_t id) {
  const bool enabled = request["enabled"] | true;
  m_config->setAutoDaily(enabled);
  replyOk(id, enabled ? "auto daily enabled" : "auto daily disabled");
}

void CommandRouter::handleAccountAdd(JsonObjectConst request, int32_t id) {
  if (m_accounts == nullptr || m_config == nullptr) {
    replyError(id, Result::NotReady, "account store unavailable");
    return;
  }
  CheckinProfile profile;
  profile.label = request["label"] | "";
  profile.studentId = String(request["user"] | "");
  profile.password = String(request["pass"] | "");
  profile.fingerprint = String(request["bfp"] | "");
  profile.latitude = request["lat"] | 0.0;
  profile.longitude = request["lng"] | 0.0;
  profile.address = String(request["addr"] | "");
  // 没给坐标就沿用设备当前配置的坐标（同宿舍多人最常见的情况）
  if (profile.latitude == 0.0 || profile.longitude == 0.0) {
    profile.latitude = m_config->location().latitude;
    profile.longitude = m_config->location().longitude;
    if (profile.address.length() == 0) profile.address = m_config->location().address;
  }
  // 没给指纹就沿用设备当前配置的指纹（同一台设备共用一个"信任浏览器"也许可行，
  // 但更稳妥的是每个账号各用自己浏览器的指纹）
  const Result result = m_accounts->addOrUpdate(profile);
  if (result == Result::InvalidParam) {
    replyError(id, result,
               "invalid: label A-Z a-z 0-9 - _ (1..12), user+pass required, max 6 accounts");
    return;
  }
  if (result != Result::Ok) {
    replyError(id, result, "save account failed");
    return;
  }
  char data[128];
  snprintf(data, sizeof(data), "{\"count\":%u,\"max\":%u}",
           static_cast<unsigned>(m_accounts->count()),
           static_cast<unsigned>(AccountStore::kMaxProfiles));
  replyOk(id, "account saved", data);
}

void CommandRouter::handleMultiSet(JsonObjectConst request, int32_t id) {
  if (m_config == nullptr) {
    replyError(id, Result::NotReady, "config unavailable");
    return;
  }
  const bool enabled = request["enabled"] | true;
  m_config->setMultiUserEnabled(enabled);
  if (request.containsKey("gap_s")) {
    const uint16_t gap = request["gap_s"] | static_cast<uint16_t>(20);
    if (m_config->setMultiGapSeconds(gap) != Result::Ok) {
      replyError(id, Result::InvalidParam, "gap_s must be 0..600");
      return;
    }
  }
  char data[160];
  snprintf(data, sizeof(data), "{\"multiUser\":%s,\"gapSeconds\":%u,\"accounts\":%u}",
           m_config->multiUserEnabled() ? "true" : "false",
           static_cast<unsigned>(m_config->multiGapSeconds()),
           m_accounts == nullptr ? 0u : static_cast<unsigned>(m_accounts->count()));
  replyOk(id, m_config->multiUserEnabled() ? "multi-user enabled" : "multi-user disabled", data);
}

void CommandRouter::handleLogs(JsonObjectConst request, int32_t id) {
  const int32_t lines = request["n"] | 30;
  const String text = Logger::instance().tail(static_cast<uint16_t>(lines));
  String data = "{\"lines\":";
  data += String(lines);
  data += ",\"dropped\":";
  data += String(Logger::instance().droppedCount());
  data += ",\"text\":\"";
  data += escapeJson(text);
  data += "\"}";
  replyOk(id, "logs", data);
}

void CommandRouter::handleSessionClear(int32_t id) {
  m_cookies->clear();
  replyOk(id, "cookies cleared");
}

void CommandRouter::handleFactoryReset(JsonObjectConst request, int32_t id) {
  const String confirm = request["confirm"] | "";
  if (confirm != "YES") {
    replyError(id, Result::InvalidParam, "send confirm:\"YES\" to erase everything");
    return;
  }
  m_cookies->clear();
  m_config->factoryReset();
  replyOk(id, "factory reset done, rebooting");
  delay(300);
  ESP.restart();
}

void CommandRouter::handleTestTime(int32_t id) {
  const bool synced = (m_time != nullptr) && m_time->isSynced();
  const int startMinutes = m_config->schedule().checkinAtMinutes;
  const int endMinutes = m_config->schedule().windowEndMinutes;
  const bool inWindow = (m_time != nullptr) && m_time->inWindow(startMinutes, endMinutes);
  char data[256];
  snprintf(data, sizeof(data),
           "{\"deviceTime\":\"%s\",\"synced\":%s,\"window\":\"%s - %s\",\"inWindow\":%s,"
           "\"nextRun\":\"%s\"}",
           (m_time != nullptr) ? m_time->nowString().c_str() : "-", synced ? "true" : "false",
           TimeService::formatMinutes(startMinutes).c_str(),
           TimeService::formatMinutes(endMinutes).c_str(), inWindow ? "true" : "false",
           (m_scheduler != nullptr) ? m_scheduler->nextRunString().c_str() : "-");
  replyOk(id, "time info", data);
}

// ---------------------------------------------------------------- 慢指令

void CommandRouter::execute(const WorkItem& item) {
  switch (item.kind) {
    case WorkKind::Command: dispatchCommand(String(item.payload)); break;
    case WorkKind::WifiConnect: workWifiConnect(item); break;
    case WorkKind::WifiScan: workWifiScan(item); break;
    case WorkKind::TimeSync: workTimeSync(item); break;
    case WorkKind::TestHttp: workTestHttp(item); break;
    case WorkKind::TestLogin: workTestLogin(item); break;
    case WorkKind::TestChain: workTestChain(item); break;
    case WorkKind::TestProbe: workTestProbe(item, false); break;
    case WorkKind::TestCheckin: workTestCheckin(item, false); break;
    case WorkKind::Probe:
      // 多人模式：预登录体检也按账号各来一遍（runAll 内部只做体检不提交）
      if (m_config != nullptr && m_config->multiUserEnabled()) {
        WorkItem probeItem;
        probeItem.kind = WorkKind::MultiCheckin;
        probeItem.force = false;
        probeItem.requestId = 0;
        memcpy(probeItem.payload, "probe", 6);
        workMultiCheckin(probeItem);
      } else {
        workTestProbe(item, true);
      }
      break;
    case WorkKind::DailyCheckin:
      if (m_config != nullptr && m_config->multiUserEnabled()) {
        workMultiCheckin(item);
      } else {
        workTestCheckin(item, true);
      }
      break;
    case WorkKind::MultiCheckin: workMultiCheckin(item); break;
  }
}

void CommandRouter::workWifiConnect(const WorkItem& item) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"wifi\",\"detail\":\"connecting\"");
  const Result result = m_wifi->connect(20000);
  if (result != Result::Ok) {
    setBusy(false);
    flashResult(false);
    emitEvent("wifi", "\"state\":\"failed\"");
    replyError(item.requestId, result, "wifi connect failed (check ssid/password)");
    return;
  }

  String eventJson = "\"state\":\"connected\",\"ip\":\"" + m_wifi->ip() + "\",\"rssi\":" +
                     String(m_wifi->rssi());
  emitEvent("wifi", eventJson);

  m_time->begin();
  const Result timeResult = m_time->waitSynced(20000);
  setBusy(false);
  flashResult(timeResult == Result::Ok);

  String data = "{\"ip\":\"" + m_wifi->ip() + "\",\"rssi\":" + String(m_wifi->rssi()) +
                ",\"timeSynced\":" + (timeResult == Result::Ok ? "true" : "false") + "}";
  replyOk(item.requestId, "wifi connected", data);
}

void CommandRouter::workWifiScan(const WorkItem& item) {
  setBusy(true);
  String json;
  const Result result = m_wifi->scan(json);
  setBusy(false);
  if (result != Result::Ok) {
    replyError(item.requestId, result, "scan failed");
    return;
  }
  replyOk(item.requestId, "scan done", "{\"aps\":" + json + "}");
}

void CommandRouter::workTimeSync(const WorkItem& item) {
  setBusy(true);
  m_time->begin();
  const Result result = m_time->waitSynced(20000);
  setBusy(false);
  if (result != Result::Ok) {
    replyError(item.requestId, result, "time sync failed");
    return;
  }
  replyOk(item.requestId, "time synced", "{\"deviceTime\":\"" + m_time->nowString() + "\"}");
}

void CommandRouter::workTestHttp(const WorkItem& item) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"http\",\"detail\":\"requesting\"");

  HttpRequest request;
  request.method = "GET";
  request.url = String(item.payload);
  request.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

  HttpResponse response;
  const Result result = m_http->request(request, response);
  setBusy(false);

  if (result != Result::Ok) {
    flashResult(false);
    replyError(item.requestId, result, response.errorDetail.length() ? response.errorDetail
                                                                    : "request failed");
    return;
  }

  const bool hasSalt = response.body.indexOf("pwdEncryptSalt") >= 0;
  const bool hasExecution = response.body.indexOf("execution") >= 0;
  flashResult(response.status == 200);

  char data[288];
  snprintf(data, sizeof(data),
           "{\"status\":%d,\"bytes\":%u,\"ms\":%u,\"hasSalt\":%s,\"hasExecution\":%s,"
           "\"location\":\"%s\"}",
           response.status, static_cast<unsigned>(response.bodyBytes),
           static_cast<unsigned>(response.durationMs), hasSalt ? "true" : "false",
           hasExecution ? "true" : "false", escapeJson(response.location).c_str());
  replyOk(item.requestId, "http ok", data);
}

void CommandRouter::workTestLogin(const WorkItem& item) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"login\",\"detail\":\"start\"");

  // 统一走 CheckinService 的登录策略：**窗口外默认不登录**。
  // 平台不会授权"窗口外建立的会话"，此时登录只会白费一次登录次数并造成误解。
  // 需要强制验证登录链本身时：{"op":"test.login","force":true}（控制台：login force）
  bool didLogin = false;
  String detail;
  const Result result = m_checkin->ensureSession(didLogin, detail, item.force);

  if (result == Result::AppUnavailable) {
    setBusy(false);
    replyError(item.requestId, Result::AppUnavailable,
               String("login skipped: ") + detail +
                   " | 当前不在平台打卡时段，窗口内(21:00-23:30)会自动登录；"
                   "如要强制测试登录链，请用 login force");
    return;
  }
  if (result == Result::NeedMfa) {
    setBusy(false);
    flashResult(false);
    emitEvent("alert", "\"kind\":\"mfa\",\"msg\":\"server requires second factor\"");
    replyError(item.requestId, Result::NeedMfa,
               "server requires second factor: fingerprint not trusted (see docs)");
    return;
  }
  if (result != Result::Ok) {
    setBusy(false);
    flashResult(false);
    replyError(item.requestId, result, detail.length() ? detail : "login failed");
    return;
  }

  // 会话可用：顺手体检一次，把窗口与今日状态一并回给用户
  emitEvent("progress", "\"stage\":\"login\",\"detail\":\"session established, verifying\"");
  ProbeReport report;
  String probeError;
  m_checkin->probe(report, probeError);
  setBusy(false);
  flashResult(report.sessionValid);

  String data = "{\"didLogin\":";
  data += didLogin ? "true" : "false";
  data += ",\"mfa\":false,\"sessionWorks\":";
  data += report.sessionValid ? "true" : "false";
  data += ",\"recordStatusCode\":\"";
  data += report.today.recordStatusCode;
  data += "\",\"window\":\"";
  data += report.windowText;
  data += "\",\"inWindow\":";
  data += report.inWindow ? "true" : "false";
  data += ",\"loginAllowed\":";
  data += report.loginAllowed ? "true" : "false";
  data += '}';

  replyOk(item.requestId, didLogin ? "logged in, session verified" : "session already valid", data);
}

void CommandRouter::workTestChain(const WorkItem& item) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"chain\",\"detail\":\"start\"");

  const uint32_t startMs = millis();
  const uint32_t waitMs = static_cast<uint32_t>(atoi(item.payload)) * 1000UL;

  // ---- 1) 网络与时间（前置条件，不通则无需继续）----
  const bool wifiOk = (m_wifi != nullptr) && (m_wifi->state() == WifiState::Connected);
  const bool timeOk = (m_time != nullptr) && m_time->isSynced();
  if (!wifiOk || !timeOk) {
    setBusy(false);
    flashResult(false);
    replyError(item.requestId, Result::NotReady,
               String("chain stopped: ") + (wifiOk ? "" : "wifi not connected ") +
                   (timeOk ? "" : "time not synced"));
    return;
  }
  emitEvent("progress", "\"stage\":\"chain\",\"detail\":\"wifi + time ok\"");

  // ---- 2) 登录页可获取且关键参数可解析（只读，不消耗登录次数）----
  String execution;
  String salt;
  const bool loginPageOk = (m_auth->fetchLoginPage(execution, salt) == Result::Ok);
  emitEvent("progress", String("\"stage\":\"chain\",\"detail\":\"") +
                            (loginPageOk ? "login page ok" : "login page FAILED") + "\"");

  // ---- 3) 会话/登录（遵守窗口策略；force 才允许窗口外登录）----
  bool didLogin = false;
  String sessionDetail;
  const Result sessionResult = m_checkin->ensureSession(didLogin, sessionDetail, item.force);
  const bool sessionOk = (sessionResult == Result::Ok);
  const bool deferred = (sessionResult == Result::AppUnavailable);
  emitEvent("progress", String("\"stage\":\"chain\",\"detail\":\"") +
                            (sessionOk ? "session ready" : "session NOT ready") + "\"");

  // ---- 3b) 打卡界面本身是否可打开（静态页，任何会话 200）----
  // 这就是浏览器"任意时间输入晚安成电网址都能进界面"的原因；
  // **但它不代表会话被授权打卡**（授权判据是下面的 appEntry）。
  bool h5Page = false;
  const Result h5Result = m_api->fetchH5Page(&h5Page);
  emitEvent("progress", String("\"stage\":\"chain\",\"detail\":\"h5 page ") +
                            (h5Page ? "reachable" : "unreachable") + " (" +
                            resultName(h5Result) + ")\"");

  // ---- 3c) 门户预热：**微应用授权的真正前提**（实测 2026-10-13）----
  // 没做这一步时，无论窗口内外、无论请求头怎么改，checkLogin 都只会被 302 退回平台首页。
  int warmStatus = 0;
  const bool warmed = (m_api->warmUpPortal(&warmStatus) == Result::Ok);
  emitEvent("progress", String("\"stage\":\"chain\",\"detail\":\"portal warm-up http ") +
                            String(warmStatus) + "\"");

  // ---- 4) 微应用授权：**这是"能不能打卡"的判据** ----
  // 预热已生效时这里会立刻通过；仍不通才需要 wait_s 轮询观察。
  bool appEntry = false;
  String entryUrl;
  int entryStatus = 0;
  uint32_t waitedMs = 0;
  uint8_t entryAttempts = 0;
  Result entryResult = Result::Ok;
  const uint32_t stepMs = 30000UL;
  while (true) {
    entryResult = m_api->enterApp(&appEntry, &entryUrl, &entryStatus);
    entryAttempts++;
    if (appEntry || waitedMs >= waitMs) break;
    emitEvent("progress", String("\"stage\":\"chain\",\"detail\":\"waiting for app entry (http ") +
                              String(entryStatus) + ", " + entryUrl + ")\"");
    delay(stepMs);
    waitedMs += stepMs;
  }
  emitEvent("progress", String("\"stage\":\"chain\",\"detail\":\"") +
                            (appEntry ? "app entry ok" : "app entry BLOCKED") + "\"");

  // ---- 5) 业务接口（只有进得了界面才有意义）----
  ProbeReport report;
  String probeError;
  if (appEntry) {
    m_checkin->probe(report, probeError);
  } else {
    report.h5Reached = false;
    report.sessionValid = false;
    report.windowStartMinutes = m_config->schedule().windowStartMinutes;
    report.windowEndMinutes = m_config->schedule().windowEndMinutes;
    report.windowText = TimeService::formatMinutes(report.windowStartMinutes) + " - " +
                        TimeService::formatMinutes(report.windowEndMinutes);
    report.inWindow = m_time->inWindow(report.windowStartMinutes, report.windowEndMinutes);
  }
  setBusy(false);
  flashResult(appEntry);

  // **把"能进界面"和"能打卡"分开报告**：窗口外能进界面但打不了卡是正常的
  const bool canCheckin = appEntry && report.sessionValid && report.inWindow;
  char data[768];
  snprintf(data, sizeof(data),
           "{\"wifi\":true,\"timeSynced\":true,\"loginPage\":%s,\"session\":%s,"
           "\"h5Page\":%s,\"warmup\":%s,\"warmStatus\":%d,"
           "\"didLogin\":%s,\"loginDeferred\":%s,\"appEntry\":%s,"
           "\"entryResult\":\"%s\",\"entryStatus\":%d,\"entryUrl\":\"%s\","
           "\"entryAttempts\":%u,\"waitedS\":%u,\"today\":\"%s\",\"window\":\"%s\","
           "\"inWindow\":%s,\"canCheckin\":%s,\"elapsedMs\":%u}",
           loginPageOk ? "true" : "false", sessionOk ? "true" : "false",
           h5Page ? "true" : "false", warmed ? "true" : "false", warmStatus, didLogin ? "true" : "false",
           deferred ? "true" : "false", appEntry ? "true" : "false",
           resultName(entryResult), entryStatus, escapeJson(entryUrl).c_str(),
           static_cast<unsigned>(entryAttempts), static_cast<unsigned>(waitedMs / 1000),
           report.today.recordStatusCode.c_str(), report.windowText.c_str(),
           report.inWindow ? "true" : "false", canCheckin ? "true" : "false",
           static_cast<unsigned>(millis() - startMs));
  LOG_I(kTag,
        "chain: loginPage=%d session=%d h5Page=%d warmup=%d/%d didLogin=%d appEntry=%d entryResult=%s "
        "today=%s inWindow=%d canCheckin=%d",
        loginPageOk ? 1 : 0, sessionOk ? 1 : 0, h5Page ? 1 : 0, warmed ? 1 : 0, warmStatus, didLogin ? 1 : 0,
        appEntry ? 1 : 0, resultName(entryResult), report.today.recordStatusCode.c_str(),
        report.inWindow ? 1 : 0, canCheckin ? 1 : 0);

  String message;
  if (canCheckin) {
    message = "chain fully ok: app entry ok and check-in window is open";
  } else if (appEntry) {
    message = "app entry ok (check-in itself only works inside the window)";
  } else if (deferred) {
    message = sessionDetail + " | 窗口内(21:00-23:30)会自动登录";
  } else if (h5Page) {
    message = "界面可打开(h5Page=true)，但平台未授权该会话打卡(appEntry=false, http " +
              String(entryStatus) + " final=" + entryUrl +
              ")：预热已尝试(warmup=" + String(warmed ? "ok" : "fail") + ", http " +
              String(warmStatus) + ")，若仍被拒则该会话不是登录态，请在窗口内重新登录";
  } else {
    message = "chain failed: http " + String(entryStatus) + " final=" + entryUrl;
  }
  replyOk(item.requestId, message, data);
}

void CommandRouter::workTestProbe(const WorkItem& item, bool fromSchedule) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"probe\",\"detail\":\"checking session\"");

  ProbeReport report;
  String error;
  const Result result = m_checkin->probe(report, error);
  setBusy(false);

  if (result != Result::Ok) {
    flashResult(false);
    if (!fromSchedule) replyError(item.requestId, result, error);
    return;
  }

  flashResult(report.sessionValid);

  char data[640];
  snprintf(data, sizeof(data),
           "{\"sessionValid\":%s,\"h5Reached\":%s,\"h5Page\":%s,\"deviceTime\":\"%s\","
           "\"window\":\"%s\",\"inWindow\":%s,\"windowFromPlatform\":%s,"
           "\"recordStatusCode\":\"%s\",\"checkinTime\":\"%s\",\"distanceMeter\":%.1f,"
           "\"abnormalFlag\":%s,\"location\":\"%s\",\"radiusMeter\":%d,\"elapsedMs\":%u}",
           report.sessionValid ? "true" : "false", report.h5Reached ? "true" : "false",
           report.h5PageReached ? "true" : "false", report.deviceTime.c_str(),
           report.windowText.c_str(), report.inWindow ? "true" : "false",
           report.windowFromPlatform ? "true" : "false", report.today.recordStatusCode.c_str(),
           report.today.checkinTime.c_str(), report.today.distanceMeter,
           report.today.abnormalFlag ? "true" : "false", report.location.name.c_str(),
           report.location.radiusMeter, static_cast<unsigned>(report.elapsedMs));

  if (fromSchedule) {
    // 预登录体检发现会话失效时，趁"仍在打卡窗口内"立刻补一次登录。
    // 平台只给窗口内建立的会话授予微应用访问权（见 docs/api-contract.md §8.4），
    // 拖到 22:00 再登录会浪费窗口时间、也会失去提前暴露问题的价值。
    if (!report.sessionValid) {
      emitEvent("alert",
                "\"kind\":\"session\",\"msg\":\"session invalid at pre-login, logging in now\"");
      bool didLogin = false;
      String loginError;
      const Result loginResult = m_checkin->ensureSession(didLogin, loginError);
      if (loginResult == Result::Ok && didLogin) {
        String probeError;
        ProbeReport refreshed;
        if (m_checkin->probe(refreshed, probeError) == Result::Ok) {
          report = refreshed;
        }
      } else if (loginResult == Result::NeedMfa) {
        emitEvent("alert", "\"kind\":\"mfa\",\"msg\":\"second factor required at pre-login\"");
      }
      flashResult(report.sessionValid);
    }
    emitEvent("result", "\"kind\":\"probe\",\"sessionValid\":" +
                            String(report.sessionValid ? "true" : "false") +
                            ",\"h5Reached\":" + String(report.h5Reached ? "true" : "false") +
                            ",\"status\":\"" + report.today.recordStatusCode + "\"");
    return;
  }

  if (!report.sessionValid) {
    replyOk(item.requestId, "probe done: session invalid, run test.login", data);
  } else {
    replyOk(item.requestId, "probe done", data);
  }
}

void CommandRouter::workTestCheckin(const WorkItem& item, bool fromSchedule) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"checkin\",\"detail\":\"start\"");

  String message;
  const CheckinOutcome outcome = m_checkin->perform(item.force, message);
  setBusy(false);

  const bool ok = (outcome == CheckinOutcome::Success) || (outcome == CheckinOutcome::AlreadyDone);
  const bool pending = (outcome == CheckinOutcome::Pending);
  if (!pending) flashResult(ok);  // Pending 既非成功也非失败，不闪灯

  // 持久化最近一次打卡结果：掉电重启后仍可通过 status 查看
  if ((outcome == CheckinOutcome::Success) || (outcome == CheckinOutcome::AlreadyDone) ||
      (outcome == CheckinOutcome::Failed)) {
    if (m_config != nullptr && m_time != nullptr) {
      m_config->setLastResult(m_time->todayString(), ok, message);
    }
  }

  switch (outcome) {
    case CheckinOutcome::Success:
      emitEvent("result", "\"ok\":true,\"msg\":\"" + escapeJson(message) + "\"");
      replyOk(item.requestId, message.isEmpty() ? "checked in" : message);
      break;
    case CheckinOutcome::AlreadyDone:
      emitEvent("result", "\"ok\":true,\"already\":true,\"msg\":\"" + escapeJson(message) + "\"");
      replyOk(item.requestId, message.isEmpty() ? "already checked in" : message);
      break;
    case CheckinOutcome::OutOfWindow:
      if (!fromSchedule) replyError(item.requestId, Result::OutOfWindow, message);
      break;
    case CheckinOutcome::NeedMfa:
      emitEvent("alert", "\"kind\":\"mfa\",\"msg\":\"second factor required\"");
      if (!fromSchedule) replyError(item.requestId, Result::NeedMfa, message);
      break;
    case CheckinOutcome::Failed:
      emitEvent("result", "\"ok\":false,\"msg\":\"" + escapeJson(message) + "\"");
      if (!fromSchedule) replyError(item.requestId, Result::InternalError, message);
      break;
    case CheckinOutcome::Skipped:
      if (!fromSchedule) replyOk(item.requestId, "skipped");
      break;
    case CheckinOutcome::Pending:
      // 会话已建立但平台尚未放行（实测有数分钟生效延迟）：**不算失败**，
      // 调度器会在窗口内按 kCheckinRetrySeconds 自动重试。
      emitEvent("progress",
                "\"stage\":\"checkin\",\"detail\":\"pending platform authorization, will retry\"");
      if (!fromSchedule) {
        replyError(item.requestId, Result::AppUnavailable, message);
      }
      break;
  }
}

void CommandRouter::workMultiCheckin(const WorkItem& item) {
  setBusy(true);
  const bool probeOnly = (strcmp(item.payload, "probe") == 0);

  String summary;
  size_t okCount = 0;
  if (m_multi != nullptr) {
    okCount = m_multi->runAll(item.force, probeOnly, summary);
  }

  setBusy(false);
  flashResult(okCount > 0);

  String message;
  const size_t total = (m_accounts != nullptr) ? m_accounts->count() : 0;
  if (total == 0) {
    message = "no accounts configured (use account.add first)";
  } else if (okCount == 0) {
    message = String("multi-user run finished: 0/") + String(static_cast<unsigned>(total)) +
              " ok - see data for each account's reason";
  } else {
    message = String("multi-user ") + (probeOnly ? "probe" : "check-in") + " done: " +
              String(static_cast<unsigned>(okCount)) + "/" + String(static_cast<unsigned>(total)) +
              " ok";
  }
  emitEvent("result",
            String("\"kind\":\"multi\",\"ok\":") + (okCount > 0 ? "true" : "false") +
                ",\"accounts\":" + summary);
  if (item.requestId != 0) {
    replyOk(item.requestId, message, summary);
  }
}
