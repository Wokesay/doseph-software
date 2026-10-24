// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>

#include "simulation.hpp"

using namespace doseph;

namespace {

struct Client {
  sim::Simulation& s;
  std::string token;
  std::string ip = "192.168.1.20";
  ApiResponse call(const std::string& method, const std::string& path, const json& body = nullptr,
                   std::map<std::string, std::string> query = {}) {
    ApiRequest r;
    r.method = method;
    r.path = "/api/v1/" + path;
    r.body = body.is_null() ? "" : body.dump();
    r.token = token;
    r.client = clientKey(ip);
    r.query = std::move(query);
    return s.api().handle(r);
  }
  json get(const std::string& path) { return json::parse(call("GET", path).body); }
  bool login(const std::string& pw) {
    auto r = call("POST", "auth/login", {{"password", pw}});
    for (const auto& [k, v] : r.headers)
      if (k == "Set-Cookie") token = sessionToken("", v.substr(0, v.find(';')));
    return r.status == 200;
  }
};

sim::Options newRobot() {
  sim::Options o;
  o.scenario = "new";
  return o;
}

sim::Options demo() {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  return o;
}

}  // namespace

TEST_CASE("API: the first password needs the setup code shown to the owner; then only signed in") {
  sim::Simulation s(newRobot());
  const std::string code = s.robot().armSetupCode();
  REQUIRE_FALSE(code.empty());
  Client c{s, "", "192.168.1.20"};
  CHECK(c.get("info")["hasPassword"] == false);
  CHECK(c.get("info")["setupCode"] == true);
  CHECK(c.call("GET", "state").status == 401);
  CHECK(c.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", "wrong"}}).status == 403);
  CHECK(c.call("POST", "auth/setup", {{"password", "short"}, {"code", code}}).status == 422);
  auto ok = c.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", code}});
  CHECK(ok.status == 200);
  CHECK(c.call("POST", "auth/setup", {{"password", "another-password"}, {"code", code}}).status == 409);
  CHECK(c.login("a-long-password"));
  CHECK(c.call("GET", "state").status == 200);
  CHECK(s.robot().config().system.passwordSet);
}

TEST_CASE("API: a device guessing the password locks only itself out, not one signed in before") {
  sim::Simulation s(demo());
  Client phone{s, "", "192.168.1.20"};
  REQUIRE(phone.login("demo-password"));
  Client guesser{s, "", "192.168.1.66"};
  for (int i = 0; i < 6; ++i) guesser.login("wrong");
  CHECK(guesser.call("POST", "auth/login", {{"password", "demo-password"}}).status == 429);
  phone.token.clear();
  CHECK(phone.login("demo-password"));
}

TEST_CASE("API: the state has the fields the web app reads (contract)") {
  sim::Simulation s(demo());
  s.step(3 * kMinute);
  Client c{s, "", "192.168.1.20"};
  REQUIRE(c.login("demo-password"));
  json st = c.get("state");
  for (const char* k : {"now", "stopped", "online", "circulation", "flowOk", "readings", "cycle", "watchdog", "latches",
                        "jumpLocks", "newTank", "effects", "dosing", "pumps", "pumpJob", "probe", "eventId"})
    CHECK_MESSAGE(st.contains(k), k);
  for (const char* ch : {"ph", "ec", "temp", "flow"}) {
    REQUIRE(st["readings"].contains(ch));
    for (const char* k : {"value", "quality", "reason", "ageS", "unit", "decimals", "usable"}) CHECK(st["readings"][ch].contains(k));
  }
  for (const char* k : {"phase", "since", "line", "notes", "progress", "round", "last", "settings", "mixPending"})
    CHECK_MESSAGE(st["cycle"].contains(k), k);
  for (const char* k : {"recipe", "ecTarget", "phTarget", "volumeL", "flowLpm", "turnoverMin", "waitEcMin", "waitPhMin",
                        "suggestEcMin", "suggestPhMin", "holdMin", "circulation"})
    CHECK_MESSAGE(st["cycle"]["settings"].contains(k), k);
  CHECK(st["watchdog"].contains("stale"));
  CHECK(st["pumps"].size() == 4);
  json cfg = c.get("config");
  for (const char* k : {"system", "tank", "control", "bottles", "recipes", "active", "pumps", "probes"}) CHECK(cfg.contains(k));
  CHECK(c.get("templates")["recipes"].size() == 3);
}

TEST_CASE("API: tank, bottles, recipes and the recipe in force; refusals say why") {
  sim::Simulation s(demo());
  Client c{s, "", "192.168.1.20"};
  REQUIRE(c.login("demo-password"));
  CHECK(c.call("PUT", "tank", {{"volumeL", 0}}).status == 422);
  CHECK(c.call("PUT", "tank", {{"volumeL", "lots"}}).status == 422);
  CHECK(c.call("PUT", "tank", {{"volumeL", 80}, {"waitPhMin", 20}}).status == 200);
  CHECK(c.get("config")["tank"]["volumeL"] == 80);
  CHECK(c.get("state")["newTank"] == true);  // another volume: checked again
  auto b = c.call("POST", "bottles", {{"name", "Bloom PK"}, {"kind", "nutrient"}, {"pump", 2}});
  CHECK(b.status == 422);  // pump 2 carries Part A
  b = c.call("POST", "bottles", {{"name", "Bloom PK"}, {"kind", "nutrient"}, {"pump", 0}});
  REQUIRE(b.status == 200);
  const std::string pk = json::parse(b.body)["id"];
  auto r = c.call("POST", "recipes", {{"name", "With pH"}, {"steps", {{{"bottle", "ph_down"}, {"mlPerL", 0.1}}}}});
  CHECK(r.status == 422);
  CHECK(json::parse(r.body)["error"]["key"] == "cfg.recipe.ph");
  r = c.call("POST", "recipes", {{"name", "PK"}, {"ecTarget", 1.0}, {"ph", {5.8, 6.2}}, {"steps", {{{"bottle", pk}, {"mlPerL", 1.0}}}}});
  REQUIRE(r.status == 200);
  const std::string rid = json::parse(r.body)["id"];
  CHECK(c.call("DELETE", "bottles/" + pk).status == 409);  // used by the recipe
  CHECK(c.call("DELETE", "recipes/two_part").status == 409);  // in force
  CHECK(c.call("PUT", "active", {{"recipe", rid}, {"days", 14}}).status == 200);
  json a = c.get("config")["active"];
  CHECK(a["recipe"] == rid);
  CHECK(a["until"].get<Epoch>() - a["from"].get<Epoch>() == 14 * 86400);
  CHECK(c.call("DELETE", "recipes/two_part").status == 200);
  CHECK(c.call("PUT", "active", {{"recipe", ""}}).status == 200);
  CHECK(c.get("state")["cycle"]["phase"] == "off");
}

TEST_CASE("API: Stop and resume; the Stop is logged") {
  sim::Simulation s(demo());
  Client c{s, "", "192.168.1.20"};
  REQUIRE(c.login("demo-password"));
  CHECK(c.call("POST", "stop").status == 200);
  CHECK(c.get("state")["stopped"] == true);
  CHECK(c.call("POST", "pumps/2/prime", {{"seconds", 5}}).status == 409);  // nothing runs while stopped
  CHECK(c.call("POST", "resume").status == 200);
  CHECK(c.get("state")["stopped"] == false);
  bool logged = false;
  for (const auto& e : json::parse(c.call("GET", "events", nullptr, {{"limit", "50"}}).body)) logged |= e["title"]["key"] == "ev.stop";
  CHECK(logged);
}

TEST_CASE("API: history of ph, ec, temp and flow only; values while flow is proven (RAT-016)") {
  sim::Simulation s(demo());
  s.step(10 * kMinute);
  Client c{s, "", "192.168.1.20"};
  REQUIRE(c.login("demo-password"));
  CHECK(c.call("GET", "history", nullptr, {{"series", "level"}}).status == 422);
  CHECK(c.call("GET", "history", nullptr, {{"series", "ph,ph"}}).status == 422);
  auto h = c.call("GET", "history", nullptr, {{"series", "ph,ec,temp,flow"}});
  REQUIRE(h.status == 200);
  json j = json::parse(h.body);
  REQUIRE(j["series"].size() == 4);
  int values = 0;
  for (const auto& v : j["series"][0]["avg"]) values += !v.is_null();
  CHECK(values > 0);
}

TEST_CASE("API: origin checks for the transport (DNS rebinding, CSRF) and the session cookie") {
  CHECK(hostAllowed("192.168.1.5:8080"));
  CHECK(hostAllowed("doseph.local"));
  CHECK(hostAllowed("localhost:5173"));
  CHECK_FALSE(hostAllowed("evil.example.com"));
  CHECK(writeAllowed("POST", "same-origin", "", "x"));
  CHECK_FALSE(writeAllowed("POST", "cross-site", "", "x"));
  CHECK_FALSE(writeAllowed("POST", "", "http://evil.example.com", "192.168.1.5"));
  CHECK(writeAllowed("GET", "cross-site", "", "x"));
  CHECK(sessionToken("Bearer abc", "") == "abc");
  CHECK(sessionToken("", "a=1; dp_session=tok; b=2") == "tok");
  CHECK(sessionToken("", "dp_session=one; dp_session=two").empty());
  CHECK(sessionToken("", "xdp_session=tok").empty());
}
