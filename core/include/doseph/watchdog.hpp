// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 9: watchdog. It only assesses (OK / problem / neutral) and switches
// nothing (RAT-074). This header includes only the read model and the
// configuration: no hardware, no gateway, no cycle. tools/arch_check.sh
// checks this in CI (rule R2).
#pragma once

#include <string>
#include <vector>

#include "doseph/config.hpp"
#include "doseph/readmodel.hpp"

namespace doseph {

struct WatchInput {
  const Config& cfg;
  const RuntimeState& rt;
  std::vector<Reading> readings;  // ph, ec, temp, flow
  bool needsFlow = false;         // the cycle needs flow now: none is a problem
  double lastPh = kNaN, lastEc = kNaN;  // the last stable measurement of the cycle
  Epoch lastAt = 0;
  Epoch epoch = 0;
  Epoch bootEpoch = 0;
  bool stopped = false;
  int pumpCount = 0;
};

struct Assessment {
  std::string id;
  Msg label;
  std::string status;  // ok | problem | neutral
  Msg text;
};

struct WatchResult {
  Epoch evaluatedAt = 0;
  std::string overall;  // ok | problem | neutral
  int ok = 0, problems = 0, neutral = 0;
  Msg headline;
  std::vector<Assessment> items;
};
void to_json(json& j, const WatchResult& w);

WatchResult evaluate(const WatchInput& in);

// An assessment older than 3 min counts as missing and is red (RAT-073).
constexpr Epoch kWatchStaleS = 180;
// For 120 s after a start target checks are neutral (RAT-005).
constexpr Epoch kStartupGraceS = 120;
// The alarm band is wider than the control band (RAT-033).
constexpr double kAlarmMargin = 0.2;

}  // namespace doseph
