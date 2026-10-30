// SPDX-License-Identifier: AGPL-3.0-or-later
// Checks of a pump's calibrated rate in use (SD-145). A pump delivers less as its tube wears; two
// checks say when its rate no longer fits, and neither changes it on its own:
//   the consumption check  a canister level read and entered against what the doses since the
//                          last reading counted; off beyond kUseShare, a rate the person may take
//   the drops per ml       learned from the first doses after a calibration, then watched; changed
//                          beyond kDriftShare, the pump's calibration falls due
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "doseph/config.hpp"

namespace doseph {

// Assumptions until measured with users (SD-145)
constexpr double kUseMinMl = 250.0;      // counted at least this much: a reading's error stays well below the share
constexpr double kUseShare = 0.05;       // used and counted differ by more: a rate is proposed
constexpr double kDriftLearnMl = 10.0;   // the drops per ml are learned from this much of the doses
constexpr double kDriftWindowMl = 10.0;  // then compared over windows of this much
constexpr double kDriftShare = 0.20;     // changed by more: the pump's calibration falls due

// A level read and entered for a canister: compared with the last reading where one stands for the
// same channel and calibration, and kept as the start of the next comparison. Returns the comparison,
// {countedMl, usedMl, share, proposal, calibrate}, or null where there is none. A proposal is kept
// in the state until taken or dismissed; beyond the calibration's window (kCalWindow) there is
// none, and `calibrate` says to calibrate the pump instead.
json checkUse(const Config& cfg, RuntimeState& rt, const CanisterCfg& b, double read, Epoch now);
// A level read that starts a comparison only: a new canister's first level
void startUse(const Config& cfg, RuntimeState& rt, const CanisterCfg& b, double read, Epoch now);
// Proposals, readings and drops per ml that no longer belong to their channel's calibration or
// canister are dropped; whether any was
bool pruneRateChecks(const Config& cfg, RuntimeState& rt);
// One run of a dose into the tank that began with a full line and ran to its end, on a channel
// calibrated at `calAt`. Returns the share by which its drops per ml changed, once beyond
// kDriftShare: the first time after its calibration.
std::optional<double> watchDrops(RuntimeState& rt, int channel, Epoch calAt, double ml, std::uint64_t drops, Epoch now);

}  // namespace doseph
