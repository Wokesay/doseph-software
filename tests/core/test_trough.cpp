// SPDX-License-Identifier: AGPL-3.0-or-later
// Dosing through the trough (SD-147): the trough runs clear before another channel, a dose's
// partial runs wait one pass, and silicate goes first in a recipe.
#include <doctest/doctest.h>

#include <algorithm>
#include <map>

#include "doseph/cycle.hpp"
#include "doseph/gateway.hpp"
#include "fakes.hpp"
#include "harness.hpp"

using namespace doseph;

namespace {

bool has(const std::vector<Msg>& errs, const std::string& key) {
  return std::any_of(errs.begin(), errs.end(), [&](const Msg& m) { return m.key == key; });
}

}  // namespace

TEST_CASE("Trough: a pass takes the trough's water at the flow; without either, none (SD-147)") {
  CHECK(troughPassMs(2.0, 12.0, 3) == 30 * kSecond);
  CHECK(troughPassMs(0.5, 10.0, 1) == 3 * kSecond);
  CHECK(troughPassMs(kNaN, 12.0, 3) == 0);
  CHECK(troughPassMs(2.0, kNaN, 3) == 0);
  CHECK(troughPassMs(2.0, 0.0, 3) == 0);
}

TEST_CASE("Trough: before another channel of a round the trough runs clear for three passes while water flows; the same channel goes on at once (SD-147)") {
  harness::Run r(harness::device("pro"));  // the trough holds 2 L, the flow is 12 L/min: 30 s
  sim::World& w = r.s.world();
  // Who ran when, second by second, through the first nutrient round
  std::map<size_t, std::vector<long>> running;
  long t = 0;
  bool doneEc = false;
  for (; t < 4 * 3600 && !doneEc; ++t) {
    r.step(kSecond);
    for (size_t i = 0; i < w.pumps.size(); ++i)
      if (w.pumps[i].state == 1) running[i].push_back(t);
    doneEc = r.phase() == "mix" && running.size() >= 2;
  }
  REQUIRE(doneEc);
  // The channels in the order they ran, and the gap from one's last second to the next one's first
  std::vector<std::pair<long, size_t>> firsts;
  for (const auto& [i, ts] : running) firsts.emplace_back(ts.front(), i);
  std::sort(firsts.begin(), firsts.end());
  REQUIRE(firsts.size() >= 2);
  for (size_t k = 1; k < firsts.size(); ++k) {
    const long lastOfPrevious = running[firsts[k - 1].second].back();
    INFO("channel " << firsts[k].second + 1 << " after channel " << firsts[k - 1].second + 1);
    CHECK(firsts[k].first - lastOfPrevious >= 29);  // 30 s, by whole seconds
  }
}

TEST_CASE("Trough: the partial runs of a dose wait the pause its order gives (RAT-055, SD-147)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg;
  cfg.setup["placement"] = {"done", 1};
  cfg.channels[2] = {60, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Doser doser;
  Msg err;
  DoseOrder o;
  o.purpose = "ec";
  o.step.canister = "a";
  o.step.name = "Part A";
  o.step.channel = 2;
  o.step.flowMlPerMin = 60;
  o.step.runs = {3000, 3000};
  o.pauseMs = 7 * kSecond;
  REQUIRE(doser.start(c, act, o, err));
  hw.runs[2].state = RunStatus::State::Done;
  hw.runs[2].actualMs = 3000;
  c.now += 3100;
  doser.tick(c, act);
  const std::string first = hw.runs[2].jobId;
  c.now += 6 * kSecond;
  doser.tick(c, act);
  CHECK(hw.runs[2].jobId == first);  // still pausing
  c.now += 1 * kSecond;
  doser.tick(c, act);
  CHECK(hw.runs[2].jobId != first);  // the second run after 7 s
}

TEST_CASE("Trough: silicate is a canister's role of its own; in a recipe it goes before every other part, and it is in no pair (SD-147)") {
  Config c;
  c.tank.volumeL = 100;
  c.canisters = {{"si", "Silicate", "silicate", 1, "", "#999"}, {"a", "Part A", "nutrient", 2, "", "#00c"}};
  RecipeCfg r;
  r.id = "r1";
  r.name = "Veg";
  r.steps = {{"si", 0.5, true}, {"a", 2.0, false}};
  r.ecTarget = 1.6;
  r.phLow = 5.8;
  r.phHigh = 6.2;
  c.recipes = {r};
  CHECK(validateConfig(c, 4).empty());
  c.recipes[0].steps = {{"a", 2.0, false}, {"si", 0.5, true}};
  CHECK(has(validateConfig(c, 4), "cfg.recipe.silicate_first"));
  c.recipes[0].steps = {{"si", 0.5, true}, {"a", 2.0, false}};
  c.canisters[0].pair = "AB";
  CHECK(has(validateConfig(c, 4), "cfg.canister.pair_role"));
}
