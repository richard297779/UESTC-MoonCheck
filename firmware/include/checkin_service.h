#pragma once

// ============================================================================
//  CheckinService：打卡业务
//    - probe()：**前置体检**，非打卡时间也能跑（会话/状态/时段/打卡点）
//    - ensureSession()：会话失效时自动登录
//    - perform()：幂等打卡 + 结果复查
//  线程约束：仅在 net_task 中调用。
// ============================================================================

#include <Arduino.h>

#include <functional>

#include "app_config.h"
#include "app_types.h"
#include "cas_auth.h"
#include "checkin_api.h"
#include "time_service.h"

/// @brief 前置体检结果。
struct ProbeReport {
  bool sessionValid = false;        ///< 会话是否**可用于业务接口**（能读到今日状态）
  bool appAuthorized = false;       ///< 微应用是否放行该会话（是否真的落地到 H5 页）
  bool h5Reached = false;           ///< 与 appAuthorized 同义（保留给既有调用方）
  bool h5PageReached = false;       ///< 打卡界面（静态 H5 页）本身是否可打开：与授权无关，恒应为 true
  bool loginAllowed = false;        ///< 当前是否处于"登录才有意义"的时段（窗口外登录不被授权）
  bool inWindow = false;            ///< 当前是否在打卡时段内
  bool windowFromPlatform = false;  ///< 时段是否来自平台（否则用本地配置）
  String deviceTime;                ///< 设备本地时间
  String windowText;                ///< 形如 "21:00 - 23:30"
  int windowStartMinutes = 0;
  int windowEndMinutes = 0;
  TodayStatus today;                ///< 今日状态
  ScheduleInfo schedule;            ///< 平台时段
  LocationInfo location;            ///< 打卡点（取第一个）
  uint32_t elapsedMs = 0;
};

/// @brief 打卡结果。
enum class CheckinOutcome : uint8_t {
  Skipped = 0,
  Success,
  Failed,
  OutOfWindow,
  NeedMfa,
  AlreadyDone,
  /// 会话已建立但微应用尚未放行（实测：登录成功后平台需要一段时间才生效）→ 稍后重试
  Pending,
};

class CheckinService {
 public:
  /// @brief 注入依赖。
  Result begin(AppConfig& config, CasAuth& auth, CheckinApi& api, TimeService& time);

  /// @brief 前置体检（只读，随时可跑）。
  /// @return Result::Ok 表示体检已完成（会话是否有效见 report.sessionValid）。
  Result probe(ProbeReport& out, String& outError);

  /// @brief 确保存在可用会话（必要时执行登录）。
  /// @param[out] outDidLogin 是否真的执行了登录。
  /// @param force true=即使不在打卡时段也登录（仅用于链路自测；这样建立的会话不会被微应用授权）。
  /// @return Result::AppUnavailable 表示"需要登录，但当前不在打卡时段，已推迟到窗口内"。
  Result ensureSession(bool& outDidLogin, String& outError, bool force = false);

  /// @brief 当前本地时间是否处于"允许发起登录"的时段（严格等于平台打卡窗口）。
  bool isLoginWindowNow() const;

  /// @brief 执行打卡。
  /// @param force true=跳过"今日已打卡"检查（仍要求处于打卡时段内）。
  CheckinOutcome perform(bool force, String& outMessage);

  /// @brief 微信操作票据供给回调：提交前没有票据时调用（每账号独立凭据）。
  /// @return Result::Ok 且 outTicket 非空 = 成功；否则打卡失败并带 outError。
  using WxTicketProvider = std::function<Result(String& outTicket, String& outError)>;
  void setWxTicketProvider(WxTicketProvider provider) { m_wxTicket = std::move(provider); }

  /// @brief 重置"登录冷却"状态。
  /// @note 多人顺序打卡**必须**在切换账号时调用：冷却是按"上一次登录"计算的，
  ///       不清零会导致第 2 个账号之后 30 分钟内无法登录。
  void resetLoginState();

  /// @brief 距最近一次成功登录过了多久（毫秒）；本次开机内没登录过返回 0xFFFFFFFF。
  /// @note 只给通知摘要用（判断"到 22:00 会话是否还在 TTL 内"），不参与任何业务判定。
  uint32_t lastLoginAgeMs() const {
    return m_lastLoginDone ? static_cast<uint32_t>(millis() - m_lastLoginMs) : 0xFFFFFFFFu;
  }

  /// @brief 结果码转字符串。
  static const char* outcomeName(CheckinOutcome outcome);

 private:
  WxTicketProvider m_wxTicket;
  AppConfig* m_config = nullptr;
  CasAuth* m_auth = nullptr;
  CheckinApi* m_api = nullptr;
  TimeService* m_time = nullptr;
  ScheduleInfo m_lastSchedule;    ///< 最近一次读到的平台时段（跨调用缓存）
  bool m_scheduleKnown = false;
  uint32_t m_lastLoginMs = 0;     ///< 最近一次登录成功的时刻（millis）
  bool m_lastLoginDone = false;   ///< 本次开机内是否成功登录过（用于冷却判断）
};
