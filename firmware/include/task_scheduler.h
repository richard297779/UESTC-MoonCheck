#pragma once

// ============================================================================
//  TaskScheduler：每日定时任务
//    - **窗口起点（21:00）**触发一次"预登录体检"（会话 TTL 约 1 h，且提交只接受窗口内）
//    - 22:00 起（+ 随机抖动）触发打卡，直到窗口结束
//    - 每天只触发一次（按日期去重）；触发动作通过 TriggerSink 投递给 net_task
//  线程约束：begin()/poll() 由主循环调用；TriggerSink 内部只做入队，不阻塞。
// ============================================================================

#include <Arduino.h>

#include <functional>

#include "app_config.h"
#include "app_types.h"
#include "time_service.h"

class TaskScheduler {
 public:
  /// @brief 触发类型。
  enum class Trigger : uint8_t {
    Probe,           ///< 预登录体检（窗口起点触发，为平台的授权延迟留出时间）
    Checkin,         ///< 打卡（到点后触发；失败会按 kCheckinRetrySeconds 持续重试）
    DeadlineMissed,  ///< 窗口已结束但当天仍未确认成功（用于告警）
  };

  /// @brief 触发回调：由上层投递到网络任务执行。
  using TriggerSink = std::function<void(Trigger trigger)>;

  /// @brief 注入依赖。
  Result begin(AppConfig& config, TimeService& time);

  void setTriggerSink(TriggerSink sink) { m_sink = sink; }
  void setAutoDailyEnabled(bool enabled);
  bool isAutoDailyEnabled() const;

  /// @brief 主循环每 1 秒调用一次。
  void poll();

  /// @brief 下一次计划打卡时间（用于 status 展示）。
  String nextRunString() const;

 private:
  AppConfig* m_config = nullptr;
  TimeService* m_time = nullptr;
  TriggerSink m_sink;
  String m_lastProbeDate;        ///< 窗口起点预登录的日期去重
  String m_lastAttemptDate;      ///< 最近一次打卡尝试所属日期
  String m_failureReportedDate;  ///< 已上报"窗口结束仍失败"的日期
  String m_confirmedDate;        ///< 已确认打卡成功的日期（RAM 缓存：避免每秒读 NVS 与刷日志）
  String m_jitterDate;           ///< 当天抖动所属日期
  uint16_t m_todayJitterSeconds = 0;  ///< 当天随机抖动（0..jitterSeconds）
  uint32_t m_lastPollMs = 0;
  uint32_t m_lastAttemptMs = 0;  ///< 最近一次打卡尝试的时刻
};
