// SPDX-License-Identifier: AGPL-3.0-or-later
// What the device's own protection reports (SD-141): faults that hold every pump while the device
// reports them, faults latched until a person releases them, warnings, and a check of the
// protection by hand where the device does not test it itself.
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "doseph/faults.hpp"
#include "doseph/gateway.hpp"
#include "doseph/journal.hpp"
#include "fakes.hpp"
#include "harness.hpp"

using namespace doseph;

namespace {

json alarm(harness::Run& r, const std::string& id) {
  const json st = r.state();
  for (const auto& a : st["alarms"])
    if (a["id"] == id) return a;
  return nullptr;
}

bool logged(harness::Run& r, const std::string& name) {
  const json ev = r.events("alarm");
  return std::any_of(ev.begin(), ev.end(), [&](const json& e) { return e["title"]["key"] == "ev.hw.fault" && e["title"]["args"]["name"]["key"] == name; });
}

void feedPumpRuns(harness::Run& r) {
  r.waitUntil("the feed pump runs", [&] { return r.twin()["output"] == true; }, 120);
}

}  // namespace

TEST_CASE("Device protection: a power trip holds every pump at once, with a critical alarm that names the part and what to do; the restart that clears it lets the check start afresh (SD-141)") {
  harness::Run r(harness::device());
  feedPumpRuns(r);
  r.act("faults", {{"ids", {"power_trip.dosing"}}});
  r.step(6 * kSecond);
  CHECK(r.twin()["output"] == false);
  const json st = r.state();
  CHECK(st["hardware"]["hold"] == "power_trip");
  CHECK(st["hardware"]["faults"] == json::array({"power_trip.dosing"}));
  CHECK(st["cycle"]["line"]["key"] == "cycle.wait.hold.fault");
  const json a = alarm(r, "hw.power_trip.dosing");
  REQUIRE(a.is_object());
  CHECK(a["level"] == "critical");
  CHECK(a["text"]["key"] == "latch.hw.power_trip");
  CHECK(a["text"]["args"]["part"]["key"] == "hw.part.dosing");
  CHECK(logged(r, "lock.hw.power_trip"));
  // Nothing runs, not even into a cup, while it stands
  CHECK(r.call("POST", "channels/1/prime", {{"seconds", 2}}).status != 200);
  r.step(2 * kMinute);
  CHECK(r.twin()["output"] == false);
  // No latch: the restart is what ends it
  CHECK_FALSE(r.state()["latches"].contains("hw.power_trip.dosing"));
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"].is_null());
  feedPumpRuns(r);
}

TEST_CASE("Device protection: a failed self-test, an invalid board description and a fault the robot does not know hold while reported; the self-test names what failed (SD-141)") {
  harness::Run r(harness::device());
  feedPumpRuns(r);
  r.act("faults", {{"ids", {"self_test.switch_off"}}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"] == "self_test");
  json a = alarm(r, "hw.self_test.switch_off");
  REQUIRE(a.is_object());
  CHECK(a["text"]["args"]["part"]["key"] == "hw.part.switch_off");
  r.act("faults", {{"ids", {"board_description"}}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"] == "board_description");
  CHECK(alarm(r, "hw.board_description")["level"] == "critical");
  r.act("faults", {{"ids", {"something_new.x"}}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"] == "fault");
  a = alarm(r, "hw.fault");
  REQUIRE(a.is_object());
  CHECK(a["text"]["args"]["id"] == "something_new.x");
  // Reported no more: the check starts afresh
  r.act("faults", {{"ids", json::array()}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"].is_null());
  feedPumpRuns(r);
}

TEST_CASE("Device protection: a latched fault holds until a person releases it, and only once the device no longer reports it; a pump's protection names the pump (SD-141)") {
  harness::Run r(harness::device("pro"));
  feedPumpRuns(r);
  r.act("faults", {{"ids", {"leak_sensor_open"}}});
  r.step(6 * kSecond);
  CHECK(r.twin()["output"] == false);
  REQUIRE(r.state()["latches"].contains("hw.leak_sensor"));
  CHECK(r.state()["hardware"]["hold"] == "leak_sensor");
  CHECK(alarm(r, "latch.hw.leak_sensor")["level"] == "critical");
  CHECK(r.call("POST", "latches/hw.leak_sensor/ack").status == 409);  // still reported
  r.act("faults", {{"ids", json::array()}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"] == "leak_sensor");  // gone, but not released yet
  r.ok("POST", "latches/hw.leak_sensor/ack");
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"].is_null());
  // The return pump's protection: its own latch, named by the pump
  r.act("faults", {{"ids", {"pump_protection.return"}}});
  r.step(6 * kSecond);
  const json st = r.state();
  REQUIRE(st["latches"].contains("hw.pump_protection.return"));
  const json a = alarm(r, "latch.hw.pump_protection.return");
  REQUIRE(a.is_object());
  CHECK(a["text"]["key"] == "latch.hw.pump_protection");
  CHECK(a["text"]["args"]["part"]["key"] == "hw.part.return");
  // It holds across a restart, as any latch
  r.act("faults", {{"ids", json::array()}});
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"] == "pump_protection");
}

TEST_CASE("Device protection: warnings hold nothing; a restart by the safety supervisor stays as a note until a person has seen it (SD-141)") {
  harness::Run r(harness::device());
  feedPumpRuns(r);
  r.act("faults", {{"ids", {"board_hot"}}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"].is_null());
  CHECK(r.twin()["output"] == true);
  CHECK(alarm(r, "hw.board_hot")["level"] == "warning");
  r.act("faults", {{"ids", {"safety_restart"}}});
  r.step(6 * kSecond);
  json st = r.state();
  CHECK(st["hardware"]["hold"].is_null());
  REQUIRE(st["latches"].contains(kSafetyRestartNote));
  CHECK_FALSE(holds(r.s.robot().runtime(), kSafetyRestartNote));
  const json a = alarm(r, std::string("latch.") + kSafetyRestartNote);
  REQUIRE(a.is_object());
  CHECK(a["level"] == "warning");
  CHECK(a["text"]["key"] == "latch.hw.safety_restart");
  CHECK(r.twin()["output"] == true);
  // Seen: it goes, also while the device still reports it for this start
  r.ok("POST", std::string("latches/") + kSafetyRestartNote + "/ack");
  r.step(6 * kSecond);
  CHECK_FALSE(r.state()["latches"].contains(kSafetyRestartNote));
}

TEST_CASE("Device protection: where the device does not test its protection itself, after every start nothing doses into the tank until a person releases the check; runs into a cup still go (SD-141)") {
  sim::Options o = harness::device();
  o.selfTest = "manual";
  harness::Run r(o);
  json st = r.state();
  REQUIRE(st["latches"].contains("protection_check"));
  CHECK(st["hardware"]["protectionCheck"] == true);
  r.step(30 * kSecond);
  CHECK(r.line() == "cycle.wait.protection_check");
  CHECK(r.twin()["output"] == false);
  const json a = alarm(r, "latch.protection_check");
  REQUIRE(a.is_object());
  CHECK(a["level"] == "warning");
  r.ok("POST", "channels/1/prime", {{"seconds", 2}});  // into a cup
  r.step(5 * kSecond);
  r.ok("POST", "latches/protection_check/ack");
  feedPumpRuns(r);
  // The next start asks again
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  r.step(5 * kSecond);
  CHECK(r.state()["latches"].contains("protection_check"));
  CHECK(r.line() == "cycle.wait.protection_check");
  // A device that tests itself is never asked
  harness::Run self(harness::device());
  CHECK_FALSE(self.state()["latches"].contains("protection_check"));
  CHECK(self.ok("GET", "info")["profile"]["selfTest"] == "auto");
}

TEST_CASE("Device protection: the gateway doses nothing into the tank while the check by hand stands, whoever asks; a slot's pump direction is never shown (SD-141)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg;
  cfg.setup["placement"] = {"done", 1};
  cfg.channels[2] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  c.protectionCheck = true;
  Actuators act(hw);
  Msg err;
  CHECK_FALSE(act.startRun(c, 2, 6000, "ec", "j1", err));
  CHECK(err.key == "act.protection_check");
  c.protectionCheck = false;
  CHECK(act.startRun(c, 2, 6000, "ec", "j2", err));
  // The direction stays inside the driver
  hw.prof.channels[1].inverted = true;
  const json p = profileJson(hw.prof);
  CHECK_FALSE(p["channels"][1].contains("inverted"));
  CHECK(p["board"] == "signed");
}

TEST_CASE("Device protection: each fault ID is taken by its kind; a part that is no short name is not shown (SD-141)") {
  CHECK(classifyFault("power_trip.return").kind == FaultInfo::Kind::Hold);
  CHECK(classifyFault("power_trip.return").id() == "hw.power_trip.return");
  CHECK(classifyFault("safety_cycle").kind == FaultInfo::Kind::Hold);
  CHECK(classifyFault("current_limit.rail_5v").kind == FaultInfo::Kind::Latch);
  CHECK(classifyFault("leak_sensor_open").id() == "hw.leak_sensor");
  CHECK(classifyFault("moisture").kind == FaultInfo::Kind::Latch);
  CHECK(classifyFault("not_off").kind == FaultInfo::Kind::Latch);
  CHECK(classifyFault("stop_mismatch").kind == FaultInfo::Kind::Latch);
  CHECK(classifyFault("supply").kind == FaultInfo::Kind::Latch);
  CHECK(classifyFault("board_hot").kind == FaultInfo::Kind::Warning);
  CHECK(classifyFault("whatever").reason == "fault");
  CHECK(classifyFault("current_limit.<script>").part.empty());
  const FaultInfo f = classifyFault("current_limit.rail_5v");
  CHECK(faultArgs(f, "current_limit.rail_5v")["part"] == "rail_5v");
  CHECK(faultArgs(classifyFault("x"), "X<y>")["id"] == "?");
}
