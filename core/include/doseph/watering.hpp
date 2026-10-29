// SPDX-License-Identifier: AGPL-3.0-or-later
// Watering from the tank (SD-134): any number of circuits, each a pump or a valve on a Shelly
// output, one running at a time. A circuit waters by its schedule (shots in a daily window, fixed
// times, an interval) or by hand; only with water fit for it (RAT-086), once the last dose or fill
// is mixed in, and while the tank keeps its reserve (RAT-067, RAT-068). A person may water anyway
// when the water is not fit (an override); never past an empty tank. Only the gateway switches
// (R1); the device's own auto-off ends a shot a hung robot would not (RAT-019).
#pragma once

#include <map>
#include <optional>
#include <string>

#include "doseph/common.hpp"

namespace doseph {

struct Ctx;
class NetOutputs;
struct WaterCircuitCfg;

constexpr Ms kWaterOpenMs = kMinute;      // a circuit that did not say "on" by then ends its shot
constexpr Epoch kWaterWaitS = 3600;       // a due shot waits this long at most, then it lapses
constexpr long long kWaterLeapMin = 90;   // the clock leapt further, either way: the schedules start
                                          // from the new time, nothing is made up (R6)
constexpr double kWaterLowBackPct = 10;   // water is no longer low this far above the reserve, in % of the tank

struct RuntimeState;

// What the robot knows around watering now
struct WaterIn {
  std::string date;         // local, YYYY-MM-DD: the day of the daily run time
  int minuteOfDay = 0;      // local
  bool clockSecured = true; // the time is set; when it becomes so, or is lost, the clock leaps
  bool mixing = false;      // a dose, a fill or a mixing wait is under way, or the check has not
                            // measured since: a shot waits (SD-112)
  bool dosing = false;      // a dose runs: a running shot stops (RAT-086)
  double ph = kNaN, ec = kNaN;  // the check's last measurement; NaN: none (R5)
  bool stale = true;        // that measurement is too old to judge the water by
  double ecTarget = kNaN;   // the EC target: the higher of this phase's and the last phase's
  double phTarget = kNaN, phTol = kNaN;  // pH not corrected only warns (RAT-084)
  bool calibrating = false; // a probe is in a buffer
  bool flowDry = false;     // the feed pump found no flow and its latch holds (SD-109)
  bool emptyAssigned = false;
  std::optional<bool> empty;      // the float "tank empty": none without a value (R5)
  bool levelAssigned = false;
  std::optional<double> levelL;   // the level probe, litres
  double volumeL = kNaN;          // the tank's volume as entered
  std::map<std::string, json> outputs;  // circuit → its output at the gateway: {state, switchedOn}
};

class Watering {
 public:
  // Why the water is not fit for watering now (RAT-086); nothing where it is
  static std::optional<Msg> unfit(const WaterIn& in);
  // Why a shot of `s` on a circuit cannot start now. An override passes water that is not fit.
  std::optional<Msg> blocked(const Ctx& c, const WaterIn& in, const WaterCircuitCfg& w, double s, bool override) const;
  // A person's "water now"
  std::optional<Msg> waterNow(Ctx& c, const WaterIn& in, const std::string& circuit, double s, bool override, NetOutputs& out);
  // Ends the circuit's shot for why (a person, a change of the circuit); false when none runs there
  bool stop(Ctx& c, NetOutputs& out, const std::string& circuit, Msg why);
  // Takes the shots the schedules give, starts one that may, and ends the one that is done or has to
  void tick(Ctx& c, const WaterIn& in, NetOutputs& out);
  bool running() const { return shot_.has_value(); }
  // A shot cut by a restart: reported, not repeated (R6), and booked as if it ran until the restart
  // or its end. True when there was one.
  static bool reportCut(Ctx& c, const std::string& date);
  // A circuit changed, paused or removed: what waits for it, and what was skipped, goes
  void forget(const std::string& circuit);
  // A configuration imported: what waits for any circuit goes
  void reset() {
    due_.clear();
    skipped_.clear();
  }
  // A circuit removed: its last shot and its run time of the day go with it
  static void forgetRuntime(RuntimeState& rt, const std::string& circuit);
  // The litres the tank may still give before its reserve; nothing where nothing tells (no count, no
  // level)
  static std::optional<double> available(const Ctx& c, const WaterIn& in);
  json status(const Ctx& c, const WaterIn& in) const;
  // A warning that does not block (RAT-084): pH outside its range, not corrected; nothing otherwise
  static std::optional<Msg> warning(const WaterIn& in);

 private:
  struct Shot {
    std::string circuit;
    std::string name;        // as it was when the shot began: a change books it as it ran
    double rateLpm = kNaN;
    Ms since = 0;
    Ms sentAt = -1;       // when the gateway switched it on: its length and litres count from then
    bool opened = false;  // the device said it is on
    double s = 0;
    bool override = false;
    std::string why;   // "schedule" | "hand"
    std::string date;  // the local day it began: its run time counts there
  };
  struct Due {
    double s = 0;
    Epoch at = 0;  // when it came due
  };
  void start(Ctx& c, const WaterCircuitCfg& w, double s, bool override, const std::string& why, const std::string& date,
             NetOutputs& out);
  void end(Ctx& c, NetOutputs& out, Msg how, bool warn);
  void skip(Ctx& c, const WaterCircuitCfg& w, const Msg& why);
  std::optional<Shot> shot_;
  std::map<std::string, Due> due_;              // circuit → its shot that waits
  std::map<std::string, std::string> skipped_;  // circuit → why its last shot was skipped
  long long lastMinute_ = -1;                   // the UTC minute the schedules were last taken
  bool secured_ = false;                        // the clock was secured then
};

// The length of a circuit's shot due at this local minute; nothing when none is
std::optional<double> shotAt(const WaterCircuitCfg& w, int minuteOfDay);

}  // namespace doseph
