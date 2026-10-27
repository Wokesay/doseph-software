// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>
#include <vector>

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
  for (const char* k : {"system", "tank", "control", "canisters", "recipes", "active", "pumps", "probes"}) CHECK(cfg.contains(k));
  CHECK(c.get("templates")["recipes"].size() == 3);
}

TEST_CASE("API: tank, canisters, recipes and the recipe in force; refusals say why") {
  sim::Simulation s(demo());
  Client c{s, "", "192.168.1.20"};
  REQUIRE(c.login("demo-password"));
  CHECK(c.call("PUT", "tank", {{"volumeL", 0}}).status == 422);
  CHECK(c.call("PUT", "tank", {{"volumeL", "lots"}}).status == 422);
  CHECK(c.call("PUT", "tank", {{"volumeL", 80}, {"waitPhMin", 20}}).status == 200);
  CHECK(c.get("config")["tank"]["volumeL"] == 80);
  CHECK(c.get("state")["newTank"] == true);  // another volume: checked again
  auto b = c.call("POST", "canisters", {{"name", "Bloom PK"}, {"role", "nutrient"}, {"pump", 2}});
  CHECK(b.status == 422);  // pump 2 carries Part A
  b = c.call("POST", "canisters", {{"name", "Bloom PK"}, {"role", "nutrient"}, {"pump", 0}});
  REQUIRE(b.status == 200);
  const std::string pk = json::parse(b.body)["id"];
  auto r = c.call("POST", "recipes", {{"name", "With pH"}, {"steps", {{{"canister", "ph_down"}, {"mlPerL", 0.1}}}}});
  CHECK(r.status == 422);
  CHECK(json::parse(r.body)["error"]["key"] == "cfg.recipe.ph");
  r = c.call("POST", "recipes", {{"name", "PK"}, {"ecTarget", 1.0}, {"ph", {5.8, 6.2}}, {"steps", {{{"canister", pk}, {"mlPerL", 1.0}}}}});
  REQUIRE(r.status == 200);
  const std::string rid = json::parse(r.body)["id"];
  CHECK(c.call("DELETE", "canisters/" + pk).status == 409);  // used by the recipe
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

TEST_CASE("API: the history as CSV in the page's format; a gap stays an empty field (RAT-016)") {
  sim::Simulation s(demo());
  s.step(10 * kMinute);
  Client c{s, "", "192.168.1.20"};
  CHECK(c.call("GET", "export.csv").status == 401);
  REQUIRE(c.login("demo-password"));
  // Stopped, the pump is off: flow is still recorded, pH no longer (RAT-016)
  REQUIRE(c.call("POST", "stop").status == 200);
  s.step(2 * kMinute);
  auto lines = [](const std::string& body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < body.size()) {
      const size_t nl = body.find('\n', pos);
      out.push_back(body.substr(pos, nl - pos));
      pos = nl == std::string::npos ? body.size() : nl + 1;
    }
    return out;
  };
  auto disposition = [](const ApiResponse& r) {
    for (const auto& [k, v] : r.headers)
      if (k == "Content-Disposition") return v;
    return std::string();
  };
  auto en = c.call("GET", "export.csv", nullptr, {{"series", "ph,flow"}, {"lang", "en"}});
  REQUIRE(en.status == 200);
  CHECK(en.contentType == "text/csv; charset=utf-8");
  CHECK(disposition(en).find("doseph-history.csv") != std::string::npos);
  auto rows = lines(en.body);
  REQUIRE(rows.size() > 10);
  CHECK(rows[0] == "time_utc,ph,flow");
  bool shape = true, phValue = false, phGap = false;
  for (size_t i = 1; i < rows.size(); ++i) {
    const std::string& r = rows[i];
    shape = shape && r.find(';') == std::string::npos && r.size() > 20 && r[4] == '-' && r[13] == ':' && r[19] == ',';
    if (!shape) break;
    const std::string ph = r.substr(20, r.find(',', 20) - 20);
    if (ph.empty()) phGap = true;
    else phValue = phValue || ph.find('.') != std::string::npos;  // a decimal point
  }
  CHECK(shape);
  CHECK(phValue);
  CHECK(phGap);  // never 0 for a missing value (RAT-006)
  auto de = c.call("GET", "export.csv", nullptr, {{"series", "ph,flow"}, {"lang", "de"}});
  REQUIRE(de.status == 200);
  CHECK(disposition(de).find("doseph-verlauf.csv") != std::string::npos);
  rows = lines(de.body);
  CHECK(rows[0] == "zeit_utc;ph;flow");
  bool point = false, comma = false;
  for (size_t i = 1; i < rows.size(); ++i) {
    point = point || rows[i].find('.') != std::string::npos;
    comma = comma || rows[i].find(',') != std::string::npos;
  }
  CHECK_FALSE(point);
  CHECK(comma);  // a decimal comma
  // All four series by default; the same names as the history
  CHECK(lines(c.call("GET", "export.csv").body)[0] == "time_utc,ph,ec,temp,flow");
  CHECK(c.call("GET", "export.csv", nullptr, {{"series", "level"}}).status == 422);
  CHECK(c.call("GET", "export.csv", nullptr, {{"series", "ph,ph"}}).status == 422);
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
