// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 8: actuator gateway and dose execution.
//
// Actuators is the ONLY way to the pumps and the suction pump's output (rule
// R1). It holds the safety minimum in code that settings can only tighten
// (R7): a calibration value present, run time limits, one run at a time,
// the Stop, and no dose without proven flow (SD-105).
// The watchdog never includes this header (architecture test, R2).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "doseph/config.hpp"
#include "doseph/events.hpp"
#include "doseph/hardware.hpp"
#include "doseph/journal.hpp"
#include "doseph/plan.hpp"
#include "doseph/truth.hpp"

namespace doseph {

// The context of one tick for gateway, doser and cycle.
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
  bool probeCalibrating = false;  // a probe is in a buffer: its readings are not the tank's
  Epoch maintenanceUntil = 0;     // maintenance mode until then: someone works at tank or device (0: none)
  bool channelJob = false;        // a channel is being calibrated or primed: no dose starts meanwhile
};

class Actuators {
 public:
  explicit Actuators(IHardware& hw) : hw_(hw) {}

  bool startRun(const Ctx& c, int channel, Ms ms, const std::string& purpose, const std::string& jobId, Msg& err);
  RunStatus runStatus(int channel) const { return hw_.runStatus(channel); }
  bool channelRunning() const { return runningChannel_ != 0; }
  void clearRun(int channel);
  void stopPumps() { hw_.stopAllPumps(); }

  // The suction pump, where the device switches it: on is refused while stopped.
  bool setSuctionPump(const Ctx& c, bool on, Msg& err);
  std::optional<bool> suctionPump() const { return hw_.suctionPump(); }

  // Stop and start: every pump and the suction pump's output off, idempotent (RAT-036, R6).
  void stopAll();
  // Each tick: a dose that loses its flow stops at once; only ever switches off (SD-105).
  void enforce(const Ctx& c);

 private:
  IHardware& hw_;
  int runningChannel_ = 0;
  std::string runningPurpose_;
  bool cutLogged_ = false;
};

// A dose order = one canister, split into partial runs. Booked per run from
// the actual run time (RAT-070), not only at the end; each run is written to
// the journal before the pump starts (SD-106).
struct DoseOrder {
  std::string id;
  std::string purpose;  // ec | ph | calibration | prime
  DoseStep step;
};

struct DoseProgress {
  enum class State { Idle, Running, Done, Failed, Aborted } state = State::Idle;
  std::string orderId;
  size_t run = 0;
  double mlDone = 0;
  Ms msDone = 0;
  Msg error;
};

// Takes `ml` from the canister's counted level, if it is known, and reports
// once when the count falls below zero (SD-119).
void takeStock(const Ctx& c, const std::string& canister, const std::string& name, double ml);

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
  void book(const Ctx& c, Ms ms);
  void logOrder(const Ctx& c);
  bool launch(const Ctx& c, Actuators& act);
  void fail(const Ctx& c, Msg error);
  std::string busJob() const;
  std::optional<DoseOrder> active_;
  DoseProgress progress_;
  std::optional<DoseProgress> finished_;
  Ms pauseUntil_ = 0;
  Ms runStartedAt_ = 0;
  Ms runRequestedMs_ = 0;
  bool running_ = false;
  int seq_ = 0;
  std::string bootTag_;
};

}  // namespace doseph
