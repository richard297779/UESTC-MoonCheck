#pragma once
#include <ArduinoJson.h>
#include <cmath>
#include <cstring>

// Pure merge/validation; caller supplies an existing record or fresh defaults.
// Empty fields never clear existing values. No storage/network side effects.
template <class Profile>
bool mergeAccountPatch(Profile& profile, JsonObjectConst fields, bool creating, const char*& error) {
  auto fail = [&](const char* text) { error = text; return false; };
  auto supplied = [&](const char* key) {
    if (!fields.containsKey(key)) return false;
    if (fields[key].is<const char*>()) return std::strlen(fields[key].as<const char*>()) != 0;
    return true;
  };
  for (JsonPairConst item : fields) {
    const char* key = item.key().c_str();
    if (std::strcmp(key,"lat") == 0 || std::strcmp(key,"lng") == 0) continue;
    if (std::strcmp(key,"label") && std::strcmp(key,"user") && std::strcmp(key,"pass") &&
        std::strcmp(key,"bfp") && std::strcmp(key,"addr")) return fail("unknown account field");
    if (!item.value().is<const char*>()) return fail("account text fields must be strings");
  }
  if (supplied("label")) profile.label = fields["label"].as<const char*>();
  if (supplied("user")) profile.studentId = fields["user"].as<const char*>();
  if (supplied("pass")) profile.password = fields["pass"].as<const char*>();
  if (supplied("bfp")) profile.fingerprint = fields["bfp"].as<const char*>();
  if (supplied("addr")) profile.address = fields["addr"].as<const char*>();
  if (profile.label.length() == 0 || profile.label.length() > 12) return fail("invalid account label");
  for (size_t i=0;i<profile.label.length();++i) { char c=profile.label[i]; if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')) return fail("invalid account label"); }
  if (!profile.studentId.length() || !profile.password.length()) return fail("user and password required");
  if (creating && !profile.fingerprint.length()) return fail("new account requires browser fingerprint");
  if (supplied("bfp") || creating) {
    if (profile.fingerprint.length() != 32) return fail("fingerprint must be 32 hex characters");
    for (size_t i=0;i<profile.fingerprint.length();++i) { char c=profile.fingerprint[i]; if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return fail("fingerprint must be 32 hex characters"); }
  }
  if (supplied("lat") != supplied("lng")) return fail("latitude and longitude required together");
  if (supplied("lat")) {
    if (!fields["lat"].is<double>() || !fields["lng"].is<double>()) return fail("coordinates must be numbers");
    const double lat=fields["lat"].as<double>(), lng=fields["lng"].as<double>();
    if (!std::isfinite(lat)||!std::isfinite(lng)||lat==0||lng==0||lat < -90||lat > 90||lng < -180||lng > 180) return fail("invalid coordinates");
    profile.latitude=lat; profile.longitude=lng;
  }
  return true;
}
