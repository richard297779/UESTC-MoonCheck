#pragma once

// ============================================================================
//  CommandRouter：BLE 指令路由与事件出口
//    - 快指令（配置类/查询类）在 BLE 回调里同步处理
//    - 慢指令（联网、登录、打卡、自测）投递到 net_task，避免阻塞蓝牙
//    - 统一事件出口：写日志 + 推送 BLE + 更新 LED
//  线程约束：onBleLine() 运行在 BLE 任务；processPending() 运行在 net_task。
// ============================================================================

#include <Arduino.h>
#include <ArduinoJson.h>

#include "account_store.h"
#include "app_config.h"
#include "app_types.h"
#include "ble_service.h"
#include "cas_auth.h"
#include "checkin_api.h"
#include "checkin_service.h"
#include "cookie_jar.h"
#include "http_client.h"
#include "multi_checkin.h"
#include "notify_service.h"
#include "status_led.h"
#include "task_scheduler.h"
#include "time_service.h"
#include "wifi_manager.h"

class CommandRouter {
 public:
  /// @brief 耗时任务类型。
  enum class WorkKind : uint8_t {
    Command = 0,      ///< 整行 BLE 指令（在 net_task 中解析并执行，避免 BLE 回调栈溢出）
    WifiConnect,      ///< 连接 WiFi 并校时
    WifiScan,         ///< 扫描 AP
    TimeSync,         ///< 重新校时
    TestHttp,         ///< 访问任意 URL（链路自测）
    TestLogin,        ///< 登录链路自测
    TestChain,        ///< 全链路自测（登录 + 进入微应用 + 业务接口，可带等待）
    TestProbe,        ///< 前置体检（只读）
    TestCheckin,      ///< 打卡
    Probe,            ///< 定时预登录体检
    DailyCheckin,     ///< 定时自动打卡
    MultiCheckin,     ///< 多人顺序打卡（一次跑完所有账号）
    NotifyTest,       ///< 立即发一条测试通知（走真实链路，不占队列去重）
    DeadlineMissed,   ///< 定时：窗口关闭仍未成功 -> 告警 + 手机通知
  };

  /// @brief 任务项（定长，可安全放入 FreeRTOS 队列）。
  struct WorkItem {
    WorkKind kind = WorkKind::WifiConnect;
    bool force = false;      ///< 打卡时=跳过"今日已打卡"检查；自测时=允许窗口外强制登录
    int32_t requestId = 0;   ///< BLE 请求 id（0 表示定时触发，无回执）
    char payload[512] = {0}; ///< 附加参数（如 test.http 的 URL、test.chain 的等待秒数）
  };

  /// @brief 注入全部依赖。
  Result begin(AppConfig& config, CookieJar& cookies, WifiManager& wifi, TimeService& time,
               CasAuth& auth, CheckinApi& api, HttpClient& http, CheckinService& checkin,
               BleService& ble, TaskScheduler& scheduler, StatusLed& led, AccountStore& accounts,
               MultiCheckin& multi, NotifyService& notify);

  /// @brief BLE 收到一行指令（**运行在 NimBLE 回调里，只入队，不做解析**）。
  void onBleLine(const String& line);

  /// @brief 投递耗时任务；队列满返回 false。
  bool enqueue(WorkKind kind, const String& payload = "", bool force = false,
               int32_t requestId = 0);

  /// @brief net_task 主循环：阻塞等待并执行任务（内部超时 200 ms）。
  void processPending();

  /// @brief 统一事件出口。
  void emitEvent(const String& type, const String& payloadJson = "");

 private:
  // ---- 指令解析（在 net_task 中执行，栈充裕）----
  void dispatchCommand(const String& line);

  // ---- 指令处理（快）----
  void handleHello(int32_t id);
  void handleStatus(int32_t id);
  void handleAuthSet(JsonObjectConst request, int32_t id);
  void handleLocationSet(JsonObjectConst request, int32_t id);
  void handleScheduleSet(JsonObjectConst request, int32_t id);
  void handleAutoSet(JsonObjectConst request, int32_t id);
  void handleLogs(JsonObjectConst request, int32_t id);
  void handleSessionClear(int32_t id);
  void handleFactoryReset(JsonObjectConst request, int32_t id);
  void handleTestTime(int32_t id);
  void handleAccountAdd(JsonObjectConst request, int32_t id);
  /// @brief 单独开关某个账号是否参与打卡：{"op":"account.enable","label":"A01","on":false}
  void handleAccountEnable(JsonObjectConst request, int32_t id);
  void handleMultiSet(JsonObjectConst request, int32_t id);
  void handleNotifySet(JsonObjectConst request, int32_t id);

  // ---- 指令处理（慢，在 net_task 中执行）----
  void execute(const WorkItem& item);
  void workWifiConnect(const WorkItem& item);
  void workWifiScan(const WorkItem& item);
  void workTimeSync(const WorkItem& item);
  void workTestHttp(const WorkItem& item);
  void workTestLogin(const WorkItem& item);
  void workTestChain(const WorkItem& item);
  void workTestProbe(const WorkItem& item);
  void workTestCheckin(const WorkItem& item);
  void workMultiCheckin(const WorkItem& item);
  /// @brief 一轮账号执行的文案（0 个生效账号时给出可操作的提示）。
  String multiRunMessage(const MultiCheckin::RunResult& run, bool probeOnly) const;
  /// @brief 21:00 状态摘要正文（逐账号一行，总长截到正文上限以内）。
  String multiStatusDigest(const MultiCheckin::RunResult& run) const;
  void workNotifyTest(const WorkItem& item);
  void workDeadlineMissed(const WorkItem& item);

  // ---- 输出辅助 ----
  void replyOk(int32_t id, const String& message, const String& dataJson = "");
  void replyError(int32_t id, Result result, const String& message);
  String buildStatusJson() const;
  void setBusy(bool busy);
  void flashResult(bool ok);

  AppConfig* m_config = nullptr;
  CookieJar* m_cookies = nullptr;
  WifiManager* m_wifi = nullptr;
  TimeService* m_time = nullptr;
  CasAuth* m_auth = nullptr;
  CheckinApi* m_api = nullptr;
  HttpClient* m_http = nullptr;
  CheckinService* m_checkin = nullptr;
  BleService* m_ble = nullptr;
  TaskScheduler* m_scheduler = nullptr;
  StatusLed* m_led = nullptr;
  AccountStore* m_accounts = nullptr;
  MultiCheckin* m_multi = nullptr;
  NotifyService* m_notify = nullptr;
  QueueHandle_t m_queue = nullptr;
};
