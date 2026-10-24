#pragma once

// ============================================================================
//  WxDns：微信 HTTPDNS（aedns.weixin.qq.com）与目标选择
//  移植依据：moon-uestc `_wechat_dns.py`。明文 HTTP/1.0，端口 80。
// ============================================================================

#include <Arduino.h>
#include <WiFiClient.h>

#include <map>
#include <vector>

#include "wx_crypto.h"

namespace wx {

struct Target {
  String ip;
  uint16_t port = 0;
  bool operator==(const Target& o) const { return ip == o.ip && port == o.port; }
};

/// @brief 一个域名的服务器描述（IP 列表 + 协议→端口列表）。
struct DomainServers {
  std::vector<String> ips;
  std::map<String, std::vector<uint16_t>> protocols;
};

/// @brief 明文 HTTP/1.0 请求（Connection: close，读完整响应）。
/// @return 是否成功（status/body 有效）。
bool httpRequest(const char* connectIp, uint16_t port, const char* hostHeader,
                 const char* method, const String& path,
                 const std::vector<std::pair<String, String>>& headers,
                 const Bytes& body, uint32_t timeoutMs, int& status, Bytes& out);

/// @brief 拉取 HTTPDNS 域名表；失败返回 false（调用方走兜底）。
bool fetchHttpdns(uint32_t timeoutMs, std::map<String, DomainServers>& out);

/// @brief 从域名表取指定 domain+protocol 的目标列表。
std::vector<Target> targetsFor(const std::map<String, DomainServers>& domains,
                               const char* domain, const char* protocol);

}  // namespace wx
