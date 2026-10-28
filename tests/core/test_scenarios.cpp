// SPDX-License-Identifier: AGPL-3.0-or-later
// Scenarios of the new water path (#10, step 3): priming, flow, dose, drops,
// blocked outlet, high level, return pump. They run the core against the
// twin through the API. Where the core does not react yet, they say which
// step brings it (#10, step 4).
#include <cmath>

#include "harness.hpp"

using harness::Run;
using harness::device;
using doseph::json;
using doseph::kMinute;
using doseph::kSecond;

namespace {

// The drops each channel's sensor has seen so far, from the robot's state
std::map<int, double> dropsOf(Run& r) {
  std::map<int, double> d;
  const json channels = r.state()["channels"];  // a copy: the state is a temporary
  for (const auto& ch : channels)
    if (ch["drops"].is_object()) d[ch["channel"].get<int>()] = ch["drops"]["count"].get<double>();
  return d;
}

}  // namespace

TEST_CASE("Scenario priming: the check switches the suction pump on; water flows once it has primed, and flow is proven before the chamber is flushed (SD-105, SD-109)") {
  Run r(device("home"));
  r.waitUntil("the suction pump is switched on", [&] { return r.twin()["output"] == true; }, 60);
  CHECK(r.twin()["primed"] == false);
  CHECK(r.twin()["flowing"] == false);
  CHECK(r.state()["flowOk"] == false);
  r.waitUntil("water flows", [&] { return r.twin()["flowing"] == true; }, 15);
  CHECK(r.state()["hardware"]["outletWet"] == true);
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 30);
  CHECK(r.state()["flowOk"] == true);
}

TEST_CASE("Scenario flow: a suction line out of the water gives no flow; the suction pump goes off and the check waits, then proves flow again (SD-105, SD-109)") {
  Run r(device("home"));
  r.act("flowFault", {{"fault", "dry"}});
  r.waitUntil("the check waits for flow", [&] { return r.line() == "cycle.wait.no_flow"; }, 4 * 60);
  CHECK(r.twin()["output"] == false);  // a pump without flow may run dry
  CHECK(r.events("alarm").size() >= 1);
  CHECK(r.state()["dosing"].is_null());
  r.act("flowFault", {{"fault", ""}});
  r.waitUntil("flow is proven again", [&] { return r.phase() == "flush"; }, 10 * 60);
}

TEST_CASE("Scenario dose and drops: every dose drips through its outlet; the drop sensor sees what went in, channel by channel (SD-111, #10 acceptance)") {
  Run r(device("home"));
  const auto before = dropsOf(r);
  REQUIRE(before.size() == 4);
  r.waitUntil("the check holds", [&] { return r.phase() == "hold"; }, 8 * 3600, 2000);
  std::map<int, double> ml;
  std::map<int, int> runs;
  const json doses = r.events("dose");
  for (const auto& e : doses) {
    ml[e["data"]["channel"].get<int>()] += e["data"]["ml"].get<double>();
    runs[e["data"]["channel"].get<int>()] += e["data"]["runs"].get<int>();
  }
  REQUIRE_FALSE(ml.empty());
  const auto after = dropsOf(r);
  for (const auto& [ch, m] : ml) {
    CAPTURE(ch);
    const double seen = (after.at(ch) - before.at(ch)) * 0.05;
    CHECK(seen > 0);
    // Booked from the run time with the calibrated flow; the drops see the true flow:
    // within 3 % of what was booked, and a drop per run that stayed in the tip
    CHECK(std::abs(seen - m) <= 0.03 * m + 0.05 * runs[ch]);
  }
}

TEST_CASE("Scenario blocked outlet: the pump runs but nothing drips and nothing reaches the tank; the core books it until step 4.3 judges the drops (SD-111)") {
  Run r(device("home"));
  r.act("channelFault", {{"channel", 2}, {"outletBlocked", true}});
  const double tankEc = r.twin()["tank"]["ec"];
  r.waitUntil("a dose of part A", [&] {
    const json doses = r.events("dose");
    for (const auto& e : doses)
      if (e["data"]["channel"] == 2) return true;
    return false;
  }, 2 * 3600, 2000);
  CHECK(r.twin()["channels"][1]["drops"] == 0);
  CHECK(r.twin()["channels"][1]["outletBlocked"] == true);
  // Part B dripped and raised EC; part A, behind the blocked outlet, added nothing
  CHECK(r.twin()["channels"][2]["drops"].get<double>() > 0);
  CHECK(r.twin()["tank"]["ec"].get<double>() + r.twin()["tank"]["pendingEc"].get<double>() > tankEc);
}

TEST_CASE("Scenario high level: a blocked return fills the chamber; the hardware release stops the suction pump and every dose, and the state says why (SD-110)") {
  Run r(device("home"));
  r.waitUntil("water flows", [&] { return r.twin()["flowing"] == true; }, 60);
  r.act("returnFault", {{"fault", "blocked"}});
  r.waitUntil("high level", [&] { return r.state()["hardware"]["release"]["highLevel"] == true; }, 30);
  const json hw = r.state()["hardware"];
  CHECK(hw["release"]["on"] == false);
  CHECK(r.twin()["output"] == false);
  r.step(2 * kMinute);
  const json channels = r.twin()["channels"];
  for (const auto& ch : channels) CHECK(ch["state"] != 1);  // no pump runs
  CHECK(r.state()["suctionPump"] == false);
  CHECK(r.state()["dosing"].is_null());
  // The core's suction pump is refused and it waits for flow; its lock and reason come with step 4.1
  const std::string line = r.line();
  CHECK((line == "cycle.suction_failed" || line == "cycle.wait_flow" || line == "cycle.wait.no_flow"));
}

TEST_CASE("Scenario return pump: it runs while water comes back and stops below its level; when it fails, the chamber rises to high level (SD-109, SD-110)") {
  Run r(device("pro"));
  bool ran = false, stopped = false;
  r.waitUntil("the return pump ran and stopped", [&] {
    const json w = r.state()["hardware"];
    if (w["returnPump"] == true) ran = true;
    if (ran && w["returnPump"] == false) stopped = true;
    return ran && stopped;
  }, 3 * 60);
  CHECK(r.state()["hardware"]["release"]["highLevel"] == false);
  r.act("returnFault", {{"fault", "failed"}});
  r.waitUntil("high level", [&] { return r.state()["hardware"]["release"]["highLevel"] == true; }, 60);
  CHECK(r.state()["hardware"]["release"]["on"] == false);
  CHECK(r.twin()["output"] == false);
}
