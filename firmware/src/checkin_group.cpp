#include "checkin_api.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr const char* kGroup = "/daily-checkin/api/group-situation";
constexpr unsigned kPageSize = 10;
constexpr unsigned kMaxPages = 20;

Result parseResponse(const HttpResponse& response, JsonDocument& doc,
                     const JsonDocument& filter, bool isIdentityResponse = false) {
  if (response.status == 301 || response.status == 302 || response.status == 303 ||
      response.status == 307 || response.status == 308 || response.status == 401) {
    return Result::SessionInvalid;
  }
  if (response.status != 200) return Result::HttpError;
  if (deserializeJson(doc, response.body, DeserializationOption::Filter(filter))) {
    return Result::ApiChanged;
  }
  const JsonVariantConst code = doc["code"];
  if (code == 4001 || code == "4001") return Result::SessionInvalid;
  if (isIdentityResponse && code.is<int>() && code.as<int>() == 5000) {
    const JsonVariantConst ok = doc["ok"];
    const JsonVariantConst data = doc["data"];
    const char* message = doc["msg"].as<const char*>();
    // The identity endpoint may use the generic 5000 code for an expired portal session.
    if (ok.is<bool>() && !ok.as<bool>() && !data.isUnbound() && data.isNull() &&
        message && std::strstr(message, "未登录")) return Result::SessionInvalid;
  }
  if (!code.is<int>() || code.as<int>() != 0 || !doc["ok"].is<bool>() ||
      !doc["ok"].as<bool>()) {
    return Result::ApiChanged;
  }
  return Result::Ok;
}

bool parseCount(JsonVariantConst value, unsigned& count) {
  const char* digits = value.as<const char*>();
  if (!digits || !*digits) return false;
  count = 0;
  for (unsigned index = 0; digits[index]; ++index) {
    if (index == 6 || digits[index] < '0' || digits[index] > '9') return false;
    count = count * 10 + (digits[index] - '0');
  }
  return true;
}

bool validRecordId(const char* id) {
  if (!id || !*id) return false;
  for (unsigned index = 0; id[index]; ++index) {
    const char c = id[index];
    if (index == 64 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
  }
  return true;
}

bool equalsBool(JsonVariantConst value, bool expected) {
  return value.is<bool>() && value.as<bool>() == expected;
}

bool equalsUnsigned(JsonVariantConst value, unsigned expected) {
  return value.is<unsigned>() && value.as<unsigned>() == expected;
}

bool canReportNormal(JsonObjectConst row, JsonObjectConst record) {
  const JsonVariantConst confirmation = row["safetyConfirmResult"];
  return row["reportingWay"] == "GROUP" && row["displayStatus"] == "UNFILLED" &&
         equalsBool(row["reportSubmitted"], false) &&
         !confirmation.isUnbound() && confirmation.isNull() &&
         record["recordStatusCode"] == "PENDING" &&
         equalsBool(record["groupReportingFlag"], true) &&
         equalsBool(record["pendingAbnormalFlag"], false) &&
         equalsBool(record["abnormalFlag"], false) &&
         equalsBool(record["instructorConfirmedFlag"], false);
}

}  // namespace

Result CheckinApi::authorizeGroupReport(const String& owner) {
  HttpResponse response;
  const Result request = getJson("/daily-checkin/api/auth/user-info", response, false);
  if (request != Result::Ok) return request;
  JsonDocument filter;
  filter["code"] = true;
  filter["ok"] = true;
  filter["msg"] = true;
  JsonObject userFilter = filter["data"]["userData"].to<JsonObject>();
  for (const char* field : {"username", "userType", "invalid", "canGroupReport"}) {
    userFilter[field] = true;
  }
  JsonDocument doc;
  const Result result = parseResponse(response, doc, filter, true);
  if (result != Result::Ok) return result;
  const JsonObjectConst user = doc["data"]["userData"];
  if (user.isNull()) return Result::ApiChanged;
  if (!owner.length() || user["username"] != owner.c_str() ||
      user["userType"] != "SAFETY_OFFICER" || !equalsBool(user["invalid"], false) ||
      !equalsBool(user["canGroupReport"], true)) return Result::AuthFailed;
  return Result::Ok;
}

Result CheckinApi::fetchGroupPendingCount(unsigned& count) {
  HttpResponse response;
  const Result request = getJson(String(kGroup) + "/safety-officer/pending-confirmation-count",
                                 response, false);
  if (request != Result::Ok) return request;
  JsonDocument filter;
  filter["code"] = true;
  filter["ok"] = true;
  filter["data"] = true;
  JsonDocument doc;
  const Result result = parseResponse(response, doc, filter);
  if (result != Result::Ok) return result;
  return parseCount(doc["data"], count) ? Result::Ok : Result::ApiChanged;
}

Result CheckinApi::fetchGroupPending(const String& date, std::vector<GroupPendingRecord>& records) {
  records.clear();
  JsonDocument filter;
  filter["code"] = true;
  filter["ok"] = true;
  JsonObject rowFilter = filter["data"][0].to<JsonObject>();
  for (const char* field : {"displayStatus", "reportingWay", "reportSubmitted", "safetyConfirmResult"}) {
    rowFilter[field] = true;
  }
  JsonObject recordFilter = rowFilter["record"].to<JsonObject>();
  for (const char* field : {"id", "businessDate", "recordStatusCode", "groupReportingFlag",
                            "pendingAbnormalFlag", "abnormalFlag", "instructorConfirmedFlag"}) {
    recordFilter[field] = true;
  }
  JsonObject pageFilter = filter["page"].to<JsonObject>();
  for (const char* field : {"pageIndex", "pageSize", "totalCount", "totalPage", "confirmed"}) {
    pageFilter[field] = true;
  }

  unsigned total = 0;
  for (unsigned pageIndex = 1; pageIndex <= kMaxPages; ++pageIndex) {
    HttpResponse response;
    const String path = String(kGroup) + "/list?pageIndex=" + String(pageIndex) +
                        "&pageSize=" + String(kPageSize) + "&confirmed=false";
    const Result request = getJson(path, response, false);
    if (request != Result::Ok) return request;
    JsonDocument doc;
    const Result result = parseResponse(response, doc, filter);
    if (result != Result::Ok) return result;

    const JsonObjectConst page = doc["page"];
    const JsonArrayConst rows = doc["data"];
    unsigned count = 0;
    if (!parseCount(page["totalCount"], count)) return Result::ApiChanged;
    const unsigned pages = (count + kPageSize - 1) / kPageSize;
    if (rows.isNull() || pages > kMaxPages || (pageIndex != 1 && total != count) ||
        !equalsUnsigned(page["pageIndex"], pageIndex) || !equalsUnsigned(page["pageSize"], kPageSize) ||
        !equalsUnsigned(page["totalPage"], pages) || !equalsBool(page["confirmed"], false) ||
        rows.size() != std::min<size_t>(kPageSize, count - records.size())) return Result::ApiChanged;
    total = count;
    for (JsonObjectConst row : rows) {
      const JsonObjectConst record = row["record"];
      const char* id = record["id"].as<const char*>();
      if (!validRecordId(id) || record["businessDate"] != date.c_str() ||
          std::any_of(records.begin(), records.end(), [id](const GroupPendingRecord& item) {
            return item.id == id;
          })) return Result::ApiChanged;
      records.push_back({String(id), canReportNormal(row, record)});
    }
    if (pageIndex >= pages) return Result::Ok;
  }
  return Result::ApiChanged;
}

Result CheckinApi::submitGroupNormal(const std::vector<GroupPendingRecord>& records) {
  if (m_http == nullptr) return Result::NotReady;
  JsonDocument payload;
  JsonArray entries = payload.to<JsonArray>();
  for (const GroupPendingRecord& record : records) {
    if (!record.allowNormal) continue;
    JsonObject item = entries.add<JsonObject>();
    item["recordId"] = record.id;
    item["action"] = "NORMAL";
  }
  if (entries.size() == 0 || payload.overflowed()) return Result::InvalidParam;
  HttpRequest request;
  request.method = "POST";
  request.url = String(AppConst::kJzszBaseUrl) + kGroup + "/report";
  addH5Headers(request, false);
  request.addHeader("Content-Type", "application/json");
  const size_t size = measureJson(entries);
  if (serializeJson(entries, request.body) != size) return Result::InternalError;

  HttpResponse response;
  const Result sent = m_http->request(request, response);
  if (sent != Result::Ok) return sent;
  JsonDocument filter;
  filter["code"] = true;
  filter["ok"] = true;
  filter["data"] = true;
  JsonDocument doc;
  const Result result = parseResponse(response, doc, filter);
  if (result != Result::Ok) return result;
  return equalsBool(doc["data"], true) ? Result::Ok : Result::ApiChanged;
}
