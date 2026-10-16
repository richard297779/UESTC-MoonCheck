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

  // ---- 账号表（唯一事实来源；单人 = 表里只有一条）----
  /// @brief 把某个账号套用到**内存**配置（不落盘）。
  /// @note 登录/打卡链路只有一套：执行器先把选定的账号 applyProfile() 进去，
  ///       再按既有的单账号流程走，因此**不需要**为多人另写一份业务代码。
  void applyProfile(const CheckinProfile& profile);

  /// @brief 当前内存配置对应的是哪个账号（status / 回执里显示，避免"给谁打了卡"看不清）。
  /// @note 从未 applyProfile 过时为 `"device"`（= 设备自身 NVS 里的 auth/loc）。
  const String& activeAccount() const { return m_activeLabel; }

  /// @brief 用设备自身的 NVS 配置（auth/loc）合成一条账号记录。
  /// @note 账号表为空时使用（老设备只配过 auth 的迁移路径），等价于「列表第一个用户」。
  CheckinProfile deviceProfile() const;

  /// @brief 相邻两个账号之间的间隔秒数（错峰，默认 20 s）。
  uint16_t multiGapSeconds() const { return m_multiGapSeconds; }
  /// @brief 设置账号间隔秒数（持久化）。
  Result setMultiGapSeconds(uint16_t seconds);

  /// @brief 设置自定义设备标签（BLE 广播名后缀）。
  /// @note 只允许 `A-Z a-z 0-9 - _`，最长 12 字符；传空串表示恢复"MAC 尾号"命名。
  Result setDeviceLabel(const String& label);

  // ---- 手机通知（只做「设备 -> 手机」单向出口，不接收任何远程指令）----
  /// @brief 总开关（关闭时任何事件都不发）。
  bool notifyEnabled() const { return m_notifyEnabled; }
  /// @brief 推送样式：0=GET 模板(Bark/Server酱) 1=POST 纯文本(ntfy) 2=POST JSON(钉钉/自建中转)。
  uint8_t notifyStyle() const { return m_notifyStyle; }
  /// @brief 完整推送 URL（含密钥，**绝不整条打印**）；style=0 时支持 {title} / {body} 占位。
  const String& notifyUrl() const { return m_notifyUrl; }
  /// @brief URL 脱敏形式（只保留 scheme + host + 掩码），供 status / 日志使用。
  String notifyUrlMasked() const;
  /// @brief 同一事件两次推送之间的最小间隔（去重/限流）。
  uint16_t notifyGapSeconds() const { return m_notifyGapSeconds; }
  /// @brief 每日推送上限（防止故障时刷屏或触发对端限流）。
  uint8_t notifyDailyQuota() const { return m_notifyDailyQuota; }

  /// @brief 分项设置通知配置：传空串/负数表示"不改这一项"。
  /// @param enabled -1=不改，0/1=设置开关
  /// @param style   -1=不改，0/1/2=设置样式
  /// @param gap     -1=不改，0-3600=设置去重间隔秒
  /// @param quota   -1=不改，0-255=设置每日上限
  Result setNotify(int enabled, int style, const String& url, int gap, int quota);

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
  String m_activeLabel = "device";  ///< 当前内存配置对应的账号标签（运行期，不落盘）
  uint16_t m_multiGapSeconds = 20;  ///< 账号之间的间隔秒数
  bool m_notifyEnabled = false;     ///< 通知总开关
  uint8_t m_notifyStyle = 2;        ///< 0=GET 模板 1=POST 纯文本 2=POST JSON
  String m_notifyUrl;               ///< 推送 URL（含 token，只输出 host）
  uint16_t m_notifyGapSeconds = 60; ///< 同事件去重间隔
  uint8_t m_notifyDailyQuota = 40;  ///< 每日推送上限
  SemaphoreHandle_t m_mutex = nullptr;
};
