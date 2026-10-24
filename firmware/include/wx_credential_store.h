#pragma once

// ============================================================================
//  WxCredentialStore：应用宝微信凭据的 NVS 持久化
//    - 每个学校账号对应一份微信凭据（本人扫码授权）
//    - 刷新轮换后立即保存；断电不丢
//  安全：凭据等价于设备级微信登录授权，只能存在本机 NVS，不外传不打日志。
// ============================================================================

#include <Arduino.h>

#include "wx_oauth.h"

namespace wx {

class CredentialStore {
 public:
  /// @brief 打开 NVS 命名空间。
  Result begin();

  /// @brief 读取指定账号槽位的凭据（slot = 账号在 AccountStore 的序号）。
  /// @return Ok=有凭据；NotFound=未导入；其他=存储错误。
  Result load(size_t slot, OauthCredentials& out);

  /// @brief 保存（导入或刷新轮换后调用）。
  Result save(size_t slot, const OauthCredentials& cred);

  /// @brief 删除指定槽位。
  Result remove(size_t slot);

  /// @brief 槽位数量上限（与账号表一致）。
  static constexpr size_t kMaxSlots = 6;

  /// @brief 状态摘要（不含凭据明文）：每槽 valid/expiresAt。
  String summary() const;

 private:
  String keyFor(size_t slot) const;
  void* m_prefs = nullptr;  // Preferences*（PIMPL）
};

}  // namespace wx
