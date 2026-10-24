#pragma once

// ============================================================================
//  WxMmtls：微信 MMTLS 连接编排（ManualAuth 建会话 + 0-RTT js-login 取码）
//  移植依据：moon-uestc `_wechat_mmtls.py`。
//    1. HTTPDNS/兜底发现 longlink 服务器 → MMTLS ECDHE 握手
//    2. ManualAuth（login_buffer）→ AppSession + access PSK
//    3. 0-RTT PSK 短连接（明文 HTTP POST + Upgrade: mmtls）→ js-login code
//  线程约束：仅在 net_task 中调用。
// ============================================================================

#include <Arduino.h>
#include <WiFiClient.h>

#include <deque>

#include "wx_dns.h"
#include "wx_wire.h"

namespace wx {

enum class WxResult {
  Ok = 0,
  Invalid,        ///< 协议/格式错误（不可重试同一材料）
  AuthRejected,   ///< 微信拒绝授权（凭据问题，需重新扫码/刷新）
  Timeout,
  Network,
};

/// @brief 取码结果。
struct WxCodeResult {
  WxResult result = WxResult::Network;
  String code;        ///< wx.login code（一次性，5 分钟）
  String errorDetail; ///< 不含凭据的失败描述
};

/// @brief 完整取码：login_buffer + 目标 appid → wx.login code。
/// @note 每次调用全新建连；不缓存不重放（code 一次性）。
WxCodeResult wxGetCode(const String& loginBuffer, const String& appId,
                       uint32_t timeoutMs = 30000);

/// @brief MMTLS 连接（一次性；析构断开）。
class MmtlsConnection {
 public:
  explicit MmtlsConnection(WiFiClient& client, uint32_t deadlineMs);
  ~MmtlsConnection() = default;

  /// @brief ECDHE 握手（ClientHello → ServerHello → 加密 flight → ServerFinished）。
  WxResult handshake();
  /// @brief 发送客户端 Finished（handshake 成功后调用一次）。
  WxResult finishHandshake();
  /// @brief 发送/接收应用短连接包。
  WxResult sendApp(uint32_t cmd, const Bytes& body);
  WxResult recvApp(ShortlinkPacket& out);
  /// @brief 取 access PSK（psk_type==1），失败返回 Invalid。
  WxResult takeAccessPsk(PSKEntry& out);

 private:
  WxResult sendRaw(const Bytes& data);
  WxResult sendPlain(uint8_t contentType, const Bytes& payload);
  WxResult sendEncrypted(uint8_t contentType, const Bytes& plaintext,
                         const Bytes& key, const Bytes& iv);
  WxResult nextRecord(Record& out);
  WxResult recvPlain(uint8_t& contentType, Bytes& payload);
  WxResult recvDecrypted(const Bytes& key, const Bytes& iv, uint8_t& contentType,
                         Bytes& plaintext);
  void feed(const Bytes& data);
  Bytes transcriptHash();

  WiFiClient& m_client;
  uint32_t m_deadlineMs;
  Bytes m_rxbuf;
  std::deque<Record> m_records;
  Bytes m_transcript;
  uint64_t m_txSeq = 0, m_rxSeq = 0, m_appSeq = 0;
  EcdhKey m_clientKey;
  Bytes m_secret;
  Bytes m_certVerifyHash;
  bool m_haveFinalHash = false;
  Bytes m_finalHash;
  std::vector<PSKEntry> m_newSessionTickets;
  TrafficKeys m_handshakeKeys, m_appKeys;
};

}  // namespace wx
