#pragma once

// ============================================================================
//  MultiCheckin：账号表顺序执行器（单人 = 表里只有一条）
//    对每个**生效**的账号依次执行：清 Cookie → 清登录冷却 → 套用该账号 → 复用既有
//    CheckinService（登录 → 门户预热 → 体检 → 幂等检查 → 提交 → 复查）
//    账号之间按 multiGapSeconds 错峰，单个账号失败不影响其余账号。
//    表为空的账号自动跳过（result=disabled），既不登录也不计入成功判定。
//  设计要点：**不复制业务逻辑**，只是把既有的单账号链路按账号跑 N 遍，
//            所以一条账号与六条账号共享全部 bug 修复。
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

  /// @brief 一轮执行的统计（**分母只算生效账号**，禁用账号绝不能计入，
  ///        否则"全员成功"永远达不到 allOk → 调度器整夜重试）。
  struct RunResult {
    size_t total = 0;          ///< 账号表条数（含禁用）
    size_t enabledTotal = 0;   ///< 本轮实际参与打卡的账号数
    size_t okCount = 0;        ///< 成功 + 今日已打过（含禁用项吗？不含）
    size_t disabledCount = 0;  ///< 被单独关掉、本轮跳过的数量
    String summary;            ///< 结果数组 JSON：[{"label","enabled","result","ok","msg"}, …]
  };

  /// @brief 依次处理全部**生效**账号。
  /// @param force 传给 perform()：跳过"该账号今日已打卡"检查（仍必须在打卡窗口内）。
  /// @param probeOnly true=只体检不提交（用于演练）。
  /// @return 统计结果；重入时全 0（summary="[]"）。
  RunResult runAll(bool force, bool probeOnly);

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
