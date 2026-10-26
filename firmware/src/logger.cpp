// ============================================================================
//  Logger 实现
// ============================================================================

#include "logger.h"

#include <esp_heap_caps.h>
#include <stdarg.h>
#include <time.h>

namespace {

/// @brief 单行日志最大长度（含结尾 '\0'）。
constexpr size_t kFormatBufferSize = 512;

}  // namespace

Logger& Logger::instance() {
  static Logger s_instance;
  return s_instance;
}

void Logger::begin(LogLevel level, uint32_t baud) {
  m_level = level;
  if (m_mutex == nullptr) {
    m_mutex = xSemaphoreCreateMutex();
  }
  Serial.begin(baud);
  delay(50);  // 等待 USB-CDC 枚举

  if (m_ring == nullptr) {
    const size_t bytes = static_cast<size_t>(AppConst::kLogRingLines) * AppConst::kLogLineMax;
    // 优先放 PSRAM，失败退回内部堆
    m_ring = static_cast<char*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (m_ring == nullptr) {
      m_ring = static_cast<char*>(malloc(bytes));
    }
    if (m_ring != nullptr) {
      memset(m_ring, 0, bytes);
    }
  }
  LOG_I("logger", "logger ready (ring=%s)", m_ring ? "ok" : "disabled");
}

void Logger::setLevel(LogLevel level) { m_level = level; }

void Logger::setSink(Sink sink) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_sink = sink;
  if (m_mutex) xSemaphoreGive(m_mutex);
}

void Logger::clearSink() { setSink(nullptr); }

String Logger::redact(const String& text) {
  // Fail closed for credential-bearing diagnostics, including quoted JSON,
  // URLs and opaque key material. Ordinary stage/error messages remain useful.
  String lowered = text;
  lowered.toLowerCase();
  const char* sensitive[] = {"pass", "pwd", "castgc", "encrypt", "private", "key=", "key:", "cookie", "session", "authorization", "token", "ticket",
      "captcha", "verifycode", "smscode", "mfacode", "fingerprint", "bfp", "secret",
      "access", "refresh", "openid", "unionid", "sendkey", "http://", "https://", "user", "student"};
  for (const char* key : sensitive) {
    if (lowered.indexOf(key) >= 0) return "[sensitive diagnostic omitted]";
  }
  // Long numbers / hex / base64-like values may be credentials without a key.
  unsigned opaque = 0;
  unsigned digits = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    opaque = (isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=') ? opaque + 1 : 0;
    digits = isdigit(static_cast<unsigned char>(c)) ? digits + 1 : 0;
    if (opaque >= 24 || digits >= 8) return "[opaque diagnostic omitted]";
  }
  return text;
}

void Logger::log(LogLevel level, const char* tag, const char* format, ...) {
  if (level < m_level) return;

  char body[kFormatBufferSize];
  va_list args;
  va_start(args, format);
  vsnprintf(body, sizeof(body), format, args);
  va_end(args);

  // 时间戳：已校时用本地时间，未校时用开机毫秒
  char stamp[24];
  const time_t nowSeconds = time(nullptr);
  if (nowSeconds > 1600000000) {
    struct tm local = {};
    localtime_r(&nowSeconds, &local);
    strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &local);
  } else {
    snprintf(stamp, sizeof(stamp), "%6lu ms", static_cast<unsigned long>(millis()));
  }

  String line;
  line.reserve(256);
  line += '[';
  line += stamp;
  line += "] ";
  line += logLevelName(level);
  line += ' ';
  line += tag;
  line += ": ";
  line += redact(String(body));
  writeLine(line);
}

void Logger::writeLine(const String& line) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  Serial.println(line);
  pushToRing(line);
  Sink sink = m_sink;
  if (m_mutex) xSemaphoreGive(m_mutex);
  if (sink) sink(line);  // 回调在锁外执行，避免回调内再打日志造成死锁
}

void Logger::pushToRing(const String& line) {
  if (m_ring == nullptr) return;
  char* slot = m_ring + static_cast<size_t>(m_head) * AppConst::kLogLineMax;
  strncpy(slot, line.c_str(), AppConst::kLogLineMax - 1);
  slot[AppConst::kLogLineMax - 1] = '\0';
  m_head = static_cast<uint16_t>((m_head + 1) % AppConst::kLogRingLines);
  if (m_count < AppConst::kLogRingLines) {
    m_count++;
  } else {
    m_dropped++;
  }
}

String Logger::tail(uint16_t maxLines) const {
  String out;
  if (m_ring == nullptr) return out;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  uint16_t lines = m_count;
  if (maxLines > 0 && maxLines < lines) lines = maxLines;
  for (uint16_t i = 0; i < lines; i++) {
    const uint16_t index =
        static_cast<uint16_t>((m_head + AppConst::kLogRingLines - lines + i) % AppConst::kLogRingLines);
    out += &m_ring[static_cast<size_t>(index) * AppConst::kLogLineMax];
    if (i + 1 < lines) out += '\n';
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  return out;
}
