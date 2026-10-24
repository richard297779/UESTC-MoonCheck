// ============================================================================
//  CheckinApi 实现
// ============================================================================

#include "checkin_api.h"

#include <ArduinoJson.h>

#include "logger.h"

namespace {

const char* const kTag = "checkin_api";

// 仅接受本站 H5 URL 的参数；拒绝控制字符，避免把 URL 内容注入 HTTP 头。
String operationTicketFromUrl(const String& url) {
  if (!url.startsWith(String(AppConst::kJzszBaseUrl) + "/daily-checkin-h5/")) return String();
  const int question = url.indexOf('?');
  if (question < 0) return String();
  int start = question + 1;
  while (start < static_cast<int>(url.length())) {
    int end = url.indexOf('&', start);
    if (end < 0) end = url.length();
    const String pair = url.substring(start, end);
    if (pair.startsWith("checkinTicket=")) {
      String encoded = pair.substring(14);
      const int hash = encoded.indexOf('#');
      if (hash >= 0) encoded = encoded.substring(0, hash);
      String ticket = HttpClient::urlDecode(encoded);
      if (ticket.length() == 0 || ticket.length() > 2048) return String();
      for (size_t i = 0; i < ticket.length(); ++i) {
        if (static_cast<uint8_t>(ticket[i]) <= 0x20 ||
            static_cast<uint8_t>(ticket[i]) >= 0x7f) return String();
      }
      return ticket;
    }
    start = end + 1;
  }
  return String();
}

/// @brief 业务路径（契约 §2）。
constexpr const char* kPathEnterApp = "/daily-checkin/checkLogin?appId=294";
/// @brief 门户"登录态初始化"接口：微应用授权的前提（见 CheckinApi::warmUpPortal）。
constexpr const char* kPathPortalWarmup = "/api/isLoginFailure";
constexpr const char* kPathToday = "/daily-checkin/api/student-checkin-record/today";
constexpr const char* kPathSchedule = "/daily-checkin/api/checkin-schedule/effective";
constexpr const char* kPathLocations = "/daily-checkin/api/checkin-location/available-locations";
constexpr const char* kPathCheckin = "/daily-checkin/api/student-checkin-record/checkin";
constexpr const char* kPathReverse = "/daily-checkin/api/map/reverse";

/// @brief 安全取字符串（避免 String(nullptr) 崩溃）。
String jsonString(JsonVariantConst value) {
  const char* text = value.as<const char*>();
  return text ? String(text) : String();
}

/// @brief 安全取 double。
double jsonDouble(JsonVariantConst value, double fallback = 0.0) {
  if (value.isNull()) return fallback;
  return value.as<double>();
}

/// @brief 安全取 int。
int jsonInt(JsonVariantConst value, int fallback = 0) {
  if (value.isNull()) return fallback;
  return value.as<int>();
}

/// @brief 安全取 bool。
bool jsonBool(JsonVariantConst value, bool fallback = false) {
  if (value.isNull()) return fallback;
  return value.as<bool>();
}

/// @brief 业务错误里表示"会话失效"的关键字（实测服务端返回 "登录无效或已过期"）。
const char* const kSessionErrorKeys[] = {"登录无效", "登录已过期", "登录过期", "未登录",
                                         "请重新登录", "重新登录", "会话失效", "会话过期"};

/// @brief 判断业务错误信息是否表示会话失效。
Result classifyBusinessError(const String& message) {
  for (const char* key : kSessionErrorKeys) {
    if (message.indexOf(key) >= 0) return Result::SessionInvalid;
  }
  return Result::ApiChanged;
}

}  // namespace

String CheckinApi::h5Url() {
  return String(AppConst::kJzszBaseUrl) + "/daily-checkin-h5/index.html?appId=" +
         String(AppConst::kAppId);
}

Result CheckinApi::begin(HttpClient& http) {
  m_http = &http;
  LOG_I(kTag, "checkin api ready");
  return Result::Ok;
}

void CheckinApi::addH5Headers(HttpRequest& request) {
  if (m_operationTicket.length() > 0) request.addHeader("X-Checkin-Ticket", m_operationTicket);
  request.addHeader("Platform", "h5");
  request.addHeader("Accept", "*/*");
  request.addHeader("Origin", AppConst::kJzszBaseUrl);
  request.addHeader("Referer", h5Url());
  request.addHeader("Sec-Fetch-Site", "same-origin");
  request.addHeader("Sec-Fetch-Mode", "cors");
  request.addHeader("Sec-Fetch-Dest", "empty");
}

Result CheckinApi::getJson(const String& path, HttpResponse& response) {
  if (m_http == nullptr) return Result::NotReady;

  HttpRequest request;
  request.method = "GET";
  request.url = String(AppConst::kJzszBaseUrl) + path;
  addH5Headers(request);

  const Result result = m_http->request(request, response);
  if (result != Result::Ok) return result;

  if (response.status == 301 || response.status == 302) {
    LOG_W(kTag, "%s -> %d (session invalid)", path, response.status);
    return Result::SessionInvalid;
  }
  if (response.status == 401 || response.status == 403) {
    return Result::SessionInvalid;
  }
  if (response.status != 200) {
    return Result::HttpError;
  }
  return Result::Ok;
}

Result CheckinApi::enterApp(bool* outReachedH5, String* outFinalUrl, int* outStatus) {
  // 集中流程：21:00 取的票要在 21:20 使用；这里只清过期票据（>25 min）
  if (m_operationTicket.length() > 0 &&
      static_cast<uint32_t>(millis() - m_ticketAtMs) >= 25UL * 60 * 1000) {
    m_operationTicket = "";
    m_ticketOwner = "";
  }
  if (m_http == nullptr) return Result::NotReady;

  HttpRequest request;
  request.method = "GET";
  request.url = String(AppConst::kJzszBaseUrl) + kPathEnterApp;
  request.addHeader("Accept",
                    "text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,image/avif,"
                    "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7");
  request.addHeader("Upgrade-Insecure-Requests", "1");
  request.addHeader("Pragma", "no-cache");
  request.addHeader("Cache-Control", "no-cache");
  // 浏览器点击图标时是"顶级导航"，这里逐字段对齐（部分网关据此放行应用入口）
  request.addHeader("Sec-Fetch-Site", "same-origin");
  request.addHeader("Sec-Fetch-Mode", "navigate");
  request.addHeader("Sec-Fetch-User", "?1");
  request.addHeader("Sec-Fetch-Dest", "document");
  request.addHeader("Priority", "u=0, i");

  HttpResponse response;
  const Result result = m_http->requestFollow(request, response, 4);
  // 先把"302 去了哪"暴露出去：这是判断问题在设备侧还是平台侧的唯一硬证据
  if (outFinalUrl != nullptr) *outFinalUrl = HttpClient::logUrl(response.url);
  if (outStatus != nullptr) *outStatus = response.status;
  if (result != Result::Ok) return result;

  // 实测：打卡窗口内会落到 /daily-checkin-h5/index.html?appId=294；
  //       窗口外会被 302 退回平台首页（此时业务接口会返回 4001 登录无效或已过期）。
  const bool reachedH5 = response.url.indexOf("/daily-checkin-h5/") >= 0;
  if (reachedH5 && response.status == 200) {
    // URL 带票据时才覆盖（小程序入口场景）；设备 CAS 入口 URL 无票据参数，
    // 不能把预取/现场取得的票据清掉（2026-10-24 实测：21:00 取号被此行冲掉）
    const String fromUrl = operationTicketFromUrl(response.url);
    if (fromUrl.length() > 0) m_operationTicket = fromUrl;
  }
  if (outReachedH5 != nullptr) *outReachedH5 = reachedH5;
  LOG_I(kTag, "entered micro app: http %d, h5=%d, final=%s", response.status, reachedH5 ? 1 : 0,
        HttpClient::logUrl(response.url).c_str());
  LOG_I(kTag, "operation ticket from entry: %s", m_operationTicket.length() ? "present" : "missing");
  if (response.status != 200 && response.status != 302) return Result::HttpError;
  if (!reachedH5) return Result::ApiChanged;
  return Result::Ok;
}

Result CheckinApi::fetchH5Page(bool* outReached) {
  if (m_http == nullptr) return Result::NotReady;

  HttpRequest request;
  request.method = "GET";
  request.url = h5Url();
  request.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
  request.addHeader("Upgrade-Insecure-Requests", "1");
  request.addHeader("Sec-Fetch-Site", "same-origin");
  request.addHeader("Sec-Fetch-Mode", "navigate");
  request.addHeader("Sec-Fetch-User", "?1");
  request.addHeader("Sec-Fetch-Dest", "document");

  HttpResponse response;
  const Result result = m_http->request(request, response);
  if (result != Result::Ok) return result;

  const bool reached = (response.status == 200) && (response.body.indexOf("<html") >= 0);
  if (outReached != nullptr) *outReached = reached;
  LOG_I(kTag, "h5 page: http %d (%u B), reached=%d", response.status,
        static_cast<unsigned>(response.bodyBytes), reached ? 1 : 0);
  return reached ? Result::Ok : Result::HttpError;
}

Result CheckinApi::warmUpPortal(int* outStatus) {
  if (m_http == nullptr) return Result::NotReady;

  // `_t` 只是浏览器的缓存穿透参数（jQuery 风格），值本身无语义，用当前毫秒即可。
  const unsigned long long stamp =
      static_cast<unsigned long long>(time(nullptr)) * 1000ULL + (millis() % 1000UL);
  char path[96];
  snprintf(path, sizeof(path), "%s?_t=%llu", kPathPortalWarmup, stamp);

  HttpRequest request;
  request.method = "GET";
  request.url = String(AppConst::kJzszBaseUrl) + path;
  request.addHeader("Accept", "application/json, text/plain, */*");
  request.addHeader("Content-Type", "application/json;charset=UTF-8");
  request.addHeader("Origin", AppConst::kJzszBaseUrl);
  request.addHeader("Referer", String(AppConst::kJzszBaseUrl) + "/");
  request.addHeader("Sec-Fetch-Site", "same-origin");
  request.addHeader("Sec-Fetch-Mode", "cors");
  request.addHeader("Sec-Fetch-Dest", "empty");
  request.addHeader("Pragma", "no-cache");
  request.addHeader("Cache-Control", "no-cache");

  HttpResponse response;
  const Result result = m_http->request(request, response);
  if (outStatus != nullptr) *outStatus = response.status;
  LOG_I(kTag, "portal warm-up: http %d (%u B)", response.status,
        static_cast<unsigned>(response.bodyBytes));
  if (result != Result::Ok) return result;
  return response.status == 200 ? Result::Ok : Result::HttpError;
}

Result CheckinApi::fetchToday(TodayStatus& out) {
  HttpResponse response;
  Result result = getJson(kPathToday, response);
  if (result != Result::Ok) return result;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, response.body);
  if (error) {
    LOG_E(kTag, "today json parse failed: %s", error.c_str());
    return Result::ApiChanged;
  }
  if (jsonInt(doc["code"], -1) != 0) {
    const String message = jsonString(doc["msg"]);
    LOG_W(kTag, "today business code=%d msg=%s", jsonInt(doc["code"], -1), message.c_str());
    return classifyBusinessError(message);
  }

  JsonObjectConst data = doc["data"].as<JsonObjectConst>();
  out = TodayStatus();
  out.businessDate = jsonString(data["businessDate"]);
  out.recordStatusCode = jsonString(data["recordStatusCode"]);
  out.checkinTime = jsonString(data["checkinTime"]);
  if (!data["distanceMeter"].isNull()) out.distanceMeter = data["distanceMeter"].as<float>();
  out.abnormalFlag = jsonBool(data["abnormalFlag"]);
  out.locationName = jsonString(data["checkinLocationName"]);
  out.locationLatitude = jsonDouble(data["checkinLocationLatitude"]);
  out.locationLongitude = jsonDouble(data["checkinLocationLongitude"]);
  out.radiusMeter = jsonInt(data["checkinRadiusMeter"]);
  out.valid = out.recordStatusCode.length() > 0;
  if (!out.valid) return Result::ApiChanged;

  LOG_I(kTag, "today: %s (checkedIn=%s)", out.recordStatusCode.c_str(),
        out.checkinTime.c_str());
  return Result::Ok;
}

Result CheckinApi::fetchSchedule(ScheduleInfo& out) {
  HttpResponse response;
  const Result result = getJson(kPathSchedule, response);
  if (result != Result::Ok) return result;

  JsonDocument doc;
  if (deserializeJson(doc, response.body)) return Result::ApiChanged;
  if (jsonInt(doc["code"], -1) != 0) {
    return classifyBusinessError(jsonString(doc["msg"]));
  }

  JsonObjectConst data = doc["data"].as<JsonObjectConst>();
  out = ScheduleInfo();
  out.scheduleName = jsonString(data["scheduleName"]);
  out.checkinStartMinutes = jsonInt(data["studentCheckinStartOffsetMinute"]);
  out.checkinEndMinutes = jsonInt(data["studentCheckinEndOffsetMinute"]);
  out.effective = jsonBool(data["effective"]);
  if (!out.effective || out.checkinEndMinutes <= out.checkinStartMinutes) {
    return Result::ApiChanged;
  }
  LOG_I(kTag, "schedule: %d - %d min", out.checkinStartMinutes, out.checkinEndMinutes);
  return Result::Ok;
}

Result CheckinApi::fetchLocations(std::vector<LocationInfo>& out) {
  out.clear();
  HttpResponse response;
  const Result result = getJson(kPathLocations, response);
  if (result != Result::Ok) return result;

  JsonDocument doc;
  if (deserializeJson(doc, response.body)) return Result::ApiChanged;
  if (jsonInt(doc["code"], -1) != 0) {
    return classifyBusinessError(jsonString(doc["msg"]));
  }

  for (JsonObjectConst item : doc["data"].as<JsonArrayConst>()) {
    LocationInfo info;
    info.name = jsonString(item["locationName"]);
    info.latitude = jsonDouble(item["latitude"]);
    info.longitude = jsonDouble(item["longitude"]);
    info.radiusMeter = jsonInt(item["radiusMeter"]);
    out.push_back(info);
  }
  LOG_I(kTag, "locations: %u", static_cast<unsigned>(out.size()));
  return out.empty() ? Result::ApiChanged : Result::Ok;
}

Result CheckinApi::reverseGeocode(double latitude, double longitude, String& outAddress) {
  char query[96];
  snprintf(query, sizeof(query), "%s?lat=%.6f&lng=%.6f", kPathReverse, latitude, longitude);

  HttpResponse response;
  const Result result = getJson(String(query), response);
  if (result != Result::Ok) return result;

  JsonDocument doc;
  if (deserializeJson(doc, response.body)) return Result::ApiChanged;
  if (jsonInt(doc["code"], -1) != 0) return Result::ApiChanged;

  JsonVariantConst data = doc["data"];
  if (data.is<const char*>()) {
    outAddress = jsonString(data);
  } else if (data.is<JsonObjectConst>()) {
    JsonObjectConst object = data.as<JsonObjectConst>();
    for (const char* key : {"address", "reverseGeocode", "formattedAddress"}) {
      const String value = jsonString(object[key]);
      if (value.length() > 0) {
        outAddress = value;
        break;
      }
    }
  }
  return outAddress.length() > 0 ? Result::Ok : Result::ApiChanged;
}

String CheckinApi::buildCheckinPayload(double latitude, double longitude, const String& address,
                                       const String& clientTime) {
  String safeAddress = address;
  safeAddress.replace("\"", "'");
  char buffer[640];
  snprintf(buffer, sizeof(buffer),
           "{\"latitude\":%.6f,\"longitude\":%.6f,\"reverseGeocode\":\"%s\","
           "\"clientTime\":\"%s\",\"userAgent\":\"%s\",\"coordinateSystemCode\":\"gcj02\","
           "\"accuracy\":86,\"locationDurationMs\":1189,\"networkTypeCode\":\"4g\","
           "\"locationEnabled\":false,\"locationAuthorized\":\"\",\"locationAccuracy\":\"\","
           "\"errorMessage\":\"\"}",
           latitude, longitude, safeAddress.c_str(), clientTime.c_str(), AppConst::kUserAgent);
  return String(buffer);
}

Result CheckinApi::submitCheckin(double latitude, double longitude, const String& address,
                                 const String& clientTime, HttpResponse& rawResponse) {
  if (m_http == nullptr) return Result::NotReady;
  if (m_operationTicket.length() == 0) {
    LOG_W(kTag, "submit stopped: current session has no check-in operation ticket");
    return Result::NotReady;
  }

  HttpRequest request;
  request.method = "POST";
  request.url = String(AppConst::kJzszBaseUrl) + kPathCheckin;
  request.body = buildCheckinPayload(latitude, longitude, address, clientTime);
  addH5Headers(request);
  request.addHeader("Content-Type", "application/json");
  request.addHeader("Pragma", "no-cache");
  request.addHeader("Cache-Control", "no-cache");

  const Result result = m_http->request(request, rawResponse);
  if (result != Result::Ok) return result;
  if (rawResponse.status == 301 || rawResponse.status == 302) return Result::SessionInvalid;
  if (rawResponse.status != 200) {
    // 服务器可能用 403 承载票据错误：识别到 4600x 就清票，让重试现场重取
    if (rawResponse.body.indexOf("46001") >= 0 || rawResponse.body.indexOf("46002") >= 0 ||
        rawResponse.body.indexOf("46003") >= 0) {
      LOG_W(kTag, "ticket rejected at http %d, clearing for refetch", rawResponse.status);
      clearOperationTicket();
    }
    return Result::HttpError;
  }
  return Result::Ok;
}

// ---- 微信小程序授权链（platform-app，platform: mp-weixin）----

Result CheckinApi::checkWechatBind(const String& code, bool& outBind,
                                   String& outSessionId) {
  outBind = false;
  outSessionId = "";
  if (m_http == nullptr || code.length() == 0) return Result::InvalidParam;
  HttpRequest request;
  request.method = "POST";
  request.url = String(AppConst::kJzszBaseUrl) + "/platform-app/api/auth/check-bind";
  JsonDocument body;
  body["code"] = code;
  body["sn"] = "";
  serializeJson(body, request.body);
  request.addHeader("Content-Type", "application/json");
  request.addHeader("platform", "mp-weixin");
  HttpResponse response;
  if (m_http->request(request, response) != Result::Ok) return Result::NetworkError;
  if (!response.ok()) {
    LOG_W(kTag, "check-bind http %d", response.status);
    return Result::NetworkError;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response.body) != DeserializationError::Ok)
    return Result::ApiChanged;
  // 小程序端信封：ok 字段独立于 code/status
  if (!doc["ok"].as<bool>()) {
    LOG_W(kTag, "check-bind ok=false");
    return Result::SessionInvalid;
  }
  JsonObject data = doc["data"];
  if (data.isNull()) return Result::ApiChanged;
  outBind = data["bind"].as<bool>();
  JsonVariant sessionV = data["sessionId"];
  if (outBind && (sessionV.isNull() || sessionV.as<String>().length() == 0))
    return Result::ApiChanged;
  if (!sessionV.isNull()) outSessionId = sessionV.as<String>();
  return Result::Ok;
}

Result CheckinApi::requestCheckinTicket(const String& code, const String& mpSessionId,
                                        String& outTicket) {
  outTicket = "";
  if (m_http == nullptr || code.length() == 0 || mpSessionId.length() == 0)
    return Result::InvalidParam;
  HttpRequest request;
  request.method = "POST";
  request.url = String(AppConst::kJzszBaseUrl) + "/platform-app/api/auth/checkin-ticket";
  JsonDocument body;
  body["code"] = code;
  serializeJson(body, request.body);
  request.addHeader("Content-Type", "application/json");
  request.addHeader("platform", "mp-weixin");
  request.addHeader("Cookie", "SESSION=" + mpSessionId);
  HttpResponse response;
  if (m_http->request(request, response) != Result::Ok) return Result::NetworkError;
  if (!response.ok()) {
    LOG_W(kTag, "ticket http %d", response.status);
    return Result::NetworkError;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response.body) != DeserializationError::Ok)
    return Result::ApiChanged;
  if (!doc["ok"].as<bool>()) {
    LOG_W(kTag, "ticket ok=false");
    return Result::SessionInvalid;
  }
  JsonVariant ticketV = doc["data"]["ticket"];
  if (ticketV.isNull()) return Result::ApiChanged;
  outTicket = ticketV.as<String>();
  // 契约：43 字符 [A-Za-z0-9_-]
  if (outTicket.length() != 43) {
    LOG_W(kTag, "ticket length %u != 43", outTicket.length());
    outTicket = "";
    return Result::ApiChanged;
  }
  for (size_t i = 0; i < outTicket.length(); ++i) {
    char c = outTicket[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-')) {
      outTicket = "";
      return Result::ApiChanged;
    }
  }
  return Result::Ok;
}
