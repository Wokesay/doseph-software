// SPDX-License-Identifier: AGPL-3.0-or-later
// How long a preparation takes (SD-150). A preparation is the EC rounds that bring a tank from
// where it stands to its target; in a tank of several cubic metres it can take more than a day.
// The estimate runs the cycle's own round plan forward at the effect the robot expects, under the
// caps it doses with: the partial-run limit per round (RAT-055), the rounds per check and the hold
// after them, the daily run time per channel (SD-113), the pauses of the trough (SD-147) and the
// mixing wait after each round (SD-112). It guides, it promises nothing: mixing, uptake and an
// effect that changes move it, and each round estimates anew from where EC stands.
#pragma once

#include <map>
#include <utility>
#include <vector>

#include "doseph/plan.hpp"

namespace doseph {

// Beyond either, the estimate stops and says "more than" (it says enough by then)
constexpr int kPrepMaxRounds = 1000;
constexpr Ms kPrepMaxMs = 14 * 24 * 60 * kMinute;
constexpr Ms kPrepDayMs = 24 * 60 * kMinute;
// A fresh fill is estimated from water without salts (osmosis water): the longest case, since tap
// water brings some EC along (SD-150)
constexpr double kFreshEc = 0.0;
// Mixed by hand, the robot only holds: it corrects an EC at most this far below its target; a
// wider gap is the person's to mix (SD-150, an assumption)
constexpr double kHoldGapEc = 0.3;

struct PrepInput {
  double volumeL = kNaN;
  double ec = kNaN, aim = kNaN, ecTol = kNaN;  // where EC stands, what the rounds aim at, the band
  double maxEcStep = 1.0;
  int maxRounds = 10;            // rounds per check; then the robot holds until the next check
  Ms roundWaitMs = 0;            // after each round: the mixing wait and the stable window
  Ms checkMs = 0;                // after the rounds of a check: the hold and the start of the next
  Ms runPauseMs = 0, channelPauseMs = 0;
  Ms tailMs = 0;                 // after the EC rounds: the pH round that follows, where one can
  std::map<int, Ms> dayCapMs;    // channel → its run time per 24 h; a channel not listed: none
  // channel → its runs of the last 24 h: [start, ms], the start relative to now (≤ 0)
  std::map<int, std::vector<std::pair<Ms, Ms>>> dayRuns;
};

struct PrepEstimate {
  bool ok = false;
  Msg reason;              // why there is none
  int rounds = 0;
  double ml = 0;           // concentrate of the scaled parts, in all
  Ms totalMs = 0;          // until EC is within its band, and the pH round after it
  bool more = false;       // longer than the estimate follows: at least totalMs
  bool limited = false;    // rounds made smaller by the partial-run limit: the pumps are slow for the tank
  bool daily = false;      // the daily run time per channel stretched it
  double reachEc = kNaN;   // where the rounds end; below the band where a round got too small to run
};

// The rounds from `in.ec` to the band around `in.aim` with the recipe at its effect `eff`
PrepEstimate estimatePrep(const Config& cfg, const RecipeCfg& recipe, const RecipeEffect& eff, const PrepInput& in);

}  // namespace doseph
