// SPDX-License-Identifier: AGPL-3.0-or-later
// Dose planning as pure functions (testable, no side effects): an EC round
// with the recipe and a pH− dose. Invariants (docs/RATIONALE.md): pH always
// last (RAT-004); paired nutrients scaled together at every limit, never
// capped or rounded per channel (RAT-054); no dosing without a calibration
// value (RAT-015); a missing quantity aborts without a stand-in (RAT-006);
// no runs under 1 s (RAT-050); large doses in equal partial runs (RAT-055).
#pragma once

#include <string>
#include <vector>

#include "doseph/config.hpp"

namespace doseph {

struct DoseStep {
  std::string canister, name, pair, color;
  int channel = 0;
  std::string device;  // "" this device; the coupled device's ID: the run goes to it (SD-131)
  double mlPerL = kNaN;
  double ml = kNaN;
  double flowMlPerMin = kNaN;
  std::vector<Ms> runs;  // partial runs in ms, equal in size
};
void to_json(json& j, const DoseStep& s);

// Splits a dose into equal runs ≤ maxRunS. Errors: a run under minRunS, more
// runs than maxRuns (0 = no limit), no channel calibration.
std::vector<Ms> splitRuns(double ml, double flowMlPerMin, const Limits& lim, int maxRuns, Msg& err);

// Raise EC with the recipe: dose = 0.8 × gap / effect × V, capped in EC per
// round, all scaled parts by one factor (RAT-055, RAT-054). Fixed additions
// are left out: the round and the effect count the scaled parts only
// (RAT-085).
struct EcDose {
  bool ok = false;
  Msg reason;
  double factor = 1.0;    // the common factor of the caps
  bool limited = false;   // made smaller to fit the partial-run limit
  bool lifted = false;    // a test dose made as large as the smallest round the pumps can run (SD-122)
  bool tooSmall = false;  // a part would run under the minimum run time (RAT-050)
  double totalMl = 0;
  std::vector<DoseStep> steps;
};
// Cap per round in ml/L: maxEcStep / max(start effect, effect). A small
// measured effect never widens the cap (RAT-056). `aimRise` caps the rise the
// round is planned for (a test dose, SD-105); NaN: no such cap. A test dose
// whose smallest part would run under the minimum run time is made as large
// as the smallest round the pumps can run, if that stays within the cap and
// the gap (SD-122).
EcDose planEcDose(const Config& cfg, const RecipeCfg& recipe, double volumeL, double gapEc, double effectPerMlL,
                  double startEffect, double maxEcStep, double aimRise = kNaN);

// pH dose: 0.8 × gap / effect × V, capped by min(0.3 pH at the effect,
// 0.3 ml/L, the largest dose) (RAT-055, RAT-050); `aimDrop` caps the fall it
// is planned for (a test dose, SD-105).
struct PhDose {
  bool ok = false;
  Msg reason;
  double rawMl = kNaN, capMl = kNaN, ml = kNaN;
};
PhDose planPhDose(double ph, double target, double effectPerMlL, double volumeL, double maxMl, double aimDrop = kNaN);

// The start effect of pH− in pH per ml/L: the strongest measured, so the
// smallest dose (RAT-050, RAT-081)
constexpr double kPhStartEffect = 5.0;
// Clamp an effect: observed < 0.25 × start → start; > 4 × start → 4 × start (RAT-055).
double clampEffect(double observed, double start);

// A recipe's EC effect, per ml/L of the parts the robot scales (RAT-085).
// Nothing learned, it starts from the larger of the start effect in code and
// the effect its EC target implies (if within a quarter to four times that),
// so a reference only ever makes a round smaller. The cap converts with the
// larger of start and dose effect, so neither a small reference nor a small
// learned effect widens it (RAT-056, RAT-081).
struct RecipeEffect {
  double start = kEcStartEffect;
  double dose = kEcStartEffect;
  double largest = kEcStartEffect;
  double expected = kEcStartEffect;  // the best guess of the true effect: learned, else the reference, else the start (SD-105)
};
RecipeEffect recipeEffect(const RecipeCfg& recipe, double learned);
// A learned EC effect within a quarter to four times the start effect; below
// it, or missing, the recipe's start (RAT-055)
double clampEcEffect(double observed, double start);
// What the robot learned for a recipe; missing = nothing learned (R5)
double learnedEcEffect(const RuntimeState& rt, const std::string& recipe);
// What an edit of a recipe keeps of its learned EC effect (RAT-085): a change
// of the scaled parts or their ratio scales it up by what is no longer
// scaled, at most to the band's top; then it stays only if it is at least
// the recipe's new start. A higher EC target on the same parts forgets it.
struct EffectEdit {
  bool changed = false;  // the scaled parts or their ratio changed: a round planned on them learns nothing (RAT-057)
  double learned = kNaN;
};
EffectEdit editEcEffect(const RecipeCfg& before, const RecipeCfg& after, double learned);

// The mixing wait the robot suggests: `turnovers` × volume / flow, in whole
// minutes, within the bounds (SD-105); NaN without volume or flow
double suggestedWaitMin(double volumeL, double flowLpm, double turnovers);

// The settling of large tanks (SD-112): a tank above this mixes by the customer's pump, the flow
// through the device is only a sample stream; its wait is the settling time learned from its doses
constexpr double kLargeTankL = 150;
constexpr double kSettleDefaultMin = 60, kSettleMinMin = 5, kSettleMaxMin = 600;
// A dose's settling time, s: from its end until the readings (t, value) stay within `band` of their
// final value; NaN without a reading after its end
double settleSeconds(const std::vector<Epoch>& t, const std::vector<double>& v, Epoch end, double final, double band, int stepS);
// The wait learned from the last three clean doses' settling times (min): 1.5 × the longest, 5 to 600
double learnedSettleMin(const std::vector<double>& lastMin);

}  // namespace doseph
