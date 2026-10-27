// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/plan.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "doseph/messages.hpp"

namespace doseph {

void to_json(json& j, const DoseStep& s) {
  j = {{"canister", s.canister}, {"name", s.name},   {"pump", s.pump},   {"pair", s.pair},
       {"color", s.color},   {"mlPerL", numOrNull(s.mlPerL)},       {"ml", numOrNull(s.ml)},
       {"flowMlPerMin", numOrNull(s.flowMlPerMin)}, {"runs", s.runs}};
}

std::vector<Ms> splitRuns(double ml, double flowMlPerMin, const Limits& limits, int maxRuns, Msg& err) {
  const Limits lim = limits.bounded();
  if (!isNum(flowMlPerMin) || flowMlPerMin <= 0) {
    err = say("dose.no_flow");
    return {};
  }
  if (!isNum(ml) || ml <= 0) {
    err = say("dose.no_amount");
    return {};
  }
  double totalMs = ml / flowMlPerMin * 60000.0;
  if (totalMs < lim.minRunS * 1000.0) {
    err = say("dose.too_small", {{"ml", ml}, {"s", lim.minRunS}});
    return {};
  }
  constexpr int kMaxRunsPerStep = 200;  // against senseless amounts
  const int cap = maxRuns > 0 ? std::min(maxRuns, kMaxRunsPerStep) : kMaxRunsPerStep;
  const double needed = std::ceil(totalMs / (lim.maxRunS * 1000.0));
  if (needed > cap) {
    err = say("dose.too_large", {{"ml", ml}, {"runs", cap}});
    return {};
  }
  const int n = static_cast<int>(needed);
  std::vector<Ms> runs;
  Ms total = static_cast<Ms>(std::llround(totalMs));
  for (int i = 0; i < n; ++i) runs.push_back(total / n + (i < total % n ? 1 : 0));
  return runs;
}

EcDose planEcDose(const Config& cfg, const RecipeCfg& recipe, double volumeL, double gapEc, double effectPerMlL,
                  double startEffect, double maxEcStep, double aimRise) {
  EcDose d;
  if (!isNum(volumeL) || volumeL <= 0) {
    d.reason = say("ec.no_volume");
    return d;
  }
  if (!isNum(effectPerMlL) || effectPerMlL <= 0) {
    d.reason = say("ec.effect_unknown");
    return d;
  }
  // Only the parts the robot scales; a fixed addition is not dosed by a round (RAT-085)
  double sumMlPerL = 0;
  bool anyFixed = false;
  for (const auto& s : recipe.steps) {
    anyFixed |= s.fixed;
    sumMlPerL += !s.fixed && isNum(s.mlPerL) ? s.mlPerL : 0;
  }
  if (sumMlPerL <= 0) {
    d.reason = anyFixed ? say("ec.recipe_no_scaled", {{"name", recipe.name}}) : say("ec.recipe");
    return d;
  }
  if (!isNum(gapEc) || gapEc <= 0) {
    d.reason = say("ec.reached");
    return d;
  }
  if (!isNum(maxEcStep) || maxEcStep <= 0) maxEcStep = 1.0;  // never without a cap
  if (!isNum(startEffect) || startEffect <= 0) startEffect = kEcStartEffect;
  const double largest = std::max({kEcStartEffect, startEffect, effectPerMlL});
  const double rawMlPerL = 0.8 * gapEc / effectPerMlL;
  // Never wider than at the start effect in code, whatever comes in (RAT-056)
  double capMlPerL = maxEcStep / largest;
  // A test dose aims at a small rise at the largest effect: the smallest dose (SD-105)
  if (isNum(aimRise) && aimRise > 0) capMlPerL = std::min(capMlPerL, aimRise / largest);
  const double totalMlPerL = std::min(rawMlPerL, capMlPerL);  // ml/L of the whole recipe
  d.factor = totalMlPerL / rawMlPerL;
  for (const auto& s : recipe.steps) {
    const CanisterCfg* b = cfg.canister(s.canister);
    if (!b || s.fixed) continue;
    DoseStep ds;
    ds.canister = b->id;
    ds.name = b->name;
    ds.pump = b->pump;
    ds.pair = b->pair;
    ds.color = b->color;
    ds.mlPerL = s.mlPerL;
    ds.ml = totalMlPerL * (s.mlPerL / sumMlPerL) * volumeL;
    ds.flowMlPerMin = cfg.pumpFlow(b->pump);
    d.steps.push_back(ds);
  }
  // A round one part of which needs more partial runs than allowed is made
  // smaller by one factor for all parts, as at every limit, instead of
  // refused (RAT-054). A part too small or without a calibration still
  // refuses the whole round.
  const Limits lim = cfg.limits.bounded();
  const double maxMs = lim.maxPartialRuns * lim.maxRunS * 1000.0;
  double scale = 1.0;
  for (const auto& ds : d.steps)
    if (isNum(ds.flowMlPerMin) && ds.flowMlPerMin > 0 && isNum(ds.ml) && ds.ml > 0)
      scale = std::min(scale, maxMs / (ds.ml / ds.flowMlPerMin * 60000.0));
  if (scale < 1.0) {
    scale *= 1.0 - 1e-9;  // just inside the limit, whatever the rounding of the run times
    for (auto& ds : d.steps) ds.ml *= scale;
    d.factor *= scale;
    d.limited = true;
  }
  // Partial runs: if one part is too small or too large, that holds for all
  // together (never capped one by one, RAT-054).
  for (auto& ds : d.steps) {
    if (ds.pump <= 0) {
      d.reason = say("dose.no_pump", {{"name", ds.name}});
      d.steps.clear();
      return d;
    }
    Msg err;
    ds.runs = splitRuns(ds.ml, ds.flowMlPerMin, cfg.limits, lim.maxPartialRuns, err);
    if (!err.key.empty()) {
      d.reason = say("dose.part", {{"name", ds.name}, {"reason", err}});
      d.steps.clear();
      return d;
    }
    d.totalMl += ds.ml;
  }
  d.ok = !d.steps.empty();
  return d;
}

PhDose planPhDose(double ph, double target, double effectPerMlL, double volumeL, double maxMl, double aimDrop) {
  PhDose d;
  if (!isNum(ph) || !isNum(target)) {
    d.reason = say("ph.no_value");
    return d;
  }
  if (!isNum(volumeL) || volumeL <= 0) {
    d.reason = say("ph.no_volume");
    return d;
  }
  if (!isNum(effectPerMlL) || effectPerMlL <= 0) effectPerMlL = kPhStartEffect;  // the smallest dose (RAT-050)
  const double gap = ph - target;
  if (gap <= 0) {
    d.reason = say("ph.reached");
    return d;
  }
  d.rawMl = 0.8 * gap / effectPerMlL * volumeL;
  const double capEffect = 0.3 / effectPerMlL * volumeL;
  const double capPerL = 0.3 * volumeL;
  d.capMl = std::min({capEffect, capPerL, isNum(maxMl) && maxMl > 0 ? maxMl : 20.0});
  if (isNum(aimDrop) && aimDrop > 0) d.capMl = std::min(d.capMl, aimDrop / effectPerMlL * volumeL);
  d.ml = std::min(d.rawMl, d.capMl);
  d.ok = true;
  return d;
}

double clampEffect(double observed, double start) {
  if (!isNum(observed) || observed <= 0) return start;
  if (observed < 0.25 * start) return start;
  if (observed > 4.0 * start) return 4.0 * start;
  return observed;
}

namespace {
// The parts the robot scales, by canister (RAT-085)
std::map<std::string, double> scaledParts(const RecipeCfg& recipe) {
  std::map<std::string, double> parts;
  for (const auto& s : recipe.steps)
    if (!s.fixed && isNum(s.mlPerL) && s.mlPerL > 0) parts[s.canister] += s.mlPerL;
  return parts;
}
double sumOf(const std::map<std::string, double>& parts) {
  double sum = 0;
  for (const auto& [canister, ml] : parts) sum += ml;
  return sum;
}
}  // namespace

RecipeEffect recipeEffect(const RecipeCfg& recipe, double learned) {
  RecipeEffect e;
  const double sum = sumOf(scaledParts(recipe));
  // A reference outside the band (a file edited by hand) counts as none
  const double ref = isNum(recipe.ecTarget) && recipe.ecTarget > 0 && sum > 0 ? recipe.ecTarget / sum : kNaN;
  const bool refOk = isNum(ref) && ref >= kRefEffectMin * kEcStartEffect && ref <= kRefEffectMax * kEcStartEffect;
  if (refOk) e.start = std::max(kEcStartEffect, ref);
  e.dose = clampEcEffect(learned, e.start);
  // A stored value above the band counts at its top: the cap never shrinks so
  // far that no round fits and nothing could be learned again (RAT-056)
  e.largest = std::max(e.start, e.dose);
  const bool learnedOk = isNum(learned) && learned >= kRefEffectMin * kEcStartEffect;
  e.expected = learnedOk ? std::min(learned, kRefEffectMax * kEcStartEffect) : refOk ? ref : kEcStartEffect;
  return e;
}

double clampEcEffect(double observed, double start) {
  if (!isNum(observed) || observed < kRefEffectMin * kEcStartEffect) return start;
  return std::min(observed, kRefEffectMax * kEcStartEffect);
}

double learnedEcEffect(const RuntimeState& rt, const std::string& recipe) {
  auto it = rt.ecEffects.find(recipe);
  return it == rt.ecEffects.end() ? kNaN : it->second;
}

EffectEdit editEcEffect(const RecipeCfg& before, const RecipeCfg& after, double learned) {
  EffectEdit e;
  const auto was = scaledParts(before), now = scaledParts(after);
  const double sumWas = sumOf(was), sumNow = sumOf(now);
  // The same parts in the same ratio (all amounts alike, or only the name or the target): unchanged
  bool same = was.size() == now.size() && sumWas > 0 && sumNow > 0;
  for (auto w = was.begin(), n = now.begin(); same && w != was.end(); ++w, ++n)
    same = w->first == n->first && std::fabs(w->second / sumWas - n->second / sumNow) < 1e-9;
  if (same || (was.empty() && now.empty())) {
    // A higher target on the same parts may be a stronger product in the same
    // canisters: what was learned below its new start is forgotten
    const double startNow = recipeEffect(after, kNaN).start;
    if (!(isNum(learned) && learned < startNow && startNow > recipeEffect(before, kNaN).start)) e.learned = learned;
    return e;
  }
  e.changed = true;
  if (!isNum(learned)) return e;
  double gone = 0;  // what was scaled and is no longer: fixed now or removed
  for (const auto& [canister, ml] : was)
    if (!now.count(canister)) gone += ml;
  if (gone >= sumWas || sumNow <= 0) return e;  // nothing it was learned on is scaled any more
  const double scaled = std::min(learned * sumWas / (sumWas - gone), kRefEffectMax * kEcStartEffect);
  if (scaled >= recipeEffect(after, kNaN).start) e.learned = scaled;
  return e;
}

double suggestedWaitMin(double volumeL, double flowLpm, double turnovers) {
  if (!isNum(volumeL) || volumeL <= 0 || !isNum(flowLpm) || flowLpm <= 0 || !isNum(turnovers)) return kNaN;
  return std::clamp(std::ceil(turnovers * volumeL / flowLpm), kWaitMinMin, kWaitMaxMin);
}

}  // namespace doseph
