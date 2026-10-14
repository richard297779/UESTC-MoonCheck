// ============================================================================
//  TaskScheduler 实现
// ============================================================================

#include "task_scheduler.h"

#include "logger.h"

namespace {

const char* const kTag = "scheduler";

/// @brief 轮询间隔。
constexpr uint32_t kPollIntervalMs = 1000;

}  // namespace

Result TaskScheduler::begin(AppConfig& config, TimeService& time) {
  m_config = &config;
  m_time = &time;
  LOG_I(kTag, "scheduler ready: checkin=%s windowEnd=%s auto=%d",
        config.schedule().checkinAt.c_str(), config.schedule().windowEnd.c_str(),
        config.schedule().autoDailyEnabled ? 1 : 0);
  return Result::Ok;
}

void TaskScheduler::setAutoDailyEnabled(bool enabled) {
  if (m_config != nullptr) m_config->setAutoDaily(enabled);
}

bool TaskScheduler::isAutoDailyEnabled() const {
  return (m_config != nullptr) && m_config->schedule().autoDailyEnabled;
}

String TaskScheduler::nextRunString() const {
  if (m_config == nullptr || m_time == nullptr) return String("--:--");
  if (!m_time->isSynced()) return String("time not synced");

  const ScheduleConfig& schedule = m_config->schedule();
  const String today = m_time->todayString();
  const int currentMinutes = TimeService::minutesOfDay();

  String target = today;
  // 今天窗口已过、或今天已确认打卡成功 -> 下一次是明天
  if (currentMinutes >= schedule.windowEndMinutes || m_config->isLastResultOkFor(today)) {
    const time_t tomorrow = m_time->now() + 86400;
    struct tm local = {};
    localtime_r(&tomorrow, &local);
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    target = String(buffer);
  }
  return target + " " + schedule.checkinAt;
}

void TaskScheduler::poll() {
  if (m_config == nullptr || m_time == nullptr || m_sink == nullptr) return;
  const uint32_t nowMs = millis();
  if (nowMs - m_lastPollMs < kPollIntervalMs) return;
  m_lastPollMs = nowMs;

  if (!m_config->schedule().autoDailyEnabled) return;
  if (!m_time->isSynced()) return;

  const ScheduleConfig& schedule = m_config->schedule();
  const String today = m_time->todayString();

  // 取"当日秒数"与当前时刻拆分
  const time_t nowSeconds = m_time->now();
  struct tm local = {};
  localtime_r(&nowSeconds, &local);
  const int secondsOfDay = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;

  // 每天生成一次随机抖动，避免固定秒发起；同时重置"当天"状态
  if (m_jitterDate != today) {
    m_jitterDate = today;
    m_lastAttemptDate = "";
    m_failureReportedDate = "";
    m_confirmedDate = "";
    const uint16_t range = schedule.jitterSeconds > 0 ? schedule.jitterSeconds : 0;
    m_todayJitterSeconds = (range > 0) ? static_cast<uint16_t>(esp_random() % (range + 1)) : 0;
    LOG_I(kTag, "today jitter = %u s", m_todayJitterSeconds);
  }

  const int checkinSeconds = schedule.checkinAtMinutes * 60 + m_todayJitterSeconds;
  const int windowEndSeconds = schedule.windowEndMinutes * 60;
  const int windowStartSeconds = schedule.windowStartMinutes * 60;

  // ---- 预登录体检：**窗口一开始就做**（每天一次）----
  // 依据实测：① 平台只给"窗口内建立的会话"授权微应用（窗口外登录 100% 无效，2026-10-13 证实）；
  //           ② 窗口内登录成功后，平台仍需约 10 分钟才放行（2026-10-12 22:00:16 → 22:10:03）。
  // 因此体检时间从"打卡前 10 分钟"提前到窗口起点（21:00），给平台留足生效时间：
  // 21:00 登录 → 21:10 前后授权完成 → 22:00 首次打卡即可成功。
  if (windowStartSeconds > 0 && secondsOfDay >= windowStartSeconds &&
      secondsOfDay < checkinSeconds && m_lastProbeDate != today) {
    m_lastProbeDate = today;
    LOG_I(kTag, "trigger pre-login probe (window start)");
    m_sink(Trigger::Probe);
    return;
  }

  // ---- 打卡：窗口内**反复重试**直到确认成功或窗口结束 ----
  // 实测（2026-10-12 22:00）：登录成功后平台约需数分钟才放行微应用，
  // "一次不成就判终态失败"会直接错过整个打卡窗口。
  if (secondsOfDay >= checkinSeconds && secondsOfDay < windowEndSeconds) {
    // 已确认成功则不再重试；状态变化时只记一次日志
    // （历史问题：这里原为每秒一行，既刷屏又会冲掉 128 行的日志环形缓冲）
    if (m_confirmedDate == today || m_config->isLastResultOkFor(today)) {
      if (m_confirmedDate != today) {
        m_confirmedDate = today;
        LOG_I(kTag, "today check-in already confirmed, no retry needed");
      }
      return;
    }
    const bool firstAttempt = (m_lastAttemptDate != today);
    if (!firstAttempt &&
        (nowMs - m_lastAttemptMs) < AppConst::kCheckinRetrySeconds * 1000UL) {
      return;  // 重试间隔未到
    }
    m_lastAttemptDate = today;
    m_lastAttemptMs = nowMs;
    LOG_I(kTag, firstAttempt ? "trigger daily checkin" : "retry daily checkin");
    m_sink(Trigger::Checkin);
    return;
  }

  // ---- 窗口已过：当天尝试过但仍未成功 → 上报一次告警 ----
  if (secondsOfDay >= windowEndSeconds && m_failureReportedDate != today &&
      m_lastAttemptDate == today && !m_config->isLastResultOkFor(today)) {
    m_failureReportedDate = today;
    LOG_W(kTag, "check-in window closed without success");
    m_sink(Trigger::DeadlineMissed);
  }
}
