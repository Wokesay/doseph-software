// SPDX-License-Identifier: AGPL-3.0-or-later
// Configuration of the mixing robot: the tank, the canisters on the channels,
// the recipes and which one is in force, and the calibrations. The run-time
// state (latches, jump locks, learned effects) is stored apart in
// RuntimeState, so a dose does not make a new configuration revision.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

// 2: bottles became canisters, their kind a role (SD-107)
constexpr int kSchemaVersion = 3;
constexpr int kMaxChannels = 8;

// A liquid on a dosing channel.
struct CanisterCfg {
  std::string id, name;
  std::string role = "nutrient";  // nutrient | ph_down (SD-108)
  int channel = 0;                // 1..channelCount; 0: on no channel
  std::string pair;               // pair group, e.g. "AB": its parts keep their ratio (RAT-054)
  std::string color = "#4c9a52";
  double capacityMl = kNaN;       // its size; NaN: not entered (SD-119)
};

// The level of a canister, counted from use (SD-119): below these it is low
// (RAT-071); below zero by the count it ran dry, or the level entered or the
// pump's calibration is wrong. A rounding difference alone is not below zero.
constexpr double kStockLowNutrientMl = 150, kStockLowPhDownMl = 20;
constexpr double kStockBelowZeroMl = -0.5;
constexpr double kCapacityMinMl = 10, kCapacityMaxMl = 25000;

struct RecipeStep {
  std::string canister;
  double mlPerL = kNaN;
  // A fixed addition (a cleanser, for example): per litre of the tank, never
  // scaled to the EC (RAT-085); never part of a pair
  bool fixed = false;
};

struct RecipeCfg {
  std::string id, name, note;
  std::vector<RecipeStep> steps;  // order = dosing order (RAT-080)
  // The EC the robot mixes to, as the chart gives it at these amounts; it is
  // also the reference for the recipe's effect (RAT-056, RAT-066). Missing
  // stays NaN (R5): then the robot doses no nutrients.
  double ecTarget = kNaN;
  // The pH range; the robot aims at its middle (RAT-066). Missing: no pH dose.
  double phLow = kNaN, phHigh = kNaN;
};

// Which recipe is in force and how long (SD-105).
struct ActiveCfg {
  std::string recipe;  // empty: none
  Epoch from = 0;
  Epoch until = 0;     // 0: no end
};

// The tank as the user describes it, and the timing of the cycle (SD-105).
struct TankCfg {
  double volumeL = kNaN;              // as entered; the test dose checks it (SD-105)
  // The user's mixing wait after a dose; NaN: the suggestion from volume and
  // flow (about 2 turnovers for EC, 3 for pH, SD-105)
  double waitEcMin = kNaN, waitPhMin = kNaN;
  double holdMin = 60;                // between two checks once on target
};

// Control settings, within the bounds in code (R7, RAT-008).
struct ControlCfg {
  double ecTolerance = 0.1;
  double phTolerance = 0.15;
  double maxEcStep = 1.0;       // largest EC rise per round (RAT-056)
  double ecFloor = 0.5;         // no pH dose below this EC (RAT-046)
  double phMaxMlPerDose = 20;   // largest pH− dose (RAT-050)
  int maxEcRounds = 10;         // per check; then the robot holds and says so
  int maxPhDoses = 8;
};

// Fixed safety limits in code. The configuration may only tighten them (R7);
// gateway and planning always use `bounded()`.
constexpr double kHardMinRunS = 1.0;    // shorter runs are not dosed (RAT-050)
constexpr double kHardMaxRunS = 60.0;   // longer doses in partial runs (RAT-055)
constexpr int kHardMaxPartialRuns = 6;  // partial runs per canister and round
// A recipe's start effect, mS/cm per ml/L of the recipe (RAT-055). A recipe's
// EC may imply an effect from a quarter to four times of it (RAT-056).
constexpr double kEcStartEffect = 0.275;
constexpr double kRefEffectMin = 0.25, kRefEffectMax = 4.0;  // factors of kEcStartEffect
// Bounds of the mixing wait and the hold (R7): the wait never below 2 min
// (RAT-052), never above 10 h; the hold 10 min to 24 h (SD-105)
constexpr double kWaitMinMin = 2, kWaitMaxMin = 600;
constexpr double kHoldMinMin = 10, kHoldMaxMin = 1440;

struct Limits {
  double minRunS = kHardMinRunS;
  double maxRunS = kHardMaxRunS;
  int maxPartialRuns = kHardMaxPartialRuns;
  // Values clamped to the fixed limits; missing or senseless ones → the fixed limit.
  Limits bounded() const;
};

struct ChannelCal {
  double flowMlPerMin = kNaN;  // measured into a cup (RAT-070); missing: the channel doses nothing (RAT-015)
  Epoch at = 0;
  // The pump head it was measured with (SD-108); another head, or none recorded, makes it void
  std::string head{};
};

struct SystemCfg {
  std::string name = "Doseph";
  std::string language = "en";  // English first
  std::string timezone = "Europe/Berlin";
  bool passwordSet = false;  // the first password was set once: no setup over the network after that
};

struct Config {
  int schemaVersion = kSchemaVersion;
  int revision = 0;
  SystemCfg system;
  TankCfg tank;
  ControlCfg control;
  Limits limits;
  std::vector<CanisterCfg> canisters;
  std::vector<RecipeCfg> recipes;
  ActiveCfg active;
  std::map<int, ChannelCal> channels;
  // Probe calibrations: pH {points: [[raw, ref], [raw, ref]], zeroMv, at},
  // EC {factor, at}; null when not calibrated (RAT-025)
  json phCal = nullptr, ecCal = nullptr;

  const CanisterCfg* canister(const std::string& id) const;
  const CanisterCfg* canisterOnChannel(int channel) const;
  const RecipeCfg* recipe(const std::string& id) const;
  // The pH− canister on a channel; there is one at most (SD-108)
  const CanisterCfg* phDown() const;
  double channelFlow(int channel) const;  // NaN when not calibrated
};

void to_json(json& j, const Config& c);
// Brings a configuration of an older schema to the current one, one step
// at a time; a newer one than this software knows, or one without a whole
// version number, throws. Pure: the stored file changes only when it is
// saved again.
json migrateConfig(json j);
Config configFromJson(const json& j);  // migrates first; throws on structure errors

// Checks references and the rules of the domain and of safety; channelCount
// is how many channels the device has.
std::vector<Msg> validateConfig(const Config& c, int channelCount);
// A name in the form of the time zone database ("Europe/Berlin", "UTC"):
// letters, digits, "_", "+" and "-" in parts separated by "/", at most 64
// characters; never "." (the firmware may look it up as a file one day).
bool isTimeZoneName(std::string_view tz);

// The recipe's pH target, the middle of its range; NaN without one (RAT-066)
double phTarget(const RecipeCfg& r);

// Run-time state, stored apart. Survives restarts (latches and jump locks
// stay, RAT-044, RAT-063).
struct RuntimeState {
  std::map<std::string, json> latches;     // latched locks with their reason
  std::map<std::string, json> jumpLocks;   // signal → {until, from, to, at} (RAT-039, RAT-044)
  double phEffect = kNaN;                  // learned effect of pH− (pH per ml/L), only from clean doses
  std::map<std::string, double> ecEffects; // learned EC effect per recipe, mS/cm per ml/L of its scaled parts (RAT-057)
  // Canister → its level in ml, counted from use; missing: unknown, never
  // assumed full (RAT-015); may fall below zero (SD-119)
  std::map<std::string, double> stockMl;
  // The pair parts of the nutrient round under way: {recipe, order, canister,
  // catchUp, parts: {canister: {planned, done}}}; null without one (SD-114)
  json pairRound = nullptr;
  // What a pair still misses after a round that stopped part way:
  // {recipe, parts: {canister: ml}}; null when nothing is missing (SD-114)
  json catchUp = nullptr;
  // Channel → how long its line was pulled back after its last dose, ms: its next run fills it
  // first, unbooked (SD-111)
  std::map<int, Ms> pulledBackMs;
  bool stopped = false;                    // a Stop survives a restart (RAT-036)
  bool newTank = true;                     // the next dose is a test dose that checks the volume (SD-105)
  double flowLpm = kNaN;                   // the flow last measured while proven, for the turnover (SD-105)
  Epoch flowAt = 0;
  std::optional<Stamp> clock;              // time and operating time when saved
  std::uint32_t bootCount = 0;
};
void to_json(json& j, const RuntimeState& s);
RuntimeState runtimeFromJson(const json& j);
// A canister's counted level: "unknown", "ok", "low" (below its minimum,
// RAT-071) or "empty" (at or below zero by the count) (SD-119)
std::string stockStatus(const CanisterCfg& b, const RuntimeState& rt);

}  // namespace doseph
