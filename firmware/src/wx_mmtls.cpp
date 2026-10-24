#include "wx_mmtls.h"

#include <esp_random.h>

#include <WiFiClient.h>

#include <algorithm>
#include <string>

#include "logger.h"

namespace wx {

namespace {
const char* kLongLinkDomain = "longcloud.weixin.com";
const char* kShortLinkDomain = "shortcloud.weixin.com";
const char* kMmtlsProtocol = "mmtlsovertcp";
const char* kShortlinkFallbackIp = "120.241.131.173";
constexpr uint16_t kLongLinkFallbackPort = 443;
constexpr uint16_t kShortlinkPort = 80;
constexpr size_t kMaxRecordBuffer = 2 * 65535;
constexpr uint8_t kType8Prefix[] = {0x00, 0x00, 0x00, 0x10, 0x08, 0x00, 0x00,
                                    0x00, 0x0B, 0x01, 0x00, 0x00, 0x00, 0x06,
                                    0x00, 0x12};
constexpr uint8_t kEarlyAlert[] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x01};

struct LoginState {
  AppSession session;
  PSKEntry psk;
};

bool timeLeft(uint32_t deadlineMs) { return static_cast<int32_t>(deadlineMs - millis()) > 0; }
}  // namespace

// ==== MmtlsConnection ====

MmtlsConnection::MmtlsConnection(WiFiClient& client, uint32_t deadlineMs)
    : m_client(client), m_deadlineMs(deadlineMs) {}

WxResult MmtlsConnection::sendRaw(const Bytes& data) {
  if (!timeLeft(m_deadlineMs)) return WxResult::Timeout;
  size_t sent = m_client.write(data.data(), data.size());
  return sent == data.size() ? WxResult::Ok : WxResult::Network;
}

WxResult MmtlsConnection::sendPlain(uint8_t contentType, const Bytes& payload) {
  WxResult r = sendRaw(buildRecord(contentType, payload));
  if (r == WxResult::Ok) m_txSeq++;
  return r;
}

WxResult MmtlsConnection::sendEncrypted(uint8_t contentType, const Bytes& plaintext,
                                        const Bytes& key, const Bytes& iv) {
  Bytes ciphertext;
  if (!encryptRecordPayload(key, iv, m_txSeq, contentType, plaintext, ciphertext))
    return WxResult::Invalid;
  WxResult r = sendRaw(buildRecord(contentType, ciphertext));
  if (r == WxResult::Ok) m_txSeq++;
  return r;
}

WxResult MmtlsConnection::nextRecord(Record& out) {
  while (m_records.empty()) {
    std::vector<Record> parsed;
    size_t consumed = 0;
    if (!parseRecords(m_rxbuf, parsed, consumed)) return WxResult::Invalid;
    if (!parsed.empty()) {
      for (auto& rec : parsed) m_records.push_back(std::move(rec));
      m_rxbuf.erase(m_rxbuf.begin(), m_rxbuf.begin() + consumed);
      break;
    }
    if (m_rxbuf.size() > kMaxRecordBuffer) return WxResult::Invalid;
    if (!timeLeft(m_deadlineMs)) return WxResult::Timeout;
    size_t avail = m_client.available();
    if (avail == 0) {
      if (!m_client.connected()) return WxResult::Network;
      delay(2);
      continue;
    }
    size_t before = m_rxbuf.size();
    m_rxbuf.resize(before + avail);
    size_t got = m_client.read(m_rxbuf.data() + before, avail);
    if (got == 0) return WxResult::Network;
    m_rxbuf.resize(before + got);
  }
  out = std::move(m_records.front());
  m_records.pop_front();
  return WxResult::Ok;
}

WxResult MmtlsConnection::recvPlain(uint8_t& contentType, Bytes& payload) {
  Record rec;
  WxResult r = nextRecord(rec);
  if (r != WxResult::Ok) return r;
  m_rxSeq++;
  contentType = rec.contentType;
  payload = std::move(rec.payload);
  return WxResult::Ok;
}

WxResult MmtlsConnection::recvDecrypted(const Bytes& key, const Bytes& iv,
                                        uint8_t& contentType, Bytes& plaintext) {
  Record rec;
  WxResult r = nextRecord(rec);
  if (r != WxResult::Ok) return r;
  if (!decryptRecordPayload(key, iv, m_rxSeq, rec.contentType, rec.payload,
                            plaintext))
    return WxResult::Invalid;
  m_rxSeq++;
  contentType = rec.contentType;
  return WxResult::Ok;
}

void MmtlsConnection::feed(const Bytes& data) { append(m_transcript, data); }

Bytes MmtlsConnection::transcriptHash() { return sha256(m_transcript); }

WxResult MmtlsConnection::handshake() {
  if (!m_clientKey.generate()) return WxResult::Invalid;
  EcdhKey key2;
  if (!key2.generate()) return WxResult::Invalid;
  Bytes pub1, pub2;
  if (!m_clientKey.publicKey(pub1) || !key2.publicKey(pub2)) return WxResult::Invalid;
  Bytes clientRandom(32);
  esp_fill_random(clientRandom.data(), clientRandom.size());
  Bytes clientHello =
      buildClientHello(clientRandom, static_cast<uint32_t>(time(nullptr) & 0xFFFFFFFF),
                       pub1, pub2);
  if (clientHello.empty()) return WxResult::Invalid;
  feed(clientHello);
  WxResult r = sendPlain(kCtHandshake, clientHello);
  if (r != WxResult::Ok) return r;

  uint8_t contentType = 0;
  Bytes serverHelloPayload;
  r = recvPlain(contentType, serverHelloPayload);
  if (r != WxResult::Ok) return r;
  if (contentType != kCtHandshake) return WxResult::Invalid;
  uint8_t messageType = 0;
  Bytes serverHelloBody;
  if (!splitHandshake(serverHelloPayload, messageType, serverHelloBody) ||
      messageType != kHsServerHello)
    return WxResult::Invalid;
  feed(serverHelloPayload);
  ServerHello hello;
  if (!parseServerHello(serverHelloBody, hello)) return WxResult::Invalid;
  if (hello.cipher != kCipherEcdhe || hello.serverGroup != kGroupSecp256r1)
    return WxResult::Invalid;
  if (!m_clientKey.mmtlsSecret(hello.serverPubKey, m_secret)) return WxResult::Invalid;
  if (!deriveHandshakeKeys(m_secret, transcriptHash(), m_handshakeKeys))
    return WxResult::Invalid;

  for (int i = 0; i < 64; i++) {
    Bytes plaintext;
    r = recvDecrypted(m_handshakeKeys.serverKey, m_handshakeKeys.serverIv,
                      contentType, plaintext);
    if (r != WxResult::Ok) return r;
    if (contentType == kCtAlert) {
      LOG_W("WX", "handshake: server sent alert");
      return WxResult::AuthRejected;
    }
    if (contentType != kCtHandshake) return WxResult::Invalid;
    // 注意：transcript 喂的是完整记录载荷；消息体另存，不能覆盖 plaintext
    Bytes hsBody;
    if (!splitHandshake(plaintext, messageType, hsBody)) return WxResult::Invalid;
    if (messageType == kHsFinished) {
      Bytes finalHash = transcriptHash();
      Bytes master = deriveMaster(m_secret, finalHash);
      if (!deriveAppKeys(master, finalHash, m_appKeys)) return WxResult::Invalid;
      Bytes expected = finishedVerifyData(m_secret, false, finalHash);
      Bytes actual;
      if (!parseFinished(hsBody, actual)) return WxResult::Invalid;
      if (actual != expected) {
        LOG_W("WX", "handshake: server finished verify failed");
        return WxResult::AuthRejected;
      }
      m_finalHash = finalHash;
      m_haveFinalHash = true;
      feed(plaintext);
      return WxResult::Ok;
    }
    feed(plaintext);
    if (messageType == kHsCertVerify) {
      m_certVerifyHash = transcriptHash();
    } else if (messageType == kHsNewSessionTicket) {
      parseNewSessionTicket(hsBody, m_newSessionTickets);
    }
  }
  return WxResult::Invalid;
}

WxResult MmtlsConnection::finishHandshake() {
  if (!m_haveFinalHash) return WxResult::Invalid;
  Bytes verifyData = finishedVerifyData(m_secret, true, m_finalHash);
  Bytes finished = buildFinished(verifyData);
  if (finished.empty()) return WxResult::Invalid;
  return sendEncrypted(kCtHandshake, finished, m_handshakeKeys.clientKey,
                       m_handshakeKeys.clientIv);
}

WxResult MmtlsConnection::sendApp(uint32_t cmd, const Bytes& body) {
  Bytes packet = buildShortlink(cmd, static_cast<uint32_t>(m_appSeq), body);
  m_appSeq++;
  return sendEncrypted(kCtAppData, packet, m_appKeys.clientKey, m_appKeys.clientIv);
}

WxResult MmtlsConnection::recvApp(ShortlinkPacket& out) {
  uint8_t contentType = 0;
  Bytes plaintext;
  WxResult r = recvDecrypted(m_appKeys.serverKey, m_appKeys.serverIv, contentType,
                             plaintext);
  if (r != WxResult::Ok) return r;
  if (contentType != kCtAppData) return WxResult::Invalid;
  if (!parseShortlink(plaintext, out)) return WxResult::Invalid;
  if (out.magic != kShortlinkMagic || out.totalLen != plaintext.size())
    return WxResult::Invalid;
  return WxResult::Ok;
}

WxResult MmtlsConnection::takeAccessPsk(PSKEntry& out) {
  if (m_certVerifyHash.empty()) return WxResult::Invalid;
  int64_t now = static_cast<int64_t>(time(nullptr));
  for (auto& ticket : m_newSessionTickets) {
    Bytes psk = derivePsk(m_secret, m_certVerifyHash, ticket.pskType);
    PSKEntry entry;
    entry.pskType = ticket.pskType;
    entry.preSharedKey = std::move(psk);
    entry.ticketEntry = ticket.ticketEntry;
    entry.lifetime = ticket.lifetime;
    entry.expiredAt = now + ticket.lifetime;
    if (entry.pskType == 1 && entry.expiredAt > now) {
      out = std::move(entry);
      return WxResult::Ok;
    }
  }
  return WxResult::Invalid;
}

// ==== 目标排序 ====

namespace {

std::vector<Target> orderedLonglinkTargets(const std::vector<Target>& targets) {
  const uint16_t preferredPorts[] = {443, 8080, 80, 5000};
  std::vector<Target> ordered;
  auto contains = [&ordered](const Target& t) {
    for (const auto& o : ordered)
      if (o == t) return true;
    return false;
  };
  for (uint16_t port : preferredPorts) {
    for (const auto& t : targets) {
      if (t.port == port && !contains(t)) ordered.push_back(t);
    }
  }
  for (const auto& t : targets) {
    if (!contains(t)) ordered.push_back(t);
  }
  Target fallback{kLongLinkDomain, kLongLinkFallbackPort};
  for (auto it = ordered.begin(); it != ordered.end();) {
    if (*it == fallback)
      it = ordered.erase(it);
    else
      ++it;
  }
  if (ordered.size() >= 6) ordered.resize(5);
  ordered.push_back(fallback);
  return ordered;
}

std::vector<Target> shortlinkTargets(const std::vector<Target>& targets) {
  std::vector<Target> result;
  for (const auto& t : targets) {
    if (t.port != kShortlinkPort) continue;
    bool seen = false;
    for (const auto& r : result)
      if (r.ip == t.ip) seen = true;
    if (!seen) result.push_back(t);
  }
  bool hasFallback = false;
  for (const auto& r : result)
    if (r.ip == String(kShortlinkFallbackIp)) hasFallback = true;
  if (!hasFallback) result.push_back(Target{String(kShortlinkFallbackIp), kShortlinkPort});
  return result;
}

// ==== 登录（ManualAuth）====

WxResult loginTarget(const String& loginBuffer, const LoginMeta& meta,
                     const Target& target, uint32_t deadlineMs, LoginState& out) {
  WiFiClient client;
  if (!client.connect(target.ip.c_str(), target.port,
                      static_cast<int32_t>(deadlineMs - millis())))
    return WxResult::Network;
  client.setTimeout(deadlineMs - millis());
  LOG_I("WX", "stage: tcp connected, starting handshake");
  MmtlsConnection mmtls(client, deadlineMs);
  WxResult r = mmtls.handshake();
  LOG_I("WX", "stage: handshake result=%d", static_cast<int>(r));
  if (r != WxResult::Ok) return r;
  r = mmtls.finishHandshake();
  LOG_I("WX", "stage: finish_hs result=%d", static_cast<int>(r));
  if (r != WxResult::Ok) return r;

  ManualAuthTemp temp;
  Bytes appDeviceId(32);
  esp_fill_random(appDeviceId.data(), appDeviceId.size());
  Bytes body;
  if (!buildLoginBody(loginBuffer, meta.deviceId, appDeviceId, temp, body))
    return WxResult::Invalid;
  LOG_I("WX", "stage: manualauth send");
  r = mmtls.sendApp(kCmdManualAuth, body);
  if (r != WxResult::Ok) return r;
  ShortlinkPacket response;
  r = mmtls.recvApp(response);
  LOG_I("WX", "stage: manualauth recv result=%d", static_cast<int>(r));
  if (r != WxResult::Ok) return r;
  if (response.cmd != kCmdManualAuth) return WxResult::AuthRejected;
  Bytes manualAuthResponse;
  if (!parseLoginResponse(response.body, temp, manualAuthResponse))
    return WxResult::AuthRejected;
  AppSession session;
  if (!extractSession(manualAuthResponse, session)) return WxResult::AuthRejected;
  PSKEntry psk;
  if (mmtls.takeAccessPsk(psk) != WxResult::Ok) return WxResult::Invalid;
  session.deviceId = meta.deviceId;
  session.hostAppId = meta.hostAppId;
  out.session = std::move(session);
  out.psk = std::move(psk);
  client.stop();
  return WxResult::Ok;
}

// ==== 0-RTT 短连接 ====

struct ZeroRttRequest {
  Bytes body;        ///< 完整请求体（4 条记录拼接）
  Bytes psk;         ///< 原 PSK
  Bytes clientHello; ///< 明文 ClientHello（响应解密试探用）
  Bytes type8;       ///< 明文 type8
};

bool buildZeroRttRequest(const PSKEntry& entry, const Bytes& envelope, uint32_t nowSec,
                         ZeroRttRequest& out) {
  if (entry.pskType != 1 || entry.expiredAt <= static_cast<int64_t>(nowSec))
    return false;
  uint32_t timestamp = nowSec & 0xFFFFFFFF;
  Bytes clientRandom(32);
  esp_fill_random(clientRandom.data(), clientRandom.size());
  Bytes clientHello = buildPskClientHello(clientRandom, timestamp, entry.ticketEntry);
  if (clientHello.empty()) return false;
  Bytes earlyHash = sha256(clientHello);
  TrafficKeys earlyKeys;
  if (!derivePskOneWayKeys(entry.preSharedKey, earlyKeysLabel(), earlyHash, earlyKeys))
    return false;
  Bytes type8(kType8Prefix, kType8Prefix + sizeof(kType8Prefix));
  appendU32Be(type8, timestamp);

  Bytes type8Ct, envelopeCt, alertCt;
  if (!encryptRecordPayload(earlyKeys.key, earlyKeys.iv, 1, kCtPskHandshake, type8,
                            type8Ct))
    return false;
  if (!encryptRecordPayload(earlyKeys.key, earlyKeys.iv, 2, kCtAppData, envelope,
                            envelopeCt))
    return false;
  Bytes earlyAlert(kEarlyAlert, kEarlyAlert + sizeof(kEarlyAlert));
  if (!encryptRecordPayload(earlyKeys.key, earlyKeys.iv, 3, kCtAlert, earlyAlert,
                            alertCt))
    return false;
  out.body = buildRecord(kCtPskHandshake, clientHello);
  append(out.body, buildRecord(kCtPskHandshake, type8Ct));
  append(out.body, buildRecord(kCtAppData, envelopeCt));
  append(out.body, buildRecord(kCtAlert, alertCt));
  out.psk = entry.preSharedKey;
  out.clientHello = std::move(clientHello);
  out.type8 = std::move(type8);
  return true;
}

/// @brief 校验业务错误（JSON errcode 或 protobuf base_response）。
bool checkBusinessError(const Bytes& response) {
  // JSON 形态：{"errcode":N,...}
  if (!response.empty() && (response[0] == '{')) {
    std::string json(response.begin(), response.end());
    static const char* keys[] = {"errcode", "errCode", "error_code", "retcode", "ret"};
    for (const char* key : keys) {
      std::string pattern = std::string("\"") + key + "\":";
      size_t pos = json.find(pattern);
      if (pos == std::string::npos) continue;
      long value = strtol(json.c_str() + pos + pattern.length(), nullptr, 10);
      if (value != 0) return false;  // 非 0 = 业务拒绝
    }
  }
  // protobuf 形态：field1（varint 或嵌套 varint）
  PbField base;
  if (pbGet(response.data(), response.size(), 1, base)) {
    bool found = false;
    int64_t errorCode = 0;
    if (!base.isBytes) {
      found = true;
      errorCode = static_cast<int64_t>(base.intValue);
    } else {
      PbField nested;
      if (pbGet(base.bytesValue.data(), base.bytesValue.size(), 1, nested) &&
          !nested.isBytes) {
        found = true;
        errorCode = static_cast<int64_t>(nested.intValue);
      }
    }
    if (found && errorCode != 0) return false;
  }
  return true;
}

WxResult responseCode(const Bytes& response, String& code) {
  if (!checkBusinessError(response)) return WxResult::AuthRejected;
  Bytes codeBytes;
  if (!extractCode(response, codeBytes)) return WxResult::Invalid;
  String result(reinterpret_cast<const char*>(codeBytes.data()), codeBytes.size());
  if (result.length() == 0) return WxResult::Invalid;
  for (size_t i = 0; i < result.length(); i++) {
    if (result[i] < 0x20) return WxResult::Invalid;  // 控制字符
  }
  code = result;
  return WxResult::Ok;
}

WxResult parseZeroRttResponse(const Bytes& responseBody, const Bytes& psk,
                              const Bytes& clientHello, const Bytes& type8,
                              const Bytes& recvKey, String& code) {
  std::vector<Record> records;
  size_t consumed = 0;
  if (!parseRecords(responseBody, records, consumed) || consumed != responseBody.size())
    return WxResult::Invalid;
  const Bytes* serverHello = nullptr;
  const Bytes* appData = nullptr;
  for (const auto& rec : records) {
    if (rec.contentType == kCtHandshake && serverHello == nullptr)
      serverHello = &rec.payload;
    else if (rec.contentType == kCtAppData && appData == nullptr)
      appData = &rec.payload;
  }
  if (serverHello == nullptr || appData == nullptr) return WxResult::Invalid;
  uint8_t messageType = 0;
  Bytes body;
  if (!splitHandshake(*serverHello, messageType, body) || messageType != kHsServerHello)
    return WxResult::Invalid;

  // 三种 transcript 顺序 × 三个序列号试探（与参考实现一致）
  Bytes transcripts[3];
  transcripts[0] = clientHello;
  append(transcripts[0], *serverHello);
  transcripts[1] = clientHello;
  append(transcripts[1], type8);
  append(transcripts[1], *serverHello);
  transcripts[2] = clientHello;
  append(transcripts[2], *serverHello);
  append(transcripts[2], type8);
  const uint64_t sequences[] = {2, 1, 3};
  for (const auto& transcript : transcripts) {
    TrafficKeys keys;
    if (!derivePskOneWayKeys(psk, handshakeKeysLabel(), sha256(transcript), keys))
      continue;
    for (uint64_t seq : sequences) {
      Bytes plaintext;
      if (!decryptRecordPayload(keys.key, keys.iv, seq, kCtAppData, *appData, plaintext))
        continue;
      ShortlinkPacket shortlink;
      Bytes response;
      bool haveResponse = false;
      if (parseShortlink(plaintext, shortlink) &&
          shortlink.magic == kShortlinkMagic &&
          shortlink.totalLen == plaintext.size()) {
        if (sessionDecrypt(shortlink.body, recvKey, response)) haveResponse = true;
      }
      if (!haveResponse && sessionDecrypt(plaintext, recvKey, response))
        haveResponse = true;
      if (haveResponse) return responseCode(response, code);
    }
  }
  return WxResult::Invalid;
}

WxResult sendZeroRtt(const Target& target, const PSKEntry& psk, const Bytes& recvKey,
                     const Bytes& envelope, uint32_t deadlineMs, String& code) {
  ZeroRttRequest request;
  uint32_t nowSec = static_cast<uint32_t>(time(nullptr));
  if (!buildZeroRttRequest(psk, envelope, nowSec, request)) return WxResult::Invalid;
  String hexv = String(nowSec, HEX);
  while (hexv.length() < 8) hexv = "0" + hexv;
  String path = "/mmtls/" + hexv;
  int status = 0;
  Bytes response;
  std::vector<std::pair<String, String>> headers = {
      {"Accept", "*/*"},
      {"Cache-Control", "no-cache"},
      {"Content-Type", "application/octet-stream"},
      {"Upgrade", "mmtls"},
      {"User-Agent", "MicroMessenger Client"},
      {"X-Online-Host", kShortLinkDomain}};
  if (!httpRequest(target.ip.c_str(), target.port, kShortLinkDomain, "POST", path,
                   headers, request.body, deadlineMs - millis(), status, response))
    return WxResult::Network;
  if (status != 200) return WxResult::AuthRejected;
  return parseZeroRttResponse(response, request.psk, request.clientHello, request.type8,
                              recvKey, code);
}

}  // namespace

// ==== 顶层编排 ====

WxCodeResult wxGetCode(const String& loginBuffer, const String& appId,
                       uint32_t timeoutMs) {
  WxCodeResult result;
  if (loginBuffer.length() == 0 || appId.length() == 0) {
    result.result = WxResult::Invalid;
    result.errorDetail = "empty login_buffer or app_id";
    return result;
  }
  uint32_t deadlineMs = millis() + timeoutMs;
  LoginMeta meta;
  if (!parseLoginBuffer(loginBuffer, meta)) {
    result.result = WxResult::AuthRejected;
    result.errorDetail = "login_buffer invalid";
    return result;
  }

  LOG_I("WX", "stage: fetch httpdns...");
  std::map<String, DomainServers> domains;
  bool haveDns = fetchHttpdns(std::min<uint32_t>(5000, deadlineMs - millis()), domains);
  LOG_I("WX", "stage: httpdns %s (%u domains)", haveDns ? "ok" : "failed",
        static_cast<unsigned>(domains.size()));
  std::vector<Target> longTargets =
      orderedLonglinkTargets(haveDns ? targetsFor(domains, kLongLinkDomain, kMmtlsProtocol)
                                     : std::vector<Target>{});

  LoginState state;
  bool loggedIn = false;
  for (size_t i = 0; i < longTargets.size(); i++) {
    if (!timeLeft(deadlineMs)) break;
    // 剩余时间均分给剩余候选
    uint32_t share = (deadlineMs - millis()) / (longTargets.size() - i);
    LOG_I("WX", "stage: longlink try %s:%u", longTargets[i].ip.c_str(), longTargets[i].port);
    WxResult r = loginTarget(loginBuffer, meta, longTargets[i], millis() + share, state);
    if (r == WxResult::Ok) {
      loggedIn = true;
      break;
    }
    if (r == WxResult::AuthRejected) {
      result.result = r;
      result.errorDetail = "manual auth rejected";
      return result;
    }
    LOG_I("WX", "longlink candidate %s:%u failed (%d)", longTargets[i].ip.c_str(),
         longTargets[i].port, static_cast<int>(r));
  }
  if (!loggedIn) {
    result.result = WxResult::Network;
    result.errorDetail = "no longlink candidate succeeded";
    return result;
  }

  std::vector<Target> shortTargets =
      shortlinkTargets(haveDns ? targetsFor(domains, kShortLinkDomain, "http")
                               : std::vector<Target>{});
  Bytes plaintext = buildJsapiPlaintext(state.session.uin, appId, Bytes{},
                                        kJsLoginCmdId, state.session.hostAppId,
                                        Bytes{});
  Bytes envelope;
  if (!buildTransferPacket(state.session, plaintext, envelope)) {
    result.result = WxResult::Invalid;
    result.errorDetail = "transfer packet build failed";
    return result;
  }

  for (size_t i = 0; i < shortTargets.size(); i++) {
    if (!timeLeft(deadlineMs)) break;
    uint32_t share = (deadlineMs - millis()) / (shortTargets.size() - i);
    WxResult r = sendZeroRtt(shortTargets[i], state.psk, state.session.recvKey,
                             envelope, millis() + share, result.code);
    if (r == WxResult::Ok) {
      result.result = WxResult::Ok;
      return result;
    }
    if (r == WxResult::AuthRejected) {
      result.result = r;
      result.errorDetail = "js-login rejected";
      return result;
    }
    LOG_I("WX", "shortlink candidate %s:%u failed (%d)", shortTargets[i].ip.c_str(),
         shortTargets[i].port, static_cast<int>(r));
  }
  result.result = WxResult::Network;
  result.errorDetail = "no shortlink candidate succeeded";
  return result;
}

}  // namespace wx
