// SPDX-License-Identifier: AGPL-3.0-or-later
// Composition root of the core: connects hardware, sensor truth, gateway,
// doser, dosing journal, cycle, watchdog, history and storage. Runs as one
// event loop with an injected clock (rule R8); the API calls the public
// methods under the robot's mutex.
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "doseph/auth.hpp"
#include "doseph/clock.hpp"
#include "doseph/cycle.hpp"
#include "doseph/history.hpp"
#include "doseph/watchdog.hpp"

namespace doseph {

struct Result {
  int status = 200;
  json body = json::object();
  static Result ok(json b = json::object()) { return {200, std::move(b)}; }
  // An error is a message with key and values, shown in the page's language.
  static Result fail(int st, const Msg& m, json extra = json::object());
};

// A channel's calibration or priming run, into a cup or the line (RAT-070).
struct ChannelJob {
  std::string id, type;  // calibration | prime (into a cup) | fill (into the tank, SD-111)
  int channel = 0;
  std::string head;               // calibration: the pump head it measures (SD-108)
  std::string state = "running";  // running | waiting_result | done | failed
  Ms ms = 0, actualMs = 0;
  double ml = kNaN;  // calibration: what the user measured in the cup
  Msg message;
  Epoch at = 0;
};

class Robot {
 public:
  Robot(IHardware& hw, IStorage& storage, const IClock& clock, RandomFn rng);

  void boot();
  void tick();
  void flush();     // saves what is not on disk yet: state, new events, history
  bool saveAuth();  // the password file, on disk when it returns true

  std::recursive_mutex& mutex() { return mtx_; }
  const Config& config() const { return cfg_; }
  const RuntimeState& runtime() const { return rt_; }
  Auth& auth() { return auth_; }
  // The password was set but auth.json is gone: no setup over the network.
  bool credentialsLost() const { return cfg_.system.passwordSet && !auth_.hasPassword(); }
  // auth.json is there but holds no password the robot can read: it is kept
  // as it is, no new password goes over it, and nobody is told to reset the
  // robot, since a restart may read it again.
  bool passwordKept() const { return authKept_; }
  void markPasswordSet();
  // The platform shows the code to its owner alone (a label, a display, the
  // simulator's console); "" when a password is set.
  std::string armSetupCode(Ms validFor = 0);
  void setPlatform(json p) { platform_ = std::move(p); }

  // ---- Reading
  json info() const;
  json state();
  Epoch now() const { return clock_.epoch(); }
  json configJson() const;
  json history(const std::string& series, Epoch from, Epoch to, size_t points) const;
  // The history as CSV for a spreadsheet: a row per time in UTC, a column
  // per series. German ("de") with semicolons and a decimal comma, else
  // commas and a decimal point; a gap stays an empty field (RAT-016).
  std::string historyCsv(const std::vector<std::string>& series, Epoch from, Epoch to, const std::string& lang) const;
  json events(Epoch from, Epoch to, const std::string& type, size_t limit) const;
  static json templates();
  // The package of "Report a problem": version, settings, states and the
  // events of the last 72 hours, with a report number. No secrets: no
  // password hash, no sessions, no addresses; the app shows it first.
  json diagnostics();
  const MixCycle& cycle() const { return cycle_; }
  const SensorTruth& truth() const { return truth_; }

  // ---- Settings
  Result setSystem(const json& j);
  Result putTank(const json& j);
  Result putControl(const json& j);
  Result newTank();
  Result putCanister(const json& j);
  Result deleteCanister(const std::string& id);
  // A canister's level by hand: {"ml": n} as measured, or {"full": true} for
  // its size after a refill (SD-119)
  Result setStock(const std::string& id, const json& j);
  // A person ends the catch-up of a pair's missing parts (SD-114)
  Result endCatchUp();
  Result putRecipe(const json& j);
  Result deleteRecipe(const std::string& id);
  Result setActive(const json& j);
  // Restores a backup (a configuration as GET /config gives it): checked like
  // every change and its calibrations like a new one; refused while a dose,
  // a pump run or a probe calibration goes on. Never takes passwordSet from
  // the file. What the robot learned stays only where the file keeps it.
  Result importConfig(const json& j);

  // ---- Operation
  Result prime(int channel, double seconds);
  Result startCalibration(int channel, double seconds);
  Result calibrationResult(double ml);
  Result cancelChannelJob();
  Result probe(const json& j);
  Result ackLatch(const std::string& id, const json& body);
  Result stop(const Msg& who);
  Result resume();
  // Maintenance mode for `minutes` (at most kMaxMaintenanceMin; 0 ends it): the
  // cycle holds, the suction pump is off, jumps of the probes are taken as the work
  // and the watchdog does not judge the targets. Locks stay. In memory only:
  // a restart ends it.
  Result maintenance(double minutes);
  static constexpr double kMaxMaintenanceMin = 240;

  // An event from the API (a sign-in, for example), logged for security
  void logEvent(const std::string& type, const std::string& sev, Msg title, Msg text = {});

 private:
  Ctx ctx();
  bool flowOk() const;
  Result saveConfig(Config next, const std::optional<Msg>& what);
  void saveState(bool force = false);
  void saveEvents();
  void sampleHistory(Epoch epoch);
  void watchRelease(Epoch epoch);
  void checkHeads(Epoch epoch);
  void mixFirstNow();
  void snapshot(Epoch epoch);
  void channelJobDone(const DoseProgress& p);
  Result startChannelJob(const std::string& type, int channel, double seconds);
  bool channelJobActive() const { return job_ && (job_->state == "running" || job_->state == "waiting_result"); }
  void restartCycle();

  mutable std::recursive_mutex mtx_;
  IHardware& hw_;
  IStorage& store_;
  HubClock clock_;
  RandomFn rng_;
  json platform_ = json::object();

  Config cfg_;
  RuntimeState rt_;
  Auth auth_;
  SensorTruth truth_;
  Actuators act_;
  Doser doser_;
  DoseJournal journal_;
  MixCycle cycle_;
  History history_;
  EventLog log_;
  WatchResult watch_;
  std::optional<ChannelJob> job_;
  // A probe calibration: the kind, its points and when the last step was
  // taken. It holds the cycle until it is saved or cancelled; it explains
  // jumps for 30 min after its last step (RAT-042).
  struct ProbeSession {
    std::string kind;  // ph | ec
    json points = json::array();
    double tempC = kNaN;
    Ms lastStep = 0;
  };
  std::optional<ProbeSession> probe_;
  bool authKept_ = false;

  Ms maintenanceUntilMs_ = 0;  // monotonic, so no clock step stretches it; 0: none

  bool stopButtonSeen_ = false;  // the stop button was pressed; a new press stops again (SD-110)

  std::string lastHold_;         // the hold of the last tick: when it ends, the cycle starts anew
  bool lastChannelJob_ = false;  // a channel job ran in the last tick: when it ends, the check starts anew
  Epoch maintenanceUntil() const;

  Epoch bootEpoch_ = 0;
  Ms bootMs_ = 0;
  Epoch lastSample_ = 0, lastWatch_ = 0, lastStateSave_ = 0, lastSnapshot_ = 0;
  std::string savedState_;
  std::uint64_t savedEventId_ = 0;
  int idSeq_ = 0;
};

}  // namespace doseph
