#pragma once

// ============================================================================
//  BleService：蓝牙串口服务（Nordic UART Service）
//    - 手机用 nRF Connect / 任意 BLE 调试工具即可配置，无需自研 App
//    - 帧格式：UTF-8 JSON + '\n'；长数据自动分片 {"seq","of","chunk"}
//    - 本阶段不启用配对（宿舍内自用），接口预留
//  线程约束：begin() 在 setup() 调用；notifyLine() 可被任意任务调用（内部加锁）。
// ============================================================================

#include <Arduino.h>

#include <functional>

#include "app_types.h"

class BleService;

/// @brief 便于回调类访问私有处理函数（定义在 .cpp 中）。
class BleServiceAccess;

class BleService {
 public:
  /// @brief 收到一整行（以 '\n' 结尾）时的回调。
  using LineCallback = std::function<void(const String& line)>;

  /// @brief 启动广播。
  /// @param deviceName 广播名，例如 "WANAN-A1B2"。
  Result begin(const String& deviceName);

  void setLineCallback(LineCallback callback) { m_lineCallback = callback; }

  /// @brief 在线更换 BLE 名字（配合 `name.set`）：更新 GATT 设备名并刷新广播。
  /// @note 已连接的中心不受影响；对端**重新扫描**即可看到新名字，无需重启设备。
  Result applyDeviceName(const String& deviceName);

  const String& deviceName() const { return m_deviceName; }

  /// @brief 发送一行 JSON。短帧直接发；长帧自动 base64 分片（{"seq","of","b64"}）。
  bool notifyLine(const String& json);

  bool isConnected() const { return m_connected; }
  uint16_t mtu() const { return m_mtu; }
  const String& peerAddress() const { return m_peerAddress; }

  /// @brief 供回调类调用（内部使用）。
  void handleConnected(uint16_t mtu, const String& address);
  void handleDisconnected(int reason);
  void handleMtuChanged(uint16_t mtu);
  void handleIncoming(const uint8_t* data, size_t length);

 private:
  /// @brief 发送一段原始文本（自动补 '\n'）。
  bool sendRaw(const String& text);
  /// @brief 处理接收缓冲中的完整行。
  void dispatchLines();
  /// @brief 当前单次通知允许的字节上限。
  size_t notifyBudget() const;
  /// @brief 暂存一条待发通知（MTU 尚未协商完成时）。
  void queuePending(const String& json);
  /// @brief MTU 提升后补发暂存的通知。
  void flushPending();

  LineCallback m_lineCallback;
  String m_deviceName;
  String m_buffer;         ///< 接收缓冲
  String m_peerAddress;
  volatile bool m_connected = false;
  volatile uint16_t m_mtu = 23;
  SemaphoreHandle_t m_txMutex = nullptr;
  void* m_txCharacteristic = nullptr;  ///< NimBLECharacteristic*

  /// 待发通知（最多 4 条）：刚连上时 MTU 常为 23，长回执会被暂存，待 MTU 协商完成后自动补发，
  /// 避免"结果回执丢失"（实测 2026-10-12 22:00 的打卡结果就是这样丢的）。
  static const uint8_t kPendingMax = 4;
  String m_pending[kPendingMax];
  uint8_t m_pendingCount = 0;
};
