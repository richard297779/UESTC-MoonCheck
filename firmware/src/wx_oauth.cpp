#include "wx_oauth.h"

#include <ArduinoJson.h>
#include <esp_random.h>

#include <cstdlib>

#include "logger.h"
#include "wx_crypto.h"

namespace wx {
namespace {
const char* const kTag = "wx_oauth";
const char* kBaseUrl = "https://yybadaccess.3g.qq.com";
const char* kRefreshPath = "/pc_yyb_auth/pcyyb_refresh_token_auth";
const char* kLoginBufferPath = "/pc_yyb_auth/pcyyb_get_wx_login_buffer_auth";
const char* kBusinessId = "pc_yyb_auth";
const char* kAccessKey = "wgrdg373hy26ww2";  // 应用宝网页端公开发布的接入键

/// @brief 组签名头：md5(body + timestamp + key + nonce)。
void addSignedHeaders(HttpRequest& request, const String& body) {
  char timestamp[24];
  snprintf(timestamp, sizeof(timestamp), "%lld",
           static_cast<long long>(time(nullptr)) * 1000LL);
  char nonce[8];
  snprintf(nonce, sizeof(nonce), "%04u",
           static_cast<unsigned>(esp_random() % 10000));
  String input = body + timestamp + kAccessKey + nonce;
  Bytes digest = md5(reinterpret_cast<const uint8_t*>(input.c_str()), input.length());
  String hex = toHex(digest);
  request.addHeader("Ual-Access-Businessid", kBusinessId);
  request.addHeader("Ual-Access-Timestamp", timestamp);
  request.addHeader("Ual-Access-Nonce", nonce);
  request.addHeader("Ual-Access-Signature", hex);
}

OauthResult postJson(HttpClient& http, const char* path, const String& body,
                     const String& cookieHeader, HttpResponse& response,
                     JsonDocument& doc) {
  HttpRequest request;
  request.method = "POST";
  request.url = String(kBaseUrl) + path;
  request.body = body;
  request.addHeader("Accept", "application/json");
  request.addHeader("Content-Type", "application/json");
  if (cookieHeader.length() > 0) request.addHeader("Cookie", cookieHeader);
  addSignedHeaders(request, body);
  request.publicCa = true;  // 公共 CA 包（含 DigiCert 根）
  if (http.request(request, response) != Result::Ok) {
    LOG_W(kTag, "%s: transport fail (%s)", path, response.errorDetail.c_str());
    return OauthResult::Network;
  }
  if (!response.ok()) {
    LOG_W(kTag, "%s: http %d body=%.60s", path, response.status, response.body.c_str());
    return OauthResult::Network;
  }
  DeserializationError err = deserializeJson(doc, response.body);
  if (err != DeserializationError::Ok) {
    LOG_W(kTag, "%s: json parse fail (%s), body head: %.120s", path, err.c_str(),
          response.body.c_str());
    return OauthResult::Invalid;
  }
  return OauthResult::Ok;
}
}  // namespace

OauthResult refreshCredentials(const OauthCredentials& in, OauthCredentials& out,
                               HttpResponse& raw) {
  if (!in.valid() || in.refreshToken.length() == 0) return OauthResult::Rejected;
  JsonDocument body;
  JsonObject userInfo = body["userInfo"].to<JsonObject>();
  userInfo["openId"] = in.openid;
  userInfo["refreshToken"] = in.refreshToken;
  userInfo["accessToken"] = in.accessToken;
  userInfo["loginType"] = "WX";
  String bodyStr;
  serializeJson(body, bodyStr);

  HttpClient http;
  if (http.begin("Mozilla/5.0", 1) != Result::Ok) return OauthResult::Network;
  JsonDocument doc;
  OauthResult r = postJson(http, kRefreshPath, bodyStr, String(), raw, doc);
  if (r != OauthResult::Ok) return r;
  int code = doc["code"] | -1;
  if (code != 0) {
    LOG_W(kTag, "refresh rejected code=%d", code);
    return OauthResult::Rejected;
  }
  JsonObject userInfoOut = doc["user_info"];
  if (userInfoOut.isNull()) return OauthResult::Invalid;
  JsonVariant accessV = userInfoOut["access_token"];
  JsonVariant refreshV = userInfoOut["refresh_token"];
  if (accessV.isNull() || !accessV.is<const char*>() ||
      accessV.as<String>().length() == 0)
    return OauthResult::Invalid;
  out.openid = in.openid;
  out.accessToken = accessV.as<String>();
  out.refreshToken = refreshV.isNull() ? in.refreshToken : refreshV.as<String>();
  // expires_in 可能是字符串或数字；0/缺失按 7200 兜底
  int64_t expiresIn = 7200;
  if (userInfoOut["expires_in"].is<const char*>()) {
    expiresIn = strtoll(userInfoOut["expires_in"].as<const char*>(), nullptr, 10);
  } else if (!userInfoOut["expires_in"].isNull()) {
    expiresIn = userInfoOut["expires_in"].as<int64_t>();
  }
  if (expiresIn <= 0) expiresIn = 7200;
  out.expiresAt = static_cast<int64_t>(time(nullptr)) + expiresIn;
  return OauthResult::Ok;
}

OauthResult fetchLoginBuffer(const OauthCredentials& cred, String& outLoginBuffer,
                             HttpResponse& raw) {
  LOG_I(kTag, "fetch_lb: enter (openid len=%u access len=%u)",
        cred.openid.length(), cred.accessToken.length());
  if (!cred.valid()) return OauthResult::Rejected;
  // 结构对齐参考实现：{"extInfo":{"listS":{"<k>":{"value":[v]}},"listI":{"user_type":{"value":[0]}}}}
  JsonDocument body;
  JsonObject extInfo = body["extInfo"].to<JsonObject>();
  JsonObject listS = extInfo["listS"].to<JsonObject>();
  listS["unionid"]["value"].to<JsonArray>().add(cred.openid);
  listS["user_id"]["value"].to<JsonArray>().add(cred.openid);
  listS["access_token"]["value"].to<JsonArray>().add(cred.accessToken);
  extInfo["listI"]["user_type"]["value"].to<JsonArray>().add(0);
  String bodyStr;
  serializeJson(body, bodyStr);

  String cookie = "openid=" + cred.openid + "; accesstoken=" + cred.accessToken +
                  "; refreshtoken=" + cred.refreshToken;
  HttpClient http;
  if (http.begin("Mozilla/5.0", 1) != Result::Ok) return OauthResult::Network;
  JsonDocument doc;
  OauthResult r = postJson(http, kLoginBufferPath, bodyStr, cookie, raw, doc);
  LOG_I(kTag, "fetch_lb: postJson result=%d status=%d bodyLen=%u",
        static_cast<int>(r), raw.status, static_cast<unsigned>(raw.body.length()));
  if (r != OauthResult::Ok) return r;
  int code = doc["code"] | -1;
  if (code != 0) {
    LOG_W(kTag, "login-buffer rejected code=%d", code);
    return OauthResult::Rejected;
  }
  JsonVariant lb = doc["ext_info"]["list_s"]["login_buffer"]["value"][0];
  if (lb.isNull() || !lb.is<const char*>()) {
    LOG_W(kTag, "lb extract fail (len=%u): %.140s",
          static_cast<unsigned>(raw.body.length()), raw.body.c_str());
    return OauthResult::Invalid;
  }
  outLoginBuffer = lb.as<String>();
  return OauthResult::Ok;
}

}  // namespace wx
