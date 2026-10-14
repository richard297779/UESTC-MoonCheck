// ============================================================================
//  MultiCheckin 实现
// ============================================================================

#include "multi_checkin.h"

#include <ArduinoJson.h>

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
  LOG_I(kTag, "multi-user orchestrator ready (accounts=%u)",
        static_cast<unsigned>(accounts.count()));
  return Result::Ok;
}

void MultiCheckin::emit(const String& type, const String& payloadJson) {
  if (m_sink) m_sink(type, payloadJson);
}

size_t MultiCheckin::runAll(bool force, bool probeOnly, String& summaryJson) {
  summaryJson = "[]";
  if (m_config == nullptr || m_accounts == nullptr || m_checkin == nullptr) return 0;
  if (m_running) {
    LOG_W(kTag, "already running, skip");
    return 0;
  }

  const size_t total = m_accounts->count();
  if (total == 0) {
    LOG_W(kTag, "no accounts configured");
    emit("alert", "\"kind\":\"multi\",\"msg\":\"no accounts; use account.add first\"");
    return 0;
  }

  m_running = true;
  const String today = (m_time != nullptr) ? m_time->todayString() : String("");
  LOG_I(kTag, "run start: %u accounts, force=%d probeOnly=%d", static_cast<unsigned>(total),
        force ? 1 : 0, probeOnly ? 1 : 0);

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  size_t okCount = 0;

  for (size_t index = 0; index < total; index++) {
    CheckinProfile profile;
    if (!m_accounts->get(index, profile)) continue;

    // 注意：payload 以 "of":N 结尾，**不要再补引号**（否则整条事件是非法 JSON）
    emit("progress", String("\"stage\":\"multi\",\"account\":\"") + profile.label +
                         "\",\"index\":" + String(static_cast<unsigned>(index + 1)) +
                         ",\"of\":" + String(static_cast<unsigned>(total)));

    // ---- 幂等：该账号今天已确认成功 → 跳过（不浪费登录次数）----
    if (!force && !probeOnly && m_accounts->isCheckedInToday(profile.label, today)) {
      okCount++;
      JsonObject item = array.add<JsonObject>();
      item["label"] = profile.label;
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
      // ★ probe() 是"只读体检"，**不会登录**；演练必须先建立会话
      //   （与单人版调度器的 Probe 分支同样的处理）。否则刚清完 Cookie 的匿名会话
      //   必然被平台退回首页，报出误导性的"warm-up did not help"。
      bool didLogin = false;
      String loginError;
      const Result session = m_checkin->ensureSession(didLogin, loginError, force);
      if (session == Result::NeedMfa) {
        outcome = CheckinOutcome::NeedMfa;
        message = "server requires second factor (该账号没有可信指纹，需补 bfp)";
      } else if (session != Result::Ok) {
        outcome = CheckinOutcome::Pending;
        message = loginError + (force ? String("") : String(" | 窗口外演练请用 multi probe force"));
      } else {
        ProbeReport report;
        String error;
        const Result result = m_checkin->probe(report, error);
        if (result != Result::Ok) {
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
    if (ok) okCount++;

    if (!probeOnly && (ok || outcome == CheckinOutcome::Failed)) {
      m_accounts->setResult(profile.label, today, ok, message);
    }

    JsonObject item = array.add<JsonObject>();
    item["label"] = profile.label;
    item["result"] = CheckinService::outcomeName(outcome);
    item["ok"] = ok;
    item["msg"] = message;

    emit("result", String("\"account\":\"") + profile.label + "\",\"ok\":" +
                       String(ok ? "true" : "false") + ",\"result\":\"" +
                       CheckinService::outcomeName(outcome) + "\",\"msg\":\"" + message + "\"");
    LOG_I(kTag, "%s -> %s (%s)", profile.label.c_str(), CheckinService::outcomeName(outcome),
          message.c_str());

    // ---- 错峰：账号之间留间隔，避免同一秒并发的机器特征 ----
    if (index + 1 < total) {
      const uint16_t gap = m_config->multiGapSeconds();
      if (gap > 0) {
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(gap) * 1000UL));
      }
    }
  }

  serializeJson(array, summaryJson);
  m_running = false;

  // 恢复成"这台设备自己保存的账号"，避免 status/hello 显示成最后一个账号
  m_config->reload();

  LOG_I(kTag, "run done: %u/%u ok", static_cast<unsigned>(okCount),
        static_cast<unsigned>(total));
  return okCount;
}
