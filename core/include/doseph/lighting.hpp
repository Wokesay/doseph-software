// SPDX-License-Identifier: AGPL-3.0-or-later
// Light (SD-135): any number of circuits, each a Shelly output switched on and off or a dimmer on a
// 0–10 V line. A circuit's day begins at its own local time and lasts its hours of light; a dimmer
// rises over its sunrise and falls over its sunset within the day, never below the lamp driver's
// switch-on threshold (RAT-013), and 0 % switches it off (RAT-029). A circuit that follows a plant
// takes the hours and the brightness from the plant's current phase where it has them (RAT-076).
// Light follows its plan through a restart, a Stop, a hold and maintenance; a person may switch it
// on or off until the plan's next change. Only the gateway switches (R1).
#pragma once

#include <optional>
#include <string>

#include "doseph/common.hpp"

namespace doseph {

struct Ctx;
class NetOutputs;
struct Config;
struct PhaseCfg;
struct LightCircuitCfg;
struct RuntimeState;

// What a circuit's plan wants at a local second of the day
struct LightPoint {
  bool day = false;   // within its hours of light
  bool on = false;    // switched on
  double pct = 0;     // a dimmer's brightness, %; 0 while off
  int changeInS = 0;  // until the day next begins or ends, s; a day of 0 or 24 h never changes
};
LightPoint lightAt(const LightCircuitCfg& l, int secondOfDay);

// The phase of the plant a light follows, while the plant runs; nothing otherwise
const PhaseCfg* followedLightPhase(const Config& cfg, const LightCircuitCfg& l);
// The circuit as its plan lights it: the hours of light and the brightness from the plant's phase
// where it has them
LightCircuitCfg effectiveLight(const Config& cfg, const LightCircuitCfg& l);

class Lighting {
 public:
  // Every light as its plan, or a person, wants it now
  void tick(Ctx& c, int secondOfDay, NetOutputs& out);
  // By hand: on or off until the plan's next change; nothing: back to the plan. False for no such
  // circuit.
  static bool hand(Ctx& c, const std::string& circuit, std::optional<bool> on, int secondOfDay);
  // A circuit removed: what was switched by hand goes with it
  static void forget(RuntimeState& rt, const std::string& circuit);
  // `outputs`: the gateway's status of the output roles
  static json status(const Ctx& c, int secondOfDay, const json& outputs);
};

}  // namespace doseph
