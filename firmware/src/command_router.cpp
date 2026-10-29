// ============================================================================
//  CommandRouter 实现
// ============================================================================

#include "command_router.h"
#include "account_patch.h"
#include "account_selection.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <memory>
#include <new>

#include "wx_mmtls.h"
#include "wx_oauth.h"

namespace {
/// 学校小程序 appid（成电智慧学工）
constexpr const char* kWxCheckinAppId = "wxaa78bf2905f01cee";
}  // namespace

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
                            StatusLed& led, AccountStore& accounts, MultiCheckin& multi,
                             NotifyService& notify) {
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
  m_notify = &notify;
  // 多人编排器与通知服务共用同一个 BLE 事件出口
  multi.setEventSink(
      [this](const String& type, const String& payload) { emitEvent(type, payload); });
  notify.setEventSink(
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
  std::unique_ptr<char[]> command;
  if (kind == WorkKind::Command) {
    if (payload.length() > 4095) return false;  // BLE 4096 字节缓冲需为换行留 1 字节
    command.reset(new (std::nothrow) char[payload.length() + 1]);
    if (!command) return false;
    memcpy(command.get(), payload.c_str(), payload.length() + 1);
    item.command = command.get();
  } else {
    const size_t copyLength =
        payload.length() < sizeof(item.payload) - 1 ? payload.length() : sizeof(item.payload) - 1;
    memcpy(item.payload, payload.c_str(), copyLength);
    item.payload[copyLength] = '\0';
  }
  if (xQueueSend(m_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
    LOG_W(kTag, "queue full, drop kind=%u", static_cast<unsigned>(kind));
    return false;
  }
  command.release();  // 队列已复制指针；失败路径仍由 unique_ptr 自动释放
  return true;
}

void CommandRouter::processPending() {
  WorkItem item;
  if (xQueueReceive(m_queue, &item, pdMS_TO_TICKS(200)) == pdTRUE) {
    execute(item);
  }
  // 通知出口：每轮都推一下待发队列（内部有退避/去重/配额，不会刷屏）
  if (m_notify != nullptr) m_notify->pump();
}

// ---------------------------------------------------------------- 事件与回执

void CommandRouter::emitEvent(const String& type, const String& payloadJson) {
  if (m_captureReply) return;
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
  if (m_captureReply) { m_capturedOk=true; m_capturedMessage=message; m_capturedData=dataJson; return; }
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
  if (m_captureReply) { m_capturedOk=false; m_capturedMessage=message; m_capturedData=""; return; }
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
  out += "},\"runtime\":";
  // Allocator low-water marks cover activity between status polls, since boot.
  JsonDocument runtime;
  const uint32_t total = ESP.getHeapSize();
  const uint32_t minimum = ESP.getMinFreeHeap();
  runtime["boardId"] = BoardProfile::kId;
  runtime["chipModel"] = ESP.getChipModel();
  runtime["uptimeSeconds"] = static_cast<uint64_t>(esp_timer_get_time() / 1000000);
  runtime["heapTotalBytes"] = total;
  runtime["heapFreeBytes"] = ESP.getFreeHeap();
  runtime["heapMinFreeBytes"] = minimum;
  runtime["heapPeakUsedBytes"] = total >= minimum ? total - minimum : 0;
  runtime["heapLargestBlockBytes"] = ESP.getMaxAllocHeap();
  runtime["psramTotalBytes"] = ESP.getPsramSize();
  runtime["psramFreeBytes"] = ESP.getFreePsram();
  runtime["psramMinFreeBytes"] = ESP.getMinFreePsram();
  runtime["netTaskStackFreeMinBytes"] = uxTaskGetStackHighWaterMark(nullptr);
  runtime["netTaskCore"] = AppConst::kNetTaskCore;
  // ArduinoJson 7 clears String destinations before writing. Serialize this
  // nested object separately so the existing status prefix is preserved.
  String runtimeJson;
  serializeJson(runtime, runtimeJson);
  out += runtimeJson;
  out += ",\"net\":{\"state\":\"";
  out += wifiState;
  out += "\",\"ssid\":\"";
  out += WiFi.isConnected() ? escapeJson(WiFi.SSID()) : String("");
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
  if (request.containsKey("expectedRevision") && (m_accounts == nullptr ||
      !request["expectedRevision"].is<uint32_t>() || request["expectedRevision"].as<uint32_t>() != m_accounts->revision())) {
    replyError(id, Result::InvalidParam, "account snapshot changed; refresh accounts and retry"); return;
  }
  if (request.containsKey("targets")) { handleSelected(request,id); return; }

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
    // 可选：bssid + channel（隐藏 SSID 直连用；不填则走普通扫描连接）
    const String bssid = doc["bssid"] | "";
    const int channel = doc["channel"] | 0;
    const Result result = m_config->setWifi(ssid, password, bssid,
                                            static_cast<uint8_t>(channel < 0 ? 0 : channel));
    if (result != Result::Ok) {
      replyError(id, result, "save wifi failed");
      return;
    }
    if (!enqueue(WorkKind::WifiConnect, "", false, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "wx.import") {
    // 凭据导入：{"op":"wx.import","slot":0,"openid":"...","access":"...","refresh":"...","expiresAt":...}
    if (m_wxCred == nullptr) { replyError(id, Result::NotReady, "wx store not ready"); return; }
    const int slot = doc["slot"] | 0;
    if (request.containsKey("target")) {
      const String target=doc["target"] | "";
      CheckinProfile profile;
      const bool primary=m_accounts != nullptr && m_accounts->count()==0 && slot==0 && target=="@primary";
      if (!request.containsKey("expectedRevision") || (!primary && (m_accounts == nullptr || slot<0 || !m_accounts->get(slot,profile) || profile.label!=target))) {
        replyError(id,Result::InvalidParam,"authorization target changed; refresh accounts"); return;
      }
    }
    wx::OauthCredentials cred;
    cred.openid = doc["openid"] | "";
    cred.accessToken = doc["access"] | "";
    cred.refreshToken = doc["refresh"] | "";
    cred.expiresAt = doc["expiresAt"] | static_cast<int64_t>(0);
    if (slot < 0 || slot >= static_cast<int>(wx::CredentialStore::kMaxSlots) ||
        !cred.valid()) { replyError(id, Result::InvalidParam, "bad credential fields"); return; }
    const Result result = m_wxCred->save(static_cast<size_t>(slot), cred);
    if (result != Result::Ok) { replyError(id, result, "save credential failed"); return; }
    replyOk(id, "wx credential imported", "{\"slot\":" + String(slot) + "}");
  } else if (op == "wx.status") {
    if (m_wxCred == nullptr) { replyError(id, Result::NotReady, "wx store not ready"); return; }
    replyOk(id, "wx credentials", "{\"slots\":" + m_wxCred->summary() + "}");
  } else if (op == "wx.test") {
    // 设备端全链路：刷新(可选)→login_buffer→MMTLS取码→check-bind→取票（不提交）
    if (m_wxCred == nullptr) { replyError(id, Result::NotReady, "wx store not ready"); return; }
    const int slot = doc["slot"] | 0;
    if (slot<0 || slot>=static_cast<int>(wx::CredentialStore::kMaxSlots)) { replyError(id,Result::InvalidParam,"invalid slot"); return; }
    if (request.containsKey("target")) {
      const String target=request["target"] | ""; CheckinProfile profile;
      const bool primary=m_accounts && !m_accounts->hasAny() && slot==0 && target=="@primary";
      if (!request.containsKey("expectedRevision") || (!primary && (!m_accounts || !m_accounts->get(slot,profile) || profile.label!=target))) {
        replyError(id,Result::InvalidParam,"authorization target changed; refresh accounts"); return;
      }
    }
    char payload[16];
    snprintf(payload, sizeof(payload), "slot=%d", slot);
    if (!enqueue(WorkKind::WxTest, payload, false, id))
      replyError(id, Result::InternalError, "device busy");
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
  } else if (op == "notify.set") {
    handleNotifySet(request, id);
  } else if (op == "notify.status") {
    if (m_notify == nullptr) {
      replyError(id, Result::NotReady, "notify service unavailable");
    } else {
      replyOk(id, "notify", m_notify->statusJson());
    }
  } else if (op == "notify.add" || op == "notify.remove" || op == "notify.enable" ||
             op == "notify.link") {
    handleNotifyChannel(op, request, id);
  } else if (op == "test.notify") {
    // 可选 "chan":"标签"：指定额外通道试发（不带=默认通道）。
    // 载荷格式 "标签\n正文"；无 chan 时保持原样（向后兼容）。
    const String chan = request["chan"] | "";
    const String body = request["body"] | "";
    const String payload = chan.length() > 0 ? chan + "\n" + body : body;
    if (!enqueue(WorkKind::NotifyTest, payload, false, id)) {
      replyError(id, Result::InternalError, "device busy");
    }
  } else if (op == "account.add") {
    handleAccountAdd(request, id);
  } else if (op == "account.snapshot") {
    handleAccountSnapshot(id);
  } else if (op == "account.patch") {
    handleAccountPatch(request,id);
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
      if (m_accounts->indexOfLabel(label)<0) { replyError(id,Result::NotFound,"account not found"); return; }
      if (!clearAccountAuthorizations(id)) return;
      const Result result = m_accounts->remove(label);
      if (result != Result::Ok) {
        replyError(id, result, "account not found");
      } else {
        replyOk(id, "account removed");
      }
    }
  } else if (op == "account.enable") {
    handleAccountEnable(request, id);
  } else if (op == "account.clear") {
    if (!clearAccountAuthorizations(id)) return;
    if (m_accounts == nullptr || m_accounts->clearAll()!=Result::Ok) { replyError(id,Result::StorageError,"clear accounts failed"); return; }
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

  char data[512];
  snprintf(data, sizeof(data),
           "{\"boardId\":\"%s\",\"fw\":\"%s\",\"proto\":%d,\"maxCommandBytes\":4095,\"built\":\"%s\","
           "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
           "\"btMac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
           "\"bleName\":\"%s-%02X%02X\","
           "\"flashMB\":%u,\"psramKB\":%u,\"caps\":[\"wifi\",\"ble\",\"cas\",\"checkin\",\"test\","
           "\"cron\",\"logs\",\"account-v2\",\"selected-execution\"]}",
           BoardProfile::kId, FW_VERSION, FW_PROTO_VERSION, FW_BUILD_TIME, mac[0], mac[1], mac[2], mac[3], mac[4],
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
  const bool identityChanged=m_config->storedProfile().studentId!=user;
  if (identityChanged && m_accounts && !m_accounts->hasAny() && m_wxCred && m_wxCred->remove(0)!=Result::Ok) {
    replyError(id,Result::StorageError,"cannot clear prior authorization; auth unchanged"); return;
  }
  const Result result = m_config->setAuth(user, password, fingerprint);
  if (result != Result::Ok) {
    replyError(id, result, "save auth failed");
    return;
  }
  char data[96];
  snprintf(data, sizeof(data), "{\"fingerprintSet\":%s}",
           fingerprint.length() > 0 ? "true" : "false");
  if (m_accounts) m_accounts->invalidateRevision();
  if(identityChanged) { m_cookies->clear(); m_checkin->resetLoginState(); m_api->clearOperationTicket(); }
  replyOk(id, "auth saved", data);
}

void CommandRouter::handleAccountSnapshot(int32_t id) {
  if (!m_accounts || !m_config) { replyError(id,Result::NotReady,"account store not ready"); return; }
  JsonDocument doc;
  if (deserializeJson(doc,m_accounts->toJsonDetails())) { replyError(id,Result::InternalError,"account snapshot allocation failed"); return; }
  if (!m_accounts->hasAny()) {
    const auto p=m_config->storedProfile();
    JsonObject row=doc["primary"].to<JsonObject>();
    row["slot"]=0; row["label"]="@primary"; row["user"]=p.studentId;
    row["passwordSet"]=p.password.length()>0; row["fingerprintSet"]=p.hasFingerprint(); row["enabled"]=true;
    row["lat"]=p.latitude; row["lng"]=p.longitude; row["addr"]=p.address;
  }
  if(doc.overflowed()) { replyError(id,Result::InternalError,"account snapshot allocation failed"); return; }
  String out; serializeJson(doc,out); replyOk(id,"account snapshot",out);
}

bool CommandRouter::clearAccountAuthorizations(int32_t id) {
  if (m_wxCred) for (size_t slot=0;slot<wx::CredentialStore::kMaxSlots;++slot) {
    if (m_wxCred->remove(slot)!=Result::Ok) {
      replyError(id,Result::StorageError,"authorization cleanup failed; account table unchanged; reimport cleared slots"); return false;
    }
  }
  return true;
}

void CommandRouter::handleAccountPatch(JsonObjectConst request,int32_t id) {
  if (!m_accounts || !m_config || !request.containsKey("expectedRevision") || !request["fields"].is<JsonObjectConst>()) {
    replyError(id,Result::InvalidParam,"account patch requires snapshot revision and fields"); return;
  }
  const bool creating=request["create"] | false;
  const String target=request["target"] | "";
  const int index=m_accounts->indexOfLabel(target);
  CheckinProfile previous, profile;
  if (!creating && (index<0 || !m_accounts->get(index,previous))) { replyError(id,Result::NotFound,"account not found"); return; }
  if (creating) {
    const auto defaults=m_config->storedProfile();
    profile.latitude=defaults.latitude; profile.longitude=defaults.longitude; profile.address=defaults.address;
  } else profile=previous;
  JsonObjectConst fields=request["fields"].as<JsonObjectConst>();
  bool changed=false;
  for (JsonPairConst field:fields) if (!field.value().is<const char*>() || strlen(field.value().as<const char*>())>0) changed=true;
  if (!changed) { replyError(id,Result::InvalidParam,"no changed fields"); return; }
  const char* error="invalid account patch";
  if (!mergeAccountPatch(profile,fields,creating,error)) { replyError(id,Result::InvalidParam,error); return; }
  if (creating && m_accounts->indexOfLabel(profile.label)>=0) { replyError(id,Result::InvalidParam,"label already exists; use update"); return; }
  if (creating && m_accounts->count()>=AccountStore::kMaxProfiles) { replyError(id,Result::InvalidParam,"maximum 6 accounts"); return; }
  const int collision=m_accounts->indexOfLabel(profile.label);
  if (!creating && collision>=0 && collision!=index) { replyError(id,Result::InvalidParam,"label already exists"); return; }
  const bool identityChanged=!creating && previous.studentId!=profile.studentId;
  if (identityChanged) { profile.lastDate=""; profile.lastOk=false; profile.lastMsg=""; }
  const int authorizationSlot=creating?static_cast<int>(m_accounts->count()):index;
  if (m_wxCred && (creating || identityChanged) && m_wxCred->remove(authorizationSlot)!=Result::Ok) {
    replyError(id,Result::StorageError,"cannot clear prior authorization; account unchanged"); return;
  }
  const Result saved=creating?m_accounts->addOrUpdate(profile):m_accounts->replaceAt(index,profile);
  if (saved!=Result::Ok) { replyError(id,saved,"save account patch failed"); return; }
  if (identityChanged) { m_cookies->clear(); m_checkin->resetLoginState(); m_api->clearOperationTicket(); }
  bool linksOk=true;
  if (!creating && previous.label!=profile.label) {
    for(size_t i=0;i<m_config->notifyChannelCount();++i) {
      const auto channel=m_config->notifyChannelAt(i);
      String csv=","+channel.accountsCsv+",";
      const String oldToken=","+previous.label+",";
      if(csv.indexOf(oldToken)<0) continue;
      csv.replace(oldToken,","+profile.label+",");
      csv=csv.substring(1,csv.length()-1);
      if(m_config->linkNotifyChannel(channel.label,csv)!=Result::Ok) linksOk=false;
    }
  }
  JsonDocument output;
  output["label"]=profile.label; output["revision"]=m_accounts->revision();
  output["authorizationCleared"]=creating||identityChanged; output["notificationLinksUpdated"]=linksOk;
  String out; serializeJson(output,out);
  replyOk(id,linksOk?"account patch saved":"account saved; notification links need manual repair",out);
}

void CommandRouter::handleSelected(JsonObjectConst request,int32_t id) {
  const String op=request["op"] | "";
  if (!m_accounts || !m_config || !m_checkin || !request.containsKey("expectedRevision") ||
      !request["targets"].is<JsonArrayConst>() ||
      (op!="test.login" && op!="test.chain" && op!="test.checkin" && op!="test.multi")) {
    replyError(id,Result::InvalidParam,"selected execution requires supported op, targets and snapshot revision"); return;
  }
  std::vector<CheckinProfile> queue;
  const char* targetError="invalid target selection";
  if(!prepareSelectedAccounts(request["targets"].as<JsonArrayConst>(),*m_accounts,m_config->storedProfile(),queue,targetError)) {
    replyError(id,Result::InvalidParam,targetError); return;
  }
  const int wait=request["wait_s"] | 0;
  if(wait<0 || wait>900) { replyError(id,Result::InvalidParam,"invalid wait time"); return; }
  auto original=m_config->deviceProfile(); original.label=m_config->activeAccount();
  JsonDocument output; JsonArray rows=output["accounts"].to<JsonArray>();
  for(size_t i=0;i<queue.size();++i) {
    const auto& profile=queue[i];
    JsonDocument event; event["account"]=profile.label; event["index"]=i+1; event["of"]=queue.size(); event["stage"]="selected";
    String ev; serializeJson(event,ev); emitEvent("progress",ev.substring(1,ev.length()-1));
    m_cookies->clear(); m_checkin->resetLoginState(); m_api->clearOperationTicket(); m_config->applyProfile(profile);
    WorkItem item; item.requestId=id; item.force=request["force"] | false;
    snprintf(item.payload,sizeof(item.payload),"%d",wait);
    m_captureReply=true; m_capturedOk=false; m_capturedMessage="no result"; m_capturedData="";
    if(op=="test.login") workTestLogin(item);
    else if(op=="test.chain") workTestChain(item);
    else if(op=="test.checkin" || !(request["probe"] | true)) workTestCheckin(item);
    else { workTestLogin(item); if(m_capturedOk) workTestProbe(item); }
    m_captureReply=false;
    JsonObject row=rows.add<JsonObject>(); row["label"]=profile.label; row["operation"]=op; row["ok"]=m_capturedOk; row["msg"]=m_capturedMessage;
    if(m_capturedData.length()) {
      JsonDocument detail;
      if(!deserializeJson(detail,m_capturedData)) {
        row["data"].set(detail.as<JsonVariantConst>());
        for(const char* flag:{"sessionWorks","sessionValid","appEntry"})
          if(detail[flag].is<bool>() && !detail[flag].as<bool>()) m_capturedOk=false;
      } else {
        m_capturedOk=false; row["msg"]="execution detail incomplete; query account status before retry";
      }
    }
    row["ok"]=m_capturedOk;
    row["status"]=m_capturedMessage=="skipped"?"skipped":m_capturedOk?"passed":"failed";
    JsonDocument done; done["account"]=profile.label; done["ok"]=m_capturedOk; done["stage"]="completed";
    String completed; serializeJson(done,completed); emitEvent("progress",completed.substring(1,completed.length()-1));
    if(i+1<queue.size() && m_config->multiGapSeconds()) vTaskDelay(pdMS_TO_TICKS(m_config->multiGapSeconds()*1000UL));
  }
  m_config->applyProfile(original); m_cookies->clear(); m_checkin->resetLoginState(); m_api->clearOperationTicket();
  if(output.overflowed()) { replyError(id,Result::InternalError,"execution finished but summary unavailable; query account status before retry"); return; }
  String out; serializeJson(output,out); replyOk(id,"selected execution completed",out);
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
  // 每个账号可单独设置是否生效；请求里不带 enabled 时**沿用已有开关**
  // （否则"改个密码"会把被关掉的账号偷偷重新打开，当晚就多打一次卡）。
  profile.enabled = request["enabled"] | true;
  const int existingIndex = m_accounts->indexOfLabel(profile.label);
  if (existingIndex >= 0 && !request.containsKey("enabled")) {
    CheckinProfile existing;
    if (m_accounts->get(static_cast<size_t>(existingIndex), existing)) {
      profile.enabled = existing.enabled;
    }
  }
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
  CheckinProfile prior;
  const bool identityChanged=existingIndex>=0 && m_accounts->get(existingIndex,prior) && prior.studentId!=profile.studentId;
  const int slot=existingIndex>=0?existingIndex:static_cast<int>(m_accounts->count());
  if ((identityChanged || existingIndex<0) && slot<static_cast<int>(AccountStore::kMaxProfiles) && m_wxCred && m_wxCred->remove(slot)!=Result::Ok) {
    replyError(id,Result::StorageError,"cannot clear prior authorization; account unchanged"); return;
  }
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
  // 单人/多人开关已取消：账号表里有几条就跑几条，只保留"账号间隔"这一项。
  if (request.containsKey("gap_s")) {
    const uint16_t gap = request["gap_s"] | static_cast<uint16_t>(20);
    if (m_config->setMultiGapSeconds(gap) != Result::Ok) {
      replyError(id, Result::InvalidParam, "gap_s must be 0..600");
      return;
    }
  }
  char data[160];
  snprintf(data, sizeof(data), "{\"gapSeconds\":%u,\"accounts\":%u,\"enabled\":%u}",
           static_cast<unsigned>(m_config->multiGapSeconds()),
           m_accounts == nullptr ? 0u : static_cast<unsigned>(m_accounts->count()),
           m_accounts == nullptr ? 0u : static_cast<unsigned>(m_accounts->enabledCount()));
  replyOk(id, "account gap updated", data);
}

void CommandRouter::handleAccountEnable(JsonObjectConst request, int32_t id) {
  if (m_accounts == nullptr) {
    replyError(id, Result::NotReady, "account store unavailable");
    return;
  }
  const String label = request["label"] | "";
  if (label.length() == 0) {
    replyError(id, Result::InvalidParam, "label required");
    return;
  }
  if (!request["on"].is<bool>()) {
    replyError(id, Result::InvalidParam, "on (true/false) required");
    return;
  }
  const bool on = request["on"].as<bool>();
  const Result result = m_accounts->setEnabled(label, on);
  if (result == Result::NotFound) {
    replyError(id, result, "account not found");
    return;
  }
  if (result != Result::Ok) {
    replyError(id, result, "save failed");
    return;
  }
  char data[128];
  snprintf(data, sizeof(data), "{\"label\":\"%s\",\"on\":%s,\"enabled\":%u,\"total\":%u}",
           label.c_str(), on ? "true" : "false",
           static_cast<unsigned>(m_accounts->enabledCount()),
           static_cast<unsigned>(m_accounts->count()));
  replyOk(id, on ? "account enabled" : "account disabled", data);
}

void CommandRouter::handleLogs(JsonObjectConst request, int32_t id) {
  const int32_t requested = request["n"] | 30;
  const int32_t lines = constrain(requested, 1, 60);
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
    case WorkKind::Command: {
      // 先转移所有权，确保 dispatchCommand 的所有返回分支都释放完整命令。
      std::unique_ptr<char[]> command(item.command);
      if (command) dispatchCommand(String(command.get()));
      break;
    }
    case WorkKind::WifiConnect: workWifiConnect(item); break;
    case WorkKind::WifiScan: workWifiScan(item); break;
    case WorkKind::TimeSync: workTimeSync(item); break;
    case WorkKind::TestHttp: workTestHttp(item); break;
    case WorkKind::TestLogin: workTestLogin(item); break;
    case WorkKind::TestChain: workTestChain(item); break;
    case WorkKind::TestProbe: workTestProbe(item); break;
    case WorkKind::TestCheckin: workTestCheckin(item); break;
    case WorkKind::Probe:
      // 预登录体检：按账号表逐个体检（runAll 内部只做体检不提交）。
      // 表为空时 runAll 自动退回设备自身配置 = 「列表第一个用户」。
      {
        WorkItem probeItem;
        probeItem.kind = WorkKind::MultiCheckin;
        probeItem.force = false;
        probeItem.requestId = 0;  // 0 = 来自调度器（回执只给 BLE 事件，不发 replyOk）
        memcpy(probeItem.payload, "probe", 6);
        workMultiCheckin(probeItem);
      }
      break;
    case WorkKind::DailyCheckin:
      // 打卡同样只有这一条路：单人 = 表里一条，多人 = 表里多条。
      workMultiCheckin(item);
      break;
    case WorkKind::MultiCheckin: workMultiCheckin(item); break;
    case WorkKind::NotifyTest: workNotifyTest(item); break;
    case WorkKind::DeadlineMissed: workDeadlineMissed(item); break;
    case WorkKind::WxTest: workWxTest(item); break;
    case WorkKind::WxPrefetch: workWxPrefetch(item); break;
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
    // 网络不可用：入队等链路恢复后再投递（同键按 notifyGapSeconds 去重，
    // 这里再按小时分桶，确保断网期间最多每小时一条）。
    if (m_notify != nullptr) {
      const uint32_t hour = m_time != nullptr ? m_time->now() / 3600 : 0;
      m_notify->post(NotifyKind::NetDown,
                     "Wi-Fi 连接失败（SSID 不可达或口令错误），设备已离线，恢复后会自动补发。",
                     String("net/") + String(static_cast<unsigned>(hour)));
    }
    replyError(item.requestId, result, "wifi connect failed (check ssid/password)");
    return;
  }

  String eventJson = "\"state\":\"connected\",\"ip\":\"" + m_wifi->ip() + "\",\"rssi\":" +
                     String(m_wifi->rssi());
  emitEvent("wifi", eventJson);

  m_time->begin();
  Result timeResult = m_time->waitSynced(20000);
  // SNTP 的首次轮询可能早于 DNS 就绪而失败（失败后默认间隔很长），这里主动重启 SNTP 再试两次。
  for (int attempt = 0; timeResult != Result::Ok && attempt < 2; attempt++) {
    LOG_W(kTag, "time sync retry %d/2", attempt + 1);
    vTaskDelay(pdMS_TO_TICKS(3000));
    m_time->begin();
    timeResult = m_time->waitSynced(15000);
  }
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
  // 原因不是"窗口外不给授权"（§8.7 已定案：授权是会话属性，靠门户预热解锁，与时段无关），
  // 而是 SESSION TTL 实测约 1 小时 + 提交只接受窗口内 —— 窗口外登录到点必过期，白费一次。
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

void CommandRouter::workTestProbe(const WorkItem& item) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"probe\",\"detail\":\"checking session\"");

  ProbeReport report;
  String error;
  const Result result = m_checkin->probe(report, error);
  setBusy(false);

  if (result != Result::Ok) {
    flashResult(false);
    replyError(item.requestId, result, error);
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

  // 手动 test.probe：只回结果。定时触发的体检（Trigger::Probe）现在一律走
  // workMultiCheckin(probe)，21:00 的每日状态摘要在那里发。
  replyOk(item.requestId,
          report.sessionValid ? "probe done" : "probe done: session invalid, run test.login", data);
}

void CommandRouter::workTestCheckin(const WorkItem& item) {
  setBusy(true);
  emitEvent("progress", "\"stage\":\"checkin\",\"detail\":\"start\"");

  String message;
  const CheckinOutcome outcome = m_checkin->perform(item.force, message);
  setBusy(false);

  const bool ok = (outcome == CheckinOutcome::Success) || (outcome == CheckinOutcome::AlreadyDone);
  const bool pending = (outcome == CheckinOutcome::Pending);
  if (!pending) flashResult(ok);  // Pending 既非成功也非失败，不闪灯

  const String todayKey = m_time != nullptr ? m_time->todayString() : String();
  // 本次手动打卡归属的账号标签（无账号表时 = "device"）：逐账号关联的额外通道靠它匹配
  const String who = m_config != nullptr ? m_config->activeAccount() : String();

  // 持久化本轮结果。★ 只有「当前只有一个账号参与」才允许写全局 last —— 那是
  // 调度器今晚不再重试的凭证；多账号时手动替某一个人补打，若写成 last=ok，
  // 剩下的人今晚就再也不会被自动打了。多人场景只记到该账号自己的当天结果上。
  const bool soleAccount = (m_accounts == nullptr) || (m_accounts->enabledCount() <= 1);
  if ((outcome == CheckinOutcome::Success) || (outcome == CheckinOutcome::AlreadyDone) ||
      (outcome == CheckinOutcome::Failed)) {
    if (m_config != nullptr) {
      if (soleAccount) m_config->setLastResult(todayKey, ok, message);
      if (m_accounts != nullptr && m_accounts->indexOfLabel(who)>=0) {
        m_accounts->setResult(m_config->activeAccount(), todayKey, ok, message);
        LOG_I(kTag, "manual check-in for %s recorded per-account; global last untouched",
              m_config->activeAccount().c_str());
      }
    }
  }
  switch (outcome) {
    case CheckinOutcome::Success:
      emitEvent("result", "\"ok\":true,\"msg\":\"" + escapeJson(message) + "\"");
      if (m_notify != nullptr) {
        m_notify->post(NotifyKind::CheckinOk, "打卡成功：" + message, todayKey + "/ok", who);
      }
      replyOk(item.requestId, message.isEmpty() ? "checked in" : message);
      break;
    case CheckinOutcome::AlreadyDone:
      emitEvent("result", "\"ok\":true,\"already\":true,\"msg\":\"" + escapeJson(message) + "\"");
      if (m_notify != nullptr) {
        m_notify->post(NotifyKind::CheckinOk, "今日已打卡：" + message, todayKey + "/ok", who);
      }
      replyOk(item.requestId, message.isEmpty() ? "already checked in" : message);
      break;
    case CheckinOutcome::OutOfWindow:
      // 手动路径不会重试：只回错误。窗口内的自动重试与「第 N 次重试」通知在 workMultiCheckin。
      replyError(item.requestId, Result::OutOfWindow, message);
      break;
    case CheckinOutcome::NeedMfa:
      emitEvent("alert", "\"kind\":\"mfa\",\"msg\":\"second factor required\"");
      if (m_notify != nullptr) {
        m_notify->post(NotifyKind::NeedMfa, "打卡被二次验证拦住：需要浏览器指纹或验证码",
                       todayKey + "/mfa", who);
      }
      replyError(item.requestId, Result::NeedMfa, message);
      break;
    case CheckinOutcome::Failed:
      emitEvent("result", "\"ok\":false,\"msg\":\"" + escapeJson(message) + "\"");
      if (m_notify != nullptr) {
        m_notify->post(NotifyKind::CheckinFail, "打卡失败：" + message, todayKey + "/fail", who);
      }
      replyError(item.requestId, Result::InternalError, message);
      break;
    case CheckinOutcome::Skipped:
      replyOk(item.requestId, "skipped");
      break;
    case CheckinOutcome::Pending:
      // 会话已建立但平台尚未放行（实测有数分钟生效延迟）：**不算失败**。
      // 手动路径不会重试，所以这里只回错误、不发「第 N 次重试」通知
      // —— 窗口内的自动重试与重试通知都由 workMultiCheckin 承担。
      emitEvent("progress",
                "\"stage\":\"checkin\",\"detail\":\"pending platform authorization\"");
      replyError(item.requestId, Result::AppUnavailable, message);
      break;
  }
}

String CommandRouter::multiRunMessage(const MultiCheckin::RunResult& run, bool probeOnly) const {
  if (run.total == 0) {
    // 账号表为空并且设备自身也没配过凭证
    return "no accounts configured (use account.add, or auth + loc for a single user)";
  }
  if (run.enabledTotal == 0) {
    return String("all ") + String(static_cast<unsigned>(run.total)) +
               " accounts are switched off (account.enable <label> 1 to turn one on)";
  }
  String message = String(probeOnly ? "probe" : "check-in") + " run: " +
                   String(static_cast<unsigned>(run.okCount)) + "/" +
                   String(static_cast<unsigned>(run.enabledTotal)) + " ok";
  if (run.disabledCount > 0) {
    message += String(" (") + String(static_cast<unsigned>(run.disabledCount)) + " switched off)";
  }
  if (run.okCount < run.enabledTotal) message += " - see data for each account's reason";
  return message;
}

String CommandRouter::multiStatusDigest(const MultiCheckin::RunResult& run) const {
  // 正文上限 256 B（NotifyItem::body），这里控制在 ~240 字节以内。
  String out;
  if (m_time != nullptr) out += m_time->nowString("%H:%M") + " ";
  out += String("生效") + String(static_cast<unsigned>(run.enabledTotal)) + "人";
  if (run.disabledCount > 0) {
    out += String("，关闭") + String(static_cast<unsigned>(run.disabledCount)) + "人";
  }
  out += String("，成功") + String(static_cast<unsigned>(run.okCount)) + "人；";

  JsonDocument doc;
  if (deserializeJson(doc, run.summary) == DeserializationError::Ok) {
    bool first = true;
    for (JsonObject one : doc.as<JsonArray>()) {
      const String oneResult = one["result"] | "?";
      if (oneResult == "disabled") continue;  // 关掉的账号不进每日摘要
      if (!first) out += "、";
      first = false;
      out += String(one["label"] | "?") + "=" + oneResult;
    }
  }
  if (m_config != nullptr) {
    out += String("｜窗口") + m_config->schedule().windowStart + "-" +
           m_config->schedule().windowEnd + " 打卡" + m_config->schedule().checkinAt;
  }
  return out;
}

void CommandRouter::workMultiCheckin(const WorkItem& item) {
  setBusy(true);
  const bool probeOnly = (strcmp(item.payload, "probe") == 0);

  MultiCheckin::RunResult run;
  if (m_multi != nullptr) {
    run = m_multi->runAll(item.force, probeOnly);
  }
  const size_t okCount = run.okCount;
  const size_t total = run.enabledTotal;  // ★ 分母只算生效账号

  setBusy(false);
  // 生效账号为 0 = 「今晚本来就没有要打卡的人」，不该闪失败灯
  if (total > 0) flashResult(okCount >= total);

  const String message = multiRunMessage(run, probeOnly);
  emitEvent("result",
            String("\"kind\":\"multi\",\"ok\":") + (total > 0 && okCount >= total ? "true" : "false") +
                ",\"accounts\":" + run.summary);
  if (item.requestId != 0) {
    replyOk(item.requestId, message, run.summary);
  }

  // ★ 必须把"整轮结论"写进 AppConfig::last —— 调度器的重试闸门与 DeadlineMissed
  //   判定都只看 isLastResultOkFor(today)。
  //   分母**只能**用生效账号数：把单独关闭的账号算进去，就永远凑不满 allOk，
  //   于是"全员成功仍整夜每 180 s 重跑一轮"的老 bug 会换个形式复发。
  //   体检（probeOnly）不提交打卡，因此**不能**写这个结论。
  const bool allOk = (total > 0 && okCount >= total);
  // 账号都在、但被**逐个单独关闭**了（enabledTotal==0）= 今晚没有需要打卡的人。
  // 必须把它当成「今晚已了结」写进 last，否则调度器会空转重试到 23:30，
  // 还会发一条「窗口已过仍未成功」的误报。
  const bool nothingToDo = (total == 0 && run.total > 0);
  const bool settled = allOk || nothingToDo;
  if (!probeOnly && m_config != nullptr && m_time != nullptr && m_time->isSynced()) {
    m_config->setLastResult(m_time->todayString(), settled, message);
    LOG_I(kTag, "aggregate saved: %u/%u ok -> last=%s", static_cast<unsigned>(okCount),
          static_cast<unsigned>(total), settled ? "confirmed" : "pending retry");
  }

  if (m_notify == nullptr || m_time == nullptr) return;

  if (probeOnly) {
    // 每天 21:00 的固定状态摘要（key=今天 -> 同一天不会重发）。
    // 即使 0 个账号生效也照发 —— 摘要本身就会说明「生效 0 人」，这正是它的作用。
    if (item.requestId != 0) return;  // 手动 probe 不占用当天这条
    const String digest = multiStatusDigest(run);
    m_notify->post(NotifyKind::StatusDigest, digest, m_time->todayString());
    return;
  }

  // 打卡结果通知：没有人需要打卡（没配表，或全部单独关闭）时一条不发，
  // 否则调度器每 180 s 一轮会反复推「本轮 0/0 成功」这种废话。
  if (total == 0) return;

  // 打卡：只发一条汇总（不逐账号刷屏）
  String body = String("本轮 ") + String(static_cast<unsigned>(okCount)) + "/" +
                String(static_cast<unsigned>(total)) + " 成功";
  if (run.disabledCount > 0) {
    body += String("（另有 ") + String(static_cast<unsigned>(run.disabledCount)) + " 个已单独关闭）";
  }
  JsonDocument doc;
  if (deserializeJson(doc, run.summary) == DeserializationError::Ok) {
    String failed;
    for (JsonObject one : doc.as<JsonArray>()) {
      const String oneResult = one["result"] | "";
      // 用 ok 布尔判成功（result 是 outcomeName："success"/"already_done"/…，别按字面 "ok" 比）
      if ((one["ok"] | false) || oneResult == "disabled") continue;
      if (failed.length() > 0) failed += "、";
      failed += String(one["label"] | "?") + "(" + oneResult + ")";
    }
    if (failed.length() > 0) body += "；未成功：" + failed;
  }
  const uint8_t round = m_notify->bumpAttempt();
  // 全成功：一天一条（固定 key，后续轮次被去重挡住）。
  // 有未成功：调度器还会重试 → 按 3 轮一组限流（只报第 1、4、7… 轮），
  // 最终失败由 DeadlineMissed 收尾告警。
  const bool announce = allOk || (round % 3 == 1);
  if (!announce) return;
  if (!allOk) {
    body += "；将在 " + String(AppConst::kCheckinRetrySeconds / 60) + " 分钟后重试";
  }
  // 汇总只进默认通道：额外通道走下面的逐账号事件，避免"汇总+单人结果"重复刷屏。
  m_notify->post(allOk ? NotifyKind::CheckinOk : NotifyKind::CheckinFail, body,
                 m_time->todayString() +
                     (allOk ? String("/multi")
                            : String("/multi-fail/") +
                                  String(static_cast<unsigned>((round - 1) / 3))),
                 String(), NotifyTarget::DefaultOnly);

  // ---- 逐账号单独推送（0.4.0）：只发给关联了该账号的额外通道 ----
  if (deserializeJson(doc, run.summary) == DeserializationError::Ok) {
    const String today = m_time->todayString();
    const unsigned bucket = static_cast<unsigned>((round - 1) / 3);
    for (JsonObject one : doc.as<JsonArray>()) {
      const String label = one["label"] | "";
      const String oneResult = one["result"] | "";
      if (label.length() == 0 || oneResult == "disabled") continue;
      const String msg = one["msg"] | "";
      if (one["ok"] | false) {
        m_notify->post(NotifyKind::CheckinOk,
                       label + "：" + (oneResult == "already_done" ? "今日已打卡"
                                                                    : "打卡成功 " + msg),
                       today + "/" + label + "/ok", label, NotifyTarget::AccountOnly);
      } else if (oneResult == "need_mfa") {
        m_notify->post(NotifyKind::NeedMfa, label + "：被二次验证拦住 " + msg,
                       today + "/" + label + "/mfa", label, NotifyTarget::AccountOnly);
      } else if (msg.length() > 0) {
        // 未成功：同样只在播报轮（第 1、4、7… 次）发，避免每 3 分钟刷屏
        m_notify->post(NotifyKind::CheckinFail, label + "：本轮未成功（" + oneResult + "）" + msg,
                       today + "/" + label + "/fail/" + String(bucket), label,
                       NotifyTarget::AccountOnly);
      }
    }
  }
}

// ---------------------------------------------------------------- 通知

void CommandRouter::handleNotifySet(JsonObjectConst request, int32_t id) {
  if (m_config == nullptr) {
    replyError(id, Result::NotReady, "config unavailable");
    return;
  }
  // 没给的字段传 -1 / 空串表示"不改这一项"
  int enabled = -1;
  int style = -1;
  int gap = -1;
  int quota = -1;
  if (request["on"].is<bool>()) enabled = request["on"].as<bool>() ? 1 : 0;
  if (request["style"].is<int>()) style = request["style"].as<int>();
  if (request["gap"].is<int>()) gap = request["gap"].as<int>();
  if (request["quota"].is<int>()) quota = request["quota"].as<int>();
  const String url = request["url"] | "";

  const Result result = m_config->setNotify(enabled, style, url, gap, quota);
  if (result != Result::Ok) {
    replyError(id, result, "save notify config failed");
    return;
  }
  replyOk(id, "notify config saved", m_notify != nullptr ? m_notify->statusJson() : String());
}

// 额外推送通道（0.4.0）：每条通道 = 标签 + 推送地址 + style + 开关 + 关联账号。
// 关联账号支持数组 ["A01","A02"] 或字符串 "A01,A02" / "all"（空=全部账号与全局事件）。
void CommandRouter::handleNotifyChannel(const String& op, JsonObjectConst request, int32_t id) {
  if (m_config == nullptr) {
    replyError(id, Result::NotReady, "config unavailable");
    return;
  }
  const String label = request["label"] | "";
  if (label.length() == 0) {
    replyError(id, Result::InvalidParam, "label required");
    return;
  }

  // 解析 accounts 字段：数组或逗号分隔串（缺省=不限制，收全部）
  String accountsCsv;
  if (op == "notify.add" || op == "notify.link") {
    JsonVariantConst acc = request["accounts"];
    if (acc.is<const char*>()) {
      accountsCsv = acc.as<const char*>();
    } else if (acc.is<JsonArray>()) {
      for (JsonVariantConst v : acc.as<JsonArrayConst>()) {
        if (accountsCsv.length() > 0) accountsCsv += ",";
        accountsCsv += v.as<String>();
      }
    }
  }

  Result result = Result::Ok;
  if (op == "notify.add") {
    const String url = request["url"] | "";
    const int style = request["style"] | 2;  // 默认 post_json：Server酱 desp 自动识别最稳
    const int on = request["on"].is<bool>() ? (request["on"].as<bool>() ? 1 : 0) : 1;
    result = m_config->addOrUpdateNotifyChannel(label, url, style, on, accountsCsv);
    if (result == Result::Ok) {
      replyOk(id, "notify channel saved", m_notify != nullptr ? m_notify->statusJson() : String());
      return;
    }
    replyError(id, result,
               "invalid channel (label: A-Z a-z 0-9 - _ <=12; url required; style 0..2; "
               "accounts: \"A01,A02\" | [\"A01\"] | \"all\"; max 5 channels)");
    return;
  }
  if (op == "notify.remove") {
    result = m_config->removeNotifyChannel(label);
    if (result == Result::Ok) {
      replyOk(id, "notify channel removed",
              m_notify != nullptr ? m_notify->statusJson() : String());
      return;
    }
    replyError(id, result, "channel not found");
    return;
  }
  if (op == "notify.enable") {
    const bool on = request["on"].is<bool>() ? request["on"].as<bool>() : true;
    result = m_config->setNotifyChannelEnabled(label, on);
    if (result == Result::Ok) {
      replyOk(id, on ? "notify channel on" : "notify channel off",
              m_notify != nullptr ? m_notify->statusJson() : String());
      return;
    }
    replyError(id, result, "channel not found");
    return;
  }
  // notify.link
  result = m_config->linkNotifyChannel(label, accountsCsv);
  if (result == Result::Ok) {
    replyOk(id, "notify channel linked", m_notify != nullptr ? m_notify->statusJson() : String());
    return;
  }
  replyError(id, result, "channel not found (or invalid accounts)");
}

void CommandRouter::workNotifyTest(const WorkItem& item) {
  setBusy(true);
  String body = String(item.payload);

  // 载荷可能带通道前缀："标签\n正文"（见 dispatchCommand 的 test.notify）。
  // 只有当首行确实是一个已存在的通道标签时才切分，否则整串按正文处理（向后兼容）。
  uint8_t channel = 0;
  String chanError;
  const int nl = body.indexOf('\n');
  if (nl > 0) {
    const String label = body.substring(0, nl);
    const int idx = (m_config != nullptr) ? m_config->notifyChannelIndexByLabel(label) : -1;
    if (idx >= 0) {
      channel = static_cast<uint8_t>(idx + 1);
      body = body.substring(nl + 1);
    } else if (label.length() <= 12 && label.indexOf(' ') < 0) {
      chanError = "channel not found: " + label;
      body = body.substring(nl + 1);
    }
  } else if (nl == 0) {
    body = body.substring(1);  // 空标签 = 默认通道
  }
  if (body.length() == 0) {
    body = "这是一条来自打卡器的测试通知";
    if (m_time != nullptr) body = m_time->nowString() + " " + body;
  }

  int status = 0;
  String error = chanError;
  bool sent = false;
  String targetUrl = m_config != nullptr ? m_config->notifyUrlMasked() : String("<unset>");
  int targetStyle = m_config != nullptr ? static_cast<int>(m_config->notifyStyle()) : -1;
  if (m_notify == nullptr) {
    error = "notify service unavailable";
  } else if (channel > 0 && m_config != nullptr) {
    const NotifyChannel& ch = m_config->notifyChannelAt(channel - 1);
    targetUrl = AppConfig::maskUrl(ch.url);
    targetStyle = static_cast<int>(ch.style);
    if (!ch.enabled) {
      error = "通道已关闭：notify enable 打开后再试";
    } else {
      sent = m_notify->sendNow(NotifyKind::Info, body, &status, error, channel);
    }
  } else if (!m_notify->active()) {
    error = "通知未启用或 URL 为空：先执行 notify set url=<推送地址> on=1";
  } else {
    sent = m_notify->sendNow(NotifyKind::Info, body, &status, error);
  }
  setBusy(false);
  flashResult(sent);

  char data[256];
  snprintf(data, sizeof(data),
           "{\"sent\":%s,\"httpStatus\":%d,\"url\":\"%s\",\"style\":%d,\"channel\":%u}",
           sent ? "true" : "false", status, targetUrl.c_str(), targetStyle,
           static_cast<unsigned>(channel));
  if (sent) {
    replyOk(item.requestId, "notification sent: " + body, String(data));
  } else {
    replyError(item.requestId, error.length() > 0 ? Result::NetworkError : Result::InternalError,
               error);
  }
}

void CommandRouter::workDeadlineMissed(const WorkItem& item) {
  (void)item;  // 定时触发，无回执
  emitEvent("alert", "\"kind\":\"deadline\",\"msg\":\"window closed without success\"");
  if (m_notify == nullptr || m_time == nullptr) return;

  // 把"最后一次尝试为什么没成功"一起带过去，手机上看一眼就知道要处理什么
  String reason;
  if (m_config != nullptr) {
    JsonDocument doc;
    const String last = m_config->lastResultJson();
    if (last.length() > 0 && deserializeJson(doc, last) == DeserializationError::Ok) {
      reason = doc["msg"] | "";
      if (doc["ok"].is<bool>() && doc["ok"].as<bool>()) reason = "（记录显示已成功？）" + reason;
    }
  }
  String body = "打卡窗口已关闭（23:30），今天仍未成功。";
  if (reason.length() > 0) body += "最后原因：" + reason;
  m_notify->post(NotifyKind::DeadlineMissed, body, m_time->todayString() + "/deadline");

  // 逐账号告警：关联了该账号的额外通道，各自收到自己那条（0.4.0）。
  // 判定只看账号表当天的确认结果；表为空（单人在默认配置上）时上面的全局条已覆盖。
  if (m_accounts != nullptr) {
    const String today = m_time->todayString();
    CheckinProfile profile;
    for (size_t i = 0; i < m_accounts->count(); i++) {
      if (!m_accounts->get(i, profile) || !profile.enabled) continue;
      if (m_accounts->isCheckedInToday(profile.label, today)) continue;
      String oneBody = profile.label + "：窗口已关闭，今天仍未打卡成功。";
      if (profile.lastMsg.length() > 0 && profile.lastDate == today) {
        oneBody += "最后原因：" + profile.lastMsg;
      }
      m_notify->post(NotifyKind::DeadlineMissed, oneBody, today + "/" + profile.label + "/deadline",
                     profile.label, NotifyTarget::AccountOnly);
    }
  }
}

// ---------------------------------------------------------------- 微信授权链自测


Result CommandRouter::ensureWxTicket(int slot, String& outTicket, String& outError) {
  const char* const tag = "wx_ticket";
  outTicket = "";
  if (m_wxCred == nullptr || m_api == nullptr) {
    outError = "wx store/api not ready";
    return Result::NotReady;
  }
  if (slot < 0 || slot >= static_cast<int>(wx::CredentialStore::kMaxSlots)) {
    outError = "bad wx slot";
    return Result::InvalidParam;
  }
  wx::OauthCredentials cred;
  if (m_wxCred->load(static_cast<size_t>(slot), cred) != Result::Ok) {
    outError = "no wx credential in slot " + String(slot) + " (wx.import needed)";
    return Result::NotFound;
  }
  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (cred.needsRefresh(now)) {
    LOG_I(tag, "refreshing wx credentials (slot %d)", slot);
    HttpResponse raw;
    wx::OauthCredentials rotated;
    if (wx::refreshCredentials(cred, rotated, raw) != wx::OauthResult::Ok) {
      outError = "wx credential refresh rejected (re-scan needed)";
      return Result::AuthFailed;
    }
    m_wxCred->save(static_cast<size_t>(slot), rotated);
    cred = rotated;
  }
  String loginBuffer;
  HttpResponse raw;
  wx::OauthResult lb = wx::fetchLoginBuffer(cred, loginBuffer, raw);
  if (lb == wx::OauthResult::Rejected) {
    // access token 可能被服务端提前作废（实测首次授权后 ~8 分钟即发生）：
    // 强制刷新凭据并原地重试一次，refresh token 仍有效即可自愈
    LOG_W(tag, "login_buffer rejected, force refresh");
    HttpResponse rawR;
    wx::OauthCredentials rotated;
    if (wx::refreshCredentials(cred, rotated, rawR) == wx::OauthResult::Ok) {
      m_wxCred->save(static_cast<size_t>(slot), rotated);
      cred = rotated;
      lb = wx::fetchLoginBuffer(cred, loginBuffer, raw);
    }
  }
  if (lb != wx::OauthResult::Ok) {
    outError = "login_buffer fetch failed";
    return Result::AuthFailed;
  }
  wx::WxCodeResult code1 = wx::wxGetCode(loginBuffer, kWxCheckinAppId);
  if (code1.result != wx::WxResult::Ok) {
    outError = String("wx get_code#1 failed: ") + code1.errorDetail;
    return Result::AuthFailed;
  }
  LOG_I(tag, "code#1 ok (%u chars)", code1.code.length());
  bool bind = false;
  String mpSession;
  if (m_api->checkWechatBind(code1.code, bind, mpSession) != Result::Ok || !bind ||
      mpSession.length() == 0) {
    outError = "check-bind failed or wechat not bound to this account";
    return Result::AuthFailed;
  }
  HttpResponse raw2;
  if (wx::fetchLoginBuffer(cred, loginBuffer, raw2) != wx::OauthResult::Ok) {
    HttpResponse rawR;
    wx::OauthCredentials rotated;
    if (wx::refreshCredentials(cred, rotated, rawR) == wx::OauthResult::Ok) {
      m_wxCred->save(static_cast<size_t>(slot), rotated);
      cred = rotated;
      if (wx::fetchLoginBuffer(cred, loginBuffer, raw2) != wx::OauthResult::Ok) {
        outError = "login_buffer#2 fetch failed";
        return Result::AuthFailed;
      }
    } else {
      outError = "login_buffer#2 fetch failed";
      return Result::AuthFailed;
    }
  }
  wx::WxCodeResult code2 = wx::wxGetCode(loginBuffer, kWxCheckinAppId);
  if (code2.result != wx::WxResult::Ok) {
    outError = String("wx get_code#2 failed: ") + code2.errorDetail;
    return Result::AuthFailed;
  }
  if (m_api->requestCheckinTicket(code2.code, mpSession, outTicket) != Result::Ok ||
      outTicket.length() != 43) {
    outError = "checkin-ticket request failed";
    return Result::AuthFailed;
  }
  LOG_I(tag, "ticket issued for slot %d (43 chars)", slot);
  return Result::Ok;
}

void CommandRouter::workWxTest(const WorkItem& item) {
  if (m_wxCred == nullptr) {
    replyError(item.requestId, Result::NotReady, "wx store not ready");
    return;
  }
  int slot = 0;
  if (strncmp(item.payload, "slot=", 5) == 0) slot = atoi(item.payload + 5);
  emitEvent("progress", "\"stage\":\"wx\"");
  String ticket;
  String error;
  const Result result = ensureWxTicket(slot, ticket, error);
  if (result != Result::Ok) {
    replyError(item.requestId, result, error);
    return;
  }
  // 不安装、不提交：仅验证链路。正式打卡由 CheckinService 走 ensureWxTicket 重新取票。
  replyOk(item.requestId, "wx chain ok (device-side ticket issued)",
          "{\"slot\":" + String(slot) + ",\"ticket\":43}");
}

// ---------------------------------------------------------------- 窗口起点取号

void CommandRouter::workWxPrefetch(const WorkItem& item) {
  // 首个启用账号未必是槽位 0；预取也必须验证当前身份与账号标签。
  if (m_wxCred == nullptr || m_api == nullptr || m_config == nullptr) return;
  const String owner = m_config->auth().studentId;
  const int slot=m_accounts?activeAuthorizationSlot<CheckinProfile>(*m_accounts,m_config->activeAccount(),owner):0;
  if(slot<0) { LOG_W("wx_prefetch","active account mapping unavailable; defer to account runner"); return; }
  if (m_api->hasFreshTicketFor(owner)) {
    LOG_I("wx_prefetch", "fresh ticket already installed, skip");
    return;
  }
  String ticket;
  String error;
  LOG_I("wx_prefetch", "prefetching wx ticket for slot %d",slot);
  if (ensureWxTicket(slot, ticket, error) == Result::Ok && ticket.length() == 43) {
    m_api->setOperationTicket(ticket, owner);
    LOG_I("wx_prefetch", "ticket installed (43 chars) for owner %.8s", owner.c_str());
  } else {
    LOG_W("wx_prefetch", "prefetch failed: %s", error.c_str());
  }
}
