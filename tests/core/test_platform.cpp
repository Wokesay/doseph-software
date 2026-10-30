// SPDX-License-Identifier: AGPL-3.0-or-later
// The firmware platform's rules in the core and the twin (SD-142): each slot's pump direction, the
// tries without flow across a restart, the return pump on high level and on Stop, large writes
// between dosing runs, and a Stop across a factory reset.
#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string>

#include "harness.hpp"

using namespace doseph;

namespace {

void feedPumpRuns(harness::Run& r) {
  r.waitUntil("the feed pump runs", [&] { return r.twin()["output"] == true; }, 120);
}

}  // namespace

TEST_CASE("Platform: each slot's pump turns as the board description says; the driver turns forwards and backwards into the motor's direction, so a run delivers and a pull-back pulls back on every slot (SD-142)") {
  harness::Run r(harness::device());
  sim::World& w = r.s.world();
  REQUIRE(w.pumps[1].inverted);
  REQUIRE_FALSE(w.pumps[0].inverted);
  // Into a cup on a slot mounted the other way: the motor turns backwards, and the cup fills
  r.ok("POST", "channels/2/prime", {{"seconds", 3}});
  r.step(kSecond);
  CHECK(w.pumps[1].motorReverse);
  CHECK_FALSE(w.pumps[1].backwards);
  r.step(4 * kSecond);
  CHECK(w.pumps[1].cupMl > 1.0);
  r.ok("POST", "channels/1/prime", {{"seconds", 3}});
  r.step(kSecond);
  CHECK_FALSE(w.pumps[0].motorReverse);
  r.step(4 * kSecond);
  CHECK(w.pumps[0].cupMl > 1.0);
  // The driver of an inverted slot: a pull-back turns its motor forwards
  std::string err;
  REQUIRE(r.s.hardware().startRun(2, 500, "pb-test", err, true));
  CHECK_FALSE(w.pumps[1].motorReverse);
  CHECK(w.pumps[1].backwards);
}

TEST_CASE("Platform: the tries without flow are kept across a restart, so restarts do not start the count afresh before the lock (SD-142, SD-127)") {
  harness::Run r(harness::device());
  feedPumpRuns(r);
  r.act("flowFault", {{"fault", "dry"}});
  r.waitUntil("one try without flow", [&] { return r.s.robot().runtime().noFlowTries == 1; }, 180);
  CHECK_FALSE(r.state()["latches"].contains("flow.dry"));
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  CHECK(r.s.robot().runtime().noFlowTries == 1);
  r.waitUntil("the lock after the second try", [&] { return r.state()["latches"].contains("flow.dry"); }, 15 * 60);
  CHECK(r.s.robot().runtime().noFlowTries == 0);
}

TEST_CASE("Platform: the return pump keeps running on high level so the chamber drains; a Stop holds it off until Resume, and so does a latched fault that names it until released (SD-142)") {
  harness::Run r(harness::device("pro"));
  feedPumpRuns(r);
  r.act("returnFault", {{"fault", "failed"}});
  r.waitUntil("high level", [&] { return r.twin()["highLevel"] == true; }, 600);
  r.step(3 * kSecond);
  CHECK(r.twin()["output"] == false);  // the feed pump is held
  CHECK(r.twin()["returnPump"] == true);
  CHECK(r.twin()["returnLocked"] == false);
  r.ok("POST", "stop");
  r.step(kSecond);
  CHECK(r.twin()["returnLocked"] == true);
  CHECK(r.twin()["returnPump"] == false);
  r.ok("POST", "resume");
  r.step(kSecond);
  CHECK(r.twin()["returnLocked"] == false);

  harness::Run p(harness::device("pro"));
  feedPumpRuns(p);
  p.act("faults", {{"ids", {"pump_protection.return"}}});
  p.step(6 * kSecond);
  CHECK(p.twin()["returnLocked"] == true);
  p.act("faults", {{"ids", json::array()}});
  p.step(6 * kSecond);
  CHECK(p.twin()["returnLocked"] == true);  // gone, but not released yet
  p.ok("POST", "latches/hw.pump_protection.return/ack");
  CHECK(p.twin()["returnLocked"] == false);
  // The stop button on the device switches it off too
  p.act("stopButton", {{"pressed", true}});
  p.step(kSecond);
  CHECK(p.twin()["returnPump"] == false);
}

TEST_CASE("Platform: the history and the event log are written whole only between dosing runs (SD-142)") {
  harness::Run r(harness::device());
  // The first snapshot is due 6 h after the start
  const long long up = r.state()["uptimeS"].get<long long>();
  r.step((6 * 3600 - 15 - up) * kSecond);
  const auto before = r.s.storage().read("history.bin");
  r.ok("POST", "channels/1/calibrate", {{"seconds", 40}});
  r.step(30 * kSecond);  // the snapshot is due meanwhile
  CHECK(r.s.storage().read("history.bin") == before);
  r.waitUntil("the snapshot after the run", [&] { return r.s.storage().read("history.bin") != before; }, 60);
}

TEST_CASE("Platform: a Stop pressed before a factory reset holds after it until Resume in the app (SD-142, SD-139)") {
  harness::Run r(harness::device());
  r.step(10 * kSecond);
  r.ok("POST", "stop");
  const std::string id = r.s.robot().deviceId();
  r.s.hardware().askReset(DeviceReset::Factory);
  r.step(5 * kSecond);
  REQUIRE(r.s.robot().deviceId() != id);
  CHECK(r.s.robot().runtime().stopped);
  const json ev = r.s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 100);
  CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.system.factory_reset"; }));
  // Without a Stop before, none after
  harness::Run plain(harness::device());
  plain.step(10 * kSecond);
  plain.s.hardware().askReset(DeviceReset::Factory);
  plain.step(5 * kSecond);
  CHECK_FALSE(plain.s.robot().runtime().stopped);
}
