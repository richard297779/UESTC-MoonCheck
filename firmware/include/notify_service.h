#pragma once

// ============================================================================
//  NotifyService：手机通知（单向出口）
//
//  设计约束（用户决策 2026-10-14，方案讨论见 docs/notify-design.md）：
//    1. **通知到手机**：走 HTTP(S) 推送服务（Bark / ntfy / Server酱 / 钉钉 / 自建中转），
//       不依赖 PC 或手机 App 在线；
//    2. **每天 21:00 固定一条状态摘要**（由 21:00 的预登录体检触发，一天一条）；
//    3. **重试过程也要通知**：每一轮打卡尝试（成功/失败/部分成功）都推一条，自动编号 #N；
//    4. **多人版默认只发汇总**：不逐个账号刷屏；0.4.0 起可为每个账号关联
//       独立推送端点（notify.add），此时逐账号结果单独推送、汇总只进默认通道；
//    5. **只接收、不远程操作**：本模块**不监听端口、不解析入站指令**，
//       只有出站 HTTP 请求，且该 URL 由用户自己配置。
//
//  可靠性：
//    - 同一 `key` 在 `notifyGapSeconds()` 内去重（按通道独立计数，互不影响）；
//    - 每日配额 `notifyDailyQuota()`（0=不限，**每通道独立计数**），故障时不会刷爆对端限流；
//    - 发送失败进入待发队列（RAM 环形 8 条，所有通道共用），按 5 s / 30 s / 5 min / 10 min
//      退避，第 4 次仍失败则放弃；重启会丢失未发出的通知（P0 取舍，不做 NVS 队列）。
//
//  多通道（0.4.0 起）：默认通道（notify.set，索引 0）之外最多 5 条额外通道
//    （notify.add，索引 1..5）。事件按 NotifyTarget 分发：
//      All          → 默认通道 + 账号命中的额外通道（单人手动打卡等普通事件）
//      DefaultOnly  → 仅默认通道（多人「汇总」，避免与各账号单独推送重复刷屏）
//      AccountOnly  → 仅命中该账号的额外通道（逐账号事件，不打扰默认通道）
//    关联 "all" 的额外通道同时收到全局事件（account 为空的事件）。
//
//  线程约束：**仅在 net_task 中调用**（`post()` 与 `pump()` 都在命令队列处理路径上）。
//  失败隔离：通知的成败只记日志，**绝不改变打卡结论，也不写 `last`**。
// ============================================================================

#include <Arduino.h>
#include <functional>

#include "app_config.h"
#include "app_types.h"

class HttpClient;
class TimeService;

/// @brief 通知事件类型（决定标题文案与默认去重粒度）。
enum class NotifyKind : uint8_t {
  StatusDigest = 0,  ///< 21:00 固定状态摘要（每天一条）
  CheckinOk,         ///< 打卡成功（多人=整轮汇总，或逐账号）
  CheckinFail,       ///< 本轮打卡失败且不再重试（终态）
  CheckinRetry,      ///< 重试进度（保留：现行固件把重试写进 CheckinFail 汇总正文，不再单独发此 kind）
  DeadlineMissed,    ///< 窗口关闭仍未成功（终态告警）
  NeedMfa,           ///< 登录需要验证码，设备无法自行完成
  NetDown,           ///< 网络不可用（内部限流：每小时最多一条）
  Info,              ///< 手动 / 测试消息
};

/// @brief 事件分发目标（多通道 fan-out 规则）。
enum class NotifyTarget : uint8_t {
  All = 0,        ///< 默认通道 + 按账号规则命中的额外通道（普通事件）
  DefaultOnly,    ///< 仅默认通道（多人「汇总」）
  AccountOnly,    ///< 仅命中该账号的额外通道（逐账号事件）
};

/// @brief 一条待发送通知。
struct NotifyItem {
  NotifyKind kind = NotifyKind::Info;
  uint8_t channel = 0;         ///< 0=默认通道（notify.set）；1..5=额外通道（索引=channel-1）
  uint32_t createdAtSec = 0;   ///< 产生时的业务时间（未同步则为 0）
  uint32_t nextTryMs = 0;      ///< 下次尝试发送的 millis()（0=立即）
  uint8_t attempts = 0;        ///< 已尝试次数
  char key[32];                ///< 去重键（含 "cN|" 通道前缀）
  char title[64];              ///< 标题（含设备名，逐账号事件含账号）
  char body[256];              ///< 正文（不含凭据）
  bool used = false;
};

class NotifyService {
 public:
  /// @brief 通道槽位数：0=默认通道 + 最多 5 条额外通道。
  static constexpr uint8_t kChannelSlots = 6;

  /// @brief 绑定依赖（不拥有）。`http` 为共享的站点客户端。
  Result begin(AppConfig& config, HttpClient& http, TimeService& time);

  /// @brief 注入 BLE 事件出口（镜像一份，控制台在线时立即可见）。
  void setEventSink(std::function<void(const String& type, const String& payload)> sink) {
    m_sink = std::move(sink);
  }

  /// @brief 是否已启用（默认通道开启+URL，或存在任一可用的额外通道）。
  bool active() const;

  /// @brief 今日已发出的「打卡重试」通知条数。
  /// @note 供调用方分桶限流：Pending 每 180 s 一次，逐条发会淹没手机，
  ///       所以只在第 1、4、7… 次重试时上报（`retryCount() % 3 == 0`）。
  uint8_t retryCount() const { return m_retryCount; }

  /// @brief 记一次「打卡重试轮次」并返回编号（从 1 开始，跨天归零）。
  /// @note 调用方用它做分桶限流：`n = bumpAttempt(); if (n % 3 == 1) post(CheckinRetry, ...)`。
  ///       被限流挡掉的轮次同样计数，所以编号等于真实的第几轮。
  uint8_t bumpAttempt();

  /// @brief 上报一个事件：内部完成按通道 fan-out、去重、配额、渲染与入队。
  /// @param body    正文（纯文本，<=250 字节；超长截断）
  /// @param key     去重键；同键同通道在 notifyGapSeconds 内只发一次。空串=用事件名作键。
  /// @param account 事件关联的账号标签；空串=全局事件（只有默认通道与关联 "all" 的通道能收到）。
  /// @param target  分发目标，见 NotifyTarget。
  void post(NotifyKind kind, const String& body, const String& key = String(),
            const String& account = String(), NotifyTarget target = NotifyTarget::All);

  /// @brief 空闲时尝试发送一条（遵守退避时间）。挂在 `processPending()` 的每次唤醒上。
  void pump();

  /// @brief 立即同步发送（`notify test` / `notify chantest` 用），不入队、不去重。
  /// @param channel 0=默认通道；1..5=额外通道（索引=channel-1）。
  /// @return true 表示 HTTP 2xx/3xx。
  bool sendNow(NotifyKind kind, const String& body, int* outStatus, String& outError,
               uint8_t channel = 0, const String& account = String());

  /// @brief 通知状态（JSON 片段，URL 只输出脱敏形式，含 channels 数组）。
  String statusJson() const;

  /// @brief URL 是不是 Server酱（Turbo `sctapi.ftqq.com/<KEY>.send` 或 ³ `sc3.ft07.com`）。
  /// @note Server酱 的正文字段叫 **`desp`**，完全不认 `body`：用 `body` 发过去它照样返回
  ///       `code:0`（HTTP 200、发送成功），但手机上的正文是空的 —— 属于"通知到了却没内容"
  ///       这种最难查的坑，所以不带占位符模板时按 host 自动切字段名（实测 2026-10-18：
  ///       `{"title":..,"body":..}` 成功但空正文；`{"title":..,"desp":..}` 正文正常显示）。
  static bool isServerChan(const String& url);

  static const char* kindName(NotifyKind kind);
  static const char* kindTitle(NotifyKind kind);

 private:
  static constexpr uint8_t kQueueCapacity = 8;
  static constexpr uint8_t kDedupSlots = 6;

  struct DedupSlot {
    char key[32];
    uint32_t lastSec;
  };

  bool dedupAllow(const char* key, uint32_t nowSec);
  bool quotaAllow(uint8_t channel);
  void noteSent(uint8_t channel);
  uint8_t totalSentToday() const;
  void rollDay();

  /// @brief 通道 ch 是否接收这条事件（开关 + NotifyTarget + 账号匹配）。
  bool channelAccepts(uint8_t ch, const String& account, NotifyTarget target) const;

  /// @brief 按通道与 style 渲染出实际请求（method / url / body / Content-Type）。
  bool render(const char* title, const char* body, uint8_t channel, String& outUrl,
              String& outMethod, String& outBody, String& outContentType) const;

  /// @brief 组装并发送一条消息。
  bool deliver(const char* title, const char* body, uint8_t channel, int* outStatus,
               String& outError);

  AppConfig* m_config = nullptr;
  HttpClient* m_http = nullptr;
  TimeService* m_time = nullptr;
  std::function<void(const String&, const String&)> m_sink;

  NotifyItem m_queue[kQueueCapacity];
  uint8_t m_count = 0;

  DedupSlot m_dedup[kDedupSlots];
  char m_quotaDate[16] = "";      ///< 配额所属日期
  uint8_t m_sentTodayCh[kChannelSlots] = {0};  ///< 各通道今日已发送条数
  uint8_t m_retryCount = 0;       ///< 今日「打卡重试」编号（自动递增）
};
