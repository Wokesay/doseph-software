// SPDX-License-Identifier: AGPL-3.0-or-later
// Network devices (SD-132): Shelly Gen2+ outputs switched only after their safety settings were
// read back, values from a Shelly, ESPHome or MQTT, and never a value that is not there.
#include <doctest/doctest.h>

#include <functional>
#include <string>

#include "doseph/api.hpp"
#include "doseph/devices.hpp"
#include "doseph/messages.hpp"
#include "simulation.hpp"

using namespace doseph;

namespace {

sim::Options demo() {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  return o;
}

bool runUntil(sim::Simulation& s, const std::function<bool()>& done, double maxS) {
  for (double t = 0; t < maxS; ++t) {
    if (done()) return true;
    s.step(kSecond);
  }
  return done();
}

json role(sim::Simulation& s, const std::string& id) { return s.robot().devices()["roles"][id]; }
json deviceStatus(sim::Simulation& s, const std::string& id) {
  const json all = s.robot().devices();
  for (const auto& d : all["devices"])
    if (d["id"] == id) return d;
  return nullptr;
}

const json* item(const json& watchdog, const std::string& id) {
  for (const auto& i : watchdog["items"])
    if (i["id"] == id) return &i;
  return nullptr;
}

// Adds the relay of the twin and binds the refill valve to its output
void bindValve(sim::Simulation& s) {
  REQUIRE(s.robot().addDevice({{"kind", "shelly"}, {"address", "shelly-1.local"}, {"name", "Valve"}}).status == 200);
  REQUIRE(s.robot().putRole("refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}}).status == 200);
}

}  // namespace

TEST_CASE("Devices: a Shelly output gets off after power loss, never on by itself, not switched by its own input, and a pulse role an auto-off at 1.11 times its longest run (SD-132, RAT-019)") {
  const RoleDef* valve = roleDef("refill_valve");
  const RoleDef* pump = roleDef("circulation_pump");
  REQUIRE(valve);
  REQUIRE(pump);
  CHECK(autoOffS(*valve) == 28 * 60);  // 25 min × 1.11, a whole minute up
  CHECK(autoOffS(*pump) == 0);
  const json relay = {{"initial_state", "restore_last"}, {"auto_on", false}, {"auto_off", false}, {"auto_off_delay", 60.0}, {"in_mode", "follow"}};
  const json plug = {{"initial_state", "restore_last"}, {"auto_on", false}, {"auto_off", false}, {"auto_off_delay", 60.0}};
  const json w = shellySwitchConfig(*valve, relay);
  CHECK(w["initial_state"] == "off");
  CHECK(w["auto_on"] == false);
  CHECK(w["auto_off"] == true);
  CHECK(w["auto_off_delay"] == 1680);
  CHECK(w["in_mode"] == "detached");
  const json wp = shellySwitchConfig(*pump, plug);
  CHECK_FALSE(wp.contains("in_mode"));  // a plug has no input of its own
  CHECK(wp["auto_off"] == false);
  CHECK_FALSE(wp.contains("auto_off_delay"));
  CHECK_FALSE(shellyConfigFits(w, relay));
  json back = relay;
  for (const auto& [k, v] : w.items()) back[k] = v;
  back["auto_off_delay"] = 1680.0;  // a device gives numbers back as it likes
  CHECK(shellyConfigFits(w, back));
  back["initial_state"] = "on";
  CHECK_FALSE(shellyConfigFits(w, back));
}

TEST_CASE("Devices: values from payloads: words, numbers and JSON fields; none rather than a stand-in (R5)") {
  CHECK(binaryOf("ON") == true);
  CHECK(binaryOf("off") == false);
  CHECK(binaryOf(true) == true);
  CHECK(binaryOf(1) == true);
  CHECK(binaryOf(0) == false);
  CHECK_FALSE(binaryOf(2).has_value());
  CHECK_FALSE(binaryOf("maybe").has_value());
  CHECK_FALSE(binaryOf(nullptr).has_value());
  CHECK(mqttValueOf("ON", "") == json("ON"));
  CHECK(mqttValueOf("21.5", "") == json(21.5));
  CHECK(mqttValueOf(R"({"float": {"full": true}})", "float.full") == json(true));
  CHECK_FALSE(mqttValueOf(R"({"float": {}})", "float.full").has_value());
  CHECK_FALSE(mqttValueOf(R"({"t": null})", "t").has_value());
  CHECK_FALSE(mqttValueOf("not json", "t").has_value());
}

TEST_CASE("Devices: the configuration: an output only on a Shelly, one role per output, entities and topics checked; devices and roles are kept with it (SD-132)") {
  Config c;
  c.devices = {{"d1", "shelly", "shelly-1.local", "Valve"}, {"d2", "esphome", "esp-1.local", "Sensors"}};
  c.roles["refill_valve"] = {"shelly", "d1", 0, "", "", false, ""};
  c.roles["tank_full"] = {"esphome", "d2", 0, "binary_sensor/tank_full", "", false, ""};
  CHECK(validateConfig(c, 4).empty());
  const Config back = configFromJson(json(c));
  CHECK(back.devices.size() == 2);
  CHECK(back.roles.at("tank_full").entity == "binary_sensor/tank_full");
  CHECK(back.device("d2")->kind == "esphome");
  auto firstKey = [](const Config& x) {
    const auto errs = validateConfig(x, 4);
    return errs.empty() ? std::string() : errs[0].key;
  };
  Config bad = c;
  bad.roles["refill_valve"] = {"esphome", "d2", 0, "binary_sensor/x", "", false, ""};
  CHECK(firstKey(bad) == "cfg.role.output_shelly");
  bad = c;
  bad.roles["refill_valve"] = {"mqtt", "", 0, "tank/valve", "", false, ""};
  CHECK(firstKey(bad) == "cfg.role.output_shelly");
  bad = c;
  bad.roles["circulation_pump"] = {"shelly", "d1", 0, "", "", false, ""};
  CHECK(firstKey(bad) == "cfg.role.shared");
  bad = c;
  bad.roles["tank_full"] = {"esphome", "d2", 0, "switch/x", "", false, ""};
  CHECK(firstKey(bad) == "cfg.role.entity");
  bad = c;
  bad.roles["tank_full"] = {"mqtt", "", 0, "tank/#", "", false, ""};
  CHECK(firstKey(bad) == "cfg.role.topic");
  bad = c;
  bad.roles["tank_full"] = {"shelly", "d2", 0, "", "", false, ""};
  CHECK(firstKey(bad) == "cfg.role.device");
  bad = c;
  bad.roles["heater"] = {"shelly", "d1", 1, "", "", false, ""};
  CHECK(firstKey(bad) == "cfg.role.unknown");
  bad = c;
  bad.devices[0].address = "shelly 1";
  CHECK(firstKey(bad) == "cfg.device.address");
  bad = c;
  bad.devices[1].kind = "tasmota";
  CHECK(firstKey(bad) == "cfg.device.kind");
}

TEST_CASE("Devices: a Shelly relay bound as the refill valve gets its settings written and read back; it switches on only after a check, off at once, and again while it says on (SD-132, RAT-019)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  REQUIRE(relay);
  REQUIRE(runUntil(s, [&] { return role(s, "refill_valve")["output"]["verified"] == true; }, 30));
  const json cfg = relay->switches[0].config;
  CHECK(cfg["initial_state"] == "off");
  CHECK(cfg["auto_on"] == false);
  CHECK(cfg["in_mode"] == "detached");
  CHECK(cfg["auto_off"] == true);
  CHECK(cfg["auto_off_delay"] == 1680);
  CHECK(deviceStatus(s, "d1")["ownId"] == "shellyplus1-sim000001");
  CHECK_FALSE(relay->switches[0].on);
  // On: only after its settings were read back again
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 10));
  // Off at once
  s.robot().wantOutput("refill_valve", false);
  s.step(kSecond);
  CHECK_FALSE(relay->switches[0].on);
  // Someone switches it on in the maker's app: off again within a poll
  relay->switches[0].on = true;
  REQUIRE(runUntil(s, [&] { return !relay->switches[0].on; }, 12));
  // Its settings changed meanwhile: written once more before it switches on
  relay->switches[0].config["initial_state"] = "restore_last";
  relay->switches[0].config["auto_off"] = false;
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 10));
  CHECK(relay->switches[0].config["initial_state"] == "off");
  CHECK(relay->switches[0].config["auto_off"] == true);
  // A Stop switches it off, and nothing is wanted any more
  REQUIRE(s.robot().stop(say("stop.app")).status == 200);
  s.step(kSecond);
  CHECK_FALSE(relay->switches[0].on);
  REQUIRE(s.robot().resume().status == 200);
  s.step(10 * kSecond);
  CHECK_FALSE(relay->switches[0].on);
}

TEST_CASE("Devices: an output whose settings do not hold is not switched on, and the watchdog says why (SD-132, RAT-019)") {
  sim::Simulation s(demo());
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  relay->ignoresConfig = true;
  bindValve(s);
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return role(s, "refill_valve")["output"]["state"] == "refused"; }, 30));
  CHECK_FALSE(relay->switches[0].on);
  CHECK(role(s, "refill_valve")["output"]["problem"]["key"] == "device.settings_differ");
  s.step(6 * kSecond);
  const json wd = s.robot().state()["watchdog"];
  const json* it = item(wd, "role.refill_valve");
  REQUIRE(it);
  CHECK((*it)["status"] == "problem");
  CHECK((*it)["text"]["key"] == "device.settings_differ");
  // Fixed: the next try after a minute writes and reads them back, and it switches on
  relay->ignoresConfig = false;
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 90));
}

TEST_CASE("Devices: a device away is offline; back after a power loss its settings are checked again; a restart of the robot switches its outputs off (SD-132, RAT-024, R6)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  REQUIRE(s.control("netOnline", {{"address", "shelly-1.local"}, {"online", false}}).is_object());
  REQUIRE(runUntil(s, [&] { return role(s, "refill_valve")["output"]["state"] == "offline"; }, 40));
  s.step(6 * kSecond);
  const json wd = s.robot().state()["watchdog"];
  const json* it = item(wd, "role.refill_valve");
  REQUIRE(it);
  CHECK((*it)["status"] == "problem");
  // Back: its initial state "off" holds after the power loss, and the robot checks before it switches on again
  s.control("netOnline", {{"address", "shelly-1.local"}, {"online", true}});
  CHECK_FALSE(relay->switches[0].on);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  // The robot restarts: everything off, nothing resumed
  s.reboot();
  s.step(2 * kSecond);
  CHECK_FALSE(relay->switches[0].on);
  s.step(30 * kSecond);
  CHECK_FALSE(relay->switches[0].on);
}

TEST_CASE("Devices: a first-generation Shelly is not used, and an address nobody answers at says so (SD-132)") {
  sim::Simulation s(demo());
  REQUIRE(s.robot().addDevice({{"kind", "shelly"}, {"address", "shelly-old.local"}, {"name", "Old"}}).status == 200);
  REQUIRE(s.robot().addDevice({{"kind", "shelly"}, {"address", "nobody.local"}, {"name", "Gone"}}).status == 200);
  s.step(3 * kSecond);
  CHECK(deviceStatus(s, "d1")["problem"]["key"] == "device.shelly_gen1");
  CHECK(deviceStatus(s, "d1")["online"] == false);
  CHECK(deviceStatus(s, "d2")["problem"]["key"] == "device.unreachable");
  REQUIRE(s.robot().putRole("refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}}).status == 200);
  s.robot().wantOutput("refill_valve", true);
  s.step(10 * kSecond);
  CHECK(role(s, "refill_valve")["output"]["state"] == "offline");
}

TEST_CASE("Devices: the float switch from a Shelly input, ESPHome or MQTT; offline, stale or unreadable it has no value (SD-132, RAT-024, R5)") {
  sim::Simulation s(demo());
  CHECK(role(s, "tank_full")["state"] == "not_assigned");
  // A Shelly input, read the other way round
  REQUIRE(s.robot().addDevice({{"kind", "shelly"}, {"address", "shelly-1.local"}, {"name", "Relay"}}).status == 200);
  REQUIRE(s.robot().putRole("tank_full", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}, {"invert", true}}).status == 200);
  REQUIRE(runUntil(s, [&] { return role(s, "tank_full")["state"] == "ok"; }, 15));
  CHECK(role(s, "tank_full")["value"] == true);
  s.control("netInput", {{"address", "shelly-1.local"}, {"channel", 0}, {"state", true}});
  REQUIRE(runUntil(s, [&] { return role(s, "tank_full")["value"] == false; }, 10));
  // ESPHome
  REQUIRE(s.robot().addDevice({{"kind", "esphome"}, {"address", "esp-1.local"}, {"name", "Sensors"}}).status == 200);
  REQUIRE(s.robot().putRole("tank_full", {{"source", "esphome"}, {"device", "d2"}, {"entity", "binary_sensor/tank_full"}}).status == 200);
  REQUIRE(runUntil(s, [&] { return role(s, "tank_full")["state"] == "ok"; }, 15));
  CHECK(role(s, "tank_full")["value"] == false);
  s.control("netValue", {{"address", "esp-1.local"}, {"entity", "binary_sensor/tank_full"}, {"value", true}});
  REQUIRE(runUntil(s, [&] { return role(s, "tank_full")["value"] == true; }, 10));
  s.control("netOnline", {{"address", "esp-1.local"}, {"online", false}});
  REQUIRE(runUntil(s, [&] { return role(s, "tank_full")["state"] != "ok"; }, 45));
  CHECK(role(s, "tank_full")["value"].is_null());
  // MQTT: only while connected, and while its availability says online
  REQUIRE(s.robot().putRole("tank_full", {{"source", "mqtt"}, {"entity", "tank/full"}, {"availability", "tank/status"}}).status == 200);
  CHECK(s.robot().mqttValueTopics() == std::vector<std::string>{"tank/full", "tank/status"});
  CHECK(role(s, "tank_full")["state"] == "offline");
  s.robot().setMqttStatus(true, {});
  s.robot().mqttValue("tank/full", "ON");
  CHECK(role(s, "tank_full")["state"] == "offline");  // its availability has not said online
  s.robot().mqttValue("tank/status", "online");
  CHECK(role(s, "tank_full")["value"] == true);
  s.robot().mqttValue("tank/full", "half");
  CHECK(role(s, "tank_full")["state"] == "unreadable");
  CHECK(role(s, "tank_full")["value"].is_null());
  s.robot().mqttValue("tank/status", "offline");
  CHECK(role(s, "tank_full")["state"] == "offline");
  s.robot().mqttValue("other/topic", "ON");  // a topic no role reads is not kept
  s.robot().setMqttStatus(false, {});
  CHECK(role(s, "tank_full")["state"] == "offline");
}

TEST_CASE("Devices: removing a device switches its output off first and releases its roles; a password is kept apart, never in the configuration or the API; only a session or an app token manages them (SD-132)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  REQUIRE(s.robot().putDeviceCredentials("d1", {{"password", "relay-secret"}}).status == 200);
  CHECK(deviceStatus(s, "d1")["hasPassword"] == true);
  CHECK(s.robot().devices().dump().find("relay-secret") == std::string::npos);
  CHECK(json(s.robot().config()).dump().find("relay-secret") == std::string::npos);
  // The requests carry it, with Shelly's own user
  NetDevices dev;
  dev.setCredentials("d1", "ignored", "relay-secret");
  dev.tick(s.robot().config(), 0);
  const auto reqs = dev.take();
  REQUIRE_FALSE(reqs.empty());
  CHECK(reqs[0].user == "admin");
  CHECK(reqs[0].password == "relay-secret");
  // Removed: off first, and its role goes with it
  REQUIRE(s.robot().removeDevice("d1").status == 200);
  s.step(kSecond);
  CHECK_FALSE(relay->switches[0].on);
  CHECK_FALSE(s.robot().config().roles.count("refill_valve"));
  CHECK(role(s, "refill_valve")["output"]["state"] == "not_assigned");
  CHECK(s.robot().removeDevice("d1").status == 404);
  for (const char* m : {"POST", "DELETE", "PUT"}) {
    CHECK(permitted(Role::App, m, {"devices"}) == true);
    CHECK_FALSE(permitted(Role::Account, m, {"devices", "d1"}));
    CHECK_FALSE(permitted(Role::Account, m, {"roles", "refill_valve"}));
    CHECK_FALSE(permitted(Role::Peer, m, {"roles", "refill_valve"}));
  }
}
