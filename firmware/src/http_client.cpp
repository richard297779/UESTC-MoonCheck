// ============================================================================
//  HttpClient 实现
// ============================================================================

#include "http_client.h"

#include "logger.h"
#include "uestc_ca_pem.h"

namespace {

const char* const kTag = "http_client";

/// @brief 单行读取上限（响应头）。
/// @note 登录页/票据响应可能带很长的 CSP / Set-Cookie 头，留足余量。
constexpr size_t kMaxLineLength = 2048;

/// @brief 无数据时的空闲等待上限：超过即认为响应结束。
/// @note 经典 ESP32（无 PSRAM）TLS 吞吐较低，4 s 容易在正文中途误判"结束"导致截断。
constexpr uint32_t kIdleTimeoutMs = 8000;

/// @brief 判断是否 3xx。
bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/// @brief 请求是否已自带某个头。
/// @note 基座头里的 `Accept: */*` 会与请求自带的 `Accept: text/html,...` **重复**；
///       真实浏览器每个请求只有一个 Accept，重复头会让网关误判请求类型
///       （例如把顶层导航当成 XHR 而拒绝跳转到 H5），因此请求自带时不再追加基座头。
bool hasHeader(const HttpRequest& request, const char* name) {
  for (const auto& header : request.headers) {
    if (header.name.equalsIgnoreCase(name)) return true;
  }
  return false;
}

/// @brief 十六进制字符转数值。
int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

String HttpResponse::headerValue(const String& name) const {
  for (const auto& header : headers) {
    if (header.name.equalsIgnoreCase(name)) return header.value;
  }
  return String();
}

// ---------------------------------------------------------------- 工具函数

String HttpClient::urlEncode(const String& value) {
  static const char* kHex = "0123456789ABCDEF";
  String out;
  out.reserve(value.length() * 2);
  for (size_t i = 0; i < value.length(); i++) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0x0F];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

String HttpClient::urlDecode(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (c == '%' && i + 2 < value.length()) {
      const int high = hexValue(value[i + 1]);
      const int low = hexValue(value[i + 2]);
      if (high >= 0 && low >= 0) {
        out += static_cast<char>((high << 4) | low);
        i += 2;
        continue;
      }
    }
    if (c == '+') {
      out += ' ';  // 表单语义
    } else {
      out += c;
    }
  }
  return out;
}

String HttpClient::hostOf(const String& url) {
  int start = url.indexOf("://");
  start = (start < 0) ? 0 : start + 3;
  int end = url.indexOf('/', start);
  if (end < 0) end = url.length();
  const int colon = url.indexOf(':', start);
  if (colon > 0 && colon < end) end = colon;
  String host = url.substring(start, end);
  host.toLowerCase();
  return host;
}

String HttpClient::pathOf(const String& url) {
  int start = url.indexOf("://");
  start = (start < 0) ? 0 : start + 3;
  const int slash = url.indexOf('/', start);
  if (slash < 0) return String("/");
  return url.substring(slash);
}

bool HttpClient::isHttps(const String& url) { return url.startsWith("https://"); }

String HttpClient::urlJoin(const String& base, const String& location) {
  if (location.startsWith("http://") || location.startsWith("https://")) return location;

  const int schemeEnd = base.indexOf("://");
  const String scheme = (schemeEnd < 0) ? String("https") : base.substring(0, schemeEnd);
  const int hostStart = (schemeEnd < 0) ? 0 : schemeEnd + 3;
  int hostEnd = base.indexOf('/', hostStart);
  if (hostEnd < 0) hostEnd = base.length();
  const String authority = base.substring(hostStart, hostEnd);

  if (location.startsWith("//")) return scheme + ":" + location;
  if (location.startsWith("/")) return scheme + "://" + authority + location;

  // 相对路径：与 base 的目录拼接
  String directory = base.substring(hostStart);
  const int lastSlash = directory.lastIndexOf('/');
  directory = (lastSlash < 0) ? String("/") : directory.substring(0, lastSlash + 1);
  return scheme + "://" + authority + directory + location;
}

// ---------------------------------------------------------------- 生命周期

Result HttpClient::begin(const String& userAgent, uint8_t tlsMode) {
  m_userAgent = userAgent;
  m_tlsMode = tlsMode;
  LOG_I(kTag, "http client ready (tlsMode=%u, ua=%s...)", tlsMode, userAgent.substring(0, 32).c_str());
  return Result::Ok;
}

// ---------------------------------------------------------------- 底层读写

bool HttpClient::readLine(Client& client, String& line, uint32_t deadline) {
  line = "";
  // 超过上限时**丢弃内容但继续吃到行尾**：绝不能因为某一行太长就提前返回，
  // 否则头部解析会中断、正文从错误的偏移开始读（症状正是"页面读到了却找不到字段"）。
  bool overflow = false;
  while (millis() < deadline) {
    if (client.available() > 0) {
      const int c = client.read();
      if (c < 0) continue;
      if (c == '\n') return true;
      if (c != '\r') {
        if (!overflow) {
          line += static_cast<char>(c);
          if (line.length() > kMaxLineLength) {
            overflow = true;
            LOG_W(kTag, "line longer than %u bytes, content discarded",
                  static_cast<unsigned>(kMaxLineLength));
          }
        }
      }
      continue;
    }
    if (!client.connected()) return line.length() > 0;
    delay(1);
  }
  return false;
}

bool HttpClient::readResponseHead(Client& client, HttpResponse& response, uint32_t deadline) {
  String statusLine;
  if (!readLine(client, statusLine, deadline)) {
    response.result = Result::NetworkError;
    response.errorDetail = "no status line";
    return false;
  }
  // 形如 "HTTP/1.1 302 "
  const int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0) {
    response.result = Result::NetworkError;
    response.errorDetail = "bad status line: " + statusLine;
    return false;
  }
  response.status = statusLine.substring(firstSpace + 1).toInt();

  while (true) {
    String line;
    if (!readLine(client, line, deadline)) break;
    if (line.length() == 0) break;  // 头部结束
    const int colon = line.indexOf(':');
    if (colon <= 0) continue;
    HttpHeader header;
    header.name = line.substring(0, colon);
    header.value = line.substring(colon + 1);
    header.name.trim();
    header.value.trim();
    response.headers.push_back(header);

    if (header.name.equalsIgnoreCase("Location")) {
      response.location = header.value;
    } else if (header.name.equalsIgnoreCase("Set-Cookie") && m_jar != nullptr) {
      m_jar->applySetCookie(response.url, header.value);
    }
  }
  return true;
}

Result HttpClient::readChunkedBody(Client& client, HttpResponse& response, uint32_t deadline) {
  String& body = response.body;
  while (millis() < deadline) {
    String sizeLine;
    if (!readLine(client, sizeLine, deadline)) break;
    sizeLine.trim();
    const int semicolon = sizeLine.indexOf(';');
    if (semicolon >= 0) sizeLine = sizeLine.substring(0, semicolon);
    if (sizeLine.length() == 0) continue;
    const long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
    if (chunkSize <= 0) break;

    long remaining = chunkSize;
    uint32_t idleDeadline = millis() + kIdleTimeoutMs;
    while (remaining > 0 && millis() < deadline) {
      if (client.available() > 0) {
        uint8_t buffer[256];
        const int want = remaining > static_cast<long>(sizeof(buffer))
                             ? static_cast<int>(sizeof(buffer))
                             : static_cast<int>(remaining);
        const int got = client.read(buffer, want);
        if (got > 0) {
          if (body.length() + got <= AppConst::kMaxBodyBytes) {
            body.concat(reinterpret_cast<const char*>(buffer), got);
          }
          remaining -= got;
          idleDeadline = millis() + kIdleTimeoutMs;
          continue;
        }
      }
      if (!client.connected() && client.available() == 0) break;
      if (static_cast<int32_t>(millis() - idleDeadline) >= 0) break;
      delay(1);
    }
    String crlf;
    readLine(client, crlf, millis() + 2000);
  }
  return Result::Ok;
}

Result HttpClient::readBody(Client& client, HttpResponse& response, bool chunked,
                            long contentLength, uint32_t deadline) {
  if (chunked) {
    const Result result = readChunkedBody(client, response, deadline);
    response.bodyBytes = response.body.length();
    return result;
  }

  String& body = response.body;
  if (contentLength > 0 && contentLength < 65536) {
    body.reserve(static_cast<unsigned int>(contentLength));
  }

  if (contentLength > 0) {
    long remaining = contentLength;
    uint32_t idleDeadline = millis() + kIdleTimeoutMs;
    while (remaining > 0 && millis() < deadline) {
      if (client.available() > 0) {
        uint8_t buffer[512];
        const int want = remaining > static_cast<long>(sizeof(buffer))
                             ? static_cast<int>(sizeof(buffer))
                             : static_cast<int>(remaining);
        const int got = client.read(buffer, want);
        if (got > 0) {
          if (body.length() + got <= AppConst::kMaxBodyBytes) {
            body.concat(reinterpret_cast<const char*>(buffer), got);
          }
          remaining -= got;
          idleDeadline = millis() + kIdleTimeoutMs;
          continue;
        }
      }
      if (!client.connected() && client.available() == 0) break;
      if (static_cast<int32_t>(millis() - idleDeadline) >= 0) break;
      delay(1);
    }
  } else {
    // 服务器用 Connection: close 表示"读到关闭为止"（实测行为）
    uint32_t idleDeadline = millis() + kIdleTimeoutMs;
    while (millis() < deadline) {
      const int available = client.available();
      if (available > 0) {
        uint8_t buffer[512];
        const int want = available > static_cast<int>(sizeof(buffer)) ? sizeof(buffer) : available;
        const int got = client.read(buffer, want);
        if (got > 0) {
          if (body.length() + got <= AppConst::kMaxBodyBytes) {
            body.concat(reinterpret_cast<const char*>(buffer), got);
          }
          idleDeadline = millis() + kIdleTimeoutMs;
          continue;
        }
      }
      if (!client.connected()) break;
      if (static_cast<int32_t>(millis() - idleDeadline) >= 0) break;
      delay(1);
    }
  }

  response.bodyBytes = body.length();
  return Result::Ok;
}

// ---------------------------------------------------------------- 请求

Result HttpClient::sendOnce(const HttpRequest& request, HttpResponse& response) {
  response = HttpResponse();
  response.url = request.url;

  const String host = hostOf(request.url);
  const String path = pathOf(request.url);
  if (host.length() == 0) {
    response.result = Result::InvalidParam;
    response.errorDetail = "bad url: " + request.url;
    return response.result;
  }

  const bool https = isHttps(request.url);
  Client* client = https ? static_cast<Client*>(&m_secureClient)
                         : static_cast<Client*>(&m_plainClient);

  if (https) {
    if (m_tlsMode == 0) {
      m_secureClient.setInsecure();
    } else {
      m_secureClient.setCACert(kUestcCaPem);
    }
    m_secureClient.setHandshakeTimeout(15);
  }
  client->stop();

  const uint32_t startMs = millis();
  if (!client->connect(host.c_str(), https ? 443 : 80)) {
    response.result = Result::NetworkError;
    response.errorDetail = "connect failed: " + host;
    response.durationMs = millis() - startMs;
    LOG_W(kTag, "connect failed: %s", host.c_str());
    return response.result;
  }

  // ---- 组装请求 ----
  String head;
  head.reserve(512 + request.body.length());
  head += request.method;
  head += ' ';
  head += path;
  head += " HTTP/1.1\r\nHost: ";
  head += host;
  head += "\r\nUser-Agent: ";
  head += m_userAgent;
  // 与真实浏览器对齐的通用头（部分网关会据此区分浏览器与脚本）
  head += "\r\n";
  if (!hasHeader(request, "Accept")) head += "Accept: */*\r\n";
  head += "Accept-Language: zh-CN,zh;q=0.9\r\nConnection: close\r\n";
  head += "Sec-Ch-Ua: \"Google Chrome\";v=\"155\", \"Chromium\";v=\"155\", \"Not(A:Brand\";v=\"24\"\r\n";
  head += "Sec-Ch-Ua-Mobile: ?0\r\n";
  head += "Sec-Ch-Ua-Platform: \"Windows\"\r\n";
  for (const auto& header : request.headers) {
    head += header.name;
    head += ": ";
    head += header.value;
    head += "\r\n";
  }
  if (m_jar != nullptr) {
    const String cookie = m_jar->headerFor(request.url);
    if (cookie.length() > 0) {
      head += "Cookie: ";
      head += cookie;
      head += "\r\n";
    }
    // 只记录名称，便于排查"会话为何没带上"
    LOG_I(kTag, "cookie sent: [%s]", m_jar->namesFor(request.url).c_str());
  }
  if (request.body.length() > 0) {
    head += "Content-Length: ";
    head += String(request.body.length());
    head += "\r\n";
  }
  head += "\r\n";

  client->print(head);
  if (request.body.length() > 0) client->print(request.body);

  // ---- 读取响应 ----
  const uint32_t deadline = millis() + request.timeoutMs;
  if (!readResponseHead(*client, response, deadline)) {
    client->stop();
    response.durationMs = millis() - startMs;
    return response.result;
  }

  const bool chunked =
      response.headerValue("Transfer-Encoding").indexOf("chunked") >= 0;
  const long contentLength = response.headerValue("Content-Length").toInt();
  readBody(*client, response, chunked, contentLength, deadline);
  client->stop();

  response.durationMs = millis() - startMs;
  LOG_I(kTag, "%s %s -> %d (%u B, %u ms)", request.method.c_str(), request.url.c_str(),
        response.status, static_cast<unsigned>(response.bodyBytes),
        static_cast<unsigned>(response.durationMs));
  return response.result;
}

Result HttpClient::request(const HttpRequest& request, HttpResponse& response) {
  Result result = sendOnce(request, response);
  if (result != Result::Ok) return result;
  if (!request.followRedirects) return Result::Ok;

  HttpRequest next = request;
  for (uint8_t hop = 0; hop < AppConst::kMaxRedirectHops; hop++) {
    if (!isRedirect(response.status) || response.location.length() == 0) break;
    const String target = urlJoin(request.url, response.location);
    LOG_I(kTag, "redirect hop %u -> %s", hop + 1, target.c_str());
    next.url = target;
    result = sendOnce(next, response);
    if (result != Result::Ok) return result;
  }
  return Result::Ok;
}

Result HttpClient::requestFollow(const HttpRequest& request, HttpResponse& response,
                                 uint8_t maxHops) {
  HttpRequest current = request;
  for (uint8_t hop = 0; hop <= maxHops; hop++) {
    const Result result = sendOnce(current, response);
    if (result != Result::Ok) return result;
    if (!isRedirect(response.status) || response.location.length() == 0) return Result::Ok;
    const String target = urlJoin(current.url, response.location);
    LOG_I(kTag, "follow %u/%u -> %s", hop + 1, maxHops, target.c_str());
    current.url = target;
  }
  LOG_W(kTag, "too many redirects");
  return Result::HttpError;
}
