// SPDX-License-Identifier: AGPL-3.0-or-later
// The access reset and the factory reset (SD-139): asked for at the device, or a factory reset by
// the owner with the password.
#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string>

#include "doseph/mqtt.hpp"
#include "harness.hpp"
#include "simulation.hpp"

using namespace doseph;

namespace {

struct Client {
  sim::Simulation& s;
  std::string token;
  std::string ip = "192.168.1.20";
  ApiResponse call(const std::string& method, const std::string& path, const json& body = nullptr) {
    ApiRequest r;
    r.method = method;
    r.path = "/api/v1/" + path;
    r.body = body.is_null() ? "" : body.dump();
    r.token = token;
    r.client = clientKey(ip);
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

sim::Options demo() {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  return o;
}

bool logged(sim::Simulation& s, const std::string& key) {
  const json ev = s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 1000);
  return std::any_of(ev.begin(), ev.end(), [&](const json& e) { return e["title"]["key"] == key; });
}

}  // namespace

TEST_CASE("Access reset at the device: the password, every session, every access token and MQTT commands go; dosing and the settings stay; the next password is a first one again (SD-139)") {
  sim::Simulation s(demo());
  s.step(10 * kSecond);
  Client owner{s};
  REQUIRE(owner.login("demo-password"));
  auto made = owner.call("POST", "tokens", {{"name", "Phone"}, {"role", "app"}});
  REQUIRE(made.status == 200);
  Client app{s, json::parse(made.body)["token"].get<std::string>()};
  CHECK(app.call("GET", "state").status == 200);
  REQUIRE(s.robot().putMqtt({{"enabled", true}, {"host", "broker.local"}, {"port", 8883}, {"tls", true}, {"user", "doseph"}, {"password", "secret-pass"}, {"commands", true}}).status == 200);
  REQUIRE(mqttCommands(s.robot().mqtt()));
  const size_t recipes = s.robot().config().recipes.size();
  const bool output = s.world().output;

  s.hardware().askReset(DeviceReset::Access);
  s.step(kSecond);
  CHECK(owner.call("GET", "state").status == 401);
  CHECK(app.call("GET", "state").status == 401);
  CHECK(owner.get("info")["hasPassword"] == false);
  CHECK_FALSE(s.robot().config().system.passwordSet);
  CHECK_FALSE(s.storage().read("auth.json"));
  CHECK_FALSE(mqttCommands(s.robot().mqtt()));
  CHECK(s.robot().mqtt().enabled);  // the values still go out
  // Dosing and the settings stay
  CHECK(s.robot().config().recipes.size() == recipes);
  CHECK(s.world().output == output);
  CHECK(logged(s, "ev.auth.access_reset"));
  // A first password again; the old one opens nothing
  CHECK(owner.call("POST", "auth/setup", {{"password", "a-new-long-password"}}).status == 200);
  CHECK_FALSE(owner.login("demo-password"));
  CHECK(owner.login("a-new-long-password"));
  CHECK(json::parse(owner.call("GET", "tokens").body)["tokens"].empty());
  CHECK(s.robot().config().system.passwordSet);
  // It holds across a restart
  s.reboot();
  Client again{s};
  CHECK(again.login("a-new-long-password"));
}

TEST_CASE("Factory reset at the device: everything off, every file removed; the device starts as new with a new ID, and its first event says so (SD-139, R6)") {
  sim::Simulation s(demo());
  bool on = false;
  for (int i = 0; i < 600 && !on; ++i) {
    s.step(kSecond);
    on = s.world().output;
  }
  REQUIRE(on);  // the feed pump runs for the check
  const std::string id = s.robot().deviceId();
  REQUIRE_FALSE(s.robot().config().recipes.empty());

  s.hardware().askReset(DeviceReset::Factory);
  s.step(5 * kSecond);
  CHECK_FALSE(s.world().output);
  CHECK(s.robot().deviceId() != id);
  CHECK(s.robot().deviceId().size() == 16);
  CHECK_FALSE(s.robot().auth().hasPassword());
  CHECK_FALSE(s.robot().config().system.passwordSet);
  CHECK(s.robot().config().recipes.empty());
  CHECK(s.robot().config().plants.empty());
  CHECK_FALSE(s.storage().read("reset.json"));
  CHECK(logged(s, "ev.system.factory_reset"));
  CHECK_FALSE(logged(s, "ev.auth.signed_in"));  // the old log is gone
  // Commissioning starts from the beginning
  CHECK(s.robot().setup()["complete"] == false);
  // As new it stays across a restart
  s.reboot();
  CHECK_FALSE(s.robot().auth().hasPassword());
  CHECK(s.robot().deviceId() != id);
}

TEST_CASE("Factory reset by the owner: with the password, from a session only; a wrong password counts as a failed sign-in; while the device restarts nothing is taken (SD-139)") {
  sim::Simulation s(demo());
  s.step(10 * kSecond);
  Client owner{s};
  REQUIRE(owner.login("demo-password"));
  auto made = owner.call("POST", "tokens", {{"name", "Phone"}, {"role", "app"}});
  Client app{s, json::parse(made.body)["token"].get<std::string>()};
  CHECK(app.call("POST", "system/factory-reset", {{"password", "demo-password"}}).status == 403);
  CHECK(owner.call("POST", "system/factory-reset", {{"password", "wrong-password"}}).status == 403);
  CHECK(logged(s, "ev.auth.failed"));
  const std::string id = s.robot().deviceId();
  auto res = owner.call("POST", "system/factory-reset", {{"password", "demo-password"}});
  REQUIRE(res.status == 200);
  CHECK(json::parse(res.body)["restarting"] == true);
  CHECK(owner.call("GET", "state").status == 503);
  s.step(5 * kSecond);
  CHECK(s.robot().deviceId() != id);
  CHECK(owner.call("GET", "state").status == 401);
  CHECK(owner.get("info")["hasPassword"] == false);
  CHECK(logged(s, "ev.system.factory_reset"));
}

TEST_CASE("Access reset where the device shows setup codes: the code is armed with the reset, so no first password is set without it; a pairing code armed before is void (SD-139, SD-129, SD-131)") {
  sim::Options o = demo();
  o.showSetupCodes = true;
  sim::Simulation s(o);
  s.step(10 * kSecond);
  CHECK(s.robot().takeCodeToShow().empty());  // a password is set: no code
  const Result pairing = s.robot().armPairingCode();
  REQUIRE(pairing.status == 200);
  const std::string pairCode = pairing.body["code"];
  s.hardware().askReset(DeviceReset::Access);
  s.step(kSecond);
  Client c{s};
  CHECK(c.call("POST", "auth/setup", {{"password", "a-new-long-password"}}).status == 403);
  const std::string code = s.robot().takeCodeToShow();
  REQUIRE_FALSE(code.empty());
  CHECK(c.call("POST", "auth/setup", {{"password", "a-new-long-password"}, {"code", code}}).status == 200);
  // The pairing code armed before the reset opens nothing
  CHECK(c.call("POST", "coupling/join", {{"code", pairCode}, {"name", "Other"}, {"address", "10.0.0.9"}}).status == 403);
  // A first start shows its code too
  sim::Options fresh;
  fresh.scenario = "new";
  fresh.showSetupCodes = true;
  sim::Simulation n(fresh);
  CHECK_FALSE(n.robot().takeCodeToShow().empty());
}

TEST_CASE("Factory reset: a network device's output is switched off before the files go, even one that runs through a Stop (SD-139, R6)") {
  harness::Run r(harness::device());
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-plug.local"}, {"name", "Circulation"}});
  r.ok("PUT", "roles/circulation_pump", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.ok("PUT", "circulation", {{"mode", "always"}});
  auto runs = [&] { return r.s.net().shelly("shelly-plug.local")->switches[0].on; };
  r.waitUntil("the pump runs", runs, 30);
  const std::string id = r.s.robot().deviceId();
  r.s.hardware().askReset(DeviceReset::Factory);
  r.step(5 * kSecond);
  CHECK_FALSE(runs());
  CHECK(r.s.robot().deviceId() != id);
  CHECK(r.s.robot().config().roles.empty());
  // A device that does not answer holds the reset up a minute at most
  harness::Run late(harness::device());
  late.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-plug.local"}, {"name", "Circulation"}});
  late.ok("PUT", "roles/circulation_pump", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  late.ok("PUT", "circulation", {{"mode", "always"}});
  late.waitUntil("the pump runs", [&] { return late.s.net().shelly("shelly-plug.local")->switches[0].on; }, 30);
  const std::string lateId = late.s.robot().deviceId();
  late.act("netOnline", {{"address", "shelly-plug.local"}, {"online", false}});
  late.s.hardware().askReset(DeviceReset::Factory);
  late.step(30 * kSecond);
  CHECK(late.s.robot().deviceId() == lateId);  // still switching off
  late.step(40 * kSecond);
  CHECK(late.s.robot().deviceId() != lateId);
}

TEST_CASE("Factory reset: one cut short by a power loss goes on at the next start; once cleared, the storage takes nothing more but its mark (SD-139)") {
  sim::Simulation s(demo());
  s.step(10 * kSecond);
  const std::string id = s.robot().deviceId();
  REQUIRE(s.storage().write("reset.json", json{{"at", 1}, {"by", "device"}, {"state", "pending"}}.dump()));
  s.reboot();
  s.step(2 * kSecond);
  CHECK(s.robot().deviceId() != id);
  CHECK_FALSE(s.robot().auth().hasPassword());
  CHECK(logged(s, "ev.system.factory_reset"));

  MemoryStorage m;
  SealableStorage store(m);
  REQUIRE(store.write("config.json", "{}"));
  REQUIRE(store.clear("reset.json"));
  store.seal("reset.json");
  CHECK_FALSE(store.write("config.json", "{}"));
  CHECK_FALSE(store.append("events.log", "x"));
  CHECK(store.remove("reset.json"));  // the mark itself may still change
  CHECK_FALSE(store.clear("reset.json"));
  CHECK(store.write("reset.json", "{}"));
  CHECK_FALSE(m.read("config.json"));
}
