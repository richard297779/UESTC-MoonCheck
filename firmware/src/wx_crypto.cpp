#include "wx_crypto.h"

#include "logger.h"
#include "wx_aesgcm.h"

#include <esp_random.h>

#include <mbedtls/aes.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

namespace wx {

// ==== 十六进制 / 追加 ====

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

Bytes fromHex(const String& hex) {
  Bytes out;
  if (hex.length() % 2 != 0) return out;
  out.reserve(hex.length() / 2);
  for (size_t i = 0; i + 1 < hex.length(); i += 2) {
    int hi = hexVal(hex[i]), lo = hexVal(hex[i + 1]);
    if (hi < 0 || lo < 0) return Bytes{};
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return out;
}

String toHex(const uint8_t* data, size_t len) {
  static const char* kDigits = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 0x0F];
  }
  return out;
}

String toHex(const Bytes& data) { return toHex(data.data(), data.size()); }

void append(Bytes& out, const uint8_t* data, size_t len) {
  out.insert(out.end(), data, data + len);
}
void append(Bytes& out, const Bytes& data) {
  out.insert(out.end(), data.begin(), data.end());
}
void appendU8(Bytes& out, uint8_t v) { out.push_back(v); }
void appendU16Be(Bytes& out, uint16_t v) {
  out.push_back(v >> 8);
  out.push_back(v & 0xFF);
}
void appendU32Be(Bytes& out, uint32_t v) {
  out.push_back(v >> 24);
  out.push_back((v >> 16) & 0xFF);
  out.push_back((v >> 8) & 0xFF);
  out.push_back(v & 0xFF);
}
void appendU64Be(Bytes& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
}

// ==== varint / protobuf ====

Bytes encodeVarint(uint64_t value) {
  Bytes out;
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
  return out;
}

bool readVarint(const uint8_t* data, size_t len, size_t& offset, uint64_t& value) {
  if (offset > len) return false;
  value = 0;
  for (int index = 0; index < 10; index++) {
    if (offset >= len) return false;
    uint8_t byte = data[offset++];
    if (index == 9 && byte > 1) return false;  // 溢出
    value |= static_cast<uint64_t>(byte & 0x7F) << (index * 7);
    if (byte < 0x80) return true;
  }
  return false;
}

Bytes pbLen(uint32_t field, const Bytes& data) {
  Bytes out = encodeVarint((static_cast<uint64_t>(field) << 3) | 2);
  Bytes len = encodeVarint(data.size());
  append(out, len);
  append(out, data);
  return out;
}

Bytes pbVar(uint32_t field, uint64_t value) {
  Bytes out = encodeVarint(static_cast<uint64_t>(field) << 3);
  append(out, encodeVarint(value));
  return out;
}

bool pbParse(const uint8_t* data, size_t len,
             bool (*visit)(uint32_t field, uint8_t wireType, const PbField& value,
                           void* ctx),
             void* ctx) {
  size_t offset = 0;
  while (offset < len) {
    uint64_t tag;
    if (!readVarint(data, len, offset, tag)) return false;
    uint32_t field = static_cast<uint32_t>(tag >> 3);
    uint8_t wireType = tag & 7;
    if (field == 0 || field > ((1 << 29) - 1)) return false;
    PbField value;
    if (wireType == 0) {
      uint64_t v;
      if (!readVarint(data, len, offset, v)) return false;
      value.isBytes = false;
      value.intValue = v;
    } else if (wireType == 1) {
      if (offset + 8 > len) return false;
      value.isBytes = true;
      value.bytesValue.assign(data + offset, data + offset + 8);
      offset += 8;
    } else if (wireType == 2) {
      uint64_t l;
      if (!readVarint(data, len, offset, l)) return false;
      if (l > len - offset) return false;
      value.isBytes = true;
      value.bytesValue.assign(data + offset, data + offset + l);
      offset += static_cast<size_t>(l);
    } else if (wireType == 5) {
      if (offset + 4 > len) return false;
      value.isBytes = true;
      value.bytesValue.assign(data + offset, data + offset + 4);
      offset += 4;
    } else {
      return false;
    }
    if (!visit(field, wireType, value, ctx)) return false;
  }
  return true;
}

namespace {
struct PbGetCtx {
  uint32_t want;
  bool found = false;
  PbField out;
};
bool pbGetVisit(uint32_t field, uint8_t wireType, const PbField& value, void* ctxVoid) {
  auto* ctx = static_cast<PbGetCtx*>(ctxVoid);
  if (field == ctx->want) {
    ctx->found = true;
    ctx->out = value;
  }
  return true;
}
}  // namespace

bool pbGet(const uint8_t* data, size_t len, uint32_t field, PbField& out) {
  PbGetCtx ctx;
  ctx.want = field;
  if (!pbParse(data, len, pbGetVisit, &ctx)) return false;
  if (!ctx.found) return false;
  out = ctx.out;
  return true;
}

// ==== LZ4 ====

Bytes lz4AllLiteral(const Bytes& data) {
  Bytes out;
  size_t length = data.size();
  if (length < 15) {
    out.push_back(static_cast<uint8_t>(length << 4));
  } else {
    out.push_back(0xF0);
    size_t remaining = length - 15;
    while (remaining >= 255) {
      out.push_back(255);
      remaining -= 255;
    }
    out.push_back(static_cast<uint8_t>(remaining));
  }
  append(out, data);
  return out;
}

bool lz4Decompress(const uint8_t* data, size_t len, Bytes& out, size_t maxOutput) {
  out.clear();
  size_t offset = 0;
  while (offset < len) {
    uint8_t token = data[offset++];
    size_t literalLength = token >> 4;
    if (literalLength == 15) {
      while (true) {
        if (offset >= len) return false;
        uint8_t ext = data[offset++];
        literalLength += ext;
        if (ext != 255) break;
      }
    }
    if (literalLength > len - offset) return false;
    if (out.size() + literalLength > maxOutput) return false;
    out.insert(out.end(), data + offset, data + offset + literalLength);
    offset += literalLength;
    if (offset == len) break;
    if (offset + 2 > len) return false;
    uint16_t matchOffset = static_cast<uint16_t>(data[offset]) |
                           (static_cast<uint16_t>(data[offset + 1]) << 8);
    offset += 2;
    if (matchOffset == 0 || matchOffset > out.size()) return false;
    size_t matchLength = (token & 0x0F) + 4;
    if ((token & 0x0F) == 15) {
      while (true) {
        if (offset >= len) return false;
        uint8_t ext = data[offset++];
        matchLength += ext;
        if (ext != 255) break;
      }
    }
    if (out.size() + matchLength > maxOutput) return false;
    size_t start = out.size() - matchOffset;
    for (size_t i = 0; i < matchLength; i++) out.push_back(out[start + i]);
  }
  return true;
}

// ==== 哈希 / HMAC / HKDF / MD5 ====

Bytes sha256(const uint8_t* data, size_t len) {
  Bytes out(32);
  mbedtls_sha256(data, len, out.data(), 0);
  return out;
}

Bytes sha256(const Bytes& data) { return sha256(data.data(), data.size()); }

Bytes sha256(const Bytes& a, const Bytes& b) {
  Bytes out(32);
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, a.data(), a.size());
  mbedtls_sha256_update(&ctx, b.data(), b.size());
  mbedtls_sha256_finish(&ctx, out.data());
  mbedtls_sha256_free(&ctx);
  return out;
}

Bytes hmacSha256(const Bytes& key, const uint8_t* data, size_t len) {
  Bytes out(32);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, key.data(), key.size());
  mbedtls_md_hmac_update(&ctx, data, len);
  mbedtls_md_hmac_finish(&ctx, out.data());
  mbedtls_md_free(&ctx);
  return out;
}

Bytes hmacSha256(const Bytes& key, const Bytes& data) {
  return hmacSha256(key, data.data(), data.size());
}

Bytes hkdfExtract(const Bytes& salt, const Bytes& ikm) {
  return hmacSha256(salt, ikm);
}

Bytes hkdfExpand(const Bytes& prk, const Bytes& info, size_t length) {
  Bytes output;
  output.reserve(length);
  Bytes previous;
  uint8_t counter = 1;
  while (output.size() < length) {
    Bytes block = previous;
    append(block, info);
    block.push_back(counter);
    previous = hmacSha256(prk, block);
    append(output, previous);
    counter++;
  }
  output.resize(length);
  return output;
}

Bytes expandLabel(const Bytes& secret, const char* label, const Bytes& context,
                  size_t length) {
  Bytes info(reinterpret_cast<const uint8_t*>(label),
             reinterpret_cast<const uint8_t*>(label) + strlen(label));
  append(info, context);
  return hkdfExpand(secret, info, length);
}

Bytes md5(const uint8_t* data, size_t len) {
  Bytes out(16);
  mbedtls_md5(data, len, out.data());
  return out;
}

// ==== AES-GCM ====

static GcmResult gcmCommon(bool encrypt, const Bytes& key, const Bytes& nonce,
                           const Bytes& aad, const uint8_t* input, size_t len) {
  GcmResult result;
  if (nonce.size() != 12) return result;
  // 192 位密钥：预编译 mbedtls 的硬件 AES 路径拒绝（INVALID_KEYLENGTH），
  // 路由到纯软件实现（ManualAuth 混合加密固定 24 字节）
  if (key.size() == 24) {
    bool ok = encrypt ? aesGcm192Encrypt(key, nonce, aad, input, len, result.data)
                      : aesGcm192Decrypt(key, nonce, aad, input, len, result.data);
    result.ok = ok;
    if (!ok) result.data.clear();
    return result;
  }
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  do {
    int setkeyRc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data(),
                                      static_cast<unsigned int>(key.size() * 8));
    if (setkeyRc != 0) {
      LOG_W("WX", "gcm setkey fail bits=%u rc=%d",
            static_cast<unsigned>(key.size() * 8), setkeyRc);
      break;
    }
    result.data.resize(len + (encrypt ? 16 : 0));
    int rc;
    if (encrypt) {
      rc = mbedtls_gcm_crypt_and_tag(
          &ctx, MBEDTLS_GCM_ENCRYPT, len, nonce.data(), nonce.size(),
          aad.data(), aad.size(), input, result.data.data(), 16,
          result.data.data() + len);
      if (rc != 0) {
        LOG_W("WX", "gcm enc fail len=%u rc=%d", static_cast<unsigned>(len), rc);
        result.data.clear();
        break;
      }
    } else {
      if (len < 16) break;
      size_t bodyLen = len - 16;
      result.data.resize(bodyLen);
      rc = mbedtls_gcm_auth_decrypt(
          &ctx, bodyLen, nonce.data(), nonce.size(), aad.data(), aad.size(),
          input + bodyLen, 16, input, result.data.data());
      if (rc != 0) {
        result.data.clear();
        break;
      }
    }
    result.ok = true;
  } while (false);
  mbedtls_gcm_free(&ctx);
  return result;
}

GcmResult aesGcmEncrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                        const uint8_t* plaintext, size_t len) {
  return gcmCommon(true, key, nonce, aad, plaintext, len);
}

GcmResult aesGcmDecrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                        const uint8_t* ciphertext, size_t len) {
  return gcmCommon(false, key, nonce, aad, ciphertext, len);
}

bool gcmEncryptLayout(const Bytes& key, const Bytes& aad, const uint8_t* plaintext,
                      size_t len, Bytes& out, Bytes* ivOut) {
  Bytes nonce(12);
  esp_fill_random(nonce.data(), nonce.size());
  GcmResult r = aesGcmEncrypt(key, nonce, aad, plaintext, len);
  if (!r.ok) return false;
  // 布局：ciphertext || IV || tag
  out.clear();
  out.reserve(len + 12 + 16);
  out.insert(out.end(), r.data.begin(), r.data.end() - 16);
  append(out, nonce);
  out.insert(out.end(), r.data.end() - 16, r.data.end());
  if (ivOut) *ivOut = nonce;
  return true;
}

bool gcmDecryptLayout(const Bytes& key, const Bytes& aad, const Bytes& blob,
                      Bytes& out) {
  if (blob.size() < 12 + 16) return false;
  size_t cipherLen = blob.size() - 12 - 16;
  Bytes nonce(blob.begin() + cipherLen, blob.begin() + cipherLen + 12);
  Bytes wire(blob.begin(), blob.begin() + cipherLen);
  wire.insert(wire.end(), blob.begin() + cipherLen + 12, blob.end());
  GcmResult r = aesGcmDecrypt(key, nonce, aad, wire.data(), wire.size());
  if (!r.ok) return false;
  out = std::move(r.data);
  return true;
}

// ==== ECDH P-256（用 ECP 底层 API，mbedtls 2.x/3.x 均稳定）====

static int ecpRng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

struct EcpKeyPair {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;        // 私钥标量
  mbedtls_ecp_point Q;  // 公钥点
  bool ready = false;

  EcpKeyPair() {
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
  }
  ~EcpKeyPair() {
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
  }
};

EcdhKey::EcdhKey() : m_ctx(nullptr) { m_ctx = new EcpKeyPair(); }

EcdhKey::~EcdhKey() { delete static_cast<EcpKeyPair*>(m_ctx); }

bool EcdhKey::generate() {
  auto* kp = static_cast<EcpKeyPair*>(m_ctx);
  mbedtls_mpi_free(&kp->d);
  mbedtls_ecp_point_free(&kp->Q);
  kp->ready = false;
  if (mbedtls_ecp_group_load(&kp->grp, MBEDTLS_ECP_DP_SECP256R1) != 0) return false;
  if (mbedtls_ecp_gen_privkey(&kp->grp, &kp->d, ecpRng, nullptr) != 0) return false;
  if (mbedtls_ecp_mul(&kp->grp, &kp->Q, &kp->d, &kp->grp.G, ecpRng, nullptr) != 0)
    return false;
  kp->ready = true;
  return true;
}

bool EcdhKey::loadPrivate(const Bytes& privateScalar) {
  auto* kp = static_cast<EcpKeyPair*>(m_ctx);
  mbedtls_mpi_free(&kp->d);
  mbedtls_ecp_point_free(&kp->Q);
  kp->ready = false;
  if (mbedtls_ecp_group_load(&kp->grp, MBEDTLS_ECP_DP_SECP256R1) != 0) return false;
  if (mbedtls_mpi_read_binary(&kp->d, privateScalar.data(), privateScalar.size()) != 0)
    return false;
  if (mbedtls_ecp_mul(&kp->grp, &kp->Q, &kp->d, &kp->grp.G, ecpRng, nullptr) != 0)
    return false;
  kp->ready = true;
  return true;
}

bool EcdhKey::publicKey(Bytes& out) const {
  auto* kp = static_cast<EcpKeyPair*>(m_ctx);
  if (!kp->ready) return false;
  out.resize(65);
  size_t olen = 0;
  if (mbedtls_ecp_point_write_binary(&kp->grp, &kp->Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                     &olen, out.data(), out.size()) != 0)
    return false;
  out.resize(olen);
  return olen == 65;
}

/// @brief 计算 d * Qp（mbedtls 要求非 const 组指针；标量乘不改组状态）。
static bool ecpMul(EcpKeyPair* kp, const Bytes& peerPublic,
                   mbedtls_ecp_point* result) {
  mbedtls_ecp_point Qp;
  mbedtls_ecp_point_init(&Qp);
  do {
    if (mbedtls_ecp_point_read_binary(&kp->grp, &Qp, peerPublic.data(),
                                      peerPublic.size()) != 0)
      break;
    if (mbedtls_ecp_check_pubkey(&kp->grp, &Qp) != 0) break;
    if (mbedtls_ecp_mul(&kp->grp, result, &kp->d, &Qp, ecpRng, nullptr) != 0) break;
    mbedtls_ecp_point_free(&Qp);
    return true;
  } while (false);
  mbedtls_ecp_point_free(&Qp);
  return false;
}

bool EcdhKey::rawShared(const Bytes& peerPublic, Bytes& out) const {
  // mbedtls 3.x 的 ecp_mul 需要非 const 组指针；标量乘不修改密钥状态
  auto* kp = const_cast<EcpKeyPair*>(static_cast<const EcpKeyPair*>(m_ctx));
  if (!kp->ready || peerPublic.size() != 65) return false;
  mbedtls_ecp_point Z;
  mbedtls_ecp_point_init(&Z);
  do {
    if (!ecpMul(kp, peerPublic, &Z)) break;
    out.resize(32);
    if (mbedtls_mpi_write_binary(&Z.X, out.data(), 32) != 0) {
      out.clear();
      break;
    }
    mbedtls_ecp_point_free(&Z);
    return true;
  } while (false);
  mbedtls_ecp_point_free(&Z);
  return false;
}

bool EcdhKey::mmtlsSecret(const Bytes& peerPublic, Bytes& out) const {
  Bytes shared;
  if (!rawShared(peerPublic, shared)) return false;
  out = sha256(shared);
  return true;
}

}  // namespace wx
