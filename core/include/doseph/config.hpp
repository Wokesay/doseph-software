// SPDX-License-Identifier: AGPL-3.0-or-later
// Configuration of the mixing robot: the tank, the canisters on the channels,
// the recipes and which one is in force, and the calibrations. The run-time
// state (latches, jump locks, learned effects) is stored apart in
// RuntimeState, so a dose does not make a new configuration revision.
#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "doseph/common.hpp"
#include "doseph/devices.hpp"

namespace doseph {

// 2: bottles became canisters, their kind a role (SD-107)
constexpr int kSchemaVersion = 3;
constexpr int kMaxChannels = 8;
constexpr size_t kMaxPhases = 20;  // phases of a plant recipe or a plant (SD-128)

// A liquid on a dosing channel.
struct CanisterCfg {
  std::string id, name;
  std::string role = "nutrient";  // nutrient | ph_down (SD-108)
  int channel = 0;                // 1..channelCount; 0: on no channel
  std::string pair;               // pair group, e.g. "AB": its parts keep their ratio (RAT-054)
  std::string color = "#4c9a52";
  double capacityMl = kNaN;       // its size; NaN: not entered (SD-119)
  double maxMlPerDay = kNaN;      // the user's daily amount, below the channel's cap (SD-113); NaN: none
  // "" on this device; the coupled device's ID for a canister on its channel (SD-131)
  std::string device = "";
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
  // The user's "at least N min after a dose", 0 to 600 (SD-112): it lengthens the robot's own wait
  // of one turnover and never shortens it; NaN: none
  double mixMinMin = kNaN;
  double holdMin = 60;                // between two checks once on target
  // The cultivation system (SD-128): "hydro", "coco", "rockwool", "soil"; empty: not chosen. It
  // sets what the app offers, never what control does (RAT-076)
  std::string system;
};

// The cultivation systems with the pH range a new phase is offered when its recipe has none
// (SD-128; assumptions, not from a chart)
struct GrowSystem {
  const char* id;
  double phLow, phHigh;
};
inline constexpr GrowSystem kGrowSystems[] = {{"hydro", 5.5, 6.1}, {"coco", 5.6, 6.2}, {"rockwool", 5.5, 6.1}, {"soil", 6.2, 6.8}};

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

// The locks a person releases that may be set to report only (SD-133), by their kind; a channel's
// are named by theirs. Leak, high level and the cable check never: they belong to the frame.
inline const std::vector<std::string>& reportableLocks() {
  static const std::vector<std::string> kinds = {"flow.dry",         "tank.volume",    "ec.no_effect", "ph.no_effect",
                                                 "channel.no_drops", "channel.siphon", "hw.return",    "refill"};
  return kinds;
}
struct LocksCfg {
  std::vector<std::string> reportOnly;  // kinds set to report only; the rest lock
};

// The refill valve (SD-132): its longest fill, 1 to 25 min (RAT-079). A fill that reaches it
// without the float saying full latches the refill (RAT-031).
constexpr double kRefillMinMin = 1, kRefillMaxMin = 25;
struct RefillCfg {
  double maxFillMin = 15;
};

// The circulation pump (SD-132, PD-421): "mixing" runs it with each dosing round and fill until
// the check has measured again after its mixing wait; "always" runs it all the time. Never during
// Stop, maintenance or a hold.
struct CirculationCfg {
  std::string mode = "mixing";
};

// Watering from the tank (SD-134): any number of circuits, each a pump or valve on a Shelly
// output, one running at a time. Limits are the user's; the code keeps only a ceiling the device
// takes (R7).
constexpr size_t kMaxCircuits = 64;
constexpr double kMaxShotS = 24 * 3600;
constexpr size_t kMaxWaterTimes = 96;
struct WaterTime {
  int atMin = 0;    // local minute of the day
  double s = 60;    // the shot's length
};
struct WaterCircuitCfg {
  std::string id;              // w1, w2, …
  std::string name;
  // "window": shots spread evenly over a daily window (RAT-010); "times": fixed times; "interval":
  // every N minutes around the clock; "manual": by hand only
  std::string mode = "manual";
  int fromMin = 8 * 60, toMin = 20 * 60;  // window
  int shotsPerDay = 6;                    // window
  double shotS = 60;                      // window and interval
  std::vector<WaterTime> times;           // times
  int everyMin = 240;                     // interval, from local midnight; at most a day
  double longestS = 300;                  // no shot is longer, by any path; the device's auto-off
                                          // is 1.11 × it (RAT-019)
  double rateLpm = kNaN;                  // what it gives, for the count; NaN: not known
  double dailyMaxS = kNaN;                // its run time per local day; NaN: none
  bool paused = false;                    // its schedule rests; by hand it still waters
  std::string plant;                      // window: a plant whose current phase sets shots a day
                                          // and their length where it has them (RAT-076)
};
struct WateringCfg {
  double reservePct = 20;  // what stays in the tank, of its volume; never below 10 % (RAT-067)
  std::vector<WaterCircuitCfg> circuits;
  const WaterCircuitCfg* circuit(const std::string& id) const;
};
// The output role of a circuit: "water.<id>"
std::string waterRole(const std::string& circuit);

// Light (SD-135): any number of circuits, each a Shelly output switched on and off or a dimmer on
// a 0–10 V line. A circuit's day begins at its own local time and lasts its hours of light; a
// dimmer rises over its sunrise and falls over its sunset within the day.
constexpr size_t kMaxLights = 16;
constexpr int kMaxRampMin = 240;
struct LightCircuitCfg {
  std::string id;              // l1, l2, …
  std::string name;
  bool dimmed = false;         // a dimmer (Shelly "Light"); otherwise a switch, on or off
  int startMin = 6 * 60;       // the day's start, local minute of the day
  double hours = 18;           // hours of light, 0 to 24
  double pct = 100;            // a dimmer's brightness by day, 1 to 100 %
  double thresholdPct = 0;     // the lamp driver's switch-on threshold: less is raised to it (RAT-013)
  int sunriseMin = 15, sunsetMin = 15;  // a dimmer's ramps within the day, 15 min unless set (RAT-035); 0: hard on and off
  std::string plant;           // a plant whose current phase sets the hours and the brightness
                               // where it has them (RAT-076)
};
struct LightingCfg {
  std::vector<LightCircuitCfg> circuits;
  const LightCircuitCfg* circuit(const std::string& id) const;
};
// The output role of a light circuit: "light.<id>"
std::string lightRole(const std::string& circuit);

// Climate (SD-136): rooms, each with an exhaust fan, a circulation fan, a heater with its own
// thermostat, a humidifier, a dehumidifier and a CO2 valve, each on a Shelly output, and sensors for
// air temperature, humidity and CO2. A room's day is its first light circuit's.
constexpr size_t kMaxRooms = 8;
constexpr double kCo2MaxTargetPpm = 1500;  // no CO2 target above it
constexpr double kCo2CutPpm = 2000;        // above it the valve closes, with an alarm
struct ClimateRoomCfg {
  std::string id;                   // r1, r2, …
  std::string name;
  std::vector<std::string> lights;  // its light circuits; the first one's day is the room's
  int startMin = 6 * 60;            // without a light circuit: its day's start, local minute of the day
  double hours = 18;                // … and its hours
  double dayC = 26, nightC = 22;    // temperature targets by day and by night (RAT-009)
  bool byVpd = false;               // humidity led by a VPD target: the humidity target follows from it (RAT-009)
  double rhPct = 70;                // humidity target, %
  double vpdKpa = 1.0;              // VPD target, kPa, where it leads
  double co2Ppm = kNaN;             // CO2 target by day, ppm; NaN: no CO2
  double heatC = 30;                // heat protection for light: its first limit; the second is 2 °C higher
  double heaterW = kNaN;            // the heater's power, W: its plug's power limit is a fifth above it (RAT-069); NaN: not set
  bool circulateAlways = false;     // the circulation fan runs at night too
  std::string plant;                // a plant whose current phase sets the targets where it has them (RAT-076)
};
struct ClimateCfg {
  std::vector<ClimateRoomCfg> rooms;
  const ClimateRoomCfg* room(const std::string& id) const;
};
// A room's role of a kind: "climate.<room>.<kind>"; the outputs exhaust, circulation, heater,
// humidifier, dehumidifier, co2_valve, and the sensors temp, rh, co2
std::string climateRole(const std::string& room, const std::string& kind);
const std::vector<std::string>& climateOutputs();
const std::vector<std::string>& climateSensors();

// Fixed safety limits in code. The configuration may only tighten them (R7);
// gateway and planning always use `bounded()`.
constexpr double kHardMinRunS = 1.0;    // shorter runs are not dosed (RAT-050)
constexpr double kHardMaxRunS = 60.0;   // longer doses in partial runs (RAT-055)
constexpr int kHardMaxPartialRuns = 6;  // partial runs per canister and round
constexpr double kDailyMaxMin = 60;     // a channel's run time into the tank per rolling 24 h, never more (SD-113)
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

// Notifications (SD-123): quiet hours in local minutes of the day; from == to: none. The
// morning report comes when they end.
struct NotifyCfg {
  int quietFromMin = 22 * 60;
  int quietToMin = 7 * 60;
};

// Maintenance dates (SD-125): an interval in days per kind of item that is set; the rest keep theirs
struct ServiceCfg {
  std::map<std::string, int> days;
};

// A phase of a plant (SD-128, RAT-076): a free name, a label only; its days; whether it moves on by
// itself at the local midnight after them or only once the user confirms (RAT-002); and the water
// it asks for: a recipe, with an EC target and a pH range of its own where they differ (missing:
// the recipe's)
struct PhaseCfg {
  std::string name;
  int days = 14;
  bool confirm = false;
  std::string recipe;
  double ecTarget = kNaN;
  double phLow = kNaN, phHigh = kNaN;
  // Watering (SD-134): shots a day and the length of a shot for a circuit that follows the plant;
  // nothing and NaN: not set, the circuit's own apply
  std::optional<int> shotsPerDay;
  double shotS = kNaN;
  // Light (SD-135): hours of light and a dimmer's brightness for a light circuit that follows the
  // plant; nothing: the circuit's own apply
  std::optional<double> lightHours, lightPct;
  // Climate (SD-136): the targets for a room that follows the plant; a VPD leads the humidity in
  // place of a humidity; nothing: the room's own apply
  std::optional<double> dayC, nightC, rhPct, vpdKpa, co2Ppm;
};

// A plant recipe: the phases a plant goes through (SD-128)
struct PlantRecipeCfg {
  std::string id, name, note;
  std::vector<PhaseCfg> phases;
};

// A plant on the tank (SD-128): started from a plant recipe, whose phases it copied, so a later
// change of the recipe does not move it. Its progress belongs to it, and a backup carries it.
struct PlantCfg {
  std::string id, name;
  std::string from;        // the plant recipe it was started from (it may be gone)
  std::vector<PhaseCfg> phases;
  Epoch start = 0;         // the local midnight of its first day: day 1 is the day it started
  int phase = 0;           // its current phase
  Epoch phaseStart = 0;    // the local midnight its current phase began
  Epoch harvestAt = 0;     // recorded by the user, never by the robot (RAT-002); 0: not yet
  Epoch completedAt = 0;   // completed, a step of its own (RAT-077); 0: running
  bool running() const { return completedAt == 0; }
};

// The commissioning steps of #10 in their order (SD-129). "device": done while the robot sees it;
// "confirm": done once the user confirms it; "later": not there yet. An optional step may be
// skipped.
struct SetupStep {
  const char* id;
  const char* how;
  bool optional;
};
inline constexpr SetupStep kSetupSteps[] = {
    {"password", "device", false}, {"network", "later", false}, {"placement", "confirm", false}, {"flow", "device", false},
    {"probes", "device", false},   {"channels", "device", false}, {"lines", "device", false},    {"test", "confirm", false},
    {"tank", "device", false},     {"push", "device", true},      {"account", "later", false}};
const SetupStep* setupStep(const std::string& id);
struct Config;
// The owner confirmed where pump and canisters stand (SD-118): until then no line is filled into
// the tank and nothing doses
bool placementConfirmed(const Config& cfg);
// What the user said of a step: "done" for a step the user confirms, "skipped" for an optional
// one. The steps the robot sees are not kept: they show what is so now.
struct SetupMark {
  std::string state;
  Epoch at = 0;
};

struct Config {
  int schemaVersion = kSchemaVersion;
  int revision = 0;
  SystemCfg system;
  NotifyCfg notify;
  ServiceCfg service;
  TankCfg tank;
  ControlCfg control;
  LocksCfg locks;
  RefillCfg refill;
  CirculationCfg circulation;
  WateringCfg watering;
  LightingCfg lighting;
  ClimateCfg climate;
  Limits limits;
  std::vector<CanisterCfg> canisters;
  std::vector<RecipeCfg> recipes;
  ActiveCfg active;
  std::vector<PlantRecipeCfg> plantRecipes;
  std::vector<PlantCfg> plants;
  // The plant that leads the water plan (SD-128); empty or not running: the earliest started
  // running plant
  std::string lead;
  std::map<int, ChannelCal> channels;
  // The commissioning steps the user confirmed or skipped (SD-129)
  std::map<std::string, SetupMark> setup;
  // Network devices and the roles bound to them (SD-132)
  std::vector<NetDeviceCfg> devices;
  std::map<std::string, RoleBinding> roles;
  // Probe calibrations: pH {points: [[raw, ref], [raw, ref]], zeroMv, at},
  // EC {factor, at}; null when not calibrated (RAT-025)
  json phCal = nullptr, ecCal = nullptr;

  const CanisterCfg* canister(const std::string& id) const;
  const CanisterCfg* canisterOnChannel(int channel) const;
  const RecipeCfg* recipe(const std::string& id) const;
  const PlantRecipeCfg* plantRecipe(const std::string& id) const;
  const PlantCfg* plant(const std::string& id) const;
  const NetDeviceCfg* device(const std::string& id) const;
  // The pH− canister on a channel; there is one at most (SD-108)
  const CanisterCfg* phDown() const;
  double channelFlow(int channel) const;  // NaN when not calibrated
  // A canister's calibrated flow on whichever device it is: on the coupled one as it said last,
  // while its link is up (SD-131); NaN when unknown
  double canisterFlow(const CanisterCfg& b) const;
  // Whether a lock of this kind is set to report only (SD-133)
  bool reportsOnly(const std::string& kind) const;
  // Not stored: the coupled device and its channels' flows as it said last, while its link is up
  std::string peerId;
  std::map<int, double> peerFlows;
};

void to_json(json& j, const Config& c);
// A list of phases in its JSON form ({phases: [{name, days, confirm, recipe, ecTarget, ph, shotsPerDay,
// shotS}]}); reading throws on a structure error
json phasesJson(const std::vector<PhaseCfg>& phases);
std::vector<PhaseCfg> phasesFromJson(const json& j);
// Brings a configuration of an older schema to the current one, one step
// at a time; a newer one than this software knows, or one without a whole
// version number, throws. Pure: the stored file changes only when it is
// saved again.
json migrateConfig(json j);
Config configFromJson(const json& j);  // migrates first; throws on structure errors
WaterCircuitCfg waterCircuitFromJson(const json& j);  // throws on structure errors
LightCircuitCfg lightCircuitFromJson(const json& j);  // throws on structure errors
ClimateRoomCfg climateRoomFromJson(const json& j);    // throws on structure errors

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
  std::map<std::string, json> latches;     // latched locks with their reason; reportOnly: holds nothing (SD-133)
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
  // Channel → its runs into the tank of the last 24 h, [epoch, ms], for the daily cap (SD-113)
  std::map<int, std::vector<std::pair<Epoch, Ms>>> dayRuns;
  Ms dayRunMs(int channel, Epoch since) const;
  // Water missing (SD-115): {recipe, target} of the last check that found EC within its band, while
  // no dose went in since; null otherwise. `topUp`: "top up water" is raised.
  json ecInBand = nullptr;
  bool topUp = false;
  // A fill of the refill valve under way: {since, why}; kept so that a restart reports it, and
  // never resumes it (SD-132, R6)
  json refill = nullptr;
  // Watering (SD-134): {givenL: litres given since the tank was last full, shot: {circuit, since,
  // s, override} while one runs, day: {date, runS: {circuit: s}}, last: {circuit: {at, s, l, end}}}.
  // `waterLow`: a shot would go below the reserve, so water is to be topped up.
  json watering = json::object();
  bool waterLow = false;
  // Light switched by hand (SD-135): {circuit: {on, until}}, until the plan's next change (epoch)
  json lighting = json::object();
  // The settling times of the last three clean doses, min (SD-112): a large tank waits 1.5 × the longest
  std::vector<double> settleMin;
  // Alarms (SD-123): watchdog item → {since, raised, level, sent, round, ackUntil, reported};
  // the time of the last morning report
  std::map<std::string, json> alarms;
  Epoch lastReport = 0;
  // Maintenance items (SD-125): when each was first seen and when it was last done by hand
  std::map<std::string, Epoch> serviceSince, serviceDone;
  bool stopped = false;                    // a Stop survives a restart (RAT-036)
  bool newTank = true;                     // the next dose is a test dose that checks the volume (SD-105)
  double flowLpm = kNaN;                   // the flow last measured while proven, for the turnover (SD-105)
  Epoch flowAt = 0;
  // The feed pump's current while it moves water, learned while flow is proven, and for how
  // long; a dry run draws far less (SD-127, RAT-049). Missing until learned (R5).
  double pumpNormalA = kNaN;
  double pumpLearnedS = 0;
  // The channels a run went through to its end, which filled the line (SD-129): a commissioning
  // step the robot sees
  std::set<int> filled;
  std::optional<Stamp> clock;              // time and operating time when saved
  std::uint32_t bootCount = 0;
};
void to_json(json& j, const RuntimeState& s);
RuntimeState runtimeFromJson(const json& j);
// Whether a latch stands that holds: one set while its lock reported only holds nothing (SD-133).
// A latch of a kind that may not report only always holds, whatever its entry says.
bool holds(const RuntimeState& rt, const std::string& id);
// Whether a latch of this kind stands that a new one must not replace: one that holds, or a report
// while its kind still reports only. Once the kind locks again, the next one holds (SD-133).
bool latched(const Config& cfg, const RuntimeState& rt, const std::string& id, const std::string& kind);
// Whether a report of this latch already stands: its event is not logged again (SD-133)
bool reportStands(const RuntimeState& rt, const std::string& id);
// Sets a latch of a kind (its ID where empty). Where that kind is set to report only, the latch
// is marked so: it is shown and alarms as any other, and holds nothing (SD-133). Returns whether
// it holds.
bool setLatch(const Config& cfg, RuntimeState& rt, const std::string& id, json entry, const std::string& kind = "");
// What the event log and the watchdog say of a latch that reported only: the lock's kind, and its
// channel where it has one (SD-133)
Msg reportedEvent(const std::string& kind, const json& latch);
// A canister's counted level: "unknown", "ok", "low" (below its minimum,
// RAT-071) or "empty" (at or below zero by the count) (SD-119)
std::string stockStatus(const CanisterCfg& b, const RuntimeState& rt);

}  // namespace doseph
