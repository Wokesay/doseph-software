// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 8: actuator gateway and dose execution.
//
// Actuators is the ONLY way to the pumps and the feed pump's output (rule
// R1). It holds the safety minimum in code that settings can only tighten
// (R7): a calibration value present, run time limits, one run at a time,
// the Stop, and no dose without proven flow (SD-105).
// The watchdog never includes this header (architecture test, R2).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "doseph/config.hpp"
#include "doseph/devices.hpp"
#include "doseph/events.hpp"
#include "doseph/hardware.hpp"
#include "doseph/journal.hpp"
#include "doseph/plan.hpp"
#include "doseph/truth.hpp"

namespace doseph {

// The context of one tick for gateway, doser and cycle.
// Runs on the coupled device (SD-131): the robot sends them there and brings back how each ended.
// This device's gateway switches nothing for them; the other device's decides.
struct RemoteResult {
  std::string state;  // done | failed | aborted
  Ms ms = 0;
  double ml = kNaN;
  Msg error;
};
class RemoteRuns {
 public:
  virtual ~RemoteRuns() = default;
  virtual bool linked() const = 0;
  virtual void send(const std::string& job, int channel, Ms ms, const std::string& purpose) = 0;
  // Once the run ended there; nothing while it is under way
  virtual std::optional<RemoteResult> result(const std::string& job) = 0;
  virtual void abort(const std::string& job) = 0;
};

struct Ctx {
  const Config& cfg;
  RuntimeState& rt;
  SensorTruth& truth;
  EventLog& log;
  DoseJournal& journal;
  Ms now = 0;
  Epoch epoch = 0;
  bool stopped = false;
  bool flowOk = false;  // flow through the device proven now: no dose without it (SD-105)
  bool pumpInTank = true;  // the feed pump lies in the tank: no priming, a short window (SD-127)
  bool pumpCut = false;    // the hardware switched the feed pump off for no flow (PD-477, SD-127)
  std::optional<double> pumpA{};  // the feed pump's current, where measured (SD-127)
  bool probeCalibrating = false;  // a probe is in a buffer: its readings are not the tank's
  Epoch maintenanceUntil = 0;     // maintenance mode until then: someone works at tank or device (0: none)
  bool channelJob = false;        // a channel is being calibrated or primed: no dose starts meanwhile
  bool refilling = false;         // the refill valve fills the tank: no dose and no round meanwhile (SD-132)
  // What holds every pump below the robot (SD-110): "leak", "high_level" (both latched until a
  // person releases them), a fault of the device's protection by its lock reason ("self_test",
  // "power_trip", …, SD-141), "return", "stop_button", "release" (off without a known input); "": nothing
  std::string hold{};
  // A device that does not test its protection itself, after a start: nothing doses into the tank
  // until a person confirmed the check by hand (SD-141)
  bool protectionCheck = false;
  // Coupled as the follower (SD-131): the leader plans. This device runs its feed pump while the
  // leader's runs, to measure and to let the leader's runs through, and doses only on its request.
  bool follower = false;
  bool linkOk = false;      // the leader was heard within 10 s
  bool leaderPump = false;  // … and its feed pump runs
  RemoteRuns* remote = nullptr;  // on the leader: the runs on the coupled device's channels
};

// A channel's own lock (SD-111): "channel.<n>.no_drops" (it ran and nothing dripped) or
// "channel.<n>.siphon" (it dripped while its pump stood). A locked channel doses nothing into the
// tank until a person releases it; a calibration or a line fill into a cup still runs.
std::string channelLatch(int channel, const std::string& what);
bool channelLocked(const RuntimeState& rt, int channel);

class Actuators {
 public:
  explicit Actuators(IHardware& hw) : hw_(hw) {}

  // Whether a run may start now, and if not why; startRun checks the same
  bool allowed(const Ctx& c, int channel, Ms ms, const std::string& purpose, Msg& err) const;
  // Why `ms` more on the channel would pass its daily amount, if they would (SD-113): its run time in
  // a rolling 24 h against the head's value and the code's ceiling, and the canister's ml per day
  std::optional<Msg> dailyCap(const Ctx& c, int channel, Ms ms) const;
  double dailyCapMin(int channel) const;  // the channel's run time per 24 h: the head's value, never over 60 min
  bool startRun(const Ctx& c, int channel, Ms ms, const std::string& purpose, const std::string& jobId, Msg& err);
  RunStatus runStatus(int channel) const { return hw_.runStatus(channel); }
  // What the channel's drop sensor has counted; nullopt without one
  std::optional<Drops> drops(int channel) const { return hw_.drops(channel); }
  // The channel's pump can run backwards, to pull its line back (SD-111)
  bool canPullBack(int channel) const {
    return channel >= 1 && channel <= hw_.channelCount() && hw_.profile().channels[static_cast<size_t>(channel - 1)].reverse;
  }
  bool channelRunning() const { return runningChannel_ != 0; }
  void clearRun(int channel);
  void stopPumps() { hw_.stopAllPumps(); }

  // The feed pump, where the device switches it: on is refused while stopped.
  bool setFeedPump(const Ctx& c, bool on, Msg& err);
  std::optional<bool> feedPump() const { return hw_.feedPump(); }
  // A return pump found dry or blocked is held off until a person releases it (SD-109)
  void allowReturnPump(bool allowed) { hw_.allowReturnPump(allowed); }

  // Stop and start: every pump and the feed pump's output off, idempotent (RAT-036, R6).
  void stopAll();
  // Each tick: a dose that loses its flow stops at once, and so does a run into the tank whose
  // outlet does not drip (SD-105, SD-111); only ever switches off.
  void enforce(const Ctx& c);
  // Drops fell while no pump ran since the last call: the next check mixes first (SD-111)
  bool takeUnplannedDrops() {
    const bool u = unplanned_;
    unplanned_ = false;
    return u;
  }

 private:
  // The drop sensor's watch over a run into the tank (SD-111)
  struct DropWatch {
    Ms since = 0;   // the run's start, or its last drop
    Ms window = 0;  // the longest wait for a drop
    Ms ended = -1;  // when the run ended; -1: it runs
    bool dripped = false;
  };
  void watchDrops(const Ctx& c);
  void lockChannel(const Ctx& c, int channel, const std::string& what);

  IHardware& hw_;
  int runningChannel_ = 0;
  std::string runningPurpose_;
  bool cutLogged_ = false;
  std::map<int, DropWatch> watch_;
  std::map<int, std::uint64_t> seen_;  // each channel's drops at the last tick
  std::map<int, Ms> lastRun_;          // when each channel's pump last ran
  bool unplanned_ = false;
};

// A dose order = one canister, split into partial runs. Booked per run from
// the actual run time (RAT-070), not only at the end; each run is written to
// the journal before the pump starts (SD-106).
struct DoseOrder {
  std::string id;
  // ec | ph | calibration | prime (a line into a cup, by time) | fill (a line into the tank, until it
  // drips, SD-111); runs of the gateway also: refill | pullback
  std::string purpose;
  DoseStep step;
};

struct DoseProgress {
  enum class State { Idle, Running, Done, Failed, Aborted } state = State::Idle;
  std::string orderId;
  int channel = 0;
  size_t run = 0;
  double mlDone = 0;
  Ms msDone = 0;
  Msg error;
};

// Takes `ml` from the canister's counted level, if it is known, and reports
// once when the count falls below zero (SD-119).
void takeStock(const Ctx& c, const std::string& canister, const std::string& name, double ml);

// What a run on the coupled device may take beyond its own time before it counts as lost: the
// link's 10 s and a call (SD-131)
constexpr Ms kRemoteGraceMs = 12 * kSecond;

class Doser {
 public:
  // The tag of this start, appended to every job ID, so an ID after a
  // restart is never taken as a repeat of an old run.
  void setBootTag(std::string tag) { bootTag_ = std::move(tag); }
  bool busy() const { return active_.has_value(); }
  bool start(const Ctx& c, Actuators& act, DoseOrder order, Msg& err);
  // Progress; books finished runs. A run the driver does not report back in
  // time is switched off and booked as run (the safe direction: no dosing again).
  void tick(const Ctx& c, Actuators& act);
  // Stops and books what has run (RAT-070).
  void abort(const Ctx& c, Actuators& act, const Msg& reason);
  const DoseProgress& progress() const { return progress_; }
  const std::optional<DoseOrder>& active() const { return active_; }
  // Fetch a finished order and reset.
  std::optional<DoseProgress> takeFinished();

 private:
  // What runs now (SD-111): the line pulled back last time filled again, a run of the dose, or
  // the line pulled back after its last run
  enum class Leg { Refill, Dose, PullBack };
  // `known`: the run time was reported; a guess (no answer, a lost link, an abort without a report)
  // is booked to the stock and the daily amount, and counts as not known for the plants' records
  void book(const Ctx& c, Ms ms, bool known = true);
  void logOrder(const Ctx& c);
  bool launch(const Ctx& c, Actuators& act);
  bool pullBack(const Ctx& c, Actuators& act);
  void legEnded(const Ctx& c, Actuators& act, bool done, Ms ran, const std::string& error);
  void finish(const Ctx& c, DoseProgress::State state);
  void fail(const Ctx& c, Msg error);
  std::string busJob() const;
  std::optional<DoseOrder> active_;
  bool dripped(Actuators& act) const;  // a line fill: the first drops fell
  Leg leg_ = Leg::Dose;
  std::string job_;  // the job ID of the run under way
  std::uint64_t fillFrom_ = 0;  // a line fill: the drops counted when it started
  bool filled_ = false;        // a line fill: it dripped, and its run was stopped
  DoseProgress progress_;
  std::optional<DoseProgress> finished_;
  Ms pauseUntil_ = 0;
  Ms runStartedAt_ = 0;
  Ms runRequestedMs_ = 0;
  bool running_ = false;
  bool remote_ = false;  // the order's runs go to the coupled device (SD-131)
  void remoteTick(const Ctx& c);
  int seq_ = 0;
  std::string bootTag_;
};

// The outputs of network devices (SD-132), switched only here (R1). Logic says what it wants of an
// output role; the gateway decides. Before every switch-on it reads the output's safety settings
// back (RAT-019), writes them once more where they differ, and refuses to switch on where they
// still differ. An answer counts only for the request it answers. Off goes at once, and again while
// the device still says on. An output bound anew, or whose device was away, or that is on without
// the robot having switched it after a check, is switched off and checked before it may switch on.
// An output let go (unbound, rebound, its device removed or moved) is switched off, and the off is
// repeated until the device answers or a minute has passed. The refill valve closes on its own
// limits, whatever wants it open (RAT-032): while the float does not say "not full", and once it
// has been on for the longest fill. Light (SD-135) is neither held nor forced off by a Stop: it
// follows its plan, and one found on after a restart is checked while it stays on; one whose settings
// do not hold is switched off. A dimmer is a Shelly "Light": it switches on at its brightness, and a
// brightness read back otherwise is set again, the brightness alone. Bound to the other kind of output
// (a dimmer on a relay, or the reverse), a light is said and nothing is sent to it.
class NetOutputs {
 public:
  // False for a role that is not an output
  bool want(const std::string& role, bool on);
  // A dimmer's brightness while on, 1 to 100 % (SD-135)
  void wantLevel(const std::string& role, double pct);
  // A Stop or a start: nothing wanted, and every bound output but light and a room's fans gets an off
  // at the next tick, whatever it said last (R6, RAT-036); light follows its plan, the fans their rule
  // (SD-135, SD-136)
  void allOff();
  // `stopped`: a Stop holds every output but light and a room's fans; `hold`: a hold of every pump
  // holds what is neither light nor a room's (SD-135, SD-136)
  void tick(const Config& cfg, NetDevices& dev, bool stopped, bool hold, Ms now);
  void tick(const Config& cfg, NetDevices& dev, bool hold, Ms now) { tick(cfg, dev, hold, hold, now); }
  void answer(const Config& cfg, NetDevices& dev, const NetAnswer& a, Ms now);
  void release(const Config& cfg, NetDevices& dev, const std::string& role, Ms now);
  // Every bound output, light and a room's fans too: an off until each device confirms it, a minute at
  // most (a factory reset, SD-139, R6)
  void releaseAll(const Config& cfg, NetDevices& dev, Ms now);
  // An off of a released output still waits for its device
  bool releasing() const { return !releasing_.empty(); }
  // Only the released outputs' offs, again until answered: while a factory reset waits for them,
  // nothing is bound anew (SD-139)
  void tickReleases(NetDevices& dev, Ms now);
  // Releases whose off no device confirmed within a minute, by the name of the device
  std::vector<std::string> takeFailedReleases();
  // Why an output does not switch, where it does not
  Msg problem(const std::string& role) const;
  // Each output role: {state, want, verified, switchedOn, problem}; state: not_assigned | offline | checking |
  // unchecked | refused | off | on. Only "off" and "on" are ready.
  json status(const Config& cfg, const NetDevices& dev, Ms now) const;

 private:
  struct Out {
    bool want = false;
    std::string key;    // device@address:channel it is bound to, to see a new binding
    std::string stage;  // "" | read | write | reread | on
    long long pending = 0;  // the request the stage waits for
    bool wrote = false;     // the settings were written once in this check
    bool verified = false;  // read back as written
    bool refused = false;   // written and read back, and still not so: it does not switch on
    bool onSent = false;    // switched on by the robot after a check
    bool wasOnline = false;
    Msg problem;
    Ms lastOff = -kNetPollMs;
    Ms retryAt = 0;
    Ms onAt = -1;  // when the robot last switched it on
    double level = 100;  // a dimmer's brightness wanted, %
    Ms lastLevel = -kNetPollMs;
  };
  struct Release {
    NetDeviceCfg device;
    std::string comp = "Switch";  // "Light" for a dimmer
    std::string user, password, ownId;
    int channel = 0;
    long long pending = 0;
    Ms lastSent = 0, until = 0;
  };
  void send(const Config& cfg, NetDevices& dev, const std::string& role, Out& o, const std::string& stage, const std::string& method, const json& params, Ms now);
  void off(const Config& cfg, NetDevices& dev, const std::string& role, Out& o, Ms now);
  static bool limited(const Config& cfg, const NetDevices& dev, const std::string& role, Ms now);
  static Ms longestFill(const Config& cfg);
  std::map<std::string, Out> outs_;
  std::vector<Release> releasing_;
  std::vector<std::string> failedReleases_;
  // Whether a Stop or a hold of every pump holds an output of this kind
  bool heldBy(const RoleDef& def) const { return def.light || def.fan ? false : def.climate ? stopped_ : stopped_ || hold_; }
  bool stopped_ = false, hold_ = false;
  bool forceOff_ = false;
};

}  // namespace doseph
