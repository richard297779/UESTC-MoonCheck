#pragma once

// ============================================================================
//  WxOauth：应用宝（YYB）HTTPS 接入层
//    - 凭据刷新：pcyyb_refresh_token_auth（轮换 access/refresh token）
//    - login_buffer 拉取：pcyyb_get_wx_login_buffer_auth（MMTLS ManualAuth 的输入）
//  签名：Ual-Access-Signature = MD5(body + timestamp + 公开接入键 + nonce)
//  移植依据：moon-uestc `_wechat_login.py`（LoginClient.fetch_login_buffer/refresh）。
//  线程约束：仅在 net_task 中调用。
// ============================================================================

#include <Arduino.h>

#include "http_client.h"

namespace wx {

/// @brief 一组应用宝凭据（对应用户一次扫码授权）。
struct OauthCredentials {
  String openid;
  String accessToken;
  String refreshToken;
  int64_t expiresAt = 0;  ///< Unix 秒（access token 到期）
  bool valid() const { return openid.length() > 0 && accessToken.length() > 0; }
  bool needsRefresh(int64_t nowSec, int64_t marginSec = 60) const {
    return !valid() || expiresAt - marginSec <= nowSec;
  }
};

enum class OauthResult {
  Ok = 0,
  Network,      ///< HTTP/TLS 层失败
  Rejected,     ///< 服务端 code != 0（凭据失效，需重新扫码）
  Invalid,      ///< 响应结构异常
};

/// @brief 刷新凭据（服务端轮换，调用方必须保存返回值）。
OauthResult refreshCredentials(const OauthCredentials& in, OauthCredentials& out,
                               HttpResponse& raw);

/// @brief 拉取 login_buffer（ManualAuth 输入，短时效，现取现用）。
OauthResult fetchLoginBuffer(const OauthCredentials& cred, String& outLoginBuffer,
                             HttpResponse& raw);

}  // namespace wx
