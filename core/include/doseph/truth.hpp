// SPDX-License-Identifier: AGPL-3.0-or-later
// Sensor truth (layer 4): raw value → calibrated value → validity. Runs
// every tick, also while the robot holds or is stopped, because dosing
// locks depend on it (RAT-075, RAT-023). Control reads only this state
// (RAT-025). A missing value is never 0 (RAT-006).
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <string>

#include "doseph/config.hpp"
#include "doseph/hardware.hpp"
#include "doseph/readmodel.hpp"

namespace doseph {

// A jump lock lifts after 15 min of calm (RAT-044).
inline constexpr Epoch kJumpHoldS = 15 * 60;
// EC is compensated to 25 °C, linear (RAT-089).
inline constexpr double kEcTempCoeff = 0.02;

// What the core knows about each signal: unit, plausible band, how old a
// value may be, the jump threshold and whether its raw value must move.
struct SignalDef {
  Signal signal;
  const char* unit;
  int decimals;
  double plausMin, plausMax;
  Ms maxAgeMs;
  double jump;        // NaN: no jump check
  bool expectsNoise;  // standstill check of the raw value (RAT-059)
};
const SignalDef& signalDef(Signal c);

class SensorTruth {
 public:
  // Once per tick: reads every signal.
  void update(const Config& cfg, const IHardware& hw, RuntimeState& rt, Ms now, Epoch epoch);
  // An intervention of the robot or of a person explains a jump until then (RAT-042).
  void expectChange(Signal c, Ms until);

  const Reading& get(Signal c) const;
  const std::map<Signal, Reading>& all() const { return readings_; }
  // The value of a signal's calibration, without validity: pH from its two
  // points, EC × factor, compensated with `tempC` (RAT-089). nullopt when
  // the calibration is missing or invalid.
  static std::optional<double> calibrate(Signal c, double raw, const Config& cfg, double tempC);
  // Checks a calibration before it is stored: the reason, or nothing.
  static std::optional<Msg> checkPh(const json& data);
  static std::optional<Msg> checkEc(const json& data);
  // EC at the water's temperature to 25 °C (RAT-089)
  static double compensate(double ec, double tempC) { return ec / (1.0 + kEcTempCoeff * (tempC - 25.0)); }

 private:
  struct Track {
    double lastRaw = kNaN;
    Ms lastRawChange = 0;
    Ms lastTs = 0;
    std::deque<std::pair<Ms, double>> window;  // for the jump check
    Ms explainedUntil = 0;
  };
  std::map<Signal, Reading> readings_;
  std::map<Signal, Track> tracks_;
};

}  // namespace doseph
