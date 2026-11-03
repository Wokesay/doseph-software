// SPDX-License-Identifier: AGPL-3.0-or-later
// The largest pH− dose in a large tank (SD-151): it grows step by step while its doses cannot be
// measured, within what one dose may do.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "doseph/cycle.hpp"
#include "doseph/plan.hpp"
#include "harness.hpp"

using namespace doseph;

namespace {

void waitFor(harness::Run& r, const std::string& what, const std::function<bool()>& done, double maxS) {
  for (double t = 0; t <= maxS; t += 10) {
    if (done()) return;
    r.step(10 * kSecond);
  }
  FAIL("not within " << maxS << " s: " << what);
}

// A tank at its EC target with pH high: only pH− is dosed. `trueL` is the twin's water, `enteredL`
// the volume the person enters. Each dose mixes 5 h, as the person set: most of it is in by then
void largeTank(harness::Run& r, double trueL, double enteredL) {
  r.act("fill", {{"volumeL", trueL}, {"ec", 1.65}, {"ph", 7.0}});
  r.ok("PUT", "tank", {{"volumeL", enteredL}, {"mixMinMin", 300}});
}

std::vector<json> events(harness::Run& r, const std::string& key) {
  std::vector<json> out;
  for (const auto& e : r.events("control"))
    if (e["title"]["key"] == key) out.push_back(e);
  return out;
}

}  // namespace

TEST_CASE("pH− in a large tank: a grown dose holds only for the set maximum, volume and pH− canister it was learned for, never beyond one round, the daily amount or the canister; a damaged record counts as nothing (SD-151, R5, R7)") {
  Config cfg;
  cfg.canisters = {{"down", "pH−", "ph_down", 1, "", "#c00"}};
  cfg.channels[1] = {45.0, 1};
  cfg.tank.volumeL = 30000;
  RuntimeState rt;
  CHECK(phMaxMl(cfg, rt) == 20.0);
  PhGrowth g;
  g.ml = 640;
  g.unseenMl = 300;
  setPhGrowth(cfg, rt, g);
  // One round of 6 × 60 s at 45 ml/min is 270 ml: never more (RAT-055)
  CHECK(phMaxMl(cfg, rt) == doctest::Approx(270.0));
  CHECK(phGrowth(cfg, rt).unseenMl == 300.0);
  // The user's daily amount (SD-113), with room for the runs' rounding to whole ms: a dose at it
  // always fits, whatever the flow
  for (const double flow : {41.0, 45.0, 33.0, 52.7}) {
    for (const double perDay : {50.0, 100.0}) {
      Config day = cfg;
      day.channels[1] = {flow, 1};
      day.canisters[0].maxMlPerDay = perDay;
      const double ml = phMaxMl(day, rt);
      CHECK(ml < perDay);
      CHECK(ml > perDay - 1);
      Msg err;
      Ms total = 0;
      for (const Ms ms : splitRuns(ml, flow, day.limits, day.limits.bounded().maxPartialRuns, err)) total += ms;
      CHECK(err.key.empty());
      CHECK(flow * static_cast<double>(total) / 60000.0 <= perDay + 1e-9);  // as the gateway counts it
    }
  }
  Config small = cfg;
  small.canisters[0].capacityMl = 80;
  CHECK(phMaxMl(small, rt) == 40.0);  // half its canister: it waits for a refill no sooner than a small one
  // A smaller set maximum holds at once (R7); another volume or another pH− canister starts afresh
  Config set = cfg;
  set.control.phMaxMlPerDose = 5;
  CHECK(phMaxMl(set, rt) == 5.0);
  CHECK(std::isnan(phGrowth(set, rt).ml));
  CHECK(phGrowth(set, rt).unseenMl == 300.0);  // the pH− already in the water still counts
  Config vol = cfg;
  vol.tank.volumeL = 29000;
  CHECK(phMaxMl(vol, rt) == 20.0);
  Config other = cfg;
  other.canisters[0].name = "pH− strong";
  CHECK(phMaxMl(other, rt) == 20.0);
  Config moved = cfg;
  moved.canisters[0].id = "down2";
  CHECK(phMaxMl(moved, rt) == 20.0);
  CHECK(phGrowth(vol, rt).unseenMl == 0.0);  // another volume: other water
  // Damaged: nothing grown, and the count used up, the safe side (R5); beyond one dose's 0.3 pH a
  // grown dose is not believed
  RuntimeState bad = rt;
  bad.phGrowth["unseenMl"] = "x";
  CHECK(phMaxMl(cfg, bad) == 20.0);
  CHECK(phGrowth(cfg, bad).unseenMl == phUnseenAllowedMl(30000));
  RuntimeState big = rt;
  big.phGrowth["ml"] = 0.3 / kPhStartEffect * 30000 + 1;
  CHECK(phMaxMl(cfg, big) == 20.0);
  // Kept as a record of the runtime state
  const RuntimeState back = runtimeFromJson(json(rt));
  CHECK(phMaxMl(cfg, back) == doctest::Approx(270.0));
}

TEST_CASE("pH− in a large tank: a dose at the largest dose that lowers pH by less than can be measured lets the next be twice as large, until one shows; the grown dose survives a restart and goes with another volume (SD-151, RAT-050)") {
  harness::Run r(harness::device("pro"));
  // 2 m³: 20 ml lower pH by about 0.02 in the 5 h, less than can be measured; 40 ml by about 0.04
  largeTank(r, 2000, 2000);
  CHECK(r.state()["cycle"]["settings"]["phMaxMl"] == 20.0);
  waitFor(r, "a pH− dose that shows", [&] {
    for (const auto& e : events(r, "ev.cycle.ph_dose"))
      if (e["data"]["from"].get<double>() - e["data"]["to"].get<double>() >= kNoEffectPh) return true;
    return false;
  }, 30 * 3600);
  const auto grown = events(r, "ev.ph.cap_grown");
  REQUIRE(grown.size() >= 1);
  CHECK(grown.back()["data"]["from"] == 20.0);  // newest first: the first step
  CHECK(grown.back()["data"]["to"] == 40.0);
  json s = r.state();
  CHECK_FALSE(s["latches"].contains("ph.no_effect"));
  CHECK(s["cycle"]["settings"]["phMaxGrown"] == true);
  const double cap = s["cycle"]["settings"]["phMaxMl"];
  CHECK(cap >= 40.0);
  // The dose that showed was larger than the set maximum, and within 0.3 pH at the strongest effect
  bool larger = false;
  for (const auto& e : events(r, "ev.cycle.ph_dose")) {
    larger = larger || e["data"]["ml"].get<double>() > 20.5;
    CHECK(e["data"]["ml"].get<double>() <= 0.3 / kPhStartEffect * 2000 + 1e-6);
  }
  CHECK(larger);
  // A dose grown beyond the set maximum confirms no volume, however it showed
  CHECK(s["newTank"] == true);
  // A restart keeps it
  r.act("reboot", {{"outageS", 10}});
  r.step(5 * kSecond);
  REQUIRE(r.login("demo-password"));  // sessions live in memory
  CHECK(r.state()["cycle"]["settings"]["phMaxMl"] == cap);
  // A smaller set maximum holds at once (R7)
  r.ok("PUT", "control", {{"phMaxMlPerDose", 5}});
  CHECK(r.state()["cycle"]["settings"]["phMaxMl"] == 5.0);
  r.ok("PUT", "control", {{"phMaxMlPerDose", 20}});
  // A new fill starts again from the set maximum: the dose grew for the water before
  r.ok("POST", "tank/new");
  CHECK(r.state()["cycle"]["settings"]["phMaxMl"] == 20.0);
  // Another volume does too
  r.ok("PUT", "tank", {{"volumeL", 1900}});
  s = r.state();
  CHECK(s["cycle"]["settings"]["phMaxMl"] == 20.0);
  CHECK(s["cycle"]["settings"]["phMaxGrown"] == false);
}

TEST_CASE("pH− in a large tank: the doses without a measurable fall stay within what one dose may do, then two without effect lock pH− as before (SD-151, RAT-041)") {
  harness::Run r(harness::device("pro"));
  // Entered 2 m³, holding 20 m³: no dose the robot may make for 2 m³ shows
  largeTank(r, 20000, 2000);
  waitFor(r, "the lock", [&] { return r.state()["latches"].contains("ph.no_effect"); }, 40 * 3600);
  double total = 0;
  for (const auto& e : events(r, "ev.cycle.ph_dose")) total += e["data"]["ml"].get<double>();
  // 0.3 pH at the strongest effect in the entered 2,000 L: 120 ml in all
  CHECK(total <= 0.3 / kPhStartEffect * 2000 + 1e-6);
  CHECK(events(r, "ev.ph.cap_grown").size() >= 1);
  // A person who released it: what grew taught nothing, the dose starts again from the set maximum
  r.ok("POST", "latches/ph.no_effect/ack", json::object());
  const json s = r.state();
  CHECK_FALSE(s["latches"].contains("ph.no_effect"));
  CHECK(s["cycle"]["settings"]["phMaxMl"] == 20.0);
  CHECK(s["cycle"]["settings"]["phMaxGrown"] == false);
}

TEST_CASE("pH− in a large tank: in 30 m³ the dose grows no larger than one round of its pump can run, so pH− keeps being dosed until it locks (SD-151, RAT-055)") {
  harness::Run r(harness::device());
  r.act("fill", {{"volumeL", 30000}, {"ec", 1.65}, {"ph", 7.0}});
  r.ok("PUT", "tank", {{"volumeL", 30000}});
  const json cfg = r.ok("GET", "config");
  std::string down, id;
  for (const auto& b : cfg["canisters"])
    if (b["role"] == "ph_down") {
      down = std::to_string(b["channel"].get<int>());
      id = b["id"];
    }
  REQUIRE_FALSE(down.empty());
  // A 3 L canister, full: its level never holds a dose back here
  r.ok("POST", "canisters", {{"id", id}, {"capacityMl", 3000}});
  r.ok("POST", "canisters/" + id + "/stock", {{"full", true}});
  const double flow = cfg["channels"][down]["flowMlPerMin"];
  const double round = 6 * 60 * flow / 60;
  bool waited = false;
  waitFor(r, "the lock", [&] {
    waited = waited || r.line() == "cycle.wait.ph_plan";
    return r.state()["latches"].contains("ph.no_effect");
  }, 30 * 3600);
  CHECK_FALSE(waited);
  double largest = 0, total = 0;
  for (const auto& e : events(r, "ev.cycle.ph_dose")) {
    largest = std::max(largest, e["data"]["ml"].get<double>());
    total += e["data"]["ml"].get<double>();
  }
  CHECK(largest > 20.5);
  CHECK(largest <= round + 1e-6);
  CHECK(total <= 0.3 / kPhStartEffect * 30000 + round);
}

TEST_CASE("pH− in a large tank: a set maximum lowered while a dose mixes holds after it; nothing grows for the old one (SD-151, R7, RAT-057)") {
  harness::Run r(harness::device("pro"));
  largeTank(r, 2000, 2000);
  waitFor(r, "the first pH− dose mixing", [&] {
    const json c = r.state()["cycle"];
    return c["phase"] == "mix" && c["round"].is_object() && c["round"]["kind"] == "ph";
  }, 6 * 3600);
  r.ok("PUT", "control", {{"phMaxMlPerDose", 5}});
  waitFor(r, "the dose judged", [&] { return !events(r, "ev.cycle.ph_dose").empty(); }, 8 * 3600);
  r.step(10 * kSecond);
  CHECK(events(r, "ev.ph.cap_grown").empty());
  CHECK(r.state()["cycle"]["settings"]["phMaxMl"] == 5.0);
  CHECK(r.state()["cycle"]["settings"]["phMaxGrown"] == false);
}

TEST_CASE("pH− in a large tank: the count of pH− without a fall survives restarts, and once it reaches what one dose may do a grown dose locks pH− at once (SD-151, RAT-041)") {
  harness::Run r(harness::device("pro"));
  // Entered 2 m³, holding 20 m³; a restart after each dose loses the count of doses in a row
  largeTank(r, 20000, 2000);
  size_t seen = 0;
  for (int i = 0; i < 12 && !r.state()["latches"].contains("ph.no_effect"); ++i) {
    waitFor(r, "a pH− dose judged, or the lock", [&] {
      return events(r, "ev.cycle.ph_dose").size() > seen || r.state()["latches"].contains("ph.no_effect");
    }, 12 * 3600);
    seen = events(r, "ev.cycle.ph_dose").size();
    r.act("reboot", {{"outageS", 10}});
    r.step(5 * kSecond);
    REQUIRE(r.login("demo-password"));
  }
  waitFor(r, "the lock", [&] { return r.state()["latches"].contains("ph.no_effect"); }, 12 * 3600);
  bool byCount = false;  // by the count, not by two in a row
  for (const auto& e : r.events("alarm")) byCount = byCount || e["title"]["key"] == "ev.ph.unseen";
  CHECK(byCount);
  double total = 0;
  for (const auto& e : events(r, "ev.cycle.ph_dose")) total += e["data"]["ml"].get<double>();
  CHECK(total <= phUnseenAllowedMl(2000) + 40.0 + 1e-6);  // what one dose may do, and one grown dose
}

TEST_CASE("pH− in a large tank: with \"no effect\" reported only, the dose does not grow while the report stands and goes back to the set maximum once the count is used up (SD-151, SD-133)") {
  harness::Run r(harness::device("pro"));
  r.ok("PUT", "locks", {{"reportOnly", {"ph.no_effect"}}});
  largeTank(r, 20000, 2000);
  waitFor(r, "the report", [&] { return r.state()["latches"].contains("ph.no_effect"); }, 30 * 3600);
  const size_t grown = events(r, "ev.ph.cap_grown").size();
  waitFor(r, "back at the set maximum", [&] { return r.state()["cycle"]["settings"]["phMaxMl"] == 20.0; }, 30 * 3600);
  // It goes on dosing, never larger than the set maximum from here, and grows no more
  const size_t before = events(r, "ev.cycle.ph_dose").size();
  waitFor(r, "another dose", [&] { return events(r, "ev.cycle.ph_dose").size() > before; }, 12 * 3600);
  CHECK(events(r, "ev.cycle.ph_dose").front()["data"]["ml"].get<double>() <= 20.0 + 0.01);  // as booked, to whole ms
  CHECK(events(r, "ev.ph.cap_grown").size() == grown);
}
