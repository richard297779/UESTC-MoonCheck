// ============================================================================
//  CheckinService 实现
// ============================================================================

#include "checkin_service.h"

#include <ArduinoJson.h>

#include "logger.h"

namespace {

const char* const kTag = "checkin_service";

}  // namespace

Result CheckinService::begin(AppConfig& config, CasAuth& auth, CheckinApi& api,
                             TimeService& time) {
  m_config = &config;
  m_auth = &auth;
  m_api = &api;
  m_time = &time;
  LOG_I(kTag, "checkin service ready");
  return Result::Ok;
}

const char* CheckinService::outcomeName(CheckinOutcome outcome) {
  switch (outcome) {
    case CheckinOutcome::Skipped: return "skipped";
    case CheckinOutcome::Success: return "success";
    case CheckinOutcome::Failed: return "failed";
    case CheckinOutcome::OutOfWindow: return "out_of_window";
    case CheckinOutcome::NeedMfa: return "need_mfa";
    case CheckinOutcome::AlreadyDone: return "already_done";
    case CheckinOutcome::Pending: return "pending";
  }
  return "unknown";
}

void CheckinService::resetLoginState() {
  // 多人模式切换账号时必须清零：否则第二个账号会被 30 分钟登录冷却挡住。
  m_lastLoginDone = false;
  m_lastLoginMs = 0;
}

Result CheckinService::probe(ProbeReport& out, String& outError) {
  if (m_api == nullptr || m_config == nullptr || m_time == nullptr) return Result::NotReady;

  const uint32_t startMs = millis();
  out = ProbeReport();
  out.deviceTime = m_time->nowString();
  // 窗口起点优先用平台时段，其次用本地配置的"窗口起点"（默认 21:00，不再误用打卡目标时间）
  out.windowStartMinutes = m_scheduleKnown ? m_lastSchedule.checkinStartMinutes
                                           : m_config->schedule().windowStartMinutes;
  out.windowEndMinutes = m_scheduleKnown ? m_lastSchedule.checkinEndMinutes
                                         : m_config->schedule().windowEndMinutes;

  const auto finishReport = [&]() {
    out.windowText = TimeService::formatMinutes(out.windowStartMinutes) + " - " +
                     TimeService::formatMinutes(out.windowEndMinutes);
    out.inWindow = m_time->inWindow(out.windowStartMinutes, out.windowEndMinutes);
    out.loginAllowed = isLoginWindowNow();
    out.elapsedMs = millis() - startMs;
  };

  // ---- EP05 进入微应用：这是"该会话是否被授权"的唯一可靠判据 ----
  bool h5Reached = false;
  Result enterResult = m_api->enterApp(&h5Reached);

  // ★ 自愈：实测（2026-10-13）会话若"登录后没做门户预热"，平台会一律拒绝微应用。
  //   补一次预热再探 —— 顺带让外部注入的会话（session.set）也能立刻可用。
  if (!h5Reached) {
    int warmStatus = 0;
    if (m_api->warmUpPortal(&warmStatus) == Result::Ok) {
      enterResult = m_api->enterApp(&h5Reached);
    }
  }
  out.h5Reached = h5Reached;
  out.appAuthorized = h5Reached;

  if (!h5Reached) {
    // 顺手确认"界面本身能不能打开"：H5 静态页恒为 200（浏览器任意时间都能进界面就是这个原因），
    // 据此把两种情况区分开：
    //   ① h5Page=true ：界面能进、只是平台未授权该会话打卡（会话不是登录态 / 预热也救不回来）；
    //   ② h5Page=false：界面也打不开 → 网络或平台故障，另当别论。
    bool h5PageReached = false;
    m_api->fetchH5Page(&h5PageReached);
    out.h5PageReached = h5PageReached;

    // ★ 这里**故意不再调用业务接口**：那只会拿到误导性的 4001「登录无效或已过期」，
    //   而且服务端会顺手下发一个短命 SESSION 覆盖设备里保存的会话。
    out.sessionValid = false;
    finishReport();
    outError = h5PageReached
                   ? "h5 page reachable, but micro app not authorized for this session "
                     "(not a logged-in session; portal warm-up did not help)"
                   : "h5 page unreachable (network or platform problem)";
    LOG_W(kTag, "probe: app NOT authorized (enter=%s, h5Page=%d) -> skip business API",
          resultName(enterResult), h5PageReached ? 1 : 0);
    return Result::Ok;  // 体检本身完成，原因见 appAuthorized=false
  }

  // ---- EP06 今日状态 ----
  const Result todayResult = m_api->fetchToday(out.today);
  if (todayResult != Result::Ok) {
    out.sessionValid = false;
    finishReport();
    outError = String("today failed: ") + resultName(todayResult);
    LOG_W(kTag, "probe: today failed (%s)", resultName(todayResult));
    return Result::Ok;
  }
  out.sessionValid = true;

  // ---- EP07 平台时段（读到就缓存，后续判断窗口不再依赖本地配置）----
  ScheduleInfo schedule;
  if (m_api->fetchSchedule(schedule) == Result::Ok) {
    out.schedule = schedule;
    out.windowStartMinutes = schedule.checkinStartMinutes;
    out.windowEndMinutes = schedule.checkinEndMinutes;
    out.windowFromPlatform = true;
    m_lastSchedule = schedule;
    m_scheduleKnown = true;
  }

  // ---- EP08 打卡点（取第一个）----
  std::vector<LocationInfo> locations;
  if (m_api->fetchLocations(locations) == Result::Ok && !locations.empty()) {
    out.location = locations.front();
  }

  finishReport();
  LOG_I(kTag, "probe done: status=%s window=%s inWindow=%d loginAllowed=%d (%u ms)",
        out.today.recordStatusCode.c_str(), out.windowText.c_str(), out.inWindow ? 1 : 0,
        out.loginAllowed ? 1 : 0, static_cast<unsigned>(out.elapsedMs));
  return Result::Ok;
}

bool CheckinService::isLoginWindowNow() const {
  if (m_time == nullptr || !m_time->isSynced()) return false;

  int start = 0;
  int end = 0;
  if (m_scheduleKnown) {
    start = m_lastSchedule.checkinStartMinutes;
    end = m_lastSchedule.checkinEndMinutes;
  } else if (m_config != nullptr) {
    start = m_config->schedule().windowStartMinutes;
    end = m_config->schedule().windowEndMinutes;
  } else {
    return false;
  }
  if (end <= start) return false;

  // ★ 登录时段**严格等于**平台打卡窗口，不提前。
  //   真实原因（docs/api-contract.md §8.7 定案）：微应用授权是会话属性，靠门户预热解锁，
  //   与时段**无关**；但 SESSION TTL 实测约 1 小时、且打卡提交只接受窗口内，
  //   所以窗口外登录只是白白消耗一次登录次数，到点仍需重新登录。
  //   （曾据此写"平台按时段授权"，2026-10-13 的门户预热二分实验已推翻该推测。）
  return m_time->inWindow(start, end);
}

Result CheckinService::ensureSession(bool& outDidLogin, String& outError, bool force) {
  outDidLogin = false;
  if (m_api == nullptr || m_auth == nullptr) return Result::NotReady;

  // 先用"能否进入微应用"判断会话是否真的有业务权限（比业务接口更准，且不会污染已保存的会话）
  bool h5Reached = false;
  m_api->enterApp(&h5Reached);
  if (!h5Reached) {
    // 同一个会话可能只是"没做过门户预热"，先补一次再判定（避免白白重新登录）
    int warmStatus = 0;
    if (m_api->warmUpPortal(&warmStatus) == Result::Ok) {
      m_api->enterApp(&h5Reached);
    }
  }
  if (h5Reached) {
    TodayStatus probeStatus;
    if (m_api->fetchToday(probeStatus) == Result::Ok) {
      outError = "";
      return Result::Ok;
    }
  }

  // ★ 登录时机仍限制在平台窗口内（21:00–23:30）：不是因为"窗口外登录不被授权"
  //   （那个旧推测已被 2026-10-13 的门户预热实验推翻），而是因为
  //   ① 会话 TTL 只有约 1 小时，提前登录会白白过期；② 打卡本身只在窗口内被接受。
  if (!force && !isLoginWindowNow()) {
    outError = "login deferred: outside the check-in window "
               "(session TTL is ~1 h and check-in is only accepted inside the window)";
    LOG_W(kTag, "login deferred: outside window (force=%d)", force ? 1 : 0);
    return Result::AppUnavailable;
  }

  // 冷却：刚登录过但仍未放行时，短时间内重复登录没有意义（只会白白消耗登录次数）。
  // 注：过去把它归因于"平台生效延迟"，实测真正原因是**没做门户预热**；预热已在登录流程内补上。
  if (!force && m_lastLoginDone &&
      (millis() - m_lastLoginMs) < AppConst::kLoginCooldownMs) {
    const uint32_t remaining = AppConst::kLoginCooldownMs - (millis() - m_lastLoginMs);
    outError = "login skipped: previous login not yet effective (cooldown "
               + String(remaining / 1000) + " s)";
    LOG_W(kTag, "login skipped: cooldown %u s remaining", static_cast<unsigned>(remaining / 1000));
    return Result::AppUnavailable;
  }

  LOG_I(kTag, "performing login (force=%d, inWindow=%d)", force ? 1 : 0, isLoginWindowNow() ? 1 : 0);
  const Result loginResult = m_auth->login(outError);
  if (loginResult != Result::Ok) {
    if (loginResult == Result::NeedMfa) {
      LOG_W(kTag, "login requires second factor");
    }
    return loginResult;
  }
  m_lastLoginDone = true;
  m_lastLoginMs = millis();
  outDidLogin = true;
  outError = "";

  // ★ 登录后必须做一次"门户预热"，否则微应用不认这个会话（实测 2026-10-13：
  //   没预热时 checkLogin 一律 302 退回平台首页、业务接口回 4001；预热后同一会话立刻可用）。
  int warmStatus = 0;
  if (m_api->warmUpPortal(&warmStatus) != Result::Ok) {
    LOG_W(kTag, "portal warm-up after login failed (http %d) — 微应用可能仍不认该会话", warmStatus);
  }
  return Result::Ok;
}

CheckinOutcome CheckinService::perform(bool force, String& outMessage) {
  if (m_api == nullptr || m_config == nullptr || m_time == nullptr) {
    outMessage = "service not ready";
    return CheckinOutcome::Failed;
  }

  ProbeReport report;
  String error;
  Result result = probe(report, error);
  if (result != Result::Ok) {
    outMessage = error;
    return CheckinOutcome::Failed;
  }

  // ---- 会话/权限不足则尝试建立 ----
  if (!report.sessionValid) {
    bool didLogin = false;
    result = ensureSession(didLogin, error);
    if (result == Result::NeedMfa) {
      outMessage = "server requires second factor (fingerprint not trusted)";
      return CheckinOutcome::NeedMfa;
    }
    if (result == Result::AppUnavailable) {
      // 两类情况都归为"稍后重试"：①窗口外登录无效；②刚登录过、平台尚未放行
      outMessage = error;
      return CheckinOutcome::Pending;
    }
    if (result != Result::Ok) {
      outMessage = error;
      return CheckinOutcome::Failed;
    }
    result = probe(report, error);
    if (result != Result::Ok) {
      outMessage = error;
      return CheckinOutcome::Failed;
    }
    if (!report.sessionValid) {
      // 登录成功但微应用仍未放行：实测平台需要数分钟生效 → 交给调度器稍后重试，不算终态失败
      outMessage = "session established but micro app not authorized yet "
                   "(platform activation delay)";
      return CheckinOutcome::Pending;
    }
  }

  // ---- 幂等：今日已打卡 ----
  if (report.today.isCheckedIn() && !force) {
    outMessage = "already checked in at " + report.today.checkinTime;
    return CheckinOutcome::AlreadyDone;
  }

  // ---- 时间窗保护（force 也不越过）----
  if (!report.inWindow) {
    outMessage = "now " + report.deviceTime + ", window " + report.windowText;
    return CheckinOutcome::OutOfWindow;
  }

  // ---- EP10 提交 ----
  const LocationConfig& location = m_config->location();
  String address = location.address;
  String refreshed;
  if (m_api->reverseGeocode(location.latitude, location.longitude, refreshed) == Result::Ok) {
    address = refreshed;  // 逆地理编码失败不影响打卡，沿用配置地址
  }

  const String clientTime = m_time->nowString("%Y-%m-%d %H:%M:%S");
  HttpResponse raw;
  result = m_api->submitCheckin(location.latitude, location.longitude, address, clientTime, raw);
  if (result != Result::Ok) {
    outMessage = String("submit failed: ") + resultName(result);
    return CheckinOutcome::Failed;
  }

  // ---- 解析业务响应 ----
  JsonDocument doc;
  if (deserializeJson(doc, raw.body)) {
    outMessage = "submit response not json";
    return CheckinOutcome::Failed;
  }
  const int code = doc["code"] | -1;
  const String message = doc["msg"] | "";
  if (code != 0) {
    outMessage = "business code " + String(code) + ": " + message;
    LOG_W(kTag, "checkin rejected: %s", outMessage.c_str());
    return CheckinOutcome::Failed;
  }

  // ---- 复查 ----
  TodayStatus verify;
  if (m_api->fetchToday(verify) == Result::Ok && verify.isCheckedIn()) {
    if (verify.abnormalFlag) {
      outMessage = "checked in but flagged abnormal";
      LOG_W(kTag, "abnormalFlag=true, please check manually");
      return CheckinOutcome::Success;
    }
    outMessage = "checked in at " + verify.checkinTime +
                 " (distance " + String(verify.distanceMeter, 1) + " m)";
    LOG_I(kTag, "checkin verified: %s", outMessage.c_str());
    return CheckinOutcome::Success;
  }

  outMessage = "submitted but not verified yet";
  LOG_W(kTag, "%s", outMessage.c_str());
  return CheckinOutcome::Failed;
}
