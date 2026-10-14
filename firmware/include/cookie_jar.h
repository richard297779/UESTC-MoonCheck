#pragma once

// ============================================================================
//  CookieJar：多域 Cookie 罐
//    - 按「域后缀 + 路径前缀」匹配组装 Cookie 头
//    - 解析 Set-Cookie（含 Max-Age / Expires / Path / Domain / 删除语义）
//    - 每次变更立即持久化到 NVS，掉电不丢 SESSION
//  线程约束：仅在 net_task 与 BLE 指令处理中访问（内部加锁）。
// ============================================================================

#include <Arduino.h>
#include <Preferences.h>

#include <vector>

#include "app_types.h"

/// @brief 单条 Cookie。
struct Cookie {
  String name;
  String value;
  String domain;         ///< 小写，不含前导点
  String path = "/";
  time_t expires = 0;    ///< 0 表示会话级
  uint32_t lastUsedMs = 0;
};

class CookieJar {
 public:
  /// @brief 打开 NVS 命名空间并恢复已保存的 Cookie。
  Result begin(const char* nvsNamespace = "cookies");

  /// @brief 清空内存与 NVS 中的 Cookie。
  void clear();

  /// @brief 直接写入/覆盖一条 Cookie（用于注入 SESSION 与指纹）。
  void set(const String& name, const String& value, const String& domain,
           const String& path = "/", time_t expires = 0);

  /// @brief 删除一条 Cookie（domain 为空表示不限域）。
  /// @note 用途：登录前清掉平台**匿名** SESSION，强制服务端在票据校验时下发登录态会话。
  void remove(const String& name, const String& domain = "");

  /// @brief 读取 Cookie 值；domain 为空时不限制域。
  String get(const String& name, const String& domain = "") const;

  /// @brief 组装请求用的 Cookie 头；无匹配返回空串。
  String headerFor(const String& url) const;

  /// @brief 只列出此请求会携带的 Cookie **名称**（用于日志，不含值）。
  String namesFor(const String& url) const;

  /// @brief 解析并应用一条 Set-Cookie（url 用于推导默认域与路径）。
  /// @return 1=新增或更新，0=被删除或忽略。
  int applySetCookie(const String& url, const String& setCookieValue);

  /// @brief 持久化到 NVS。
  Result persist();
  /// @brief 从 NVS 恢复。
  Result restore();

  size_t count() const { return m_cookies.size(); }

  /// @brief 只输出 Cookie 名称与到期信息（不含值）——用于 BLE status。
  String toJsonSummary() const;

 private:
  std::vector<Cookie>::iterator findCookie(const String& name, const String& domain);
  void evictIfNeeded();

  std::vector<Cookie> m_cookies;
  Preferences m_prefs;
  bool m_opened = false;
  SemaphoreHandle_t m_mutex = nullptr;
};
