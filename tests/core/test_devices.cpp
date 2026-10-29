// SPDX-License-Identifier: AGPL-3.0-or-later
// Network devices (SD-132): Shelly Gen2+ outputs switched only after their safety settings were
// read back, values from a Shelly, ESPHome or MQTT, and never a value that is not there.
#include <doctest/doctest.h>

#include <functional>
#include <limits>
#include <string>

#include "doseph/api.hpp"
#include "doseph/devices.hpp"
#include "doseph/gateway.hpp"
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

// Adds the relay of the twin and binds the refill valve to its output, and the float switch, which
// the valve needs (RAT-032), to its input: it says "not full"
void bindValve(sim::Simulation& s) {
  REQUIRE(s.robot().addDevice({{"kind", "shelly"}, {"address", "shelly-1.local"}, {"name", "Valve"}}).status == 200);
  REQUIRE(s.robot().putRole("refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}}).status == 200);
  REQUIRE(s.robot().putRole("tank_full", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}}).status == 200);
  s.step(kSecond);  // the gateway takes the binding up; a want from before it counts for nothing
  // A want while the float has no value yet is dropped (RAT-032)
  for (int i = 0; i < 30 && role(s, "tank_full")["state"] != "ok"; ++i) s.step(kSecond);
  REQUIRE(role(s, "tank_full")["state"] == "ok");
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
  // The float on its input went with it, so the valve opens only when wanted anew (RAT-032)
  s.control("netOnline", {{"address", "shelly-1.local"}, {"online", true}});
  CHECK_FALSE(relay->switches[0].on);
  s.step(20 * kSecond);
  CHECK_FALSE(relay->switches[0].on);
  s.robot().wantOutput("refill_valve", true);
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

namespace {

// The gateway and the device layer alone, with a twin Shelly that answers when told to (SD-132)
struct Rig {
  Config cfg;
  NetDevices dev;
  NetOutputs out;
  sim::FakeShelly relay;
  Ms now = 0;
  std::vector<NetRequest> queued;
  Rig() {
    cfg.devices = {{"d1", "shelly", "a.local", "Valve"}};
    cfg.roles["refill_valve"] = {"shelly", "d1", 0, "", "", false, ""};
    cfg.roles["tank_full"] = {"shelly", "d1", 1, "", "", false, ""};  // the float: not full
    relay.address = "a.local";
    relay.id = "shellyplus2pm-rig";
    for (int i = 0; i < 2; ++i)
      relay.switches.push_back({false, 0, {{"initial_state", "restore_last"}, {"auto_on", false}, {"auto_off", false}, {"auto_off_delay", 60.0}, {"in_mode", "follow"}}});
    relay.inputs = {false, false};
  }
  void tick(bool hold = false) {
    for (const auto& a : dev.tick(cfg, now)) out.answer(cfg, dev, a, now);
    out.tick(cfg, dev, hold, now);
    for (auto& r : dev.take()) queued.push_back(std::move(r));
  }
  void deliver(const NetRequest& r) {
    if (const auto a = dev.response(cfg, r.id, 200, relay.handle(r.body, now), now)) out.answer(cfg, dev, *a, now);
    for (auto& x : dev.take()) queued.push_back(std::move(x));
  }
  // Answers what waits, and what that brings, in order
  void answerAll() {
    for (int guard = 0; guard < 50 && !queued.empty(); ++guard) {
      const NetRequest r = queued.front();
      queued.erase(queued.begin());
      deliver(r);
    }
  }
  void run(Ms ms) {
    for (Ms t = 0; t < ms; t += kSecond) {
      now += kSecond;
      tick();
      answerAll();
    }
  }
  bool switchesOn() const {
    for (const auto& r : queued)
      if (r.body.is_object() && r.body.value("method", "") == "Switch.Set" && r.body["params"].value("on", false)) return true;
    return false;
  }
};

}  // namespace

TEST_CASE("Devices: an answer counts only for the request it answers: a late read-back of the output bound before never switches on the one bound now (SD-132, RAT-019)") {
  Rig g;
  g.run(10 * kSecond);
  REQUIRE(g.out.status(g.cfg, g.dev, g.now)["refill_valve"]["state"] == "off");
  REQUIRE(g.out.want("refill_valve", true));
  g.now += kSecond;
  g.tick();  // a read-back of output 0 is under way, unanswered
  std::vector<NetRequest> late = g.queued;
  g.queued.clear();
  REQUIRE_FALSE(late.empty());
  // Bound to output 1 meanwhile; the logic wants it on again
  g.cfg.roles["refill_valve"].channel = 1;
  g.now += kSecond;
  g.tick();
  g.out.want("refill_valve", true);
  // The old answer comes now: it says nothing of output 1
  for (const auto& r : late) g.deliver(r);
  CHECK_FALSE(g.switchesOn());
  CHECK_FALSE(g.relay.switches[1].on);
  // Output 1 switches on only after its own settings were written and read back
  g.run(10 * kSecond);
  CHECK(g.relay.switches[1].on);
  CHECK(g.relay.switches[1].config["initial_state"] == "off");
}

TEST_CASE("Devices: an output that is on without the robot having switched it after a check is switched off and checked first, also when it is wanted (SD-132, RAT-019)") {
  sim::Simulation s(demo());
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  relay->switches[0].on = true;  // someone switched it on in the maker's app
  bindValve(s);
  s.robot().wantOutput("refill_valve", true);
  bool wasOff = false;
  REQUIRE(runUntil(s, [&] {
    wasOff |= !relay->switches[0].on;
    return wasOff && relay->switches[0].on;
  }, 30));
  CHECK(relay->switches[0].config["initial_state"] == "off");
  CHECK(role(s, "refill_valve")["output"]["state"] == "on");
}

TEST_CASE("Devices: a device that answers as another one is not it: it is offline, says so, and its output is never switched; the ID it had stays across a restart (SD-132)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  REQUIRE(runUntil(s, [&] { return deviceStatus(s, "d1")["ownId"] == "shellyplus1-sim000001"; }, 15));
  s.control("netId", {{"address", "shelly-1.local"}, {"id", "shellyplusplugs-heater"}});
  REQUIRE(runUntil(s, [&] { return role(s, "refill_valve")["output"]["state"] == "offline"; }, 45));
  CHECK(deviceStatus(s, "d1")["problem"]["key"] == "device.other");
  s.robot().wantOutput("refill_valve", true);
  s.step(20 * kSecond);
  CHECK_FALSE(relay->switches[0].on);
  s.reboot();
  s.step(20 * kSecond);
  CHECK(deviceStatus(s, "d1")["problem"]["key"] == "device.other");
  // Replaced on purpose: removed and added again, it is known anew
  REQUIRE(s.robot().removeDevice("d1").status == 200);
  REQUIRE(s.robot().addDevice({{"kind", "shelly"}, {"address", "shelly-1.local"}, {"name", "Valve"}}).status == 200);
  REQUIRE(runUntil(s, [&] { return deviceStatus(s, "d1")["online"] == true; }, 15));
  CHECK(deviceStatus(s, "d1")["ownId"] == "shellyplusplugs-heater");
}

TEST_CASE("Devices: one device added twice, by two addresses, switches nothing under the later entry, which says so, also as light through a Stop; the first entry removed, the later one is it (SD-132, SD-135)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  relay->aliases.push_back("192.168.1.10");  // the same relay by its IP
  REQUIRE(runUntil(s, [&] { return deviceStatus(s, "d1")["ownId"] == "shellyplus1-sim000001"; }, 15));
  REQUIRE(s.robot().addDevice({{"kind", "shelly"}, {"address", "192.168.1.10"}, {"name", "Valve by IP"}}).status == 200);
  REQUIRE(runUntil(s, [&] { return deviceStatus(s, "d2")["problem"]["key"] == "device.twice"; }, 30));
  CHECK(deviceStatus(s, "d2")["problem"]["args"]["name"] == "Valve");
  CHECK(deviceStatus(s, "d2")["online"] == false);
  CHECK(deviceStatus(s, "d1")["online"] == true);
  // A light all day on the valve's output by the later entry: never switched on, not even through a
  // Stop, which leaves light on; the valve's own limit stays (RAT-032)
  REQUIRE(s.robot().putLight({{"name", "Shelf"}, {"hours", 24}}).status == 200);
  REQUIRE(s.robot().putRole("light.l1", {{"source", "shelly"}, {"device", "d2"}, {"channel", 0}}).status == 200);
  REQUIRE(s.robot().stop(say("stop.app")).status == 200);
  bool on = false;
  for (int i = 0; i < 60; ++i) {
    s.step(kSecond);
    on |= relay->switches[0].on;
  }
  CHECK_FALSE(on);
  CHECK(role(s, "light.l1")["output"]["state"] == "offline");
  CHECK(relay->switches[0].config["auto_off"] == true);
  // The first entry removed: the later one is the device, and the light goes on
  REQUIRE(s.robot().resume().status == 200);
  REQUIRE(s.robot().removeDevice("d1").status == 200);
  REQUIRE(runUntil(s, [&] { return deviceStatus(s, "d2")["online"] == true; }, 30));
  CHECK(deviceStatus(s, "d2")["ownId"] == "shellyplus1-sim000001");
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 60));
}

TEST_CASE("Devices: an import that drops or moves a binding switches the output off; a device that went or moved is forgotten with its password; a want from before never switches a later binding on (SD-132, R6)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  REQUIRE(s.robot().putDeviceCredentials("d1", {{"password", "relay-secret"}}).status == 200);
  json file = s.robot().configJson();
  file["roles"] = json::object();
  file["devices"][0]["address"] = "shelly-plug.local";
  REQUIRE(s.robot().importConfig(file).status == 200);
  s.step(2 * kSecond);
  CHECK_FALSE(relay->switches[0].on);
  CHECK(deviceStatus(s, "d1")["hasPassword"] == false);
  CHECK(deviceStatus(s, "d1")["ownId"] == "shellyplusplugs-sim000002");  // the old one's ID is forgotten, the new one's learned
  // Bound again: nothing is wanted until the logic wants it anew
  REQUIRE(s.robot().putRole("refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}}).status == 200);
  s.step(20 * kSecond);
  CHECK_FALSE(s.net().shelly("shelly-plug.local")->switches[0].on);
}

TEST_CASE("Devices: an output let go while its device cannot be reached is switched off once it can be, within a minute; otherwise an alarm says it may still be on (SD-132)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  s.control("netReachable", {{"address", "shelly-1.local"}, {"reachable", false}});
  REQUIRE(s.robot().deleteRole("refill_valve").status == 200);
  s.step(20 * kSecond);
  CHECK(relay->switches[0].on);  // it cannot be reached
  s.control("netReachable", {{"address", "shelly-1.local"}, {"reachable", true}});
  REQUIRE(runUntil(s, [&] { return !relay->switches[0].on; }, 15));
  CHECK(s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 100).dump().find("ev.device.release_failed") == std::string::npos);
  // Never reachable again: the alarm after a minute
  REQUIRE(s.robot().putRole("refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}}).status == 200);
  s.step(kSecond);
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  s.control("netReachable", {{"address", "shelly-1.local"}, {"reachable", false}});
  REQUIRE(s.robot().removeDevice("d1").status == 200);
  s.step(70 * kSecond);
  CHECK(s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 100).dump().find("ev.device.release_failed") != std::string::npos);
}

TEST_CASE("Devices: a Stop switches every bound output off at once, whatever its device said last (SD-132, RAT-036)") {
  Rig g;
  g.run(10 * kSecond);
  g.relay.switches[0].on = true;  // switched on in the maker's app since the last status
  g.out.allOff();
  g.now += 100;
  g.tick();
  bool off = false;
  for (const auto& r : g.queued)
    off |= r.body.value("method", "") == "Switch.Set" && r.body["params"].value("on", true) == false;
  CHECK(off);
  g.answerAll();
  CHECK_FALSE(g.relay.switches[0].on);
}

TEST_CASE("Devices: a Shelly input is as old as the device's last status, whatever else it answered since (SD-132, RAT-024)") {
  Rig g;
  g.cfg.roles["tank_full"] = {"shelly", "d1", 1, "", "", false, ""};
  g.run(5 * kSecond);
  REQUIRE(g.dev.value(g.cfg, "tank_full", g.now).state == "ok");
  // Its status is no longer answered, while the gateway's requests are
  for (int i = 0; i < 40; ++i) {
    g.now += kSecond;
    g.tick();
    std::vector<NetRequest> keep;
    for (const auto& r : g.queued)
      if (r.body.value("method", "") == "Shelly.GetStatus") keep.push_back(r);
      else g.deliver(r);
    g.queued.clear();
    if (i % 5 == 0) g.out.want("refill_valve", i % 10 == 0);
  }
  CHECK(g.dev.online("d1", g.now));
  CHECK(g.dev.value(g.cfg, "tank_full", g.now).state == "stale");
  CHECK_FALSE(g.dev.value(g.cfg, "tank_full", g.now).value.has_value());
}

TEST_CASE("Devices: MQTT values count only while connected: after an outage or a change of the MQTT settings, a value counts once it comes again (SD-132, R5)") {
  sim::Simulation s(demo());
  REQUIRE(s.robot().putRole("tank_full", {{"source", "mqtt"}, {"entity", "tank/full"}, {"availability", "tank/status"}}).status == 200);
  s.robot().setMqttStatus(true, {});
  s.robot().mqttValue("tank/status", "online");
  s.robot().mqttValue("tank/full", "ON");
  CHECK(role(s, "tank_full")["value"] == true);
  s.robot().setMqttStatus(false, say("mqtt.lost"));
  s.robot().setMqttStatus(true, {});
  CHECK(role(s, "tank_full")["state"] == "offline");  // its availability has not come again
  s.robot().mqttValue("tank/status", "online");
  CHECK(role(s, "tank_full")["state"] == "waiting");
  s.robot().mqttValue("tank/full", "ON");
  CHECK(role(s, "tank_full")["value"] == true);
  REQUIRE(s.robot().putMqtt({{"enabled", false}}).status == 200);
  CHECK(role(s, "tank_full")["state"] == "offline");
  CHECK(role(s, "tank_full")["value"].is_null());
}

TEST_CASE("Devices: out of the box a Shelly's input switches its output; bound to a role it does not (SD-132, RAT-019)") {
  sim::Simulation s(demo());
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  s.control("netInput", {{"address", "shelly-1.local"}, {"channel", 0}, {"state", true}});
  CHECK(relay->switches[0].on);
  s.control("netInput", {{"address", "shelly-1.local"}, {"channel", 0}, {"state", false}});
  CHECK_FALSE(relay->switches[0].on);
  bindValve(s);
  REQUIRE(runUntil(s, [&] { return role(s, "refill_valve")["output"]["verified"] == true; }, 30));
  s.control("netInput", {{"address", "shelly-1.local"}, {"channel", 0}, {"state", true}});
  CHECK_FALSE(relay->switches[0].on);
}

TEST_CASE("Devices: a device is added once by its address; a role that is refused leaves the output it had as it was (SD-132)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  CHECK(s.robot().addDevice({{"kind", "shelly"}, {"address", "SHELLY-1.local"}, {"name", "Again"}}).status == 422);
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  CHECK(s.robot().putRole("refill_valve", {{"source", "mqtt"}, {"entity", "x"}}).status == 422);
  s.step(10 * kSecond);
  CHECK(relay->switches[0].on);
  CHECK_FALSE(s.robot().wantOutput("tank_full", true));
  CHECK_FALSE(s.robot().wantOutput("heater", true));
}

TEST_CASE("Devices: a device removed while reachable has its output switched off with no alarm; one moved by an import does not disturb the device now at the new address (SD-132)") {
  sim::Simulation s(demo());
  bindValve(s);
  sim::FakeShelly* relay = s.net().shelly("shelly-1.local");
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  REQUIRE(s.robot().removeDevice("d1").status == 200);
  s.step(70 * kSecond);
  CHECK_FALSE(relay->switches[0].on);
  CHECK(s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 200).dump().find("ev.device.release_failed") == std::string::npos);
  // Moved by an import: the release goes to the old address, and the device at the new one stays well
  bindValve(s);
  s.robot().wantOutput("refill_valve", true);
  REQUIRE(runUntil(s, [&] { return relay->switches[0].on; }, 30));
  json file = s.robot().configJson();
  file["devices"][0]["address"] = "shelly-plug.local";
  REQUIRE(s.robot().importConfig(file).status == 200);
  for (int i = 0; i < 70; ++i) {
    s.step(kSecond);
    if (i > 10) REQUIRE(deviceStatus(s, "d1")["online"] == true);
  }
  CHECK_FALSE(relay->switches[0].on);
  CHECK(deviceStatus(s, "d1")["problem"].is_null());
  CHECK(s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 200).dump().find("ev.device.release_failed") == std::string::npos);
}

TEST_CASE("Devices: a status that shows the output on before the answer to its switch-on does not switch it off again (SD-132)") {
  Rig g;
  g.run(10 * kSecond);
  REQUIRE(g.out.want("refill_valve", true));
  // Read-back, then the switch-on goes out; the device's status comes back before its answer
  std::vector<NetRequest> late;
  for (int i = 0; i < 20 && late.empty(); ++i) {
    g.now += kSecond;
    g.tick();
    for (int round = 0; round < 10 && !g.queued.empty() && late.empty(); ++round) {
      const std::vector<NetRequest> batch = g.queued;
      g.queued.clear();
      for (const auto& r : batch)
        if (r.body.value("method", "") == "Switch.Set" && r.body["params"].value("on", false)) {
          g.relay.handle(r.body, g.now);  // it switches on; its answer is late
          late.push_back(r);
        } else {
          g.deliver(r);
        }
    }
  }
  REQUIRE(g.relay.switches[0].on);
  g.now += 5 * kSecond;
  g.tick();
  g.answerAll();  // the status says on
  g.now += 100;
  g.tick();  // the gateway looks before the switch-on's answer is in
  for (const auto& r : late) g.deliver(r);
  g.run(20 * kSecond);
  CHECK(g.relay.switches[0].on);
  CHECK(g.out.status(g.cfg, g.dev, g.now)["refill_valve"]["state"] == "on");
}
