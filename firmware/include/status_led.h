#pragma once

// ============================================================================
//  StatusLed：板载 LED 状态指示（最小硬件方案，无 OLED）
//  线程约束：仅由主循环（loopTask）调用 update()。
// ============================================================================

#include "app_types.h"

/// @brief 状态灯图案（语义见 docs/firmware-design.md §6.3）。
enum class LedPattern : uint8_t {
  Off = 0,           ///< 熄灭
  WifiConnecting,    ///< 蓝灯快闪：正在连接 WiFi
  WifiOk,            ///< 绿灯呼吸：已联网待机
  NotConfigured,     ///< 蓝灯慢闪：尚未配置
  Busy,              ///< 紫灯常亮：正在执行网络请求
  Testing,           ///< 青灯常亮：自测指令进行中
  Success,           ///< 绿灯常亮：最近一次操作成功
  Failure,           ///< 红灯双闪：最近一次操作失败
  NeedCode,          ///< 橙灯快闪：等待验证码（预留）
};

class StatusLed {
 public:
  /// @brief 初始化 LED 引脚。
  /// @param pin   LED 引脚（RGB 灯为数据脚）。
  /// @param isRgb true=WS2812 类 RGB 灯；false=普通单色 LED。
  Result begin(int8_t pin = AppConst::kLedPin, bool isRgb = AppConst::kLedIsRgb);

  /// @brief 设置常驻图案。
  void setPattern(LedPattern pattern);

  /// @brief 设置临时图案（到期后自动回落到常驻图案）。
  void setTemporary(LedPattern pattern, uint32_t holdMs);

  /// @brief 当前生效图案。
  LedPattern pattern() const { return m_temporaryUntil ? m_temporary : m_pattern; }

  /// @brief 由主循环每 20 ms 调用一次，内部非阻塞闪烁。
  void update();

  /// @brief 图案名称（日志与 BLE 用）。
  static const char* patternName(LedPattern pattern);

 private:
  /// @brief 立即输出颜色。
  void writeColor(uint8_t red, uint8_t green, uint8_t blue);

  int8_t m_pin = -1;
  bool m_isRgb = true;
  LedPattern m_pattern = LedPattern::Off;
  LedPattern m_temporary = LedPattern::Off;
  uint32_t m_temporaryUntil = 0;
  uint32_t m_phaseStartMs = 0;
  bool m_phaseOn = true;
  bool m_initialized = false;
};
