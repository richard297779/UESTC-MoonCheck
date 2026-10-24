#pragma once

// ============================================================================
//  WxWire：微信 MMTLS / ManualAuth / js-login 协议编解码
//  移植依据：moon-uestc `_wechat_wire.py`（与 YYB-Go-Enhanced 参考逐字节一致）。
//  纯编解码层，不含网络；网络编排在 WxMmtls。
// ============================================================================

#include <Arduino.h>

#include "wx_crypto.h"

namespace wx {

// ---- 协议常量（与参考实现严格一致，勿改） ----
constexpr uint16_t kRecordVersion = 0xF103;
constexpr uint8_t kCtAlert = 0x15;
constexpr uint8_t kCtHandshake = 0x16;
constexpr uint8_t kCtAppData = 0x17;
constexpr uint8_t kCtPskHandshake = 0x19;
constexpr uint8_t kHsClientHello = 0x01;
constexpr uint8_t kHsServerHello = 0x02;
constexpr uint8_t kHsNewSessionTicket = 0x04;
constexpr uint8_t kHsCertVerify = 0x0F;
constexpr uint8_t kHsFinished = 0x14;
constexpr uint16_t kCipherEcdhe = 0xC02B;
constexpr uint16_t kCipherPsk = 0x00A8;
constexpr uint8_t kExtKeyShare = 0x01;
constexpr uint32_t kGroupSecp256r1 = 0x01;
constexpr uint16_t kShortlinkMagic = 0x1110;
constexpr uint16_t kShortlinkVersion = 0x076D;
constexpr uint16_t kTransferCmd = 0x0B41;
constexpr uint32_t kJsLoginCmdId = 1029;
constexpr uint16_t kCmdManualAuth = 0x0D7D;
constexpr uint64_t kClientVersion = 524545;
constexpr uint64_t kAppVersion = 1901;
constexpr uint64_t kSessionClientVersion = 1661404927;
constexpr size_t kKeyLen = 65;
constexpr size_t kAesIvLen = 12;
constexpr size_t kGcmTagLen = 16;

/// @brief ManualAuth 服务器静态公钥（公开常量，P-256 未压缩点）。
Bytes manualAuthServerPub();

// ---- 数据结构 ----

struct Record {
  uint8_t contentType = 0;
  Bytes payload;
};

struct ServerHello {
  Bytes serverRandom;
  uint16_t cipher = 0;
  uint32_t serverGroup = 0;
  Bytes serverPubKey;
};

struct ShortlinkPacket {
  uint32_t totalLen = 0;
  uint16_t magic = 0;
  uint16_t ver = 0;
  uint32_t cmd = 0;
  uint32_t seq = 0;
  Bytes body;
};

/// @brief ManualAuth 建立的应用会话密钥（敏感，勿打印）。
struct AppSession {
  Bytes sendKey;
  Bytes recvKey;
  Bytes f9;
  uint64_t uin = 0;
  Bytes ticket;
  Bytes deviceId;
  Bytes hostAppId;
};

struct PSKEntry {
  uint8_t pskType = 0;
  Bytes preSharedKey;
  Bytes ticketEntry;
  uint32_t lifetime = 0;
  int64_t expiredAt = 0;
};

struct LoginMeta {
  Bytes ticket;
  Bytes deviceId;
  Bytes hostAppId;
};

/// @brief ManualAuth 一次性中间态（ephemeral 私钥 + 派生密钥材料）。
struct ManualAuthTemp {
  EcdhKey* ephemeral = nullptr;  // 拥有权；用完由调用方释放
  Bytes okm;
  Bytes compBody;
  ~ManualAuthTemp() { delete ephemeral; }
};

// ---- MMTLS 记录 / 握手 ----

Bytes buildRecord(uint8_t contentType, const Bytes& payload);
/// @brief 解析完整记录；incomplete 之前未消费的字节数（用于续读）。
bool parseRecords(const Bytes& data, std::vector<Record>& out, size_t& consumed);

Bytes buildHandshake(uint8_t messageType, const Bytes& body);
bool splitHandshake(const Bytes& payload, uint8_t& messageType, Bytes& body);

Bytes buildClientHello(const Bytes& clientRandom32, uint32_t timestamp,
                       const Bytes& g1Public65, const Bytes& g2Public65);
Bytes buildPskClientHello(const Bytes& clientRandom32, uint32_t timestamp,
                          const Bytes& ticketEntry);
bool parseServerHello(const Bytes& body, ServerHello& out);
Bytes buildFinished(const Bytes& verifyData32);
bool parseFinished(const Bytes& body, Bytes& verifyData);
bool parseNewSessionTicket(const Bytes& body, std::vector<PSKEntry>& out);

// ---- 记录加解密（AAD = seq || type || 0xF103 || cipherLen）----

Bytes buildRecordAad(uint64_t sequence, uint8_t contentType, size_t cipherLen);
bool encryptRecordPayload(const Bytes& key, const Bytes& iv, uint64_t sequence,
                          uint8_t contentType, const Bytes& plaintext, Bytes& out);
bool decryptRecordPayload(const Bytes& key, const Bytes& iv, uint64_t sequence,
                          uint8_t contentType, const Bytes& ciphertext, Bytes& out);

// ---- MMTLS 密钥派生 ----

struct TrafficKeys {
  Bytes clientKey, serverKey, clientIv, serverIv;  // 双向（56 字节块）
  Bytes key, iv;                                    // 单向（28 字节块）
};
Bytes deriveMaster(const Bytes& secret, const Bytes& handshakeHash);
bool deriveHandshakeKeys(const Bytes& secret, const Bytes& handshakeHash,
                         TrafficKeys& out);
bool deriveAppKeys(const Bytes& master, const Bytes& handshakeHash, TrafficKeys& out);
Bytes finishedVerifyData(const Bytes& secret, bool isClient, const Bytes& handshakeHash);
Bytes derivePsk(const Bytes& secret, const Bytes& certVerifyHash, uint8_t pskType);
bool derivePskOneWayKeys(const Bytes& psk, const char* label, const Bytes& hash,
                         TrafficKeys& out);
/// @brief 0-RTT 早期数据密钥 label（wx_mmtls 用）。
const char* earlyKeysLabel();
const char* handshakeKeysLabel();

// ---- 短连接包 ----

Bytes buildShortlink(uint32_t cmd, uint32_t seq, const Bytes& body,
                     uint16_t version = 0);
bool parseShortlink(const Bytes& packet, ShortlinkPacket& out);

// ---- WPKG 头 ----

/// @brief 组 WPKG 头（ints/byts 均按字段号升序；参考实现要求）。
Bytes wpkgHead(const std::vector<std::pair<uint32_t, uint64_t>>& ints,
               const std::vector<std::pair<uint32_t, Bytes>>& byts);
struct WpkgHead {
  std::vector<std::pair<uint32_t, uint64_t>> ints;
  std::vector<std::pair<uint32_t, Bytes>> byts;
  size_t offset = 0;  ///< 头结束位置（含 trailer）
};
bool decodeWpkgHead(const Bytes& data, WpkgHead& out);

// ---- login_buffer / ManualAuth ----

bool parseLoginBuffer(const String& loginBufferB64, LoginMeta& out);
/// @brief ManualAuth 请求体（WPKG 头 + 混合 ECDH 加密体）。
/// @param appDeviceId 32 字节随机（可注入以便测试）。
bool buildLoginBody(const String& loginBuffer, const Bytes& deviceId,
                    const Bytes& appDeviceId, ManualAuthTemp& temp, Bytes& out);
/// @brief 解密 ManualAuth 响应，返回明文 protobuf。
bool parseLoginResponse(const Bytes& body, ManualAuthTemp& temp, Bytes& out);
bool extractSession(const Bytes& manualAuthResponse, AppSession& out);

// ---- js-login ----

/// @brief 构造 js-login 传输明文（sessionDevice 可注入固定值用于测试）。
Bytes buildJsapiPlaintext(uint64_t uin, const String& appId, const Bytes& url,
                          uint32_t cmd, const Bytes& hostAppId,
                          const Bytes& sessionDevice);
/// @brief 加密并封装 wxaruntime_transfer 信封。
bool buildTransferPacket(const AppSession& session, const Bytes& plaintext,
                         Bytes& out);
/// @brief 解密传输响应（WPKG 头偏移探测 + AES-GCM 布局 + LZ4）。
bool sessionDecrypt(const Bytes& body, const Bytes& recvKey, Bytes& out);
/// @brief 从 js-login 响应中提取 code（field2.field3）。
bool extractCode(const Bytes& response, Bytes& code);

}  // namespace wx
