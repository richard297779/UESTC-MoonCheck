// ============================================================================
//  main：启动编排
//    1) 日志 → 2) 配置 → 3) LED → 4) 蓝牙（最先行，任何失败都能被手机看到）
//    5) Cookie/HTTP/认证/打卡 → 6) 调度器 → 7) 指令路由
//    8) 创建 net_task（所有网络操作都在此串行执行）
//    9) 若已配网则投递"连接 WiFi + 校时"任务
// ============================================================================

#include <Arduino.h>
#include <esp_mac.h>  // esp_read_mac(ESP_MAC_BT)：广播名要用蓝牙 MAC，才与扫描到的地址一致

#include "app_config.h"
#include "app_types.h"
#include "app_version.h"
#include "ble_service.h"
#include "cas_auth.h"
#include "checkin_api.h"
#include "checkin_service.h"
#include "command_router.h"
#include "cookie_jar.h"
#include "http_client.h"
#include "logger.h"
#include "multi_checkin.h"
#include "notify_service.h"
#include "status_led.h"
#include "task_scheduler.h"
#include "time_service.h"
#include "wifi_manager.h"

namespace {

const char* const kTag = "main";

// ---- 全局对象（静态分配，避免堆碎片）----
AppConfig g_config;
StatusLed g_led;
WifiManager g_wifi;
TimeService g_time;
CookieJar g_cookies;
HttpClient g_http;
CasAuth g_auth;
CheckinApi g_api;
CheckinService g_checkin;
AccountStore g_accounts;
MultiCheckin g_multi;
NotifyService g_notify;
TaskScheduler g_scheduler;
BleService g_ble;
CommandRouter g_router;

/// @brief 生成广播名：优先用自定义标签（`name.set`），否则用蓝牙 MAC 末两字节。
/// @note 早期实现用 `ESP.getEfuseMac() & 0xFFFF`，但那个值的低 16 位其实是 MAC 的
///       **前**两字节（Espressif 的 OUI，同批次全相同）→ 多块板子会撞名。已修正。
String buildDeviceName() { return g_config.bleDeviceName(); }

/// @brief WiFi 状态变化 -> 推送 BLE 事件。
void onWifiStateChanged(WifiState state, const String& info) {
  const char* text = "unknown";
  switch (state) {
    case WifiState::Idle: text = "idle"; break;
    case WifiState::Connecting: text = "connecting"; break;
    case WifiState::Connected: text = "connected"; break;
    case WifiState::Failed: text = "failed"; break;
  }
  String payload = String("\"state\":\"") + text + "\",\"detail\":\"" + info + "\"";
  if (state == WifiState::Connected) {
    payload += ",\"ip\":\"" + g_wifi.ip() + "\"";
  }
  g_router.emitEvent("wifi", payload);
}

/// @brief 定时任务触发 -> 投递到 net_task。
void onScheduledTrigger(TaskScheduler::Trigger trigger) {
  switch (trigger) {
    case TaskScheduler::Trigger::Probe:
      g_router.enqueue(CommandRouter::WorkKind::Probe);
      break;
    case TaskScheduler::Trigger::Checkin:
      g_router.enqueue(CommandRouter::WorkKind::DailyCheckin);
      break;
    case TaskScheduler::Trigger::DeadlineMissed:
      // 窗口结束仍未成功：告警 + 手机通知。
      // 必须投递到 net_task —— 通知要走 HTTP，BLE 回调/定时器任务里不能做网络请求。
      g_router.enqueue(CommandRouter::WorkKind::DeadlineMissed);
      break;
  }
}

/// @brief net_task：串行执行所有耗时网络任务。
void netTaskEntry(void* parameter) {
  (void)parameter;
  LOG_I(kTag, "net_task started on core %d", xPortGetCoreID());
  for (;;) {
    g_router.processPending();
  }
}

}  // namespace

void setup() {
  Logger::instance().begin(LogLevel::Info, 115200);
  LOG_I(kTag, "=== WANAN checkin device booting ===");
  LOG_I(kTag, "firmware %s, built %s", FW_VERSION, FW_BUILD_TIME);

  // 1) 配置
  g_config.begin();

  // 2) LED
  g_led.begin();
  g_led.setPattern(g_config.isReady() ? LedPattern::WifiConnecting : LedPattern::NotConfigured);

  // 3) 蓝牙（先起来，保证任何后续失败都能被手机看到）
  const String deviceName = buildDeviceName();
  if (g_ble.begin(deviceName) != Result::Ok) {
    LOG_E(kTag, "ble init failed");
  }

  // 4) 存储与传输
  g_cookies.begin();
  g_http.begin(AppConst::kUserAgent, 1);
  g_http.attachCookieJar(&g_cookies);

  // 5) 业务模块
  g_auth.begin(g_config, g_http, g_cookies);
  g_api.begin(g_http);
  g_checkin.begin(g_config, g_auth, g_api, g_time);

  // 5b) 账号表（打卡的唯一事实来源：单人 = 只有一条，表为空 = 用设备自身配置）
  g_accounts.begin();
  g_multi.begin(g_config, g_accounts, g_cookies, g_checkin, g_time);
  g_notify.begin(g_config, g_http, g_time);
  // 启动就把「列表第一个生效账号」套进内存配置：这样 status / probe / chain
  // 看到的人，和调度器今晚要打的人，始终是同一个（不会出现"显示的是设备旧凭证"）。
  {
    const size_t count = g_accounts.count();
    for (size_t index = 0; index < count; index++) {
      CheckinProfile profile;
      if (g_accounts.get(index, profile) && profile.enabled) {
        g_config.applyProfile(profile);
        LOG_I(kTag, "default account = %s (enabled %u/%u)", profile.label.c_str(),
              static_cast<unsigned>(g_accounts.enabledCount()), static_cast<unsigned>(count));
        break;
      }
    }
  }

  // 6) 调度器
  g_scheduler.begin(g_config, g_time);
  g_scheduler.setTriggerSink(onScheduledTrigger);

  // 7) 指令路由
  g_router.begin(g_config, g_cookies, g_wifi, g_time, g_auth, g_api, g_http, g_checkin, g_ble,
                 g_scheduler, g_led, g_accounts, g_multi, g_notify);
  g_ble.setLineCallback([](const String& line) { g_router.onBleLine(line); });
  g_wifi.begin(g_config);
  g_wifi.setStateCallback(onWifiStateChanged);

  // 8) 网络任务
  xTaskCreatePinnedToCore(netTaskEntry, "net_task", AppConst::kNetTaskStackBytes, nullptr,
                          AppConst::kNetTaskPriority, nullptr, AppConst::kNetTaskCore);

  // 9) 已配网则自动连接并校时
  if (g_config.wifi().isConfigured()) {
    LOG_I(kTag, "wifi configured -> connecting in background");
    g_router.enqueue(CommandRouter::WorkKind::WifiConnect);
  } else {
    LOG_W(kTag, "wifi not configured; please provision over BLE (see firmware/README.md)");
  }

  LOG_I(kTag, "setup done, free heap=%u B, psram=%u B", static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getFreePsram()));
}

void loop() {
  const uint32_t nowMs = millis();

  static uint32_t lastLedMs = 0;
  if (nowMs - lastLedMs >= 20) {
    lastLedMs = nowMs;
    g_led.update();
  }

  static uint32_t lastWifiMs = 0;
  if (nowMs - lastWifiMs >= 1000) {
    lastWifiMs = nowMs;
    g_wifi.poll();
  }

  static uint32_t lastScheduleMs = 0;
  if (nowMs - lastScheduleMs >= 1000) {
    lastScheduleMs = nowMs;
    g_scheduler.poll();
  }

  delay(5);
}
