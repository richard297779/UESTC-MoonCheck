// ============================================================================
//  CryptoUtil 实现（基于 mbedTLS）
// ============================================================================

#include "crypto_util.h"

#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

#include "logger.h"

namespace {

const char* const kTag = "crypto_util";

/// @brief 与网页 encrypt.js 中的 $aes_chars 完全一致。
constexpr const char* kAesChars = "ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678";

/// @brief 密码明文前缀长度（4 个 AES 分组）。
constexpr size_t kRandomPrefixLength = 64;

/// @brief PKCS7 补齐：把数据补齐到 16 字节整数倍，并写入填充字节。
size_t applyPkcs7(uint8_t* buffer, size_t length) {
  const size_t padding = 16 - (length % 16);
  for (size_t i = 0; i < padding; i++) {
    buffer[length + i] = static_cast<uint8_t>(padding);
  }
  return length + padding;
}

}  // namespace

const char* CryptoUtil::charset() { return kAesChars; }

String CryptoUtil::randomString(size_t length) {
  const size_t charsetLength = strlen(kAesChars);
  String out;
  out.reserve(length + 1);
  for (size_t i = 0; i < length; i++) {
    out += kAesChars[esp_random() % charsetLength];
  }
  return out;
}

String CryptoUtil::randomFingerprint() {
  static const char* kHex = "0123456789ABCDEF";
  String out;
  out.reserve(33);
  for (int i = 0; i < 32; i++) {
    out += kHex[esp_random() % 16];
  }
  return out;
}

String CryptoUtil::base64Encode(const uint8_t* data, size_t length) {
  size_t encodedLength = 0;
  if (mbedtls_base64_encode(nullptr, 0, &encodedLength, data, length) != 0 &&
      encodedLength == 0) {
    return String();
  }
  uint8_t* buffer = static_cast<uint8_t*>(malloc(encodedLength + 1));
  if (buffer == nullptr) return String();
  size_t written = 0;
  const int rc = mbedtls_base64_encode(buffer, encodedLength + 1, &written, data, length);
  String out;
  if (rc == 0) {
    buffer[written] = '\0';
    out = String(reinterpret_cast<char*>(buffer));
  }
  free(buffer);
  return out;
}

String CryptoUtil::toHex(const uint8_t* data, size_t length) {
  static const char* kHex = "0123456789ABCDEF";
  String out;
  out.reserve(length * 2 + 1);
  for (size_t i = 0; i < length; i++) {
    out += kHex[(data[i] >> 4) & 0x0F];
    out += kHex[data[i] & 0x0F];
  }
  return out;
}

String CryptoUtil::encryptCasPassword(const String& password, const String& salt) {
  if (salt.length() != 16) {
    LOG_E(kTag, "invalid salt length: %u (expect 16)", static_cast<unsigned>(salt.length()));
    return String();
  }

  // 明文 = randomString(64) + password
  String plain = randomString(kRandomPrefixLength) + password;
  const size_t plainLength = plain.length();
  const size_t paddedLength = plainLength + 16;  // 最坏情况多一个分组

  uint8_t* input = static_cast<uint8_t*>(malloc(paddedLength));
  uint8_t* output = static_cast<uint8_t*>(malloc(paddedLength));
  if (input == nullptr || output == nullptr) {
    free(input);
    free(output);
    LOG_E(kTag, "oom during password encryption");
    return String();
  }
  memcpy(input, plain.c_str(), plainLength);
  const size_t totalLength = applyPkcs7(input, plainLength);

  // IV：与网页一致取 16 个随机字符（服务端不依赖其具体取值）
  const String ivString = randomString(16);
  uint8_t iv[16];
  memcpy(iv, ivString.c_str(), 16);

  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  const int keyRc = mbedtls_aes_setkey_enc(&context, reinterpret_cast<const uint8_t*>(salt.c_str()), 128);
  int cryptRc = -1;
  if (keyRc == 0) {
    cryptRc = mbedtls_aes_crypt_cbc(&context, MBEDTLS_AES_ENCRYPT, totalLength, iv, input, output);
  }
  mbedtls_aes_free(&context);
  free(input);

  String encoded;
  if (cryptRc == 0) {
    encoded = base64Encode(output, totalLength);
  } else {
    LOG_E(kTag, "aes enc failed: key=%d crypt=%d", keyRc, cryptRc);
  }
  free(output);
  return encoded;
}
