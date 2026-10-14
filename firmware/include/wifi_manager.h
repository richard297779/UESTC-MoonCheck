#pragma once

// ============================================================================
//  WifiManager：宿舍 WiFi 连接管理
//    - STA 模式连接（本阶段仅 WPA2/WPA3 个人版，无 802.1X / Portal）
//    - 指数退避重连（1s → 60s 封顶）
//    - 状态变化回调，供上层推送 BLE 事件
//  线程约束：connect() 可在 net_task 阻塞调用；poll() 由主循环每 1s 调用。
// ============================================================================

#include <Arduino.h>

#include <functional>

#include "app_config.h"
#include "app_types.h"

/// @brief WiFi 连接状态。
enum class WifiState : uint8_t { Idle, Connecting, Connected, Failed };

class WifiManager {
 public:
  /// @brief 状态变化回调：state + 可读信息（IP 或失败原因）。
  using StateCallback = std::function<void(WifiState state, const String& info)>;

  /// @brief 注入配置并初始化 WiFi 为 STA 模式（不立即连接）。
  Result begin(AppConfig& config);

  /// @brief 使用配置中的 SSID/密码连接；成功返回 Ok。
  Result connect(uint32_t timeoutMs = 20000);

  /// @brief 主动断开并清空重连计划。
  void disconnect();

  /// @brief 主循环周期调用：掉线后按退避策略自动重连。
  void poll();

  WifiState state() const { return m_state; }
  String ip() const;
  int32_t rssi() const;

  void setStateCallback(StateCallback callback) { m_callback = callback; }

  /// @brief 扫描周边 AP，结果以 JSON 数组写入 outJson。
  Result scan(String& outJson);

 private:
  void setState(WifiState state, const String& info);
  void startConnect();

  AppConfig* m_config = nullptr;
  WifiState m_state = WifiState::Idle;
  uint32_t m_nextRetryMs = 0;        ///< 下次重试的绝对时刻（0 表示不等待）
  uint32_t m_connectDeadlineMs = 0;  ///< 本次连接的超时时刻
  uint32_t m_retryCount = 0;
  bool m_connecting = false;
  StateCallback m_callback;
};
