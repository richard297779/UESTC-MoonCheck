#pragma once

// ============================================================================
//  CryptoUtil：加密工具
//    - 复刻网页 authserver/uestcTheme/static/common/encrypt.js 的密码加密
//    - 随机串 / 随机指纹 / Base64 / HEX
//  算法依据：docs/api-contract.md §6（已用抓包密文实测验证）
// ============================================================================

#include <Arduino.h>

class CryptoUtil {
 public:
  /// @brief 生成随机串（字符集与网页 $aes_chars 完全一致）。
  /// @param length 目标长度。
  static String randomString(size_t length);

  /// @brief 生成 32 位大写 HEX 浏览器指纹（bfp）。
  static String randomFingerprint();

  /// @brief 复刻网页 encryptPassword。
  /// @param password 明文密码。
  /// @param salt     登录页隐藏域 pwdEncryptSalt（16 字符）。
  /// @return base64(AES-128-CBC/PKCS7(randomString(64) + password))，不含 IV。
  /// @note 服务端从第 64 字节之后读取密码，因此 IV 取任意值均可（已实测）。
  static String encryptCasPassword(const String& password, const String& salt);

  /// @brief Base64 编码。
  static String base64Encode(const uint8_t* data, size_t length);

  /// @brief 字节数组转大写 HEX。
  static String toHex(const uint8_t* data, size_t length);

  /// @brief 网页同款随机字符集（供自检比对）。
  static const char* charset();
};
