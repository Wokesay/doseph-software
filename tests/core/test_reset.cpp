// SPDX-License-Identifier: AGPL-3.0-or-later
// The access reset and the factory reset (SD-139): asked for at the device, or a factory reset by
// the owner with the password.
#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string>

#include "doseph/mqtt.hpp"
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
  CHECK(s.robot().needsSetupCode());
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
  s.step(kSecond);
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
  CHECK(s.robot().needsSetupCode());
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
  s.step(kSecond);
  CHECK(s.robot().deviceId() != id);
  CHECK(owner.call("GET", "state").status == 401);
  CHECK(owner.get("info")["hasPassword"] == false);
  CHECK(logged(s, "ev.system.factory_reset"));
}
