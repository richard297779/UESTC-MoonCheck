#pragma once

// ============================================================================
//  CasAuth：统一身份认证（idas.uestc.edu.cn）
//    - 取登录页并解析 execution / pwdEncryptSalt（盐值每次变化，必须每次重取）
//    - 注入 MULTIFACTOR_BROWSER_FINGERPRINT（免二次验证的关键）
//    - 提交登录并判定 成功 / 需要二次验证 / 账号密码错误
//    - 跟随票据换取平台 SESSION
//  实现依据：docs/api-contract.md §3 / §6，行为对齐 tools/pc_reference/wsc_cas.py
//  线程约束：仅在 net_task 中调用。
// ============================================================================

#include <Arduino.h>

#include <functional>

#include "app_config.h"
#include "app_types.h"
#include "cookie_jar.h"
#include "http_client.h"

/// @brief 认证流程状态。
enum class AuthState : uint8_t {
  Idle,
  FetchingLoginPage,
  Submitting,
  NeedMfa,
  ExchangingTicket,
  Trusted,
  Failed,
};

class CasAuth {
 public:
  /// @brief 状态变化回调（state + 脱敏后的可读信息）。
  using StateCallback = std::function<void(AuthState state, const String& detail)>;

  /// @brief 注入依赖。
  Result begin(AppConfig& config, HttpClient& http, CookieJar& jar);

  /// @brief 执行完整登录链；成功后 CookieJar 中已有 SESSION。
  /// @param[out] outDetail 失败原因（脱敏，不含凭据）。
  Result login(String& outDetail);

  /// @brief 仅取登录页并解析两个关键参数（用于链路探活）。
  Result fetchLoginPage(String& outExecution, String& outSalt);

  /// @brief 匿名预访问平台首页，让服务端先建立 SESSION（浏览器流程的第 ① 步）。
  /// @note 缺少这一步时，票据换来的 SESSION 无法用于微应用，业务接口会报 4001。
  Result ensurePlatformSession(String& outDetail);

  /// @brief 把配置中的指纹写入 Cookie 罐（等价于浏览器已上报且获批）。
  void applyFingerprintCookie();

  AuthState state() const { return m_state; }
  const String& lastDetail() const { return m_lastDetail; }
  void setStateCallback(StateCallback callback) { m_callback = callback; }

  /// @brief 登录页绝对地址。
  static String loginUrl();

 private:
  void setState(AuthState state, const String& detail);
  Result submitLogin(const String& execution, const String& salt, HttpResponse& response);
  Result classifyLogin(const HttpResponse& response, String& outDetail);
  Result exchangeTicket(const String& ticketUrl, String& outDetail);

  AppConfig* m_config = nullptr;
  HttpClient* m_http = nullptr;
  CookieJar* m_jar = nullptr;
  AuthState m_state = AuthState::Idle;
  String m_lastDetail;
  StateCallback m_callback;
};
