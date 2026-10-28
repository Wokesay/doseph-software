// SPDX-License-Identifier: AGPL-3.0-or-later
// Read model of the readings (rule R2): the watchdog and the UI see only
// these types, no hardware and no actuators.
#pragma once

#include <optional>
#include <string>

#include "doseph/common.hpp"

namespace doseph {

enum class Quality {
  Offline,       // the measuring part does not answer
  NoData,        // no reading yet
  Stale,         // reading too old
  Frozen,        // the raw value stands still though it should be noisy (RAT-023, RAT-059)
  Implausible,   // outside the plausible band
  Jump,          // jump lock (RAT-039, RAT-044)
  Uncalibrated,  // calibration or compensation missing: no value for control (RAT-025, RAT-089)
  Ok
};
const char* qualityName(Quality q);

struct Reading {
  std::string signal, unit;
  int decimals = 2;
  std::optional<double> value;  // the value shown (Uncalibrated: the raw value, or EC not compensated)
  Quality quality = Quality::NoData;
  Msg reason;
  Ms ts = 0;
  Ms ageMs = -1;
  bool usable() const { return quality == Quality::Ok && value.has_value(); }
};

}  // namespace doseph
