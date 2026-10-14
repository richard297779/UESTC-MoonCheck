#pragma once

// ============================================================================
//  Logger：统一日志出口
//    - 串口输出（带时间戳与级别）
//    - 128 行环形缓冲（供 BLE logs 指令拉取）
//    - 可选实时回调（BLE 已连接时推送）
//    - 强制脱敏：密码 / Cookie / 验证码永不落日志
//  线程约束：可被任意任务调用（内部加锁）。
// ============================================================================

#include <Arduino.h>

#include <functional>

#include "app_types.h"

class Logger {
 public:
  /// @brief 实时日志回调类型。
  using Sink = std::function<void(const String& line)>;

  /// @brief 单例入口。
  static Logger& instance();

  /// @brief 初始化串口并分配日志环形缓冲。
  /// @param level 初始日志级别。
  /// @param baud  串口波特率。
  void begin(LogLevel level = LogLevel::Info, uint32_t baud = 115200);

  /// @brief 设置日志级别（低于该级别的日志被丢弃）。
  void setLevel(LogLevel level);
  LogLevel level() const { return m_level; }

  /// @brief 输出一条日志（printf 风格）。
  /// @param level  级别。
  /// @param tag    模块标签，如 "cas_auth"。
  /// @param format printf 格式串。
  void log(LogLevel level, const char* tag, const char* format, ...)
      __attribute__((format(printf, 4, 5)));

  /// @brief 取最近若干行日志（新的在后），用于 BLE logs 指令。
  String tail(uint16_t maxLines) const;

  /// @brief 设置实时日志回调；传 nullptr 等价于 clearSink()。
  void setSink(Sink sink);
  void clearSink();

  /// @brief 因环形缓冲满而被丢弃的行数。
  uint32_t droppedCount() const { return m_dropped; }

  /// @brief 对文本做脱敏：password/验证码/Cookie/Token 等字段值替换为 ******。
  static String redact(const String& text);

 private:
  Logger() = default;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  /// @brief 写入串口与环形缓冲，并按需触发 Sink。
  void writeLine(const String& line);
  void pushToRing(const String& line);

  LogLevel m_level = LogLevel::Info;
  char* m_ring = nullptr;                  ///< [kLogRingLines][kLogLineMax]
  volatile uint16_t m_head = 0;            ///< 下一个写入位置
  volatile uint16_t m_count = 0;           ///< 已存行数
  volatile uint32_t m_dropped = 0;
  SemaphoreHandle_t m_mutex = nullptr;
  Sink m_sink;
};

// ---------------------------------------------------------------- 便捷宏

/// @brief Debug 级日志。
#define LOG_D(tag, ...) Logger::instance().log(LogLevel::Debug, tag, __VA_ARGS__)
/// @brief Info 级日志。
#define LOG_I(tag, ...) Logger::instance().log(LogLevel::Info, tag, __VA_ARGS__)
/// @brief Warn 级日志。
#define LOG_W(tag, ...) Logger::instance().log(LogLevel::Warn, tag, __VA_ARGS__)
/// @brief Error 级日志。
#define LOG_E(tag, ...) Logger::instance().log(LogLevel::Error, tag, __VA_ARGS__)
