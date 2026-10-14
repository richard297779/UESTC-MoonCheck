#pragma once

// ============================================================================
//  MultiCheckin：多人顺序打卡编排器
//    对每个账号依次执行：清 Cookie → 清登录冷却 → 套用该账号 → 复用既有
//    CheckinService（登录 → 门户预热 → 体检 → 幂等检查 → 提交 → 复查）
//    账号之间按 multiGapSeconds 错峰，单个账号失败不影响其余账号。
//  设计要点：**不复制业务逻辑**，只是把既有的单账号链路按账号跑 N 遍，
//            所以单人版与多人版共享全部 bug 修复。
//  线程约束：runAll() 只能在 net_task 中调用（一次运行可达数十秒）。
// ============================================================================

#include <Arduino.h>

#include <functional>

#include "account_store.h"
#include "app_config.h"
#include "app_types.h"
#include "checkin_service.h"
#include "cookie_jar.h"
#include "time_service.h"

class MultiCheckin {
 public:
  /// @brief 事件出口（由 CommandRouter 注入，用于 BLE 推送）。
  using EventSink = std::function<void(const String& type, const String& payloadJson)>;

  /// @brief 注入依赖。
  Result begin(AppConfig& config, AccountStore& accounts, CookieJar& cookies,
               CheckinService& checkin, TimeService& time);

  void setEventSink(EventSink sink) { m_sink = sink; }

  /// @brief 依次处理全部账号。
  /// @param force 传给 perform()：跳过"该账号今日已打卡"检查（仍必须在打卡窗口内）。
  /// @param probeOnly true=只体检不提交（用于演练）。
  /// @param[out] summaryJson 结果数组 JSON：[{"label","result","ok","msg"}, …]。
  /// @return 成功（含"今天已打过"）的账号数；无账号或重入时返回 0。
  size_t runAll(bool force, bool probeOnly, String& summaryJson);

  bool isRunning() const { return m_running; }

 private:
  void emit(const String& type, const String& payloadJson);

  AppConfig* m_config = nullptr;
  AccountStore* m_accounts = nullptr;
  CookieJar* m_cookies = nullptr;
  CheckinService* m_checkin = nullptr;
  TimeService* m_time = nullptr;
  EventSink m_sink;
  volatile bool m_running = false;
};
