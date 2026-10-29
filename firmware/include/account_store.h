#pragma once

// ============================================================================
//  AccountStore：账号表（打卡的唯一事实来源）
//    - 一个 NVS 键存整张表（JSON 数组），最多 kMaxProfiles 个账号
//    - 每个账号自带：标签 / **是否生效** / 学号 / 密码 / 指纹 / 坐标 / 最近一次结果
//    - 单人 = 表里只有一条；表为空时执行器退回 AppConfig::deviceProfile()
//    - 与 AppConfig 解耦：执行器用 applyProfile() 把某账号"套进"内存配置，
//      从而**原样复用**既有的单账号登录与打卡链路
//  线程约束：所有公开方法内部加锁，可从 net_task 与 BLE 指令处理中调用。
// ============================================================================

#include <Arduino.h>
#include <Preferences.h>

#include <vector>

#include "app_types.h"

/// @brief 单个打卡账号。
struct CheckinProfile {
  String label;             ///< 短标签（A-Z a-z 0-9 - _，≤12 字符），如 "A01"
  bool enabled = true;      ///< 本账号是否参与打卡（false=每天跳过，不计入总数与成功判定）
  String studentId;
  String password;
  String fingerprint;       ///< 可空（该账号未做免验证登记时会要验证码）
  double latitude = 0.0;
  double longitude = 0.0;
  String address;
  String lastDate;          ///< 最近一次打卡结果所属日期
  bool lastOk = false;
  String lastMsg;

  bool hasFingerprint() const { return fingerprint.length() > 0; }
  bool hasLocation() const { return latitude != 0.0 && longitude != 0.0; }
};

class AccountStore {
 public:
  /// @brief 最多支持的账号数（受 NVS 单值大小与打卡窗口总时长约束）。
  static constexpr size_t kMaxProfiles = 6;

  /// @brief 打开 NVS 命名空间并加载账号表。
  Result begin(const char* nvsNamespace = "accounts");

  size_t count() const;
  bool get(size_t index, CheckinProfile& out) const;
  int indexOfLabel(const String& label) const;
  bool hasAny() const { return count() > 0; }

  /// @brief 新增或按 label 覆盖一个账号（超出上限返回 InvalidParam）。
  Result addOrUpdate(const CheckinProfile& profile);
  Result replaceAt(size_t index, const CheckinProfile& profile);
  uint32_t revision() const { return m_revision; }
  void invalidateRevision() { ++m_revision; }
  String toJsonDetails() const;
  /// @brief 单独开关某个账号是否参与打卡（不改动其它字段）。
  Result setEnabled(const String& label, bool enabled);
  /// @brief 启用中的账号数（禁用的不参与打卡，也不计入成功判定分母）。
  size_t enabledCount() const;
  /// @brief 按 label 删除。
  Result remove(const String& label);
  /// @brief 清空全部账号。
  Result clearAll();

  /// @brief 记录某个账号本次的打卡结果（用于幂等与 status 展示）。
  Result setResult(const String& label, const String& date, bool success, const String& message);
  /// @brief 该账号在指定日期是否已确认成功。
  bool isCheckedInToday(const String& label, const String& date) const;

  /// @brief 脱敏摘要（不含密码/指纹值；学号只留前 4 位）——用于 BLE 回执。
  String toJsonSummary() const;
  /// @brief 逗号分隔的标签列表（如 "A01,A02"）。
  String labels() const;

 private:
  Result persist();

  std::vector<CheckinProfile> m_list;
  Preferences m_prefs;
  bool m_opened = false;
  SemaphoreHandle_t m_mutex = nullptr;
  uint32_t m_revision = 1;
};
