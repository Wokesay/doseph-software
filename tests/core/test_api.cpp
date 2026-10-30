// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <functional>
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
  int status(const std::string& method, const std::string& path, const json& body = nullptr) { return call(method, path, body).status; }
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
  for (const char* k : {"now", "stopped", "online", "feedPump", "flowOk", "pump", "hardware", "readings", "cycle", "watchdog", "latches",
                        "jumpLocks", "newTank", "effects", "dosing", "channels", "stock", "catchUp", "channelJob", "probe",
                        "maintenanceUntil", "eventId"})
    CHECK_MESSAGE(st.contains(k), k);
  for (const char* ch : {"ph", "ec", "temp", "flow"}) {
    REQUIRE(st["readings"].contains(ch));
    for (const char* k : {"value", "quality", "reason", "ageS", "unit", "decimals", "usable"}) CHECK(st["readings"][ch].contains(k));
  }
  for (const char* k : {"phase", "since", "line", "notes", "progress", "round", "last", "settings", "mixPending", "checks"})
    CHECK_MESSAGE(st["cycle"].contains(k), k);
  for (const char* g : {"all", "ec", "ph"}) {
    REQUIRE(st["cycle"]["checks"][g].is_array());
    for (const auto& i : st["cycle"]["checks"][g]) CHECK((i["ok"].is_boolean() && i["key"].is_string() && i["text"].is_string()));
  }
  for (const char* k : {"recipe", "ecTarget", "phTarget", "volumeL", "flowLpm", "turnoverMin", "mixMin", "mixOwnMin",
                        "mixUserMin", "mixWhy", "stableS", "holdMin"})
    CHECK_MESSAGE(st["cycle"]["settings"].contains(k), k);
  CHECK(st["watchdog"].contains("stale"));
  CHECK(st["channels"].size() == 4);
  for (const auto& [id, x] : st["stock"].items()) CHECK((x.contains("ml") && x["status"].is_string()));
  CHECK(c.call("DELETE", "catchup").status == 404);  // nothing to catch up
  json cfg = c.get("config");
  for (const char* k : {"system", "tank", "control", "canisters", "recipes", "active", "channels", "probes"}) CHECK(cfg.contains(k));
  CHECK(c.get("templates")["recipes"].size() == 3);
}

TEST_CASE("API: tank, canisters, recipes and the recipe in force; refusals say why") {
  sim::Simulation s(demo());
  Client c{s, "", "192.168.1.20"};
  REQUIRE(c.login("demo-password"));
  CHECK(c.call("PUT", "tank", {{"volumeL", 0}}).status == 422);
  CHECK(c.call("PUT", "tank", {{"volumeL", "lots"}}).status == 422);
  CHECK(c.call("PUT", "tank", {{"volumeL", 80}, {"mixMinMin", 20}}).status == 200);
  CHECK(c.get("config")["tank"]["volumeL"] == 80);
  CHECK(c.get("state")["newTank"] == true);  // another volume: checked again
  auto b = c.call("POST", "canisters", {{"name", "Bloom PK"}, {"role", "nutrient"}, {"channel", 2}});
  CHECK(b.status == 422);  // pump 2 carries Part A
  b = c.call("POST", "canisters", {{"name", "Bloom PK"}, {"role", "nutrient"}, {"channel", 0}});
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
  CHECK(c.call("POST", "channels/2/prime", {{"seconds", 5}}).status == 409);  // nothing runs while stopped
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

TEST_CASE("API: the diagnostics package of \"Report a problem\" carries the states and events, never a secret") {
  sim::Simulation s(demo());
  s.step(5 * kMinute);
  Client c{s, "", "192.168.1.20"};
  CHECK(c.call("GET", "diagnostics").status == 401);
  REQUIRE(c.login("demo-password"));
  auto r = c.call("GET", "diagnostics");
  REQUIRE(r.status == 200);
  const json d = json::parse(r.body);
  const std::string id = d["reportId"];
  CHECK(id.size() == 11);
  CHECK(id.rfind("DP-", 0) == 0);
  for (const char* k : {"createdAt", "info", "config", "readings", "cycle", "watchdog", "latches", "jumpLocks", "channels", "events", "redacted"})
    CHECK_MESSAGE(d.contains(k), k);
  CHECK(d["info"]["version"].is_string());
  CHECK_FALSE(d["events"].empty());
  CHECK(d["cycle"].contains("checks"));
  // No secret: none of the password file's values (hash, salt, sessions), no address of a client
  json rest = d;
  rest.erase("redacted");  // names what is left out, by key
  const std::string all = rest.dump();
  std::vector<std::string> secrets = {"192.168.1.20", c.token};
  std::function<void(const json&)> collect = [&](const json& j) {
    if (j.is_string() && j.get<std::string>().size() >= 16) secrets.push_back(j.get<std::string>());
    if (j.is_structured())
      for (const auto& v : j) collect(v);
  };
  collect(s.robot().auth().toJson());
  REQUIRE(secrets.size() > 3);
  for (const auto& secret : secrets) CHECK_MESSAGE(all.find(secret) == std::string::npos, secret);
  CHECK(json::parse(c.call("GET", "diagnostics").body)["reportId"] != d["reportId"]);  // a number per report
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

TEST_CASE("API: a backup restores the settings, checked like a change; never the password lock, and learning only where kept (RAT-025, RAT-085)") {
  sim::Simulation s(demo());
  s.step(5 * kMinute);
  Client c{s};
  CHECK(c.call("GET", "config/export").status == 401);
  CHECK(c.call("POST", "config/import", s.robot().configJson()).status == 401);
  REQUIRE(c.login("demo-password"));
  auto r = c.call("GET", "config/export");
  REQUIRE(r.status == 200);
  bool named = false;
  for (const auto& [k, v] : r.headers) named |= k == "Content-Disposition" && v.find("filename=\"doseph-backup-20") != std::string::npos;
  CHECK(named);
  const json backup = json::parse(r.body);
  CHECK(backup == s.robot().configJson());
  const int rev = backup["revision"];

  // Refused, and nothing changes: not a backup, a newer one, a damaged one, one that breaks a rule,
  // one with a calibration that a probe could not have given (RAT-025)
  auto refused = [&](const json& file) {
    auto x = c.call("POST", "config/import", file);
    CHECK(x.status == 422);
    CHECK(s.robot().configJson()["revision"] == rev);
    return json::parse(x.body)["error"]["key"].get<std::string>();
  };
  CHECK(c.call("POST", "config/import", json::array({1, 2})).status == 400);  // the API takes objects only
  CHECK(s.robot().importConfig(json::array({1, 2})).body["error"]["key"] == "config.not_object");
  json f = backup;
  f["schemaVersion"] = kSchemaVersion + 1;
  CHECK(refused(f) == "config.newer");
  f = backup;
  f["canisters"] = "none";
  CHECK(refused(f) == "config.import");
  f = backup;
  f["system"]["timezone"] = "Europe/../passwd";
  CHECK(refused(f) == "cfg.system.timezone");
  f = backup;
  f["probes"]["ph"] = {{"points", {{1.0, 4.0}, {1.2, 7.0}}}};  // a slope no probe has
  CHECK(refused(f) == "cfg.calibration.ph");
  f = backup;
  f["probes"]["ec"] = {{"factor", 5.0}};
  CHECK(refused(f) == "cfg.calibration.ec");

  // An older schema comes in through the same migration as the stored file
  f = backup;
  f["schemaVersion"] = 1;
  f["bottles"] = f["canisters"];
  f.erase("canisters");
  for (auto& b : f["bottles"]) {
    b["kind"] = b["role"];
    b.erase("role");
  }
  for (auto& rc : f["recipes"])
    for (auto& st : rc["steps"]) {
      st["bottle"] = st["canister"];
      st.erase("canister");
    }
  // The file says no password was set: the lock against a second first setup stays all the same
  f["system"]["passwordSet"] = false;
  f["system"]["name"] = "Restored";
  REQUIRE(c.call("POST", "config/import", f).status == 200);
  const json now = s.robot().configJson();
  CHECK(now["revision"] == rev + 1);
  CHECK(now["system"]["name"] == "Restored");
  CHECK(now["system"]["passwordSet"] == true);
  CHECK(now["canisters"] == backup["canisters"]);
  CHECK(now["recipes"] == backup["recipes"]);
  CHECK(now["probes"] == backup["probes"]);
  bool logged = false;
  for (const auto& e : c.get("events")) logged |= e["title"]["key"] == "ev.config.imported";
  CHECK(logged);

  // And the other way round: a file never sets the lock on a robot without a password
  sim::Simulation fresh(newRobot());
  json g = backup;
  g["system"]["passwordSet"] = true;
  REQUIRE(fresh.robot().importConfig(g).status == 200);
  CHECK(fresh.robot().configJson()["system"]["passwordSet"] == false);
}

TEST_CASE("API: a backup is not restored while a channel runs; what the robot learned stays only where the file keeps it (RAT-085)") {
  sim::Simulation s(demo());
  s.step(6 * kHour);
  Robot& robot = s.robot();
  const json backup = robot.configJson();
  REQUIRE_FALSE(robot.runtime().ecEffects.empty());
  const std::string learned = robot.runtime().ecEffects.begin()->first;
  REQUIRE(isNum(robot.runtime().phEffect));

  // The same file keeps everything learned
  const auto effects = robot.runtime().ecEffects;
  REQUIRE(robot.importConfig(backup).status == 200);
  CHECK(robot.runtime().ecEffects == effects);
  CHECK(isNum(robot.runtime().phEffect));

  // A file without that recipe, with another pH− canister: both start afresh
  json f = backup;
  f["active"] = {{"recipe", ""}, {"from", 0}, {"until", 0}};
  json kept = json::array();
  for (const auto& rc : f["recipes"])
    if (rc["id"] != learned) kept.push_back(rc);
  f["recipes"] = kept;
  for (auto& b : f["canisters"])
    if (b["role"] == "ph_down") b["name"] = "pH down, another make";
  REQUIRE(robot.importConfig(f).status == 200);
  CHECK(robot.runtime().ecEffects.count(learned) == 0);
  CHECK_FALSE(isNum(robot.runtime().phEffect));

  // A pump run (or a dose, or a probe calibration) is not cut by a restore
  REQUIRE(robot.prime(1, 30).status == 200);
  CHECK(robot.importConfig(backup).status == 409);
  CHECK(robot.configJson()["recipes"] == kept);
}

TEST_CASE("API: a password file the robot cannot read is kept: no new password over it, no advice to reset, an alarm") {
  sim::Simulation s(demo());
  s.step(kMinute);
  REQUIRE(s.robot().auth().hasPassword());
  const std::string good = *s.storage().read("auth.json");
  for (const std::string damaged : {std::string("{\"salt\": \"ab"), std::string("{}"), std::string("[]")}) {
    CAPTURE(damaged);
    REQUIRE(s.storage().write("auth.json", damaged));
    s.reboot();
    Client c{s};
    CHECK(s.robot().passwordKept());
    const json info = c.get("info");
    CHECK(info["hasPassword"] == false);
    CHECK(info["passwordProblem"]["key"] == "auth.kept");
    CHECK(s.robot().armSetupCode().empty());  // nobody is offered a first password
    auto r = c.call("POST", "auth/setup", {{"password", "a-new-password"}});
    CHECK(r.status == 423);
    CHECK(json::parse(r.body)["error"]["key"] == "auth.kept");
    CHECK_FALSE(c.login("demo-password"));
    CHECK(*s.storage().read("auth.json") == damaged);  // left as it is
    bool alarm = false;
    for (const auto& e : s.robot().events(0, s.robot().now() + 1, "", 50)) alarm |= e["title"]["key"] == "ev.auth.kept";
    CHECK(alarm);
  }
  // Read again after a restart: everything as before
  REQUIRE(s.storage().write("auth.json", good));
  s.reboot();
  CHECK_FALSE(s.robot().passwordKept());
  Client c{s};
  CHECK(c.get("info")["passwordProblem"].is_null());
  CHECK(c.login("demo-password"));
}

TEST_CASE("API: access tokens by role: the app may do all but the password and tokens; the account reads, stops, acknowledges alarms and changes recipes and plants; a peer reads (SD-129)") {
  sim::Simulation s(demo());
  s.step(kMinute);
  Client owner{s};
  REQUIRE(owner.login("demo-password"));
  auto make = [&](const char* name, const char* role) {
    auto r = owner.call("POST", "tokens", {{"name", name}, {"role", role}});
    REQUIRE(r.status == 200);
    const json j = json::parse(r.body);
    CHECK(j["role"] == role);
    return j["token"].get<std::string>();
  };
  Client app{s, make("Phone", "app"), "192.168.1.30"};
  Client account{s, make("Account", "account"), "192.168.1.31"};
  Client peer{s, make("Second device", "peer"), "192.168.1.32"};
  CHECK(owner.status("POST", "tokens", {{"name", "x"}, {"role", "owner"}}) == 422);
  CHECK(owner.status("POST", "tokens", {{"name", " "}, {"role", "app"}}) == 422);

  // Everyone reads
  for (Client* c : {&app, &account, &peer}) {
    CHECK(c->status("GET", "state") == 200);
    CHECK(c->status("GET", "config") == 200);
    CHECK(c->status("GET", "plants") == 200);
    CHECK(c->status("GET", "tokens") == 403);  // only the owner sees the tokens
  }
  const json session = json::parse(account.call("GET", "auth/session").body);
  CHECK(session["authenticated"] == true);
  CHECK(session["role"] == "account");
  for (const auto& [k, v] : account.call("GET", "auth/session").headers) CHECK(k != "Set-Cookie");  // never a token in a cookie
  CHECK(json::parse(owner.call("GET", "auth/session").body)["role"] == "owner");

  // The account: recipes, the recipe in force, plants, Stop, alarms
  const json recipe = json::parse(owner.call("GET", "config").body)["recipes"][0];
  json changed = recipe;
  changed["name"] = "Changed from the account";
  CHECK(account.status("POST", "recipes", changed) == 200);
  CHECK(account.status("PUT", "active", {{"recipe", recipe["id"]}}) == 200);
  CHECK(account.status("POST", "alarms/ack") != 403);  // allowed, whether or not an alarm stands
  // Never limits, releases, calibration, single pumps, settings or a backup
  const auto refused = account.call("PUT", "control", {{"ecTolerance", 0.5}});
  CHECK(refused.status == 403);
  CHECK(json::parse(refused.body)["error"]["key"] == "auth.role");
  CHECK(account.status("POST", "latches/flow.dry/ack") == 403);
  CHECK(account.status("POST", "resume") == 403);
  CHECK(account.status("POST", "channels/1/prime", {{"seconds", 1}}) == 403);
  CHECK(account.status("POST", "channels/1/pump-calibration", {{"method", "scale"}}) == 403);
  CHECK(account.status("POST", "probe", {{"action", "start"}}) == 403);
  CHECK(account.status("PUT", "system", {{"name", "x"}}) == 403);
  CHECK(account.status("PUT", "push", json::object()) == 403);
  CHECK(account.status("POST", "config/import", json::object()) == 403);
  CHECK(account.status("POST", "maintenance", {{"minutes", 10}}) == 403);
  CHECK(account.status("PUT", "tank", {{"volumeL", 50}}) == 403);
  CHECK(account.status("POST", "tokens", {{"name", "x"}, {"role", "app"}}) == 403);
  CHECK(account.status("POST", "stop") == 200);

  // A peer reads only, until the coupling's calls come (#10 step 8)
  CHECK(peer.status("POST", "stop") == 403);
  CHECK(peer.status("PUT", "active", {{"recipe", recipe["id"]}}) == 403);

  // The app: all but the password and the tokens
  CHECK(app.status("POST", "resume") == 200);
  CHECK(app.status("PUT", "control", {{"ecTolerance", 0.5}}) == 200);
  CHECK(app.status("PUT", "auth/password", {{"old", "demo-password"}, {"new", "another-password"}}) == 403);
  CHECK(app.status("POST", "tokens", {{"name", "x"}, {"role", "app"}}) == 403);
  CHECK(app.status("DELETE", "tokens/x") == 403);

  // The owner sees the list, never a token; a revoked token is refused at once
  const json list = owner.get("tokens");
  REQUIRE(list["tokens"].size() == 3);
  CHECK(list["tokens"].dump().find(app.token.substr(4)) == std::string::npos);
  CHECK(list["tokens"][0]["used"].is_number());
  CHECK(owner.status("DELETE", "tokens/" + list["tokens"][1]["id"].get<std::string>()) == 200);
  CHECK(account.status("GET", "state") == 401);
  CHECK(owner.status("DELETE", "tokens/" + list["tokens"][1]["id"].get<std::string>()) == 404);
  CHECK(app.status("GET", "state") == 200);

  // A restart keeps the tokens; a password change ends the sessions, not the tokens
  s.reboot();
  CHECK(app.status("GET", "state") == 200);
  CHECK(peer.status("GET", "state") == 200);
  Client again{s};
  REQUIRE(again.login("demo-password"));
  CHECK(again.status("PUT", "auth/password", {{"old", "demo-password"}, {"new", "another-password"}}) == 200);
  CHECK(again.status("GET", "state") == 401);
  CHECK(app.status("GET", "state") == 200);

  // Created and revoked are events, with the name and the role, never the token
  int created = 0, revoked = 0;
  for (const auto& e : s.robot().events(0, s.robot().now() + 1, "auth", 50)) {
    created += e["title"]["key"] == "ev.auth.token_created";
    revoked += e["title"]["key"] == "ev.auth.token_revoked";
    CHECK(e.dump().find(app.token.substr(4)) == std::string::npos);
  }
  CHECK(created == 3);
  CHECK(revoked == 1);
}

TEST_CASE("API: the device ID is random, made once and kept across a restart; never in a backup (SD-129)") {
  sim::Simulation s(demo());
  Client c{s};
  const std::string id = c.get("info")["deviceId"];
  CHECK(id.size() == 16);
  CHECK(id.find_first_not_of("0123456789abcdef") == std::string::npos);
  s.reboot();
  CHECK(c.get("info")["deviceId"] == id);
  REQUIRE(c.login("demo-password"));
  CHECK(c.call("GET", "config/export").body.find(id) == std::string::npos);
  CHECK(json::parse(c.call("GET", "diagnostics").body)["info"]["deviceId"] == id);
  // A damaged identity file gives a new one, and that one is kept
  REQUIRE(s.storage().write("device.json", "{\"id\": \"XYZ\"}"));
  s.reboot();
  const std::string fresh = c.get("info")["deviceId"];
  CHECK(fresh != id);
  CHECK(fresh.size() == 16);
  s.reboot();
  CHECK(c.get("info")["deviceId"] == fresh);
}

TEST_CASE("API: commissioning has the steps of #10 in order and resumes after a restart; the robot's own steps are done while it sees them, the user confirms or skips the others (SD-129)") {
  sim::Simulation s(newRobot());
  const std::string code = s.robot().armSetupCode();
  Client c{s};
  REQUIRE(c.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", code}}).status == 200);
  REQUIRE(c.login("a-long-password"));
  auto stateOf = [](const json& su, const std::string& id) {
    for (const auto& st : su["steps"])
      if (st["id"] == id) return st["state"].get<std::string>();
    return std::string();
  };
  json su = c.get("commissioning");
  std::vector<std::string> ids;
  for (const auto& st : su["steps"]) ids.push_back(st["id"]);
  CHECK(ids == std::vector<std::string>{"password", "network", "placement", "flow", "probes", "channels", "lines", "test", "tank", "push", "account"});
  CHECK(stateOf(su, "password") == "done");
  CHECK(stateOf(su, "network") == "later");
  CHECK(stateOf(su, "flow") == "open");
  CHECK(su["next"] == "placement");
  CHECK(su["complete"] == false);
  CHECK(c.get("state")["setup"] == json{{"complete", false}, {"next", "placement"}});

  // A step the robot sees is never confirmed by hand; only an optional one is skipped
  CHECK(json::parse(c.call("PUT", "commissioning/flow", {{"state", "done"}}).body)["error"]["key"] == "setup.seen");
  CHECK(json::parse(c.call("PUT", "commissioning/placement", {{"state", "skipped"}}).body)["error"]["key"] == "setup.needed");
  CHECK(c.status("PUT", "commissioning/nope", {{"state", "done"}}) == 404);
  CHECK(c.status("PUT", "commissioning/placement", {{"state", "maybe"}}) == 422);
  REQUIRE(c.status("PUT", "commissioning/placement", {{"state", "done"}}) == 200);
  REQUIRE(c.status("PUT", "commissioning/push", {{"state", "skipped"}}) == 200);
  CHECK(c.get("commissioning")["next"] == "flow");

  // A pH− channel calibrated, and its line filled into the tank: the robot sees both steps
  CHECK(stateOf(c.get("commissioning"), "channels") == "open");
  REQUIRE(s.robot().putCanister({{"id", "ph_down"}, {"name", "pH down"}, {"role", "ph_down"}, {"channel", 1}}).status == 200);
  REQUIRE(s.robot().setChannelFlow(1, 50).status == 200);
  CHECK(stateOf(c.get("commissioning"), "channels") == "done");
  REQUIRE(c.status("POST", "channels/1/prime", json::object()) == 200);
  s.step(12 * kSecond, 500);

  // A restart resumes where it stood
  s.reboot();
  REQUIRE(c.login("a-long-password"));
  su = c.get("commissioning");
  CHECK(stateOf(su, "placement") == "done");
  CHECK(su["steps"][2]["at"].get<Epoch>() > 0);
  CHECK(stateOf(su, "push") == "skipped");
  CHECK(stateOf(su, "channels") == "done");
  CHECK(stateOf(su, "lines") == "done");
  CHECK(su["next"] == "flow");
  // Opened again, it is the next step once more
  REQUIRE(c.status("PUT", "commissioning/placement", {{"state", "open"}}) == 200);
  CHECK(c.get("commissioning")["next"] == "placement");
}

TEST_CASE("API: the demo is set up as a device in use: commissioning is complete once flow is proven (SD-129)") {
  sim::Simulation s(demo());
  Client c{s};
  REQUIRE(c.login("demo-password"));
  s.step(10 * kMinute);
  const json su = c.get("commissioning");
  CHECK(su["complete"] == true);
  CHECK(su["next"].is_null());
}

TEST_CASE("API: before the placement is confirmed nothing doses and the check says why; a backup never brings the confirmation (SD-118, SD-129)") {
  sim::Simulation s(demo());
  Client c{s};
  REQUIRE(c.login("demo-password"));
  const json backup = json::parse(c.call("GET", "config/export").body);
  CHECK(backup["setup"]["placement"]["state"] == "done");
  REQUIRE(c.status("PUT", "commissioning/placement", {{"state", "open"}}) == 200);
  const json stock = c.get("state")["stock"];
  s.step(30 * kMinute);
  const json st = c.get("state");
  CHECK(st["cycle"]["line"]["key"] == "cycle.off.placement");
  bool listed = false;
  for (const auto& item : st["cycle"]["checks"]["all"]) listed |= item["key"] == "cycle.off.placement" && item["ok"] == false;
  CHECK(listed);
  CHECK(st["stock"] == stock);  // nothing ran towards the tank
  CHECK(c.status("POST", "channels/2/prime", {{"seconds", 5}}) == 409);  // a line fill into the tank waits too
  // The file says it was confirmed; the robot keeps its own word
  REQUIRE(c.status("POST", "config/import", backup) == 200);
  CHECK(c.get("commissioning")["next"] == "placement");
  REQUIRE(c.status("PUT", "commissioning/placement", {{"state", "done"}}) == 200);
  bool logged = false;
  for (const auto& e : s.robot().events(0, s.robot().now() + 1, "config", 20)) logged |= e["title"]["key"] == "ev.setup.placement";
  CHECK(logged);
}
