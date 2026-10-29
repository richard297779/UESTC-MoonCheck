#include "wx_credential_store.h"

#include <Preferences.h>

#include "logger.h"

namespace wx {
namespace {
const char* const kTag = "wx_cred";
const char* kNamespace = "wxauth";
}  // namespace

String CredentialStore::keyFor(size_t slot) const {
  char key[8];
  snprintf(key, sizeof(key), "cred%u", static_cast<unsigned>(slot));
  return String(key);
}

Result CredentialStore::begin() {
  auto* prefs = new Preferences();
  if (!prefs->begin(kNamespace, false)) {
    delete prefs;
    LOG_E(kTag, "nvs begin failed");
    return Result::StorageError;
  }
  m_prefs = prefs;
  return Result::Ok;
}

Result CredentialStore::load(size_t slot, OauthCredentials& out) {
  if (slot >= kMaxSlots || m_prefs == nullptr) return Result::InvalidParam;
  auto* prefs = static_cast<Preferences*>(m_prefs);
  String key = keyFor(slot);
  if (!prefs->isKey(key.c_str())) return Result::NotFound;
  String blob = prefs->getString(key.c_str(), "");
  if (blob.length() == 0) return Result::NotFound;
  // 格式：openid\naccess\nrefresh\nexpiresAt
  int p1 = blob.indexOf('\n');
  int p2 = blob.indexOf('\n', p1 + 1);
  int p3 = blob.indexOf('\n', p2 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0) return Result::StorageError;
  out.openid = blob.substring(0, p1);
  out.accessToken = blob.substring(p1 + 1, p2);
  out.refreshToken = blob.substring(p2 + 1, p3);
  out.expiresAt = strtoll(blob.substring(p3 + 1).c_str(), nullptr, 10);
  return out.valid() ? Result::Ok : Result::StorageError;
}

Result CredentialStore::save(size_t slot, const OauthCredentials& cred) {
  if (slot >= kMaxSlots || m_prefs == nullptr || !cred.valid())
    return Result::InvalidParam;
  auto* prefs = static_cast<Preferences*>(m_prefs);
  String blob = cred.openid + "\n" + cred.accessToken + "\n" + cred.refreshToken +
                "\n" + String(static_cast<long long>(cred.expiresAt));
  if (prefs->putString(keyFor(slot).c_str(), blob) != blob.length())
    return Result::StorageError;
  return Result::Ok;
}

Result CredentialStore::remove(size_t slot) {
  if (slot >= kMaxSlots || m_prefs == nullptr) return Result::InvalidParam;
  auto* prefs = static_cast<Preferences*>(m_prefs);
  const String key = keyFor(slot);
  if (!prefs->isKey(key.c_str())) return Result::Ok;
  return prefs->remove(key.c_str()) ? Result::Ok : Result::StorageError;
}

String CredentialStore::summary() const {
  if (m_prefs == nullptr) return String("{\"error\":\"not ready\"}");
  auto* prefs = static_cast<Preferences*>(m_prefs);
  String out = "[";
  int64_t now = static_cast<int64_t>(time(nullptr));
  for (size_t slot = 0; slot < kMaxSlots; slot++) {
    OauthCredentials cred;
    bool present = prefs->isKey(keyFor(slot).c_str());
    if (slot > 0) out += ",";
    out += "{\"slot\":";
    out += String(static_cast<unsigned>(slot));
    out += ",\"present\":";
    out += present ? "true" : "false";
    if (present && const_cast<CredentialStore*>(this)->load(slot, cred) == Result::Ok) {
      out += ",\"expiresIn\":";
      out += String(static_cast<long long>(cred.expiresAt - now));
    }
    out += "}";
  }
  out += "]";
  return out;
}

}  // namespace wx
