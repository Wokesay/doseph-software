// SPDX-License-Identifier: AGPL-3.0-or-later
// Dose planning.
#include <doctest/doctest.h>

#include <limits>

#include "doseph/plan.hpp"

using namespace doseph;

namespace {

struct Fix {
  Config cfg;
  Fix() {
    cfg.canisters = {{"a", "Part A", "nutrient", 1, "AB", "#00c"},
                   {"b", "Part B", "nutrient", 2, "AB", "#0c0"},
                   {"cm", "CalMag", "nutrient", 3, "", "#cc0"},
                   {"down", "pH−", "ph_down", 4, "", "#c00"}};
    for (int n = 1; n <= 4; ++n) cfg.pumps[n] = {50.0, 1};
    cfg.recipes.push_back(RecipeCfg{"r", "Test", "", {{"a", 1.0}, {"b", 1.0}}});
  }
  RecipeCfg& recipe() { return cfg.recipes[0]; }
};

double total(const EcDose& d) {
  double s = 0;
  for (const auto& x : d.steps) s += x.ml;
  return s;
}

}  // namespace

TEST_CASE("Partial runs: equal in size, none under 1 s, no more than allowed (RAT-050, RAT-055)") {
  Limits lim;
  Msg err;
  auto r = splitRuns(100, 50, lim, 0, err);  // 120 s
  REQUIRE(r.size() == 2);
  CHECK(r[0] == 60000);
  CHECK(r[1] == 60000);
  splitRuns(0.5, 50, lim, 0, err);  // 0.6 s
  CHECK(err.key == "dose.too_small");
  err = {};
  splitRuns(500, 50, lim, 6, err);  // 10 runs > 6
  CHECK(err.key == "dose.too_large");
  err = {};
  splitRuns(10, kNaN, lim, 0, err);  // no calibration: nothing (RAT-015)
  CHECK(err.key == "dose.no_flow");
}

TEST_CASE("EC round: 0.8 × gap, the pair scaled together, its ratio kept at every cap (RAT-054, RAT-055)") {
  Fix f;
  auto d = planEcDose(f.cfg, f.recipe(), 20, 1.0, 0.275, 0.275, 1.0);
  REQUIRE(d.ok);
  CHECK(total(d) == doctest::Approx(58.18).epsilon(0.001));
  CHECK(d.steps[0].ml == doctest::Approx(d.steps[1].ml));
  // A cap of 0.4 mS/cm: both cut together, the ratio stays
  auto c = planEcDose(f.cfg, f.recipe(), 20, 1.0, 0.275, 0.275, 0.4);
  REQUIRE(c.ok);
  CHECK(c.factor == doctest::Approx(0.5));
  CHECK(c.steps[0].ml / c.steps[1].ml == doctest::Approx(1.0));
}

TEST_CASE("EC round over the partial-run limit: made smaller by one factor for all parts, never refused or capped per pump (RAT-054)") {
  Fix f;
  f.recipe() = {"r", "Test", "", {{"a", 2.0}, {"b", 1.0}}};
  f.cfg.pumps[1] = {6.0, 1};  // a small pump: part A alone needs more than 6 runs of 60 s
  const auto d = planEcDose(f.cfg, f.recipe(), 100, 1.0, 0.275, 0.275, 1.0);
  REQUIRE(d.ok);
  CHECK(d.limited);
  CHECK(d.steps[0].ml == doctest::Approx(36.0).epsilon(0.001));  // 6 runs × 60 s × 6 ml/min
  CHECK(d.steps[0].ml == doctest::Approx(2.0 * d.steps[1].ml));
  CHECK(d.steps[0].runs.size() <= 6);
  const auto small = planEcDose(f.cfg, f.recipe(), 10, 1.0, 0.275, 0.275, 1.0);
  REQUIRE(small.ok);
  CHECK_FALSE(small.limited);
  // Made smaller, a part below one accurate run still refuses the round, for the whole pair
  f.recipe() = {"r", "Test", "", {{"a", 2.0}, {"b", 0.01}}};
  const auto tiny = planEcDose(f.cfg, f.recipe(), 100, 1.0, 0.275, 0.275, 1.0);
  CHECK_FALSE(tiny.ok);
  CHECK(tiny.steps.empty());
  CHECK(tiny.reason.key == "dose.part");
}

TEST_CASE("EC cap: a small learned effect never widens it, and it never goes without one (RAT-056, R5)") {
  Fix f;
  // Effect 0.3 × start: raw 0.8 / 0.0825 = 9.7 ml/L; cap 1.0 / max(0.275, 0.0825) = 3.64 ml/L
  auto d = planEcDose(f.cfg, f.recipe(), 20, 1.0, 0.0825, 0.275, 1.0);
  REQUIRE(d.ok);
  CHECK(total(d) == doctest::Approx(1.0 / 0.275 * 20).epsilon(0.001));
  const double capMl = 1.0 / kEcStartEffect * 20;
  for (double step : {1.0, kNaN, -1.0, std::numeric_limits<double>::infinity()})
    for (double start : {kNaN, 0.1, -1.0}) {
      const auto x = planEcDose(f.cfg, f.recipe(), 20, 3.0, 0.1, start, step);
      REQUIRE(x.ok);
      CHECK(total(x) <= doctest::Approx(capMl));
    }
}

TEST_CASE("EC round: a fixed addition is neither dosed nor scaled; the round goes to the other parts in their ratio (RAT-085)") {
  Fix f;
  f.recipe() = {"r", "Test", "", {{"a", 1.0}, {"b", 1.0}, {"cm", 0.5, true}}};
  auto d = planEcDose(f.cfg, f.recipe(), 20, 1.0, 0.275, 0.275, 1.0);
  REQUIRE(d.ok);
  REQUIRE(d.steps.size() == 2);
  for (const auto& s : d.steps) CHECK(s.canister != "cm");
  CHECK(total(d) == doctest::Approx(58.18).epsilon(0.001));
  f.recipe() = {"r", "Test", "", {{"cm", 0.5, true}}};
  auto n = planEcDose(f.cfg, f.recipe(), 20, 1.0, 0.275, 0.275, 1.0);
  CHECK_FALSE(n.ok);
  CHECK(n.reason.key == "ec.recipe_no_scaled");
}

TEST_CASE("EC round: a canister on no pump, or an uncalibrated pump, refuses the round (RAT-015)") {
  Fix f;
  f.cfg.canisters[1].pump = 0;
  auto d = planEcDose(f.cfg, f.recipe(), 20, 1.0, 0.275, 0.275, 1.0);
  CHECK_FALSE(d.ok);
  CHECK(d.reason.key == "dose.no_pump");
  Fix g;
  g.cfg.pumps.erase(2);
  auto e = planEcDose(g.cfg, g.recipe(), 20, 1.0, 0.275, 0.275, 1.0);
  CHECK_FALSE(e.ok);
  CHECK(e.reason.key == "dose.part");
  CHECK(planEcDose(g.cfg, g.recipe(), kNaN, 1.0, 0.275, 0.275, 1.0).reason.key == "ec.no_volume");  // R5
}

TEST_CASE("Test dose: the first round of a new tank aims at a small rise at the largest effect (SD-105)") {
  Fix f;
  // A gap of 1.5 mS/cm in 100 L: a normal round caps at 1.0 / 0.275 × 100 = 364 ml
  auto normal = planEcDose(f.cfg, f.recipe(), 100, 1.5, 0.275, 0.275, 1.0);
  auto test = planEcDose(f.cfg, f.recipe(), 100, 1.5, 0.275, 0.275, 1.0, 0.1);
  REQUIRE(normal.ok);
  REQUIRE(test.ok);
  CHECK(total(test) == doctest::Approx(0.1 / 0.275 * 100).epsilon(0.001));
  CHECK(total(test) < total(normal) / 5);
  CHECK(test.steps[0].ml == doctest::Approx(test.steps[1].ml));  // the pair still together
  // A gap smaller than the test's aim: 0.8 × gap, as always
  auto near = planEcDose(f.cfg, f.recipe(), 100, 0.05, 0.275, 0.275, 1.0, 0.1);
  REQUIRE(near.ok);
  CHECK(total(near) == doctest::Approx(0.8 * 0.05 / 0.275 * 100).epsilon(0.001));
  // pH: at most 0.1 pH at the start effect
  auto ph = planPhDose(7.6, 6.0, kPhStartEffect, 100, 20, 0.1);
  REQUIRE(ph.ok);
  CHECK(ph.ml == doctest::Approx(0.1 / 5.0 * 100));
}

TEST_CASE("pH dose: its cap and the start effect (RAT-050)") {
  auto d = planPhDose(6.5, 5.9, 5.0, 20, 20);
  REQUIRE(d.ok);
  CHECK(d.rawMl == doctest::Approx(1.92));
  CHECK(d.ml == doctest::Approx(1.2));
  CHECK(clampEffect(0.5, 5.0) == doctest::Approx(5.0));
  CHECK(clampEffect(30, 5.0) == doctest::Approx(20.0));
  CHECK(clampEffect(kNaN, 5.0) == doctest::Approx(5.0));
  CHECK_FALSE(planPhDose(5.8, 5.9, 5.0, 20, 20).ok);
  CHECK_FALSE(planPhDose(6.5, 5.9, 5.0, kNaN, 20).ok);
}

TEST_CASE("EC effect of a recipe: its EC target only ever makes a round smaller; the best guess for the test dose (RAT-056, RAT-081, SD-105)") {
  RecipeCfg r{"r", "Test", "", {{"a", 2.0}, {"b", 2.0}, {"cm", 1.0, true}}};
  auto e = recipeEffect(r, kNaN);
  CHECK(e.start == doctest::Approx(kEcStartEffect));
  CHECK(e.expected == doctest::Approx(kEcStartEffect));
  r.ecTarget = 2.0;  // 2.0 / 4 ml/L = 0.5, the fixed part left out
  e = recipeEffect(r, kNaN);
  CHECK(e.start == doctest::Approx(0.5));
  CHECK(e.expected == doctest::Approx(0.5));
  r.ecTarget = 0.6;  // 0.15 per ml/L, weaker than the start: the dose keeps the start, the guess is the reference
  e = recipeEffect(r, kNaN);
  CHECK(e.start == doctest::Approx(kEcStartEffect));
  CHECK(e.expected == doctest::Approx(0.15));
  e = recipeEffect(r, 0.45);  // learned: used, and the best guess
  CHECK(e.dose == doctest::Approx(0.45));
  CHECK(e.expected == doctest::Approx(0.45));
  e = recipeEffect(r, 3.0);  // above four times: four times
  CHECK(e.dose == doctest::Approx(4 * kEcStartEffect));
  CHECK(e.largest == doctest::Approx(4 * kEcStartEffect));
  for (double ref : {0.2, 5.0, -1.0, 0.0}) {  // outside the band: none
    r.ecTarget = ref;
    CHECK(recipeEffect(r, kNaN).start == doctest::Approx(kEcStartEffect));
  }
}

TEST_CASE("EC effect after an edit: scaled up by parts no longer scaled, kept only at or above the new start (RAT-085)") {
  const RecipeCfg before{"r", "Test", "", {{"a", 2.0}, {"b", 2.0}, {"cm", 0.6}}};
  RecipeCfg same = before;
  for (auto& s : same.steps) s.mlPerL *= 1.5;
  same.name = "Renamed";
  auto e = editEcEffect(before, same, 0.32);
  CHECK_FALSE(e.changed);
  CHECK(e.learned == doctest::Approx(0.32));
  RecipeCfg fixed = before;
  fixed.steps[2].fixed = true;
  e = editEcEffect(before, fixed, 0.3);
  CHECK(e.changed);
  CHECK(e.learned == doctest::Approx(0.3 * 4.6 / 4.0));
  RecipeCfg ratio = before;
  ratio.steps[0].mlPerL = 3.0;
  CHECK(std::isnan(editEcEffect(before, ratio, 0.2).learned));
  RecipeCfg stronger = before;
  stronger.ecTarget = 2.3;  // 0.5 per ml/L of 4.6
  CHECK(std::isnan(editEcEffect(before, stronger, 0.3).learned));
  CHECK(editEcEffect(before, stronger, 0.6).learned == doctest::Approx(0.6));
}

TEST_CASE("The mixing wait the robot suggests: about two turnovers for EC, three for pH, in whole minutes within the bounds (SD-105)") {
  CHECK(suggestedWaitMin(100, 10, 2) == 20);   // 100 L at 10 L/min: a turnover of 10 min
  CHECK(suggestedWaitMin(100, 10, 3) == 30);
  CHECK(suggestedWaitMin(100, 12, 2) == 17);   // 16.7 rounded up
  CHECK(suggestedWaitMin(10, 20, 2) == kWaitMinMin);  // never below 2 min (RAT-052)
  CHECK(suggestedWaitMin(10000, 5, 3) == kWaitMaxMin);
  CHECK(std::isnan(suggestedWaitMin(kNaN, 10, 2)));  // no volume: no suggestion (R5)
  CHECK(std::isnan(suggestedWaitMin(100, 0, 2)));
}
