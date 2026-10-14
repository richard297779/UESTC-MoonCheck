#pragma once

// ============================================================================
//  共享类型与常量：所有模块共用的错误码、配置结构体与编译期常量。
//  本文件只有声明与内联实现，因此不配 .cpp（设计文档 §4 说明的例外项）。
// ============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------- 返回码

/// @brief 全局返回码。所有可失败接口统一返回该枚举，不使用异常。
enum class Result : int8_t {
  Ok = 0,            ///< 成功
  NotReady,          ///< 未就绪（未配网 / 未校时 / 未配置）
  InvalidParam,      ///< 参数非法
  NotFound,          ///< 配置或 Cookie 缺失
  NetworkError,      ///< DNS / TCP / TLS 层失败
  HttpError,         ///< 已连通但状态码非预期
  AuthFailed,        ///< 账号或密码错误
  NeedMfa,           ///< 服务端要求二次验证
  SessionInvalid,    ///< 会话失效
  AppUnavailable,    ///< 微应用不授权该会话（实测：会话不是在打卡时段内建立的）
  ApiChanged,        ///< 页面或接口结构与预期不符
  OutOfWindow,       ///< 不在打卡时段
  AlreadyCheckedIn,  ///< 今日已打卡
  StorageError,      ///< NVS 读写失败
  InternalError,     ///< 其他内部错误
};

/// @brief 返回码的可读名称（写日志用）。
inline const char* resultName(Result value) {
  switch (value) {
    case Result::Ok: return "Ok";
    case Result::NotReady: return "NotReady";
    case Result::InvalidParam: return "InvalidParam";
    case Result::NotFound: return "NotFound";
    case Result::NetworkError: return "NetworkError";
    case Result::HttpError: return "HttpError";
    case Result::AuthFailed: return "AuthFailed";
    case Result::NeedMfa: return "NeedMfa";
    case Result::SessionInvalid: return "SessionInvalid";
    case Result::AppUnavailable: return "AppUnavailable";
    case Result::ApiChanged: return "ApiChanged";
    case Result::OutOfWindow: return "OutOfWindow";
    case Result::AlreadyCheckedIn: return "AlreadyCheckedIn";
    case Result::StorageError: return "StorageError";
    case Result::InternalError: return "InternalError";
  }
  return "Unknown";
}

/// @brief 返回码对应的 BLE 错误码（UPPER_SNAKE，供手机侧判断）。
inline const char* resultCode(Result value) {
  switch (value) {
    case Result::Ok: return "OK";
    case Result::NotReady: return "NOT_READY";
    case Result::InvalidParam: return "INVALID_PARAM";
    case Result::NotFound: return "NOT_FOUND";
    case Result::NetworkError: return "NETWORK_ERROR";
    case Result::HttpError: return "HTTP_ERROR";
    case Result::AuthFailed: return "AUTH_FAILED";
    case Result::NeedMfa: return "NEED_MFA";
    case Result::SessionInvalid: return "SESSION_INVALID";
    case Result::AppUnavailable: return "APP_UNAVAILABLE";
    case Result::ApiChanged: return "API_CHANGED";
    case Result::OutOfWindow: return "OUT_OF_WINDOW";
    case Result::AlreadyCheckedIn: return "ALREADY_CHECKED_IN";
    case Result::StorageError: return "STORAGE_ERROR";
    case Result::InternalError: return "INTERNAL_ERROR";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------- 日志级别

enum class LogLevel : uint8_t { Debug = 0, Info, Warn, Error };

inline const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "D";
    case LogLevel::Info: return "I";
    case LogLevel::Warn: return "W";
    case LogLevel::Error: return "E";
  }
  return "?";
}

// ---------------------------------------------------------------- 配置结构

/// @brief 宿舍 WiFi 配置（本阶段仅支持 WPA2/WPA3 个人版）。
struct WifiConfig {
  String ssid;
  String password;

  bool isConfigured() const { return ssid.length() > 0; }
};

/// @brief 统一身份认证配置。
struct AuthConfig {
  String studentId;
  String password;
  String fingerprint;  ///< MULTIFACTOR_BROWSER_FINGERPRINT：免二次验证的关键

  bool isConfigured() const { return studentId.length() > 0 && password.length() > 0; }
  bool hasFingerprint() const { return fingerprint.length() > 0; }
};

/// @brief 打卡定位配置。
struct LocationConfig {
  double latitude = 0.0;
  double longitude = 0.0;
  String address;

  bool isConfigured() const { return latitude != 0.0 && longitude != 0.0; }
};

/// @brief 打卡时段配置（分钟偏移由 setter 同步计算）。
struct ScheduleConfig {
  String windowStart = "21:00";  ///< 窗口起点（平台时段，实测 21:00）
  String checkinAt = "22:00";    ///< 目标打卡时间
  String windowEnd = "23:30";    ///< 重试截止（窗口结束）
  int windowStartMinutes = 21 * 60;
  int checkinAtMinutes = 22 * 60;
  int windowEndMinutes = 23 * 60 + 30;
  uint16_t jitterSeconds = 30;  ///< 随机抖动，避免整点特征
  bool autoDailyEnabled = true; ///< 是否启用每日自动打卡
};

// ---------------------------------------------------------------- 编译期常量

namespace AppConst {

/// @brief 状态 LED：按编译目标自动选择引脚与类型。
/// @note 可用编译标志覆盖，无需改代码：
///         -DWANAN_LED_PIN=-1   关闭 LED
///         -DWANAN_LED_PIN=2 -DWANAN_LED_IS_RGB=0   指定普通 LED
#if !defined(WANAN_LED_PIN)
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
#define WANAN_LED_PIN 48  ///< S3/C3 DevKit 常见板载 WS2812
#else
#define WANAN_LED_PIN 2   ///< 经典 ESP32 板载普通 LED
#endif
#endif
#if !defined(WANAN_LED_IS_RGB)
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
#define WANAN_LED_IS_RGB 1
#else
#define WANAN_LED_IS_RGB 0
#endif
#endif

inline constexpr int8_t kLedPin = static_cast<int8_t>(WANAN_LED_PIN);
inline constexpr bool kLedIsRgb = (WANAN_LED_IS_RGB != 0);

/// @brief HTTP 相关。
inline constexpr uint16_t kHttpTimeoutMs = 15000;   ///< 单次请求总超时
inline constexpr uint16_t kHttpConnectTimeoutMs = 8000;
inline constexpr uint32_t kMaxBodyBytes = 32768;    ///< 响应体上限（超出即截断）
inline constexpr uint8_t kMaxRedirectHops = 8;

/// @brief Cookie 罐容量。
inline constexpr uint8_t kMaxCookies = 12;

/// @brief 日志环形缓冲。
inline constexpr uint16_t kLogRingLines = 128;
inline constexpr uint16_t kLogLineMax = 192;

/// @brief 必须与抓包完全一致的 User-Agent（契约 §4）。
inline constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36";

/// @brief 站点地址（契约 §2）。
inline constexpr const char* kCasBaseUrl = "https://idas.uestc.edu.cn/authserver";
inline constexpr const char* kCasLoginPath =
    "/authserver/login?service=https%3A%2F%2Fjzsz.uestc.edu.cn%2F";
inline constexpr const char* kIdasHost = "idas.uestc.edu.cn";
inline constexpr const char* kJzszHost = "jzsz.uestc.edu.cn";
inline constexpr const char* kJzszBaseUrl = "https://jzsz.uestc.edu.cn";

/// @brief 微应用标识（契约 §4）。
inline constexpr int kAppId = 294;

/// @brief NTP 服务器与时区（CST-8，无夏令时）。
inline constexpr const char* kNtpServer1 = "ntp.aliyun.com";
inline constexpr const char* kNtpServer2 = "cn.pool.ntp.org";
inline constexpr int32_t kTimezoneOffsetSeconds = 8 * 3600;

/// @brief 登录冷却：登录成功后若微应用仍未放行，此期间内不再重复登录。
/// @note 实测（2026-10-12 22:00）登录成功后平台需要一段时间才放行微应用，
///       立刻重试登录既无意义又白白消耗登录次数。
inline constexpr uint32_t kLoginCooldownMs = 30UL * 60UL * 1000UL;

/// @brief 打卡重试间隔：窗口内每次尝试失败后的重试周期。
inline constexpr uint32_t kCheckinRetrySeconds = 180;

/// @brief net_task 参数。
inline constexpr uint32_t kNetTaskStackBytes = 10240;
inline constexpr UBaseType_t kNetTaskPriority = 2;
inline constexpr BaseType_t kNetTaskCore = 1;

}  // namespace AppConst
