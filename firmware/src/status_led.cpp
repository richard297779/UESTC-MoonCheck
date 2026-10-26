// ============================================================================
//  StatusLed 实现
// ============================================================================

#include "status_led.h"

#include "logger.h"

namespace {

const char* const kTag = "status_led";

/// @brief 图案描述：颜色 + 亮/灭时长（0 表示常亮或常灭）。
struct LedSpec {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint16_t onMs;
  uint16_t offMs;
};

/// @brief 取图案对应的显示规格。
LedSpec specOf(LedPattern pattern) {
  switch (pattern) {
    case LedPattern::Off: return {0, 0, 0, 0, 0};
    case LedPattern::WifiConnecting: return {0, 0, 255, 125, 125};       // 蓝 4 Hz
    case LedPattern::WifiOk: return {0, 180, 0, 100, 1900};              // 绿呼吸
    case LedPattern::NotConfigured: return {0, 0, 255, 500, 500};        // 蓝 1 Hz
    case LedPattern::Busy: return {160, 0, 220, 0, 0};                   // 紫常亮
    case LedPattern::Testing: return {0, 200, 200, 0, 0};                // 青常亮
    case LedPattern::Success: return {0, 220, 0, 0, 0};                  // 绿常亮
    case LedPattern::Failure: return {220, 0, 0, 150, 150};              // 红双闪
    case LedPattern::NeedCode: return {255, 120, 0, 100, 100};           // 橙 5 Hz
  }
  return {0, 0, 0, 0, 0};
}

}  // namespace

Result StatusLed::begin(int8_t pin, bool isRgb) {
  if (pin < 0) {
    LOG_W(kTag, "led disabled (pin=%d)", pin);
    return Result::InvalidParam;
  }
  m_pin = pin;
  m_isRgb = isRgb;
  if (!m_isRgb) {
    pinMode(static_cast<uint8_t>(pin), OUTPUT);
    digitalWrite(static_cast<uint8_t>(pin), BoardProfile::kLedActiveLow ? HIGH : LOW);
  } else {
    writeColor(0, 0, 0);  // 复位灯带
  }
  m_initialized = true;
  m_phaseStartMs = millis();
  LOG_I(kTag, "led ready: pin=%d rgb=%d", pin, isRgb ? 1 : 0);
  return Result::Ok;
}

void StatusLed::setPattern(LedPattern pattern) {
  if (m_pattern == pattern) return;
  m_pattern = pattern;
  m_phaseStartMs = millis();
  m_phaseOn = true;
}

void StatusLed::setTemporary(LedPattern pattern, uint32_t holdMs) {
  m_temporary = pattern;
  m_temporaryUntil = millis() + holdMs;
  m_phaseStartMs = millis();
  m_phaseOn = true;
}

void StatusLed::writeColor(uint8_t red, uint8_t green, uint8_t blue) {
  if (!m_initialized || m_pin < 0) return;
  if (m_isRgb) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    rgbLedWrite(static_cast<uint8_t>(m_pin), red, green, blue);
#else
    neopixelWrite(static_cast<uint8_t>(m_pin), red, green, blue);
#endif
  } else {
    digitalWrite(static_cast<uint8_t>(m_pin), ((red + green + blue) > 0) != BoardProfile::kLedActiveLow ? HIGH : LOW);
  }
}

void StatusLed::update() {
  if (!m_initialized) return;

  const uint32_t now = millis();
  if (m_temporaryUntil != 0 && static_cast<int32_t>(now - m_temporaryUntil) >= 0) {
    m_temporaryUntil = 0;
    m_phaseStartMs = now;
    m_phaseOn = true;
  }

  const LedPattern active = m_temporaryUntil ? m_temporary : m_pattern;
  const LedSpec spec = specOf(active);

  if (spec.onMs == 0 && spec.offMs == 0) {
    // 常亮或常灭：只需在图案切换时写一次
    if (m_phaseOn) {
      writeColor(spec.red, spec.green, spec.blue);
      m_phaseOn = false;
      m_phaseStartMs = now;
    }
    return;
  }

  const uint32_t elapsed = now - m_phaseStartMs;
  const uint16_t target = m_phaseOn ? spec.onMs : spec.offMs;
  if (elapsed >= target) {
    m_phaseOn = !m_phaseOn;
    m_phaseStartMs = now;
    writeColor(m_phaseOn ? spec.red : 0, m_phaseOn ? spec.green : 0, m_phaseOn ? spec.blue : 0);
  }
}

const char* StatusLed::patternName(LedPattern pattern) {
  switch (pattern) {
    case LedPattern::Off: return "off";
    case LedPattern::WifiConnecting: return "wifi_connecting";
    case LedPattern::WifiOk: return "wifi_ok";
    case LedPattern::NotConfigured: return "not_configured";
    case LedPattern::Busy: return "busy";
    case LedPattern::Testing: return "testing";
    case LedPattern::Success: return "success";
    case LedPattern::Failure: return "failure";
    case LedPattern::NeedCode: return "need_code";
  }
  return "unknown";
}
