// ============================================================================
//  TimeService 实现
// ============================================================================

#include "time_service.h"

#include "logger.h"

namespace {

const char* const kTag = "time_service";

/// @brief 2020-09-13 12:26:40 UTC：早于该值即视为未校时。
constexpr time_t kSaneTimestamp = 1600000000;

}  // namespace

Result TimeService::begin(const char* ntpPrimary, const char* ntpSecondary,
                          int32_t timezoneOffsetSeconds) {
  // 用 configTzTime 显式给定 POSIX 时区串。
  // 原实现 configTime(offset, 0, ...) 在 core 4.x 里会拼出 "UTC-8DST-8" 这类
  // 带 DST 的字符串，部分 newlib 版本解析失败后回落成 UTC0（实测设备时间比北京时间慢 8 小时，
  // 会导致 22:00 的定时任务在真实时间 06:00 才触发）。
  // POSIX 约定 "UTC-8" = UTC+8，因此这里对偏移取反。
  const int32_t hours = timezoneOffsetSeconds / 3600;
  char tz[16];
  snprintf(tz, sizeof(tz), "UTC%+ld", static_cast<long>(-hours));
  configTzTime(tz, ntpPrimary, ntpSecondary);
  LOG_I(kTag, "sntp configured: %s / %s (tz=%s, offset=%d s)", ntpPrimary, ntpSecondary, tz,
        static_cast<int>(timezoneOffsetSeconds));
  return Result::Ok;
}

Result TimeService::waitSynced(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (isSynced()) {
      LOG_I(kTag, "time synced: %s", nowString().c_str());
      return Result::Ok;
    }
    delay(200);
  }
  LOG_W(kTag, "time sync timeout after %u ms", static_cast<unsigned>(timeoutMs));
  return Result::NetworkError;
}

bool TimeService::isSynced() const { return time(nullptr) > kSaneTimestamp; }

time_t TimeService::now() const { return time(nullptr); }

String TimeService::nowString(const char* format) const {
  const time_t seconds = time(nullptr);
  struct tm local = {};
  localtime_r(&seconds, &local);
  char buffer[32];
  strftime(buffer, sizeof(buffer), format, &local);
  return String(buffer);
}

String TimeService::todayString() const { return nowString("%Y-%m-%d"); }

bool TimeService::inWindow(int startMinutes, int endMinutes) const {
  const int current = minutesOfDay();
  if (current < 0) return false;
  if (startMinutes <= endMinutes) {
    return current >= startMinutes && current < endMinutes;
  }
  // 跨零点窗口（如 23:30 - 00:30）
  return current >= startMinutes || current < endMinutes;
}

int TimeService::parseHhmm(const String& text) {
  const int colon = text.indexOf(':');
  if (colon <= 0) return -1;
  const int hour = text.substring(0, colon).toInt();
  const int minute = text.substring(colon + 1).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
  return hour * 60 + minute;
}

int TimeService::minutesOfDay(time_t timestamp) {
  if (timestamp == 0) timestamp = time(nullptr);
  struct tm local = {};
  localtime_r(&timestamp, &local);
  return local.tm_hour * 60 + local.tm_min;
}

String TimeService::formatMinutes(int minutes) {
  if (minutes < 0) return String("--:--");
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", (minutes / 60) % 24, minutes % 60);
  return String(buffer);
}
