// ============================================================================
//  CasAuth 实现
// ============================================================================

#include "cas_auth.h"

#include "crypto_util.h"
#include "logger.h"

namespace {

const char* const kTag = "cas_auth";

/// @brief 指纹 Cookie 名与域。
constexpr const char* kFingerprintCookie = "MULTIFACTOR_BROWSER_FINGERPRINT";
constexpr const char* kIdasHost = "idas.uestc.edu.cn";

/// @brief 指纹 Cookie 有效期（10 年；服务器实际下发到 2094 年）。
constexpr long kFingerprintLifetimeSeconds = 315360000L;

/// @brief 页面中定位字段的**锚点**（只锚定字段名，不假设属性顺序/引号写法）。
constexpr const char* kExecutionAnchor = "name=\"execution\"";
constexpr const char* kSaltAnchor = "pwdEncryptSalt";

/// @brief 登录页约 25 KB；经典 ESP32 的 TLS 吞吐较低，给更宽的超时与重试。
constexpr uint32_t kLoginPageTimeoutMs = 30000;
constexpr uint8_t kLoginPageAttempts = 3;

/// @brief 在 anchor 附近（±200 字符）取 `value="…"` 的内容。
/// @note 兼容 `<input id="pwdEncryptSalt" value="X">` 与 `<input value="X" id="pwdEncryptSalt">`
///       两种属性顺序；旧实现要求 marker 完全连续，页面写法稍有变化就会误报"字段不存在"。
bool extractValueNear(const String& html, const char* anchor, String& out) {
  const int at = html.indexOf(anchor);
  if (at < 0) return false;
  const int anchorEnd = at + static_cast<int>(strlen(anchor));

  // 1) 向后找 value="
  const int fwd = html.indexOf("value=\"", at);
  if (fwd >= 0 && fwd - at <= 200) {
    const int start = fwd + 7;
    const int end = html.indexOf('"', start);
    if (end > start) {
      out = html.substring(start, end);
      return true;
    }
  }
  // 2) 向前找（属性顺序相反的写法）
  const int bwd = html.lastIndexOf("value=\"", at);
  if (bwd >= 0 && at - bwd <= 200) {
    const int start = bwd + 7;
    const int end = html.indexOf('"', start);
    if (end > start && end <= anchorEnd) {
      out = html.substring(start, end);
      return true;
    }
  }
  return false;
}

}  // namespace

String CasAuth::loginUrl() { return String("https://idas.uestc.edu.cn") + AppConst::kCasLoginPath; }

Result CasAuth::begin(AppConfig& config, HttpClient& http, CookieJar& jar) {
  m_config = &config;
  m_http = &http;
  m_jar = &jar;
  m_state = AuthState::Idle;
  LOG_I(kTag, "cas auth ready (fingerprint=%s)",
        config.auth().hasFingerprint() ? "configured" : "missing");
  return Result::Ok;
}

void CasAuth::setState(AuthState state, const String& detail) {
  m_state = state;
  m_lastDetail = detail;
  if (m_callback) m_callback(state, detail);
}

void CasAuth::applyFingerprintCookie() {
  if (m_config == nullptr || m_jar == nullptr) return;
  if (!m_config->auth().hasFingerprint()) {
    LOG_W(kTag, "no browser fingerprint configured -> server may require MFA");
    return;
  }
  // 未校时时用会话级 Cookie，避免算出 1970 年的过期时间
  const time_t nowSeconds = time(nullptr);
  const time_t expires =
      (nowSeconds > 1600000000) ? nowSeconds + kFingerprintLifetimeSeconds : 0;
  m_jar->set(kFingerprintCookie, m_config->auth().fingerprint, kIdasHost, "/", expires);
  LOG_I(kTag, "fingerprint cookie applied");
}

Result CasAuth::ensurePlatformSession(String& outDetail) {
  if (m_http == nullptr || m_jar == nullptr) return Result::NotReady;

  // 浏览器流程：GET https://jzsz.uestc.edu.cn/ （匿名）→ 302 到 CAS，同时下发匿名 SESSION。
  // 之后 CAS 登录回来时，?ticket= 请求带着这个 SESSION，服务端才会把身份绑定到该会话上。
  HttpRequest request;
  request.method = "GET";
  request.url = String(AppConst::kJzszBaseUrl) + "/";
  request.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
  request.addHeader("Upgrade-Insecure-Requests", "1");

  HttpResponse response;
  const Result result = m_http->request(request, response);  // 不跟随重定向，只需拿到 Set-Cookie
  if (result != Result::Ok) {
    outDetail = "platform pre-visit failed";
    return result;
  }
  const String session = m_jar->get("SESSION", AppConst::kJzszHost);
  LOG_I(kTag, "pre-visit platform: http %d, anonymous SESSION %s", response.status,
        session.length() > 0 ? "ready" : "missing");
  if (session.length() == 0) {
    LOG_W(kTag, "no anonymous SESSION issued; ticket may not bind to a session");
  }
  return Result::Ok;
}

Result CasAuth::fetchLoginPage(String& outExecution, String& outSalt) {
  if (m_http == nullptr) return Result::NotReady;

  setState(AuthState::FetchingLoginPage, "fetching login page");

  Result lastResult = Result::ApiChanged;
  String lastDetail = "login page parse failed";

  for (uint8_t attempt = 1; attempt <= kLoginPageAttempts; attempt++) {
    HttpRequest request;
    request.method = "GET";
    request.url = loginUrl();
    request.timeoutMs = kLoginPageTimeoutMs;
    request.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request.addHeader("Upgrade-Insecure-Requests", "1");

    HttpResponse response;
    const Result result = m_http->request(request, response);
    if (result != Result::Ok) {
      lastResult = result;
      lastDetail = "login page network error";
      LOG_W(kTag, "login page fetch failed (try %u): %s", attempt, resultName(result));
      vTaskDelay(pdMS_TO_TICKS(800));
      continue;
    }
    if (response.status != 200) {
      setState(AuthState::Failed, "login page http " + String(response.status));
      return Result::HttpError;  // 状态码不对，重试无意义
    }

    const bool okExec = extractValueNear(response.body, kExecutionAnchor, outExecution);
    const bool okSalt = extractValueNear(response.body, kSaltAnchor, outSalt);
    if (okExec && okSalt) {
      LOG_I(kTag, "login page parsed: %u B (try %u), execution=%s salt=%s",
            static_cast<unsigned>(response.body.length()), attempt, outExecution.c_str(),
            outSalt.length() > 0 ? "ok" : "empty");
      return Result::Ok;
    }

    // ★ 诊断：区分「响应被截断」与「页面结构变了」——两者处置完全不同
    const bool rawHasSalt = response.body.indexOf(kSaltAnchor) >= 0;
    const long declared = response.headerValue("Content-Length").toInt();
    // freeHeap 很关键：经典 ESP32 无 PSRAM，25 KB 的 String 可能分配不出后续增量，
    // String::concat 失败是**静默**的，症状与网络截断一模一样。
    LOG_W(kTag,
          "login page parse failed (try %u): got=%u B declared=%ld exec=%d salt=%d "
          "rawExec=%d rawSalt=%d freeHeap=%u B",
          attempt, static_cast<unsigned>(response.body.length()), declared, okExec ? 1 : 0,
          okSalt ? 1 : 0, response.body.indexOf("execution") >= 0 ? 1 : 0, rawHasSalt ? 1 : 0,
          static_cast<unsigned>(ESP.getFreeHeap()));

    if (rawHasSalt) {
      // 字段在页面里但取不出来 → 是页面结构问题，重试没有意义
      lastResult = Result::ApiChanged;
      lastDetail = "login page structure changed (pwdEncryptSalt format)";
      break;
    }
    // 页面里没有该字段 → 大概率被截断，换个超时再试
    lastResult = Result::NetworkError;
    lastDetail = "login page truncated";
    vTaskDelay(pdMS_TO_TICKS(800));
  }

  setState(AuthState::Failed, lastDetail);
  return lastResult;
}

Result CasAuth::submitLogin(const String& execution, const String& salt, HttpResponse& response) {
  const AuthConfig& auth = m_config->auth();
  const String encrypted = CryptoUtil::encryptCasPassword(auth.password, salt);
  if (encrypted.length() == 0) {
    setState(AuthState::Failed, "password encryption failed");
    return Result::InternalError;
  }

  String body;
  body.reserve(320);
  body += "username=";
  body += HttpClient::urlEncode(auth.studentId);
  body += "&password=";
  body += HttpClient::urlEncode(encrypted);
  body += "&captcha=&_eventId=submit&cllt=userNameLogin&dllt=generalLogin&lt=&execution=";
  body += HttpClient::urlEncode(execution);

  setState(AuthState::Submitting, "submitting credentials");
  HttpRequest request;
  request.method = "POST";
  request.url = loginUrl();
  request.body = body;
  request.addHeader("Content-Type", "application/x-www-form-urlencoded");
  request.addHeader("Origin", "https://idas.uestc.edu.cn");
  request.addHeader("Referer", loginUrl());
  request.addHeader("Upgrade-Insecure-Requests", "1");
  request.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

  const Result result = m_http->request(request, response);
  if (result != Result::Ok) {
    setState(AuthState::Failed, "submit network error");
    return result;
  }
  return Result::Ok;
}

Result CasAuth::classifyLogin(const HttpResponse& response, String& outDetail) {
  const String location = response.location;

  if (location.indexOf("ticket=") >= 0) {
    outDetail = "ticket issued";
    return Result::Ok;
  }
  if (location.indexOf("reAuthLoginView") >= 0 || location.indexOf("isMultifactor=true") >= 0) {
    setState(AuthState::NeedMfa, "server requires second factor");
    outDetail = "second factor required";
    return Result::NeedMfa;
  }
  if (response.status == 200) {
    // 停留在登录页 = 失败；进一步区分"账号密码错误"与"页面结构变化"
    if (response.body.indexOf("pwdEncryptSalt") >= 0 || response.body.indexOf("loginFromId") >= 0) {
      if (response.body.indexOf("密码") >= 0 || response.body.indexOf("错误") >= 0) {
        setState(AuthState::Failed, "bad credentials");
        outDetail = "bad credentials";
        return Result::AuthFailed;
      }
      setState(AuthState::Failed, "login did not proceed (unknown reason)");
      outDetail = "login rejected by server";
      return Result::AuthFailed;
    }
    setState(AuthState::Failed, "unexpected page");
    outDetail = "unexpected response body";
    return Result::ApiChanged;
  }
  setState(AuthState::Failed, "unexpected http " + String(response.status));
  outDetail = "http " + String(response.status);
  return Result::HttpError;
}

Result CasAuth::exchangeTicket(const String& ticketUrl, String& outDetail) {
  setState(AuthState::ExchangingTicket, "exchanging ticket");
  HttpRequest request;
  request.method = "GET";
  request.url = ticketUrl;
  request.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
  request.addHeader("Upgrade-Insecure-Requests", "1");
  request.addHeader("Referer", "https://idas.uestc.edu.cn/");
  request.addHeader("Pragma", "no-cache");
  request.addHeader("Cache-Control", "no-cache");
  request.addHeader("Sec-Fetch-Site", "same-site");
  request.addHeader("Sec-Fetch-Mode", "navigate");
  request.addHeader("Sec-Fetch-User", "?1");
  request.addHeader("Sec-Fetch-Dest", "document");

  HttpResponse response;
  const Result result = m_http->requestFollow(request, response, 6);
  if (result != Result::Ok) {
    setState(AuthState::Failed, "ticket exchange network error");
    outDetail = "ticket exchange failed";
    return result;
  }
  if (m_jar->get("SESSION", AppConst::kJzszHost).length() == 0) {
    setState(AuthState::Failed, "session cookie missing");
    outDetail = "SESSION not issued (http " + String(response.status) + ")";
    return Result::HttpError;
  }
  setState(AuthState::Trusted, "session established");
  outDetail = "session established";
  return Result::Ok;
}

Result CasAuth::login(String& outDetail) {
  if (m_config == nullptr || m_http == nullptr || m_jar == nullptr) return Result::NotReady;
  if (!m_config->auth().isConfigured()) {
    outDetail = "auth not configured";
    return Result::NotReady;
  }

  applyFingerprintCookie();

  // ★ 登录前清掉平台的**匿名** SESSION，强制服务端在票据校验时下发"登录态"会话。
  //   实测（2026-10-13 21:00 设备日志 + PC 对照）：
  //     · 预登录探针先调 checkLogin，会拿到一个 1 小时的匿名 SESSION；
  //     · 之后走 CAS 登录，票据校验那一步**不再下发新 SESSION**（客户端已带 SESSION）；
  //     · 该会话此后拿不到微应用授权（checkLogin 一直被 302 退回平台首页、today 回 4001）。
  //   PC 对照：登录前 jar 中没有 SESSION 时，票据校验会下发新 SESSION —— 两者行为不同，
  //   因此这里显式清掉，让设备的登录行为与浏览器首登一致。
  m_jar->remove("SESSION", AppConst::kJzszHost);

  // 步骤 0：匿名预访问平台首页（对齐浏览器流程，否则微应用会认为未登录）
  String preVisitDetail;
  const Result preVisit = ensurePlatformSession(preVisitDetail);
  if (preVisit != Result::Ok) {
    LOG_W(kTag, "pre-visit failed: %s (continue anyway)", preVisitDetail.c_str());
  }

  String execution;
  String salt;
  Result result = fetchLoginPage(execution, salt);
  if (result != Result::Ok) {
    outDetail = m_lastDetail;
    return result;
  }

  HttpResponse response;
  result = submitLogin(execution, salt, response);
  if (result != Result::Ok) {
    outDetail = m_lastDetail;
    return result;
  }

  result = classifyLogin(response, outDetail);
  if (result != Result::Ok) return result;

  result = exchangeTicket(response.location, outDetail);
  if (result != Result::Ok) return result;

  LOG_I(kTag, "login ok, SESSION established");
  return Result::Ok;
}
