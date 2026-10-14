#pragma once

// ============================================================================
//  TimeService：时间服务
//    - SNTP 校时（默认 ntp.aliyun.com / cn.pool.ntp.org，时区 CST-8）
//    - 时间格式化与"是否落在打卡窗口内"的判断
//  线程约束：waitSynced() 会阻塞（内部 yield），其余接口线程安全。
// ============================================================================

#include <Arduino.h>
#include <time.h>

#include "app_types.h"

class TimeService {
 public:
  /// @brief 配置 NTP 并启动异步同步。
  Result begin(const char* ntpPrimary = AppConst::kNtpServer1,
               const char* ntpSecondary = AppConst::kNtpServer2,
               int32_t timezoneOffsetSeconds = AppConst::kTimezoneOffsetSeconds);

  /// @brief 阻塞等待同步完成（内部 delay 让出 CPU）。
  Result waitSynced(uint32_t timeoutMs = 15000);

  /// @brief 是否已获得有效时间（以 2020-09-13 为界）。
  bool isSynced() const;

  time_t now() const;
  String nowString(const char* format = "%Y-%m-%d %H:%M:%S") const;
  String todayString() const;

  /// @brief 当前本地时间是否落在 [startMinutes, endMinutes) 内（自 00:00 起的分钟偏移）。
  bool inWindow(int startMinutes, int endMinutes) const;

  /// @brief 把 "HH:MM" 解析为分钟偏移；非法返回 -1。
  static int parseHhmm(const String& text);

  /// @brief 某个时间戳的"当日分钟数"；timestamp 为 0 时取当前时间。
  static int minutesOfDay(time_t timestamp = 0);

  /// @brief 分钟偏移格式化为 "HH:MM"。
  static String formatMinutes(int minutes);
};
