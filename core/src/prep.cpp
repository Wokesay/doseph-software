// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/prep.hpp"

#include <algorithm>

#include "doseph/messages.hpp"

namespace doseph {

PrepEstimate estimatePrep(const Config& cfg, const RecipeCfg& recipe, const RecipeEffect& eff, const PrepInput& in) {
  PrepEstimate e;
  if (!isNum(in.volumeL) || in.volumeL <= 0 || !isNum(in.ec) || !isNum(in.aim) || !isNum(in.ecTol)) {
    e.reason = say("prep.unknown");
    return e;
  }
  // The rise each round is expected to bring: the best guess of the true effect, as the test dose
  // uses it (SD-105)
  const double expected = isNum(eff.expected) && eff.expected > 0 ? eff.expected : eff.dose;
  auto runs = in.dayRuns;
  auto usedAt = [&](int channel, Ms t) {
    Ms used = 0;
    for (const auto& [start, ms] : runs[channel])
      if (start > t - kPrepDayMs) used += ms;
    return used;
  };
  Ms t = 0;
  double ec = in.ec;
  int inCheck = 0;
  while (ec < in.aim - in.ecTol) {
    if (e.rounds >= kPrepMaxRounds || t >= kPrepMaxMs) {
      e.more = true;
      break;
    }
    if (inCheck >= std::max(1, in.maxRounds)) {
      t += in.checkMs;
      inCheck = 0;
    }
    // Runs older than a day count no more: dropped, so a long estimate stays quick
    for (auto& [channel, list] : runs)
      list.erase(std::remove_if(list.begin(), list.end(), [&](const std::pair<Ms, Ms>& r) { return r.first <= t - kPrepDayMs; }), list.end());
    const EcDose d = planEcDose(cfg, recipe, in.volumeL, in.aim - ec, eff.dose, eff.largest, in.maxEcStep);
    if (!d.ok) {
      if (d.tooSmall) break;  // the cycle goes on to pH: the rounds end short of the band (SD-122)
      e.reason = d.reason;
      return e;
    }
    // A channel without room in its daily run time waits until runs of the last 24 h fall out of it
    for (const auto& st : d.steps) {
      const auto cap = st.device.empty() ? in.dayCapMs.find(st.channel) : in.dayCapMs.end();
      if (cap == in.dayCapMs.end()) continue;  // a coupled device's channel counts there
      Ms need = 0;
      for (Ms m : st.runs) need += m;
      if (need > cap->second) {
        e.reason = say("prep.daily_short", {{"name", st.name}});
        return e;
      }
      while (usedAt(st.channel, t) + need > cap->second) {
        Ms next = t;
        for (const auto& [start, ms] : runs[st.channel])
          if (start > t - kPrepDayMs) next = std::min(next, start);
        t = next + kPrepDayMs;
        e.daily = true;
        inCheck = 0;  // a wait ends the check: the next one starts afresh
      }
    }
    Ms doseMs = 0;
    for (size_t i = 0; i < d.steps.size(); ++i) {
      if (i > 0) doseMs += in.channelPauseMs;
      const auto& st = d.steps[i];
      for (size_t k = 0; k < st.runs.size(); ++k) {
        if (k > 0) doseMs += in.runPauseMs;
        if (st.device.empty()) runs[st.channel].push_back({t + doseMs, st.runs[k]});
        doseMs += st.runs[k];
      }
    }
    t += doseMs + in.roundWaitMs;
    e.ml += d.totalMl;
    e.limited = e.limited || d.limited;
    ec += expected * d.totalMl / in.volumeL;
    e.rounds++;
    inCheck++;
  }
  if (e.rounds > 0) t += in.tailMs;
  e.ok = true;
  e.totalMs = t;
  e.reachEc = ec;
  return e;
}

}  // namespace doseph
