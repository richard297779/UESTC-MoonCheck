#pragma once

// ============================================================================
//  WxAesGcm：纯软件 AES-GCM（192 位密钥专用）
//  背景：Arduino-esp32 预编译 mbedtls 走硬件 AES（CONFIG_MBEDTLS_HARDWARE_AES），
//        实测拒绝 192 位密钥（setkey 返回 AES_INVALID_KEYLENGTH=-114），而
//        微信 ManualAuth 混合加密固定使用 SHA256(ECDH) 的前 24 字节作密钥。
//  本实现：AES-192 加密方向 + GHASH + CTR（GCM 解密只用正向加密，无需解密轮）。
//  仅当 key.size()==24 时由 wx_crypto 的 gcm 封装路由到本实现；
//  128/256 位继续走 mbedtls 硬件加速。正确性由黄金向量对拍保证。
// ============================================================================

#include <stdint.h>

#include <vector>

#include "wx_crypto.h"

namespace wx {

/// @brief AES-GCM（key 24 字节）：加密，输出 ciphertext || tag(16)。
bool aesGcm192Encrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                      const uint8_t* plaintext, size_t len, Bytes& out);
/// @brief AES-GCM（key 24 字节）：校验 tag 并解密。
bool aesGcm192Decrypt(const Bytes& key, const Bytes& nonce, const Bytes& aad,
                      const uint8_t* ciphertext, size_t len, Bytes& out);

}  // namespace wx
