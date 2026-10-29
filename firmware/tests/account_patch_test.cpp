#include <cassert>
#include <string>
#include "account_patch.h"
#include "account_selection.h"

struct Profile {
  std::string label="A", studentId="fixture-user", password="fixture-password";
  std::string fingerprint="0123456789abcdef0123456789abcdef", address="fixture address";
  double latitude=30.1, longitude=104.2;
  bool enabled=false;
  bool hasLocation() const { return latitude!=0 && longitude!=0; }
};

struct Store {
  std::vector<Profile> rows;
  bool hasAny() const { return !rows.empty(); }
  int indexOfLabel(const std::string& label) const { for(size_t i=0;i<rows.size();++i) if(rows[i].label==label)return i;return -1; }
  bool get(size_t i,Profile& out) const { if(i>=rows.size())return false;out=rows[i];return true; }
};
int main() {
  const char* error="";
  auto merge=[&](Profile& p,const char* json,bool creating=false) {
    JsonDocument doc; assert(!deserializeJson(doc,json));
    return mergeAccountPatch(p,doc.as<JsonObjectConst>(),creating,error);
  };
  Profile p;
  assert(merge(p,R"({"pass":"","bfp":"","addr":"new address"})"));
  assert(p.password=="fixture-password" && p.fingerprint.length()==32);
  assert(p.latitude==30.1 && p.longitude==104.2 && !p.enabled);
  assert(p.address=="new address");
  assert(merge(p,R"({"label":"renamed","user":"new-user"})"));
  assert(p.label=="renamed" && p.studentId=="new-user");
  for(const char* invalid:{R"({"lat":31})",R"({"lat":0,"lng":104})",R"({"lat":91,"lng":104})",R"({"lat":"31","lng":104})",R"({"bfp":"bad"})",R"({"enabled":true})",R"({"pass":null})"}) {
    Profile copy=p; assert(!merge(copy,invalid));
  }
  Profile defaults; defaults.enabled=true;
  Profile a=defaults; a.label="A";a.latitude=31.1;a.longitude=105.2;a.address="own A";
  Profile b=defaults; b.label="B";b.latitude=0;b.longitude=0;b.address="";
  Store store{{a,b}};std::vector<Profile> selected;
  assert(activeAuthorizationSlot<Profile>(store,std::string("B"),b.studentId)==1);
  assert(activeAuthorizationSlot<Profile>(store,std::string("missing"),b.studentId)==-1);
  assert(activeAuthorizationSlot<Profile>(store,std::string("B"),std::string("other-user"))==-1);
  auto select=[&](const char* text,const Store& accounts) { JsonDocument doc;assert(!deserializeJson(doc,text));return prepareSelectedAccounts(doc.as<JsonArrayConst>(),accounts,defaults,selected,error); };
  assert(select(R"(["B","A"])",store));
  assert(selected.size()==2 && selected[0].label=="B" && selected[1].label=="A");
  assert(selected[0].latitude==defaults.latitude && selected[0].address==defaults.address);
  assert(selected[1].latitude==31.1 && selected[1].address=="own A");
  assert(!select(R"(["A","A"])",store) && selected.empty());
  assert(!select(R"(["missing"])",store));
  store.rows[1].enabled=false;assert(!select(R"(["A","B"])",store) && selected.empty());
  assert(!select(R"([])",store));assert(!select(R"([0])",store));
  Store empty;assert(select(R"(["@primary"])",empty) && selected[0].studentId==defaults.studentId);
  Profile fresh; fresh.fingerprint="";
  assert(!merge(fresh,R"({"label":"new","user":"fixture","pass":"fixture"})",true));
  assert(merge(fresh,R"({"bfp":"abcdef0123456789abcdef0123456789"})",true));
}
