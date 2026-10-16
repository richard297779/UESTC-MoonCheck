// ============================================================================
//  MultiCheckin 实现
// ============================================================================

#include "multi_checkin.h"

#include <ArduinoJson.h>

#include <vector>

#include "logger.h"

namespace {

const char* const kTag = "multi";

}  // namespace

Result MultiCheckin::begin(AppConfig& config, AccountStore& accounts, CookieJar& cookies,
                           CheckinService& checkin, TimeService& time) {
  m_config = &config;
  m_accounts = &accounts;
  m_cookies = &cookies;
  m_checkin = &checkin;
  m_time = &time;
  LOG_I(kTag, "account runner ready (accounts=%u)", static_cast<unsigned>(accounts.count()));
  return Result::Ok;
}

void MultiCheckin::emit(const String& type, const String& payloadJson) {
  if (m_sink) m_sink(type, payloadJson);
}

MultiCheckin::RunResult MultiCheckin::runAll(bool force, bool probeOnly) {
  RunResult result;
  result.summary = "[]";
  if (m_config == nullptr || m_accounts == nullptr || m_checkin == nullptr) return result;
  if (m_running) {
    LOG_W(kTag, "already running, skip");
    return result;
  }

  const String today = (m_time != nullptr) ? m_time->todayString() : String("");

  // ---- 取本轮要跑的账号：表非空用表；表为空退回设备自身配置（= 列表第一个）----
  std::vector<CheckinProfile> queue;
  const bool fromStore = m_accounts->count() > 0;
  if (fromStore) {
    result.total = m_accounts->count();
    for (size_t index = 0; index < result.total; index++) {
      CheckinProfile profile;
      if (m_accounts->get(index, profile)) queue.push_back(profile);
    }
  } else {
    result.total = 1;
    queue.push_back(m_config->deviceProfile());
    LOG_I(kTag, "account table empty -> using device profile \"%s\"", queue[0].label.c_str());
  }

  // ---- 禁用项：不登录、不提交、不进分母 ----
  std::vector<CheckinProfile> active;
  for (const CheckinProfile& profile : queue) {
    if (profile.enabled) {
      active.push_back(profile);
    } else {
      result.disabledCount++;
    }
  }
  result.enabledTotal = active.size();

  LOG_I(kTag, "run start: %u accounts (%u enabled, %u off), force=%d probeOnly=%d",
        static_cast<unsigned>(result.total), static_cast<unsigned>(result.enabledTotal),
        static_cast<unsigned>(result.disabledCount), force ? 1 : 0, probeOnly ? 1 : 0);

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (const CheckinProfile& profile : queue) {
    if (profile.enabled) continue;
    JsonObject item = array.add<JsonObject>();
    item["label"] = profile.label;
    item["enabled"] = false;
    item["result"] = "disabled";
    item["ok"] = false;
    item["msg"] = "该账号已单独关闭，本轮跳过";
    LOG_I(kTag, "%s: disabled, skip", profile.label.c_str());
  }

  m_running = true;
  size_t processed = 0;

  for (const CheckinProfile& profile : active) {
    processed++;
    // 注意：payload 以 "of":N 结尾，**不要再补引号**（否则整条事件是非法 JSON）
    emit("progress", String("\"stage\":\"multi\",\"account\":\"") + profile.label +
                         "\",\"index\":" + String(static_cast<unsigned>(processed)) +
                         ",\"of\":" + String(static_cast<unsigned>(result.enabledTotal)));

    // ---- 幂等：该账号今天已确认成功 → 跳过（不浪费登录次数）----
    // 表为空时没有 per-account 记录，退回设备级 last 结果（同一天同源）。
    const bool alreadyDone =
        !force && !probeOnly &&
        (fromStore ? m_accounts->isCheckedInToday(profile.label, today)
                   : m_config->isLastResultOkFor(today));
    if (alreadyDone) {
      result.okCount++;
      JsonObject item = array.add<JsonObject>();
      item["label"] = profile.label;
      item["enabled"] = true;
      item["result"] = "already_done";
      item["ok"] = true;
      item["msg"] = "today already confirmed";
      emit("result", String("\"account\":\"") + profile.label + "\",\"ok\":true,\"skipped\":true,"
                         "\"msg\":\"already checked in today\"");
      LOG_I(kTag, "%s: already done today, skip", profile.label.c_str());
      continue;
    }

    // ---- 换人：清会话 + 清登录冷却 + 套用该账号到内存配置 ----
    if (m_cookies != nullptr) m_cookies->clear();
    m_checkin->resetLoginState();
    m_config->applyProfile(profile);

    String message;
    CheckinOutcome outcome = CheckinOutcome::Failed;
    if (probeOnly) {
      // ★ probe() 是"只读体检"，**不会登录**；演练必须先建立会话，否则刚清完 Cookie
      //   的匿名会话必然被退回首页，报出误导性的"warm-up did not help"。
      bool didLogin = false;
      String loginError;
      const Result session = m_checkin->ensureSession(didLogin, loginError, force);
      if (session == Result::NeedMfa) {
        outcome = CheckinOutcome::NeedMfa;
        message = "server requires second factor (该账号没有可信指纹，需补 bfp)";
      } else if (session != Result::Ok) {
        outcome = CheckinOutcome::Pending;
        message = loginError + (force ? String("") : String(" | 窗口外演练请用 probe force"));
      } else {
        ProbeReport report;
        String error;
        const Result probeResult = m_checkin->probe(report, error);
        if (probeResult != Result::Ok) {
          outcome = CheckinOutcome::Failed;
          message = error;
        } else if (report.sessionValid) {
          outcome = CheckinOutcome::Success;
          message = String("session ok") + (didLogin ? " (new login)" : " (reused)") +
                    ", today=" + report.today.recordStatusCode + ", window=" + report.windowText +
                    (report.inWindow ? " OPEN" : " CLOSED");
        } else {
          outcome = CheckinOutcome::Pending;
          message = error;
        }
      }
    } else {
      outcome = m_checkin->perform(force, message);
    }

    const bool ok = (outcome == CheckinOutcome::Success) || (outcome == CheckinOutcome::AlreadyDone);
    if (ok) result.okCount++;

    if (!probeOnly && (ok || outcome == CheckinOutcome::Failed)) {
      if (fromStore) m_accounts->setResult(profile.label, today, ok, message);
    }

    JsonObject item = array.add<JsonObject>();
    item["label"] = profile.label;
    item["enabled"] = true;
    item["result"] = CheckinService::outcomeName(outcome);
    item["ok"] = ok;
    item["msg"] = message;

    emit("result", String("\"account\":\"") + profile.label + "\",\"ok\":" +
                       String(ok ? "true" : "false") + ",\"result\":\"" +
                       CheckinService::outcomeName(outcome) + "\",\"msg\":\"" + message + "\"");
    LOG_I(kTag, "%s -> %s (%s)", profile.label.c_str(), CheckinService::outcomeName(outcome),
          message.c_str());

    // ---- 错峰：账号之间留间隔，避免同一秒并发的机器特征 ----
    if (processed < result.enabledTotal) {
      const uint16_t gap = m_config->multiGapSeconds();
      if (gap > 0) {
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(gap) * 1000UL));
      }
    }
  }

  serializeJson(array, result.summary);
  m_running = false;

  // 收尾：恢复设备级配置；再把**第一个生效账号**套进内存，
  // 这样 status / probe 显示的就是"列表第一个用户"，而不是残留的最后一个账号。
  m_config->reload();
  if (!active.empty()) m_config->applyProfile(active[0]);

  LOG_I(kTag, "run done: %u/%u ok (%u disabled)", static_cast<unsigned>(result.okCount),
        static_cast<unsigned>(result.enabledTotal), static_cast<unsigned>(result.disabledCount));
  return result;
}
