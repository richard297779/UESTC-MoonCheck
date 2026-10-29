// ============================================================================
//  AccountStore 实现
// ============================================================================

#include "account_store.h"

#include <ArduinoJson.h>
#include <esp_system.h>

#include "logger.h"

namespace {

const char* const kTag = "account_store";

/// @brief NVS 键（整张表存一个 JSON 数组）。
constexpr const char* kNvsProfiles = "profiles";

/// @brief 标签校验：只允许 A-Z a-z 0-9 - _ ，最长 12 字符。
bool labelOk(const String& text) {
  if (text.length() == 0 || text.length() > 12) return false;
  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

/// @brief 学号打码：只保留前 4 位。
String maskUser(const String& studentId) {
  if (studentId.length() <= 4) return studentId;
  return studentId.substring(0, 4) + "******";
}

}  // namespace

Result AccountStore::begin(const char* nvsNamespace) {
  if (m_mutex == nullptr) {
    m_mutex = xSemaphoreCreateMutex();
  }
  if (!m_prefs.begin(nvsNamespace, false)) {
    LOG_E(kTag, "NVS open failed: %s", nvsNamespace);
    return Result::StorageError;
  }
  m_opened = true;
  m_revision = esp_random();

  m_list.clear();
  if (m_prefs.isKey(kNvsProfiles)) {
    const String serialized = m_prefs.getString(kNvsProfiles, "");
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, serialized);
    if (error) {
      LOG_E(kTag, "profiles parse failed: %s", error.c_str());
      return Result::StorageError;
    }
    for (JsonObject item : doc.as<JsonArray>()) {
      CheckinProfile profile;
      profile.label = item["l"] | "";
      // 缺 "en" 视为启用：兼容旧版本（无此字段）写下的账号表
      profile.enabled = item["en"] | true;
      profile.studentId = item["u"] | "";
      profile.password = item["p"] | "";
      profile.fingerprint = item["f"] | "";
      profile.latitude = item["lat"] | 0.0;
      profile.longitude = item["lng"] | 0.0;
      profile.address = item["a"] | "";
      profile.lastDate = item["d"] | "";
      profile.lastOk = item["ok"] | false;
      profile.lastMsg = item["m"] | "";
      if (profile.label.length() > 0 && profile.studentId.length() > 0) {
        m_list.push_back(profile);
      }
    }
  }
  LOG_I(kTag, "accounts loaded: %u", static_cast<unsigned>(m_list.size()));
  return Result::Ok;
}

Result AccountStore::persist() {
  if (!m_opened) return Result::NotReady;
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (const CheckinProfile& profile : m_list) {
    JsonObject item = array.add<JsonObject>();
    item["l"] = profile.label;
    item["en"] = profile.enabled;
    item["u"] = profile.studentId;
    item["p"] = profile.password;
    item["f"] = profile.fingerprint;
    item["lat"] = profile.latitude;
    item["lng"] = profile.longitude;
    item["a"] = profile.address;
    item["d"] = profile.lastDate;
    item["ok"] = profile.lastOk;
    item["m"] = profile.lastMsg;
  }
  String serialized;
  serializeJson(doc, serialized);
  if (m_prefs.putString(kNvsProfiles, serialized) == 0) {
    LOG_E(kTag, "persist failed (%u bytes)", static_cast<unsigned>(serialized.length()));
    return Result::StorageError;
  }
  return Result::Ok;
}

size_t AccountStore::count() const { return m_list.size(); }

bool AccountStore::get(size_t index, CheckinProfile& out) const {
  if (index >= m_list.size()) return false;
  out = m_list[index];
  return true;
}

int AccountStore::indexOfLabel(const String& label) const {
  for (size_t i = 0; i < m_list.size(); i++) {
    if (m_list[i].label == label) return static_cast<int>(i);
  }
  return -1;
}

Result AccountStore::addOrUpdate(const CheckinProfile& profile) {
  if (!labelOk(profile.label)) {
    LOG_W(kTag, "invalid label (A-Z a-z 0-9 - _, 1..12 chars)");
    return Result::InvalidParam;
  }
  if (profile.studentId.length() == 0 || profile.password.length() == 0) {
    LOG_W(kTag, "studentId/password required");
    return Result::InvalidParam;
  }

  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const int existing = indexOfLabel(profile.label);
  const auto previous = m_list;
  Result result = Result::Ok;
  if (existing >= 0) {
    // 覆盖：保留已有的打卡结果，避免改密码后误判"今天没打过"
    CheckinProfile& target = m_list[static_cast<size_t>(existing)];
    const bool sameIdentity=target.studentId==profile.studentId;
    const String lastDate = sameIdentity?target.lastDate:String();
    const bool lastOk = sameIdentity&&target.lastOk;
    const String lastMsg = sameIdentity?target.lastMsg:String();
    target = profile;
    target.lastDate = lastDate;
    target.lastOk = lastOk;
    target.lastMsg = lastMsg;
  } else {
    if (m_list.size() >= kMaxProfiles) {
      result = Result::InvalidParam;
    } else {
      m_list.push_back(profile);
    }
  }
  if (result == Result::Ok) result = persist();
  if (result == Result::Ok) ++m_revision; else m_list = previous;
  if (m_mutex) xSemaphoreGive(m_mutex);

  if (result == Result::Ok) {
    LOG_I(kTag, "account saved: %s (%s), total=%u", profile.label.c_str(),
          maskUser(profile.studentId).c_str(), static_cast<unsigned>(m_list.size()));
  }
  return result;
}

Result AccountStore::setEnabled(const String& label, bool enabled) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const int index = indexOfLabel(label);
  if (index < 0) {
    if (m_mutex) xSemaphoreGive(m_mutex);
    return Result::NotFound;
  }
  const bool previous = m_list[static_cast<size_t>(index)].enabled;
  m_list[static_cast<size_t>(index)].enabled = enabled;
  const Result result = persist();
  if (result == Result::Ok) ++m_revision; else m_list[static_cast<size_t>(index)].enabled = previous;
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "account %s -> %s, enabled=%u/%u", label.c_str(), enabled ? "ON" : "OFF",
        static_cast<unsigned>(enabledCount()), static_cast<unsigned>(m_list.size()));
  return result;
}

size_t AccountStore::enabledCount() const {
  size_t n = 0;
  for (const CheckinProfile& profile : m_list) {
    if (profile.enabled) n++;
  }
  return n;
}

Result AccountStore::remove(const String& label) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const int index = indexOfLabel(label);
  const auto previous = m_list;
  Result result = Result::NotFound;
  if (index >= 0) {
    m_list.erase(m_list.begin() + index);
    result = persist();
    if (result == Result::Ok) ++m_revision; else m_list = previous;
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  if (index >= 0) LOG_W(kTag, "account removed: %s", label.c_str());
  return result;
}

Result AccountStore::clearAll() {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const auto previous = m_list;
  m_list.clear();
  Result result = persist();
  if (result == Result::Ok) ++m_revision; else m_list = previous;
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_W(kTag, "all accounts cleared");
  return result;
}

Result AccountStore::setResult(const String& label, const String& date, bool success,
                               const String& message) {
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  Result result = Result::NotFound;
  const int index = indexOfLabel(label);
  if (index >= 0) {
    CheckinProfile& profile = m_list[static_cast<size_t>(index)];
    profile.lastDate = date;
    profile.lastOk = success;
    profile.lastMsg = message;
    result = persist();
  }
  if (m_mutex) xSemaphoreGive(m_mutex);
  LOG_I(kTag, "account %s result: date=%s ok=%d", label.c_str(), date.c_str(), success ? 1 : 0);
  return result;
}

bool AccountStore::isCheckedInToday(const String& label, const String& date) const {
  if (date.length() == 0) return false;
  for (const CheckinProfile& profile : m_list) {
    if (profile.label == label) {
      return profile.lastOk && profile.lastDate == date;
    }
  }
  return false;
}

String AccountStore::toJsonSummary() const {
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (const CheckinProfile& profile : m_list) {
    JsonObject item = array.add<JsonObject>();
    item["label"] = profile.label;
    item["enabled"] = profile.enabled;
    item["user"] = maskUser(profile.studentId);
    item["passwordSet"] = profile.password.length() > 0;
    item["fingerprintSet"] = profile.hasFingerprint();
    item["location"] = profile.hasLocation();
    item["lastDate"] = profile.lastDate;
    item["lastOk"] = profile.lastOk;
    item["lastMsg"] = profile.lastMsg;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String AccountStore::labels() const {
  String out;
  for (size_t i = 0; i < m_list.size(); i++) {
    if (i > 0) out += ',';
    out += m_list[i].label;
  }
  return out;
}

Result AccountStore::replaceAt(size_t index, const CheckinProfile& profile) {
  if (!labelOk(profile.label) || !profile.studentId.length() || !profile.password.length()) return Result::InvalidParam;
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
  const int collision = indexOfLabel(profile.label);
  if (index >= m_list.size() || (collision >= 0 && static_cast<size_t>(collision) != index)) {
    if (m_mutex) xSemaphoreGive(m_mutex);
    return Result::InvalidParam;
  }
  const CheckinProfile previous = m_list[index];
  m_list[index] = profile;
  Result result = persist();
  if (result == Result::Ok) ++m_revision; else m_list[index] = previous;
  if (m_mutex) xSemaphoreGive(m_mutex);
  return result;
}

String AccountStore::toJsonDetails() const {
  JsonDocument doc;
  doc["revision"] = m_revision;
  JsonArray rows = doc["accounts"].to<JsonArray>();
  for (size_t i=0;i<m_list.size();++i) {
    const auto& p=m_list[i];
    JsonObject row=rows.add<JsonObject>();
    row["slot"]=i; row["label"]=p.label; row["user"]=p.studentId;
    row["enabled"]=p.enabled; row["passwordSet"]=p.password.length()>0;
    row["fingerprintSet"]=p.hasFingerprint(); row["lat"]=p.latitude; row["lng"]=p.longitude; row["addr"]=p.address;
    row["lastDate"]=p.lastDate; row["lastOk"]=p.lastOk; row["lastMsg"]=p.lastMsg;
  }
  String out; serializeJson(doc,out); return out;
}
