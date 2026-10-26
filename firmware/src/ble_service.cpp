// ============================================================================
//  BleService 实现（NimBLE-Arduino）
// ============================================================================

#include "ble_service.h"

#include <NimBLEDevice.h>

#include "crypto_util.h"
#include "logger.h"

namespace {

const char* const kTag = "ble_service";

/// @brief Nordic UART Service UUID（通用约定，任何 BLE 调试工具都认识）。
constexpr const char* kNusServiceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* kNusRxUuid = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  ///< 手机 → 设备
constexpr const char* kNusTxUuid = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  ///< 设备 → 手机

/// @brief 接收缓冲上限（超过即丢弃，防止畸形数据吃内存）。
constexpr size_t kMaxReceiveBuffer = 4096;

/// @brief 单次通知的安全上限（特征值上限 512，留出余量）。
constexpr size_t kNotifyAttLimit = 480;

/// @brief 分片净荷上限（base64 前的原始字节数）。
constexpr size_t kMaxChunkBytes = 320;

/// @brief 分片封装开销（{"seq":0,"of":0,"b64":""}）。
constexpr size_t kChunkFrameOverhead = 40;

/// @brief 当前实例（回调是 C 风格函数指针，需要一个入口）。
BleService* g_instance = nullptr;

}  // namespace

// ---------------------------------------------------------------- 回调类

/// @brief GATT 服务端回调。
class BleServerCallbacks : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    if (g_instance == nullptr) return;
    char address[20];
    snprintf(address, sizeof(address), "%s", connInfo.getAddress().toString().c_str());
    g_instance->handleConnected(connInfo.getMTU(), String(address));
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    if (g_instance == nullptr) return;
    g_instance->handleDisconnected(reason);
    // 断开后重新广播，便于再次连接
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    if (g_instance != nullptr) g_instance->handleMtuChanged(MTU);
  }
};

/// @brief RX 特征值回调（手机写入）。
class BleRxCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    if (g_instance == nullptr) return;
    const std::string value = characteristic->getValue();
    g_instance->handleIncoming(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }
};

// ---------------------------------------------------------------- 生命周期

Result BleService::begin(const String& deviceName) {
  m_deviceName = deviceName;
  g_instance = this;
  if (m_txMutex == nullptr) {
    m_txMutex = xSemaphoreCreateMutex();
  }

  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setMTU(512);

  NimBLEServer* server = NimBLEDevice::createServer();
  if (server == nullptr) {
    LOG_E(kTag, "createServer failed");
    return Result::InternalError;
  }
  server->setCallbacks(new BleServerCallbacks());

  NimBLEService* service = server->createService(kNusServiceUuid);
  if (service == nullptr) {
    LOG_E(kTag, "createService failed");
    return Result::InternalError;
  }

  NimBLECharacteristic* tx = service->createCharacteristic(kNusTxUuid, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* rx = service->createCharacteristic(
      kNusRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  if (tx == nullptr || rx == nullptr) {
    LOG_E(kTag, "createCharacteristic failed");
    return Result::InternalError;
  }
  rx->setCallbacks(new BleRxCallbacks());
  m_txCharacteristic = tx;

  // NimBLE 2.x：服务随 server 启动，单独调用 start() 已弃用且无效果
  // 广播包只有 31 字节：128 位服务 UUID(18B) + 名字(约 12B) 会超长，
  // 因此把名字放广播包、把服务 UUID 放扫描响应包（这也是标准做法）。
  NimBLEAdvertisementData advertisementData;
  advertisementData.addServiceUUID(kNusServiceUuid);

  NimBLEAdvertisementData scanResponseData;
  scanResponseData.setName(deviceName.c_str());

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(advertisementData);
  advertising->setScanResponseData(scanResponseData);
  advertising->enableScanResponse(true);
  advertising->start();

  LOG_I(kTag, "ble ready: name=%s (service %s)", deviceName.c_str(), kNusServiceUuid);
  return Result::Ok;
}

Result BleService::applyDeviceName(const String& deviceName) {
  if (deviceName.length() == 0) return Result::InvalidParam;
  m_deviceName = deviceName;

  // 1) GATT 的 Generic Access「Device Name」特征值
  NimBLEDevice::setDeviceName(std::string(deviceName.c_str()));

  // 2) 广播/扫描响应里的名字（begin() 把名字放在扫描响应包里，这里同样只换它）。
  //    以下 API 均已在 NimBLE-Arduino 2.5.1 的 NimBLEAdvertising.h 中确认存在：
  //      bool stop();  bool start(uint32_t duration = 0, const NimBLEAddress* = nullptr);
  //      bool setScanResponseData(const NimBLEAdvertisementData&);  void enableScanResponse(bool);
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (advertising != nullptr) {
    advertising->stop();
    NimBLEAdvertisementData scanResponseData;
    scanResponseData.setName(deviceName.c_str());
    advertising->setScanResponseData(scanResponseData);
    advertising->enableScanResponse(true);
    advertising->start();
  }
  LOG_I(kTag, "ble name -> %s (广播已刷新，无需重启)", deviceName.c_str());
  return Result::Ok;
}

// ---------------------------------------------------------------- 事件
void BleService::handleConnected(uint16_t mtu, const String& address) {
  m_connected = true;
  m_mtu = (mtu > 23) ? mtu : 23;
  m_peerAddress = address;
  m_buffer = "";
  LOG_I(kTag, "central connected: %s (mtu=%u)", address.c_str(), m_mtu);
}

void BleService::handleDisconnected(int reason) {
  m_connected = false;
  m_buffer = "";
  // 打印断开原因码：0x13=对方主动断开（通常是 PC 端程序退出）0x08=链路超时（信号/供电）
  //                  0x16=本地主动断开 0x3E=连接建立失败
  const char* meaning = "其他";
  switch (reason) {
    case 0x08: meaning = "链路超时(信号/供电)"; break;
    case 0x13: meaning = "对方主动断开(PC 端程序退出?)"; break;
    case 0x16: meaning = "本地主动断开"; break;
    case 0x22: meaning = "LMP 响应超时"; break;
    case 0x3E: meaning = "连接建立失败"; break;
    default: break;
  }
  LOG_I(kTag, "central disconnected (reason=0x%02X %s)", reason & 0xFF, meaning);
}

void BleService::handleMtuChanged(uint16_t mtu) {
  if (mtu > 23) {
    m_mtu = mtu;
    LOG_I(kTag, "mtu updated: %u", static_cast<unsigned>(mtu));
    flushPending();  // 补发因 MTU 过小而被暂存的回执
  }
}

void BleService::handleIncoming(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) return;
  if (m_buffer.length() + length > kMaxReceiveBuffer) {
    LOG_W(kTag, "rx buffer overflow, dropped %u bytes", static_cast<unsigned>(length));
    m_buffer = "";
    return;
  }
  m_buffer.concat(reinterpret_cast<const char*>(data), length);
  dispatchLines();
}

void BleService::dispatchLines() {
  while (true) {
    const int newline = m_buffer.indexOf('\n');
    if (newline < 0) break;
    String line = m_buffer.substring(0, newline);
    m_buffer.remove(0, newline + 1);
    line.trim();
    if (line.length() == 0) continue;
    LOG_D(kTag, "rx command: %u bytes", static_cast<unsigned>(line.length()));
    if (m_lineCallback) m_lineCallback(line);
  }
}

// ---------------------------------------------------------------- 发送

bool BleService::sendRaw(const String& text) {
  if (!m_connected || m_txCharacteristic == nullptr) return false;
  NimBLECharacteristic* tx = static_cast<NimBLECharacteristic*>(m_txCharacteristic);
  if (m_txMutex) xSemaphoreTake(m_txMutex, portMAX_DELAY);
  tx->setValue(reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
  const bool ok = tx->notify();
  if (m_txMutex) xSemaphoreGive(m_txMutex);
  return ok;
}

size_t BleService::notifyBudget() const {
  const size_t mtuLimit = (m_mtu > 23) ? static_cast<size_t>(m_mtu) - 3 : 20;
  return (mtuLimit < kNotifyAttLimit) ? mtuLimit : kNotifyAttLimit;
}

void BleService::queuePending(const String& json) {
  if (m_pendingCount >= kPendingMax) {
    // 队列满：丢弃最旧的一条（最新结果更有价值）
    for (uint8_t i = 1; i < kPendingMax; i++) m_pending[i - 1] = m_pending[i];
    m_pendingCount = kPendingMax - 1;
  }
  m_pending[m_pendingCount++] = json;
}

void BleService::flushPending() {
  if (m_pendingCount == 0 || m_mtu <= 23) return;
  String items[kPendingMax];
  const uint8_t count = m_pendingCount;
  for (uint8_t i = 0; i < count; i++) items[i] = m_pending[i];
  m_pendingCount = 0;
  LOG_I(kTag, "flushing %u queued notification(s), mtu=%u", static_cast<unsigned>(count),
        static_cast<unsigned>(m_mtu));
  for (uint8_t i = 0; i < count; i++) notifyLine(items[i]);
}

bool BleService::notifyLine(const String& json) {
  if (!m_connected) return false;

  // 单次可发送上限 = min(特征值上限, MTU 可承载) - 余量
  size_t budget = notifyBudget();

  // MTU 尚未协商完成（刚连上时通常是 23）：先等一会儿，仍不够就暂存，等 MTU 提升后补发
  if (budget <= kChunkFrameOverhead + 16) {
    for (int i = 0; i < 30 && m_connected && notifyBudget() <= kChunkFrameOverhead + 16; i++) {
      delay(50);  // 最多等 1.5 s
    }
    budget = notifyBudget();
    if (budget <= kChunkFrameOverhead + 16) {
      LOG_W(kTag, "mtu too small (mtu=%u), queue %u-byte notification for later",
            static_cast<unsigned>(m_mtu), static_cast<unsigned>(json.length()));
      queuePending(json);
      return false;
    }
  }

  if (json.length() + 1 <= budget) {
    return sendRaw(json + "\n");
  }

  // 长数据用 base64 分片：base64 不含引号/反斜杠，帧长度可精确预测，
  // 不会被 JSON 转义撑爆（历史故障：转义后 477 字节的片变成 581 字节，超过 ATT 上限 512）。
  size_t chunkBytes = ((budget - kChunkFrameOverhead) * 3) / 4;
  if (chunkBytes > kMaxChunkBytes) chunkBytes = kMaxChunkBytes;
  if (chunkBytes < 32) chunkBytes = 32;

  const size_t total = (json.length() + chunkBytes - 1) / chunkBytes;
  bool allOk = true;
  for (size_t index = 0; index < total; index++) {
    const size_t from = index * chunkBytes;
    size_t to = from + chunkBytes;
    if (to > json.length()) to = json.length();

    String frame = "{\"seq\":";
    frame += String(static_cast<unsigned>(index));
    frame += ",\"of\":";
    frame += String(static_cast<unsigned>(total));
    frame += ",\"b64\":\"";
    frame += CryptoUtil::base64Encode(reinterpret_cast<const uint8_t*>(json.c_str()) + from,
                                      to - from);
    frame += "\"}\n";
    allOk = sendRaw(frame) && allOk;
    delay(10);  // 给 BLE 协议栈留出排空时间
  }
  LOG_I(kTag, "notify chunked: %u bytes -> %u frames (mtu=%u)", static_cast<unsigned>(json.length()),
        static_cast<unsigned>(total), static_cast<unsigned>(m_mtu));
  return allOk;
}
