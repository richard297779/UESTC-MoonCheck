#pragma once

// ============================================================================
//  CheckinApi：智慧学工平台接口适配层（jzsz.uestc.edu.cn）
//    **本模块是唯一持有 URL、请求头与字段名的地方**（契约 §2 的适配层隔离）
//    - EP05 进入微应用 / EP06 今日状态 / EP07 有效时段
//    - EP08 可用打卡点 / EP09 逆地理编码 / EP10 提交打卡
//    - 隐私红线：EP06 响应含姓名/学号/手机号，本模块只解析白名单字段
//  实现依据：docs/api-contract.md §3，行为对齐 tools/pc_reference/wsc_checkin.py
//  线程约束：仅在 net_task 中调用。
// ============================================================================

#include <Arduino.h>

#include <vector>

#include "app_types.h"
#include "http_client.h"

/// @brief 今日打卡状态（仅白名单字段）。
struct TodayStatus {
  bool valid = false;
  String businessDate;
  String recordStatusCode;  ///< PENDING / CHECKED_IN / 其他
  String checkinTime;
  float distanceMeter = -1.0f;
  bool abnormalFlag = false;
  String locationName;
  double locationLatitude = 0.0;
  double locationLongitude = 0.0;
  int radiusMeter = 0;

  bool isCheckedIn() const { return recordStatusCode == "CHECKED_IN"; }
};

/// @brief 有效时段（分钟偏移，自 00:00 起）。
struct ScheduleInfo {
  bool effective = false;
  int checkinStartMinutes = 0;
  int checkinEndMinutes = 0;
  String scheduleName;
};

/// @brief 可用打卡点。
struct LocationInfo {
  String name;
  double latitude = 0.0;
  double longitude = 0.0;
  int radiusMeter = 0;
};

class CheckinApi {
 public:
  /// @brief 注入 HTTP 客户端。
  Result begin(HttpClient& http);

  /// @brief EP05：进入微应用（302 → H5 页）。
  /// @param[out] outReachedH5 是否真的落到 H5 页面（false 表示被退回平台首页＝应用当前不可用）。
  /// @param[out] outFinalUrl 跟随重定向后的最终 URL（排查"302 到底去了哪"用，可选）。
  /// @param[out] outStatus 最终 HTTP 状态码（可选）。
  Result enterApp(bool* outReachedH5 = nullptr, String* outFinalUrl = nullptr,
                  int* outStatus = nullptr);

  /// @brief 直接 GET H5 页（`/daily-checkin-h5/index.html?appId=294`）。
  /// @note 该页是**静态页**：任何会话（含未授权会话）都返回 200 —— 这正是浏览器"任何时候
  ///       都能打开打卡界面"的原因。它**不能**代表会话被授权打卡：授权判据仍是 enterApp()。
  /// @param[out] outReached 页面是否真的打开（200 且含 HTML）。
  Result fetchH5Page(bool* outReached = nullptr);

  /// @brief 门户预热（**微应用授权的前提**）：登录后必须先调用一次 `/api/isLoginFailure`。
  /// @note 实测（2026-10-13，`tools/pc_reference/diag_minimal_warmup.py`）：CAS 登录成功、
  ///       `GET /` 返回 200，但若没做这一步，微应用一律拒绝（`checkLogin` 被 302 退回平台首页、
  ///       业务接口回 `4001 登录无效或已过期`）；调用这一个 71 字节的接口后，同一会话立刻被授权。
  /// @param[out] outStatus HTTP 状态码（可选，供诊断）。
  Result warmUpPortal(int* outStatus = nullptr);

  /// @brief EP06：查询今日状态。
  Result fetchToday(TodayStatus& out);

  /// @brief EP07：查询有效时段。
  Result fetchSchedule(ScheduleInfo& out);

  /// @brief EP08：查询可用打卡点。
  Result fetchLocations(std::vector<LocationInfo>& out);

  /// @brief EP09：逆地理编码（失败不致命，调用方保留原地址即可）。
  Result reverseGeocode(double latitude, double longitude, String& outAddress);

  /// @brief EP10：提交打卡。
  /// @param[out] rawResponse 原始响应（供日志与错误分析）。
  Result submitCheckin(double latitude, double longitude, const String& address,
                       const String& clientTime, HttpResponse& rawResponse);

  /// @brief 组装打卡载荷（与实测成功载荷逐字段一致，不做裁剪）。
  static String buildCheckinPayload(double latitude, double longitude, const String& address,
                                    const String& clientTime);

  /// @brief 微应用 H5 页面地址（同时作为 Referer）。
  static String h5Url();

 private:
  /// @brief 为请求追加微应用必需的请求头。
  static void addH5Headers(HttpRequest& request);
  /// @brief GET 一个业务接口并校验 HTTP 状态。
  Result getJson(const String& path, HttpResponse& response);

  HttpClient* m_http = nullptr;
};
