#pragma once

// ============================================================================
//  WxCrypto：微信 MMTLS/应用宝协议的密码学与编码基础件
//    - Bytes 缓冲区 + protobuf varint / 字段编解码
//    - LZ4 block（全字面量编码 + 完整解码）
//    - mbedtls 封装：SHA-256 / HMAC / HKDF / AES-GCM / ECDH P-256 / MD5
//  移植依据：moon-uestc `_wechat_crypto.py` / `_wechat_wire.py`（与
//  YYB-Go-Enhanced 参考实现逐字节一致，2026-10-24 版）。
//  线程约束：仅在 net_task 中调用。
// ============================================================================

#include <Arduino.h>
#include <mbedtls/md.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace wx {

using Bytes = std::vector<uint8_t>;

/// @brief 把十六进制字符串解码为字节串（长度必须为偶数）。
Bytes fromHex(const String& hex);
/// @brief 字节串转小写十六进制。
String toHex(const uint8_t* data, size_t len);
String toHex(const Bytes& data);

/// @brief 追加辅助。
void append(Bytes& out, const uint8_t* data, size_t len);
void append(Bytes& out, const Bytes& data);
void appendU8(Bytes& out, uint8_t v);
void appendU16Be(Bytes& out, uint16_t v);
void appendU32Be(Bytes& out, uint32_t v);
void appendU64Be(Bytes& out, uint64_t v);

// ---------------------------------------------------------------------------
// protobuf varint / 字段（仅覆盖协议用到的 wire type 0/1/2/5）
// ---------------------------------------------------------------------------

/// @brief 编码无符号 base-128 varint。
Bytes encodeVarint(uint64_t value);
/// @brief 读取一个 varint；失败返回 false 并把 offset 置 0。
bool readVarint(const uint8_t* data, size_t len, size_t& offset, uint64_t& value);

/// @brief protobuf 字段：length-delimited（wire type 2）。
Bytes pbLen(uint32_t field, const Bytes& data);
/// @brief protobuf 字段：varint（wire type 0）。
Bytes pbVar(uint32_t field, uint64_t value);

/// @brief 解析后的 protobuf 字段（值或 bytes，二选一）。
struct PbField {
  bool isBytes = false;
  uint64_t intValue = 0;
  Bytes bytesValue;
};

/// @brief 解析一段 protobuf 消息（只保留最后一个同号字段，与参考实现一致）。
/// @return 成功与否；失败时 offset 无意义。
bool pbParse(const uint8_t* data, size_t len,
             bool (*visit)(uint32_t field, uint8_t wireType, const PbField& value,
                           void* ctx),
             void* ctx);

/// @brief 便捷版：取指定字段的值（最后一个出现的）。
bool pbGet(const uint8_t* data, size_t len, uint32_t field, PbField& out);

// ---------------------------------------------------------------------------
// LZ4 block
// ---------------------------------------------------------------------------

/// @brief LZ4 全字面量压缩（单序列、无匹配；协议请求侧固定使用）。
Bytes lz4AllLiteral(const Bytes& data);
/// @brief LZ4 block 解码；maxOutput 防炸弹（默认 256 KB 足够协议包）。
bool lz4Decompress(const uint8_t* data, size_t len, Bytes& out,
                   size_t maxOutput = 256 * 1024);

// ---------------------------------------------------------------------------
// mbedtls 封装
// ---------------------------------------------------------------------------

/// @brief SHA-256 摘要（32 字节）。
Bytes sha256(const uint8_t* data, size_t len);
Bytes sha256(const Bytes& data);           ///< 单段
Bytes sha256(const Bytes& a, const Bytes& b);  ///< 两段拼接

/// @brief HMAC-SHA256。
Bytes hmacSha256(const Bytes& key, const uint8_t* data, size_t len);
Bytes hmacSha256(const Bytes& key, const Bytes& data);

/// @brief HKDF-Extract(SHA-256)。
Bytes hkdfExtract(const Bytes& salt, const Bytes& ikm);
/// @brief HKDF-Expand(SHA-256)，info = label || context（协议的自定义 label 拼接）。
Bytes hkdfExpand(const Bytes& prk, const Bytes& info, size_t length);
/// @brief expand_label(secret, label, context, length) = hkdfExpand(secret, label+context, length)。
Bytes expandLabel(const Bytes& secret, const char* label, const Bytes& context,
                  size_t length);

/// @brief MD5 摘要（16 字节，应用宝 Ual-Access-Signature 用）。
Bytes md5(const uint8_t* data, size_t len);

/// @brief AES-GCM 结果（data || tag，与 mbedtls 输出一致）。
struct GcmResult {
  bool ok = false;
  Bytes data;  // 加密时为密文+tag；解密时为明文
};

/// @brief AES-GCM（key 16/24/32 字节，nonce 12 字节，aad 可空）。
GcmResult aesGcmEncrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                        const uint8_t* plaintext, size_t len);
GcmResult aesGcmDecrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                        const uint8_t* ciphertext, size_t len);

/// @brief YYB blob 布局加密：ciphertext || IV(12) || tag(16)，IV 内部随机生成。
/// @param ivOut 返回使用的 IV（调用方需要时用于对拍/测试）。
bool gcmEncryptLayout(const Bytes& key, const Bytes& aad, const uint8_t* plaintext,
                      size_t len, Bytes& out, Bytes* ivOut = nullptr);
/// @brief YYB blob 布局解密：入参 ciphertext || IV || tag。
bool gcmDecryptLayout(const Bytes& key, const Bytes& aad, const Bytes& blob,
                      Bytes& out);

// ---------------------------------------------------------------------------
// ECDH P-256
// ---------------------------------------------------------------------------

/// @brief 一次性 ECDH 密钥对（RAII）。
class EcdhKey {
 public:
  EcdhKey();
  ~EcdhKey();
  bool generate();                       ///< 生成新密钥对
  bool publicKey(Bytes& out) const;      ///< SEC1 未压缩公钥（65 字节）
  /// @brief 计算与对端公钥的 ECDH 共享秘密的 SHA-256（MMTLS secret）。
  bool mmtlsSecret(const Bytes& peerPublic, Bytes& out) const;
  /// @brief 原始 ECDH 共享 X 坐标（ManualAuth 混合加密用）。
  bool rawShared(const Bytes& peerPublic, Bytes& out) const;
  /// @brief 派生私钥（测试向量用固定密钥）。
  bool loadPrivate(const Bytes& privateScalar);

 private:
  void* m_ctx;  // mbedtls_ecdh_context*（PIMPL，避免头文件依赖）
};

}  // namespace wx
