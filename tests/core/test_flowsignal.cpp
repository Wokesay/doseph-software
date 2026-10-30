// SPDX-License-Identifier: AGPL-3.0-or-later
// The flow signal (SD-146): a flow meter's pulses are no flow while the feed pump's current says it
// moves no water, and the device's protection names what it holds for.
#include <doctest/doctest.h>

#include <algorithm>
#include <limits>

#include "harness.hpp"

using namespace doseph;

namespace {

json alarm(harness::Run& r, const std::string& id) {
  const json st = r.state();
  for (const auto& a : st["alarms"])
    if (a["id"] == id) return a;
  return nullptr;
}

}  // namespace

TEST_CASE("Flow signal: air that turns the flow meter's rotor is no flow; the check doses nothing, and after its tries the feed pump locks (SD-146, SD-127)") {
  harness::Run r(harness::device("pro"));
  r.act("flowFault", {{"fault", "air"}});
  r.waitUntil("the feed pump runs", [&] { return r.twin()["output"] == true; }, 120);
  r.step(4 * kSecond);
  // The flow meter counts pulses, but the pump draws the current of a dry run
  const json st = r.state();
  CHECK(st["readings"]["flow"]["value"].get<double>() >= kFlowMinLpm);
  CHECK(st["flowOk"] == false);
  r.waitUntil("the lock for no flow", [&] { return r.state()["latches"].contains("flow.dry"); }, 15 * 60);
  const json ev = r.s.robot().events(0, std::numeric_limits<Epoch>::max(), "dose", 100);
  CHECK(std::none_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.dose"; }));
  CHECK(r.s.robot().runtime().pumpNormalA != r.s.robot().runtime().pumpNormalA);  // nothing learned from air (NaN)
}

TEST_CASE("Flow signal: flow pulses, pump speed and current that disagree, and a failed sensor supply, latch until released; an overvoltage the supply holds says to unplug it (SD-146)") {
  harness::Run r(harness::device("pro"));
  r.waitUntil("the feed pump runs", [&] { return r.twin()["output"] == true; }, 120);
  for (const std::string id : {"flow_implausible", "sensor_supply"}) {
    INFO(id);
    r.act("faults", {{"ids", {id}}});
    r.step(6 * kSecond);
    CHECK(r.twin()["output"] == false);
    REQUIRE(r.state()["latches"].contains("hw." + id));
    CHECK(alarm(r, "latch.hw." + id)["level"] == "critical");
    r.act("faults", {{"ids", json::array()}});
    r.step(6 * kSecond);
    CHECK(r.state()["hardware"]["hold"] == id);  // gone, but not released yet
    r.ok("POST", "latches/hw." + id + "/ack");
    r.step(6 * kSecond);
    CHECK(r.state()["hardware"]["hold"].is_null());
  }
  // Held by the supply itself until unplugged: no latch of the robot's own
  r.act("faults", {{"ids", {"supply_overvoltage_latched"}}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"] == "supply_overvoltage");
  CHECK_FALSE(r.state()["latches"].contains("hw.supply_overvoltage"));
  CHECK(alarm(r, "hw.supply_overvoltage")["text"]["key"] == "latch.hw.supply_overvoltage");
  r.act("faults", {{"ids", json::array()}});
  r.step(6 * kSecond);
  CHECK(r.state()["hardware"]["hold"].is_null());
  // An open leak or moisture input names the input
  r.act("faults", {{"ids", {"leak_sensor_open.tray"}}});
  r.step(6 * kSecond);
  REQUIRE(r.state()["latches"].contains("hw.leak_sensor.tray"));
  CHECK(alarm(r, "latch.hw.leak_sensor.tray")["text"]["args"]["part"] == "tray");
}
