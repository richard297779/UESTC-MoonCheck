#pragma once

// ============================================================================
//  AppConfig：配置中心
//    - 用 Preferences(NVS) 持久化全部配置
//    - 提供默认值、就绪判断与脱敏摘要
//  线程约束：begin() 在 setup() 调用；其余接口可从任意任务调用（内部加锁）。
// ============================================================================

#include <Arduino.h>
#include <Preferences.h>

#include "app_types.h"

/// @brief 多账号条目（完整定义见 account_store.h）。
struct CheckinProfile;

class AppConfig {
 public:
  /// @brief 打开 NVS 命名空间并加载配置（缺失项写回默认值）。
  Result begin(const char* nvsNamespace = "wanan");

  /// @brief 是否已具备联网与登录的最低配置。
  bool isReady() const;

  // ---- 读取 ----
  const WifiConfig& wifi() const { return m_wifi; }
  const AuthConfig& auth() const { return m_auth; }
  const LocationConfig& location() const { return m_location; }
  const ScheduleConfig& schedule() const { return m_schedule; }

  /// @brief 自定义设备标签（可为空 = 用蓝牙 MAC 尾号）。
  const String& deviceLabel() const { return m_deviceLabel; }

  /// @brief 有效 BLE 广播名：`WANAN-<标签>`；无标签时 `WANAN-<蓝牙MAC末两字节>`。
  String bleDeviceName() const;

  // ---- 写入（成功即落盘）----
  /// @brief 写入宿舍 WiFi。
  Result setWifi(const String& ssid, const String& password);
  /// @brief 写入学号 / 密码 / 指纹（指纹可为空）。
  Result setAuth(const String& studentId, const String& password, const String& fingerprint);
  /// @brief 写入打卡坐标与地址。
  Result setLocation(double latitude, double longitude, const String& address);
  /// @brief 写入打卡时间（"HH:MM"）、窗口结束与抖动秒数；windowStart 留空表示沿用当前值。
  Result setSchedule(const String& checkinAt, const String& windowEnd, uint16_t jitterSeconds,
                     const String& windowStart = "");
  /// @brief 开关每日自动打卡。
  Result setAutoDaily(bool enabled);

  /// @brief 重新从 NVS 载入配置（丢弃 applyProfile() 的运行期覆盖）。
  Result reload();

  // ---- 多人顺序打卡（单人版默认关闭；关闭时以下接口不影响任何既有行为）----
  /// @brief 把某个账号套用到**内存**配置（不落盘）。
  /// @note 多人执行器据此**原样复用**既有的单账号登录/打卡链路，无需两套业务代码。
  void applyProfile(const CheckinProfile& profile);

  /// @brief 是否启用多人顺序打卡。
  bool multiUserEnabled() const { return m_multiEnabled; }
  /// @brief 开关多人模式（持久化）。
  Result setMultiUserEnabled(bool enabled);

  /// @brief 相邻两个账号之间的间隔秒数（错峰，默认 20 s）。
  uint16_t multiGapSeconds() const { return m_multiGapSeconds; }
  /// @brief 设置账号间隔秒数（持久化）。
  Result setMultiGapSeconds(uint16_t seconds);

  /// @brief 设置自定义设备标签（BLE 广播名后缀）。
  /// @note 只允许 `A-Z a-z 0-9 - _`，最长 12 字符；传空串表示恢复"MAC 尾号"命名。
  Result setDeviceLabel(const String& label);

  /// @brief 记录最近一次打卡结果（持久化，掉电重启后仍可查询）。
  Result setLastResult(const String& date, bool success, const String& message);

  /// @brief 最近一次打卡结果（JSON 片段，供 BLE status 使用）。
  String lastResultJson();

  /// @brief 最近一次打卡结果是否为"成功"且日期与给定值一致。
  /// @note 调度器据此判断"今天是否已确认成功"，从而决定是否继续重试。
  bool isLastResultOkFor(const String& date);

  /// @brief 生成脱敏摘要（学号打码，密码/指纹只报是否存在）——用于 BLE status。
  String toJsonSummary() const;

  /// @brief 擦除全部配置并写回默认值。
  Result factoryReset();

 private:
  Result loadAll();
  Result persistString(const char* key, const String& value);

  Preferences m_prefs;
  bool m_opened = false;
  WifiConfig m_wifi;
  AuthConfig m_auth;
  LocationConfig m_location;
  ScheduleConfig m_schedule;
  String m_deviceLabel;   ///< 自定义 BLE 名标签（空=用 MAC 尾号）
  bool m_multiEnabled = false;   ///< 多人顺序打卡开关
  uint16_t m_multiGapSeconds = 20;  ///< 账号之间的间隔秒数
  SemaphoreHandle_t m_mutex = nullptr;
};
