// SPDX-License-Identifier: AGPL-3.0-or-later
// The largest pH− dose in a large tank (SD-151): it grows step by step while its doses cannot be
// measured, within what one dose may do.
#include <doctest/doctest.h>

#include <functional>
#include <string>

#include "doseph/cycle.hpp"
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
  // A restart keeps it
  r.act("reboot", {{"outageS", 10}});
  r.step(5 * kSecond);
  REQUIRE(r.login("demo-password"));  // sessions live in memory
  CHECK(r.state()["cycle"]["settings"]["phMaxMl"] == cap);
  // Another volume starts again from the set maximum
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
  // A person who released it lets the doses grow once more
  r.ok("POST", "latches/ph.no_effect/ack", json::object());
  CHECK_FALSE(r.state()["latches"].contains("ph.no_effect"));
}
