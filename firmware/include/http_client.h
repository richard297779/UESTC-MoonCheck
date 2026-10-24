#pragma once

// ============================================================================
//  HttpClient：HTTP/HTTPS 客户端
//    - 手工组装请求、手工解析响应（与 tools/pc_reference/wsc_http.py 语义一致）
//    - 手工跟随 302（每次跳转都记日志），不自动重定向
//    - 自动携带 CookieJar 中的 Cookie，并解析响应里的 Set-Cookie
//    - TLS 默认用内置 CA 校验；tlsMode=0 时跳过校验（仅调试）
//  线程约束：仅在 net_task 中调用（串行，禁止并发）。
// ============================================================================

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <vector>

#include "app_types.h"
#include "cookie_jar.h"

/// @brief 单个 HTTP 头。
struct HttpHeader {
  String name;
  String value;
};

/// @brief 请求描述。
struct HttpRequest {
  String method = "GET";
  String url;
  std::vector<HttpHeader> headers;
  String body;
  bool followRedirects = false;
  uint32_t timeoutMs = AppConst::kHttpTimeoutMs;
  /// @brief true=用**公共根证书包**校验（通知类第三方站点），false=校园站点 CA。
  /// @note 两个证书包互不影响：*.uestc.edu.cn 仍然只用抓包得到的那条链校验，
  ///       不会因为内置了公网根证书而放宽对平台的校验。
  bool publicCa = false;
  /// @brief true=本请求跳过证书校验（等价于全局 tls_mode=0，只对单个请求生效）。
  bool insecure = false;

  /// @brief 追加一个请求头。
  void addHeader(const String& name, const String& value) {
    headers.push_back(HttpHeader{name, value});
  }
};

/// @brief 响应描述。
struct HttpResponse {
  int status = 0;
  String url;                   ///< 实际请求的 URL（用于 Cookie 域推导）
  String location;              ///< 3xx 的 Location
  String body;
  size_t bodyBytes = 0;
  uint32_t durationMs = 0;
  Result result = Result::Ok;   ///< 链路层结果（DNS/TLS/超时）
  String errorDetail;           ///< 失败原因（不含凭据）
  std::vector<HttpHeader> headers;

  /// @brief 取指定响应头（不区分大小写），不存在返回空串。
  String headerValue(const String& name) const;
  /// @brief 是否 2xx。
  bool ok() const { return result == Result::Ok && status >= 200 && status < 300; }
};

class HttpClient {
 public:
  /// @brief 初始化。
  /// @param userAgent 固定 UA（必须与抓包一致）。
  /// @param tlsMode   1=内置 CA 校验（默认）；0=跳过校验（仅调试）。
  Result begin(const String& userAgent = AppConst::kUserAgent, uint8_t tlsMode = 1);

  /// @brief 绑定 Cookie 罐（可空）。
  void attachCookieJar(CookieJar* jar) { m_jar = jar; }

  /// @brief 发送一次请求（是否跟随重定向由 request.followRedirects 决定）。
  Result request(const HttpRequest& request, HttpResponse& response);

  /// @brief 始终手工跟随 3xx，直到非 3xx 或达到跳数上限。
  Result requestFollow(const HttpRequest& request, HttpResponse& response,
                       uint8_t maxHops = AppConst::kMaxRedirectHops);

  // ---- 工具函数 ----
  static String urlEncode(const String& value);
  static String urlDecode(const String& value);
  /// 日志仅保留 URL 的主机和路径，防止泄露票据等查询参数。
  static String logUrl(const String& url);
  static String hostOf(const String& url);
  static String pathOf(const String& url);
  static bool isHttps(const String& url);
  /// @brief 按 RFC 3986 规则把 Location 解析为绝对 URL。
  static String urlJoin(const String& base, const String& location);

 private:
  Result sendOnce(const HttpRequest& request, HttpResponse& response);
  bool readLine(Client& client, String& line, uint32_t deadline);
  bool readResponseHead(Client& client, HttpResponse& response, uint32_t deadline);
  Result readBody(Client& client, HttpResponse& response, bool chunked, long contentLength,
                  uint32_t deadline);
  Result readChunkedBody(Client& client, HttpResponse& response, uint32_t deadline);

  String m_userAgent = AppConst::kUserAgent;
  uint8_t m_tlsMode = 1;
  CookieJar* m_jar = nullptr;
  WiFiClientSecure m_secureClient;
  WiFiClient m_plainClient;
};
