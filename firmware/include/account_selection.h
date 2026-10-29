#pragma once
#include <ArduinoJson.h>
#include <vector>
#include <cstring>

// Labels distinguish profiles even when two rows contain the same student ID.
template <class Profile, class Store, class Text>
int activeAuthorizationSlot(const Store& store, const Text& label, const Text& user) {
  if (!store.hasAny()) return 0;
  const int slot = store.indexOfLabel(label);
  Profile profile;
  return slot >= 0 && store.get(slot, profile) && profile.studentId == user ? slot : -1;
}

// Resolve and copy every target before any network/session side effects.
template <class Store, class Profile>
bool prepareSelectedAccounts(JsonArrayConst labels, const Store& store, const Profile& defaults,
                             std::vector<Profile>& output, const char*& error) {
  auto fail=[&](const char* text) { output.clear(); error=text; return false; };
  output.clear();
  if(labels.size()==0 || labels.size()>6) return fail("select 1 to 6 accounts");
  for(JsonVariantConst label:labels) {
    if(!label.is<const char*>()) return fail("target labels must be strings");
    const char* name=label.as<const char*>(); Profile profile;
    if(!store.hasAny() && std::strcmp(name,"@primary")==0) profile=defaults;
    else {
      const int slot=store.indexOfLabel(name);
      if(slot<0 || !store.get(slot,profile)) return fail("selected account not found");
    }
    for(const auto& prior:output) if(prior.label==profile.label) return fail("duplicate target");
    if(!profile.enabled) return fail("selected account disabled; enable it explicitly first");
    if(!profile.hasLocation()) { profile.latitude=defaults.latitude; profile.longitude=defaults.longitude; }
    if(!profile.address.length()) profile.address=defaults.address;
    output.push_back(profile);
  }
  return true;
}
