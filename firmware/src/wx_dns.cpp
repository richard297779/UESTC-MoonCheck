#include "wx_dns.h"

#include <WiFiClient.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "ArduinoJson.h"

namespace wx {

namespace {
const char* kNewdnsHost = "aedns.weixin.qq.com";
const char* kNewdnsBackupIp = "180.153.202.85";
const char* kNewdnsPath = "/cgi-bin/default/getdns";
constexpr size_t kMaxHttpResponse = 64 * 1024;
}  // namespace

bool httpRequest(const char* connectIp, uint16_t port, const char* hostHeader,
                 const char* method, const String& path,
                 const std::vector<std::pair<String, String>>& headers,
                 const Bytes& body, uint32_t timeoutMs, int& status, Bytes& out) {
  WiFiClient client;
  uint32_t deadline = millis() + timeoutMs;
  if (!client.connect(connectIp, port, timeoutMs)) return false;
  client.setTimeout(timeoutMs);

  String req;
  req.reserve(256 + headers.size() * 48);
  req += method;
  req += " ";
  req += path;
  req += " HTTP/1.0\r\nHost: ";
  req += hostHeader;
  req += "\r\nConnection: close\r\n";
  bool hasLength = false;
  for (const auto& h : headers) {
    req += h.first;
    req += ": ";
    req += h.second;
    req += "\r\n";
    if (h.first.equalsIgnoreCase("Content-Length")) hasLength = true;
  }
  if (!body.empty() && !hasLength) {
    req += "Content-Length: ";
    req += String(body.size());
    req += "\r\n";
  }
  req += "\r\n";
  client.print(req);
  if (!body.empty()) client.write(body.data(), body.size());

  // 读完整响应（Connection: close，读到 EOF 或 Content-Length 满足）
  Bytes raw;
  raw.reserve(2048);
  while (millis() < deadline && client.connected()) {
    size_t avail = client.available();
    if (avail == 0) {
      delay(2);
      continue;
    }
    size_t before = raw.size();
    raw.resize(before + avail);
    size_t got = client.read(raw.data() + before, avail);
    if (got == 0) break;
    raw.resize(before + got);
    if (raw.size() > kMaxHttpResponse) return false;
  }
  client.stop();

  // 解析状态行 + 头
  int headerEnd = -1;
  for (size_t i = 0; i + 3 < raw.size(); i++) {
    if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
      headerEnd = static_cast<int>(i);
      break;
    }
  }
  if (headerEnd < 0) return false;
  std::string header(reinterpret_cast<char*>(raw.data()),
                     static_cast<size_t>(headerEnd));
  int sp1 = header.find(' ');
  int sp2 = header.find(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0 || header.rfind("HTTP/", 0) != 0) return false;
  status = atoi(header.substr(sp1 + 1, sp2 - sp1 - 1).c_str());

  std::string lower = header;  // 头区小写化后找 content-length / chunked
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  size_t clPos = lower.find("content-length:");
  size_t bodyStart = headerEnd + 4;
  if (clPos != std::string::npos) {
    size_t lineEnd = lower.find('\n', clPos);
    long contentLength =
        strtol(lower.substr(clPos + 15, lineEnd - clPos - 15).c_str(), nullptr, 10);
    if (contentLength < 0 || static_cast<size_t>(contentLength) > kMaxHttpResponse)
      return false;
    if (raw.size() - bodyStart < static_cast<size_t>(contentLength)) return false;
    out.assign(raw.begin() + bodyStart, raw.begin() + bodyStart + contentLength);
    return true;
  }
  if (lower.find("transfer-encoding: chunked") != std::string::npos) {
    // 逐块解码
    out.clear();
    size_t offset = bodyStart;
    while (offset < raw.size()) {
      size_t crlf = std::string::npos;
      for (size_t i = offset; i + 1 < raw.size(); i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n') {
          crlf = i;
          break;
        }
      }
      if (crlf == std::string::npos) return false;
      std::string sizeLine(reinterpret_cast<char*>(raw.data() + offset), crlf - offset);
      size_t semi = sizeLine.find(';');
      if (semi != std::string::npos) sizeLine = sizeLine.substr(0, semi);
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize == 0) return true;
      offset = crlf + 2;
      if (offset + chunkSize + 2 > raw.size()) return false;
      out.insert(out.end(), raw.begin() + offset, raw.begin() + offset + chunkSize);
      offset += chunkSize + 2;
      if (out.size() > kMaxHttpResponse) return false;
    }
    return true;
  }
  // 无长度说明：读到 EOF 的全部就是 body
  out.assign(raw.begin() + bodyStart, raw.end());
  return true;
}

bool fetchHttpdns(uint32_t timeoutMs, std::map<String, DomainServers>& out) {
  String path = String(kNewdnsPath) +
                "?clientversion=0&devicetype=Windows&uin=0&format=json";
  const char* hosts[] = {kNewdnsHost, kNewdnsBackupIp};
  for (const char* host : hosts) {
    int status = 0;
    Bytes body;
    if (!httpRequest(host, 80, kNewdnsHost, "GET", path,
                     {{"User-Agent", "MicroMessenger Client"}, {"Accept", "*/*"}},
                     Bytes{}, timeoutMs, status, body))
      continue;
    if (status != 200) continue;

    JsonDocument doc;  // ArduinoJson v7，弹性容量
    DeserializationError err = deserializeJson(doc, body.data(), body.size());
    if (err != DeserializationError::Ok) continue;
    JsonObject dns = doc["dns"];
    if (dns.isNull() || dns["retcode"].as<int>() != 0) continue;
    JsonArray domainList = dns["domainlist"];
    if (domainList.isNull()) continue;

    out.clear();
    for (JsonObject item : domainList) {
      const char* name = item["name"];
      if (name == nullptr || name[0] == '\0') continue;
      DomainServers servers;
      for (JsonObject ipItem : item["iplist"].as<JsonArray>()) {
        const char* ip = ipItem["ip"];
        if (ip == nullptr) continue;
        String ipStr(ip);
        bool dup = false;
        for (const auto& existing : servers.ips)
          if (existing == ipStr) dup = true;
        if (!dup) servers.ips.push_back(ipStr);
      }
      for (JsonObject proto : item["protocollist"].as<JsonArray>()) {
        const char* pname = proto["name"];
        if (pname == nullptr) continue;
        std::vector<uint16_t> ports;
        for (JsonVariant v : proto["portlist"].as<JsonArray>()) {
          int port = v.as<int>();
          if (port > 0 && port <= 65535) ports.push_back(static_cast<uint16_t>(port));
        }
        if (!ports.empty()) servers.protocols[String(pname)] = std::move(ports);
      }
      out[String(name)] = std::move(servers);
    }
    return true;
  }
  return false;
}

std::vector<Target> targetsFor(const std::map<String, DomainServers>& domains,
                               const char* domain, const char* protocol) {
  std::vector<Target> out;
  auto it = domains.find(String(domain));
  if (it == domains.end()) return out;
  auto pit = it->second.protocols.find(String(protocol));
  if (pit == it->second.protocols.end()) return out;
  for (const auto& ip : it->second.ips) {
    for (uint16_t port : pit->second) out.push_back(Target{ip, port});
  }
  return out;
}

}  // namespace wx
