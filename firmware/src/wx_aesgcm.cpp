#include "wx_aesgcm.h"

#include <string.h>

namespace wx {
namespace {

// ---- AES-192（FIPS-197，字节序实现，仅正向加密；GCM 解密只需正向）----

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe,
    0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4,
    0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7,
    0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3,
    0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09,
    0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3,
    0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe,
    0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
    0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92,
    0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c,
    0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19,
    0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2,
    0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5,
    0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25,
    0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86,
    0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e,
    0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

inline uint8_t xtime(uint8_t x) {
  return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

struct Aes192 {
  uint8_t rk[208];  // 52 字 × 4 字节（Nr=12）

  explicit Aes192(const uint8_t key[24]) {
    memcpy(rk, key, 24);
    uint8_t rcon = 1;
    for (int i = 24; i < 208; i += 4) {
      uint8_t t0 = rk[i - 4], t1 = rk[i - 3], t2 = rk[i - 2], t3 = rk[i - 1];
      if (i % 24 == 0) {
        // RotWord + SubWord + Rcon
        uint8_t tmp = t0;
        t0 = static_cast<uint8_t>(kSbox[t1] ^ rcon);
        t1 = kSbox[t2];
        t2 = kSbox[t3];
        t3 = kSbox[tmp];
        rcon = xtime(rcon);
      }
      rk[i] = static_cast<uint8_t>(rk[i - 24] ^ t0);
      rk[i + 1] = static_cast<uint8_t>(rk[i - 23] ^ t1);
      rk[i + 2] = static_cast<uint8_t>(rk[i - 22] ^ t2);
      rk[i + 3] = static_cast<uint8_t>(rk[i - 21] ^ t3);
    }
  }

  void encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    memcpy(s, in, 16);
    addRoundKey(s, rk);
    for (int round = 1; round <= 12; round++) {
      subBytes(s);
      shiftRows(s);
      if (round < 12) mixColumns(s);
      addRoundKey(s, rk + round * 16);
    }
    memcpy(out, s, 16);
  }

 private:
  static void addRoundKey(uint8_t s[16], const uint8_t* k) {
    for (int i = 0; i < 16; i++) s[i] ^= k[i];
  }
  static void subBytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];
  }
  static void shiftRows(uint8_t s[16]) {
    // 状态列主序：s[c*4+r]；行 r 循环左移 r
    uint8_t t[16];
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++) t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
    memcpy(s, t, 16);
  }
  static void mixColumns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
      uint8_t* p = s + c * 4;
      uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
      p[0] = static_cast<uint8_t>(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
      p[1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
      p[2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
      p[3] = static_cast<uint8_t>(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
    }
  }
};

// ---- GF(2^128) 乘法（GCM 约定，NIST SP 800-38D）----

void gf128Mul(const uint8_t X[16], const uint8_t Y[16], uint8_t out[16]) {
  uint8_t Z[16] = {0};
  uint8_t V[16];
  memcpy(V, Y, 16);
  for (int i = 0; i < 128; i++) {
    if (X[i / 8] & (0x80 >> (i % 8))) {
      for (int j = 0; j < 16; j++) Z[j] ^= V[j];
    }
    bool lsb = (V[15] & 1) != 0;
    for (int j = 15; j > 0; j--) V[j] = static_cast<uint8_t>((V[j] >> 1) | (V[j - 1] << 7));
    V[0] >>= 1;
    if (lsb) V[0] ^= 0xE1;
  }
  memcpy(out, Z, 16);
}

void xor16(uint8_t* dst, const uint8_t* src) {
  for (int i = 0; i < 16; i++) dst[i] ^= src[i];
}

void ghash(const uint8_t H[16], const Bytes& aad, const Bytes& ct, uint8_t out[16]) {
  memset(out, 0, 16);
  auto absorb = [&](const uint8_t* data, size_t len) {
    size_t full = len / 16;
    for (size_t i = 0; i < full; i++) {
      xor16(out, data + i * 16);
      gf128Mul(out, H, out);
    }
    size_t rem = len % 16;
    if (rem) {
      uint8_t block[16] = {0};
      memcpy(block, data + full * 16, rem);
      xor16(out, block);
      gf128Mul(out, H, out);
    }
  };
  absorb(aad.data(), aad.size());
  absorb(ct.data(), ct.size());
  uint8_t lenBlock[16] = {0};
  uint64_t aadBits = static_cast<uint64_t>(aad.size()) * 8;
  uint64_t ctBits = static_cast<uint64_t>(ct.size()) * 8;
  for (int i = 0; i < 8; i++)
    lenBlock[i] = static_cast<uint8_t>(aadBits >> (56 - 8 * i));
  for (int i = 0; i < 8; i++)
    lenBlock[8 + i] = static_cast<uint8_t>(ctBits >> (56 - 8 * i));
  xor16(out, lenBlock);
  gf128Mul(out, H, out);
}

bool gcmCore(bool encrypt, const uint8_t key[24], const uint8_t nonce[12],
             const Bytes& aad, const uint8_t* input, size_t len, Bytes& out) {
  Aes192 aes(key);
  uint8_t H[16];
  {
    uint8_t zero[16] = {0};
    aes.encryptBlock(zero, H);
  }
  uint8_t J0[16];
  memcpy(J0, nonce, 12);
  J0[12] = 0;
  J0[13] = 0;
  J0[14] = 0;
  J0[15] = 1;

  // GCM 规范：先 CTR 得密文，GHASH 的输入是密文（不是明文）
  size_t dataLen = encrypt ? len : (len >= 16 ? len - 16 : 0);
  Bytes ctrData;
  {
    uint8_t cb[16], keystream[16];
    memcpy(cb, J0, 16);
    ctrData.resize(dataLen);
    for (size_t off = 0; off < dataLen; off += 16) {
      for (int i = 15; i >= 12; i--)
        if (++cb[i] != 0) break;
      aes.encryptBlock(cb, keystream);
      size_t n = (dataLen - off < 16) ? (dataLen - off) : 16;
      for (size_t i = 0; i < n; i++)
        ctrData[off + i] = static_cast<uint8_t>(input[off + i] ^ keystream[i]);
    }
  }
  // GHASH 输入：加密方向=CTR 输出的密文；解密方向=收到的密文
  Bytes ctForHash;
  if (encrypt) ctForHash = ctrData;
  else ctForHash.assign(input, input + dataLen);
  uint8_t S[16];
  ghash(H, aad, ctForHash, S);
  uint8_t encJ0[16], tag[16];
  aes.encryptBlock(J0, encJ0);
  for (int i = 0; i < 16; i++) tag[i] = static_cast<uint8_t>(S[i] ^ encJ0[i]);

  if (encrypt) {
    out = std::move(ctrData);
    out.insert(out.end(), tag, tag + 16);
    return true;
  }
  // 解密：常数时间 tag 校验（tag 对应收到的密文）
  uint8_t diff = 0;
  for (int i = 0; i < 16; i++)
    diff |= static_cast<uint8_t>(input[dataLen + i] ^ tag[i]);
  if (diff != 0) return false;
  out = std::move(ctrData);
  return true;
}

}  // namespace

bool aesGcm192Encrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                      const uint8_t* plaintext, size_t len, Bytes& out) {
  if (key.size() != 24 || nonce.size() != 12) return false;
  return gcmCore(true, key.data(), nonce.data(), aad, plaintext, len, out);
}

bool aesGcm192Decrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                      const uint8_t* ciphertext, size_t len, Bytes& out) {
  if (key.size() != 24 || nonce.size() != 12 || len < 16) return false;
  return gcmCore(false, key.data(), nonce.data(), aad, ciphertext, len, out);
}

}  // namespace wx
