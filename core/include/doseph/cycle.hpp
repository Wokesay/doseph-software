// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 7: the control cycle of the mixing robot as a state machine (SD-105).
//
//   suction pump on → prove flow → flush → measure until stable → EC before pH →
//   dose in partial doses → wait for mixing (the clock runs only with flow)
//   → measure again and learn the effect → hold, and after the hold anew.
//
// It reads only the sensor truth and the settings (R3, R4) and asks the
// actuator gateway for every switch (R1). No dose without proven flow: the
// gateway refuses it and stops a dose that loses its flow (SD-105). The
// first dose of a new tank is a small test dose that checks the entered
// volume (SD-105). All times are starting points, not findings.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "doseph/gateway.hpp"

namespace doseph {

enum class Phase { Off, Stopped, SuctionOn, ProveFlow, Flush, Measure, Dose, Mix, Remeasure, Hold, Wait };
const char* phaseName(Phase p);

// Timing of the cycle: starting points, assumptions until measured (SD-105)
constexpr double kFlowMinLpm = 0.5;           // the least flow that counts as flow
constexpr Ms kFlowProveMs = 10 * kSecond;     // … for this long without a break
constexpr Ms kFlowWaitMs = 2 * kMinute;       // no flow by then: wait and try again
constexpr Ms kFlushMs = 60 * kSecond;         // flow time until the measuring chamber holds tank water
constexpr Ms kStableWindowMs = 60 * kSecond;  // readings within the band for this long
constexpr double kStablePh = 0.05, kStableEc = 0.03;
constexpr Ms kMeasureMaxMs = 15 * kMinute;    // not stable by then: wait and try again
constexpr Ms kRetryMs = 5 * kMinute;          // a wait tries again after this
constexpr Ms kNoFlowMixMaxMs = 30 * kMinute;  // the mixing clock stood this long: wait
constexpr Epoch kEcRestS = 240;               // pH waits this long after an EC dose (RAT-046)
constexpr double kTestRiseEc = 0.1;           // a test dose's aim (SD-105)
constexpr double kTestDropPh = 0.1;
// The test dose's rise against the one expected at the entered volume: above twice it, the tank is
// likely smaller than entered (the dangerous side: every dose too large); below a third, likely
// larger. In between, incomplete mixing and a chart EC that holds the water's own EC pass.
constexpr double kVolumeHigh = 2.0, kVolumeLow = 1.0 / 3.0;
constexpr double kNoEffectEc = 0.02;          // a round that raised EC by less had no effect (RAT-055)
constexpr double kNoEffectPh = 0.03;          // … a pH− dose that lowered pH by less (RAT-020, RAT-041)
constexpr double kSmallRoundEc = 0.06;        // rounds made smaller that should raise less are judged as a sum (RAT-055)
constexpr int kNoEffectLatch = 2;             // rounds without effect in a row, then a latch
constexpr double kCatchUpMinMl = 0.1;         // a pair part missing less is not caught up (SD-114)

// What a check measured once the readings were stable: the means of the window
struct Measured {
  double ph = kNaN, ec = kNaN, temp = kNaN;
  Epoch at = 0;
};

// The settings the cycle runs on now, worked out each tick
struct CycleSettings {
  const RecipeCfg* recipe = nullptr;
  Msg off;  // why the robot does not mix now; empty key when it does
  double volumeL = kNaN;
  double ecTarget = kNaN, ecTol = kNaN, phTarget = kNaN, phTol = kNaN;
  double flowLpm = kNaN, turnoverMin = kNaN;
  double waitEcMin = kNaN, waitPhMin = kNaN;  // in force: the user's, else the suggestion
  double suggestEcMin = kNaN, suggestPhMin = kNaN;
  Ms holdMs = 60 * kMinute;
  std::string circulation = "cycle";
};
CycleSettings cycleSettings(const Ctx& c);

// Drops what a pair still misses, with an event naming the parts and why
// (SD-114): the recipe changed its parts, a new tank, or a person ended it.
void dropCatchUp(Ctx& c, const Msg& why);
// A pair part dosing when the robot restarted: what its runs booked counts,
// a run cut by the restart as if it all went in (SD-114)
void pairAfterRestart(RuntimeState& rt, const std::vector<JournalRun>& runs);

class MixCycle {
 public:
  void tick(Ctx& c, Actuators& act, Doser& doser);
  // The Stop: the running dose is aborted (booked), the cycle stops (RAT-036).
  void stop(Ctx& c, Actuators& act, Doser& doser);
  // A new cycle from the next tick: resume, a new tank, a latch released, a
  // setting changed. A dose not mixed in yet is mixed first; a dose running
  // is aborted (booked).
  void restart(Ctx& c, Actuators& act, Doser& doser);
  // After a start with doses not mixed in (the journal): the first cycle
  // mixes for this long with flow before it measures, and learns nothing.
  void mixFirst(Ms ms) { mixPendingMs_ = std::max(mixPendingMs_, ms); }
  // The recipe a round doses changed or went: the round learns nothing (RAT-057)
  // A dose order of the cycle finished (the robot routes the doser's result here)
  void doseFinished(Ctx& c, const DoseProgress& p);
  void recipeChanged(const std::string& id) {
    if (id == roundRecipe_) clean_ = false;
  }

  Phase phase() const { return phase_; }
  bool dosing() const { return phase_ == Phase::Dose; }
  const Measured& last() const { return last_; }
  json status(const Ctx& c) const;
  // Why the robot doses or not: what each kind of dose needs, each with
  // whether it holds now ({ok, key, text, args}), in groups "all", "ec"
  // and "ph". The cycle decides; the list only explains it.
  json checks(const Ctx& c) const;

 private:
  enum class Kind { None, Ec, Ph };
  void enter(const Ctx& c, Phase p);
  void wait(Ctx& c, Actuators& act, Msg why, Ms retry = kRetryMs);
  void suctionPump(Ctx& c, Actuators& act, bool on);
  bool measuring(Ctx& c, Actuators& act);  // true once the window is stable; fills `measured_`
  void evaluate(Ctx& c, Actuators& act);
  void startEc(Ctx& c, Actuators& act, const CycleSettings& s, double ec, double aim);
  void startPh(Ctx& c, Actuators& act, const CycleSettings& s, double ph);
  void learn(Ctx& c, Actuators& act);
  void holdNow(Ctx& c, Actuators& act);
  // SD-114: the pair parts of a round, and what they miss once it ended
  void recordPairs(Ctx& c, const std::string& recipe, const std::vector<DoseStep>& steps, bool catchUp);
  void settlePairs(Ctx& c);
  bool startCatchUp(Ctx& c, Actuators& act, const CycleSettings& s, double ec);

  Phase phase_ = Phase::Off;
  Ms since_ = 0;        // the phase began (monotonic)
  Epoch sinceEpoch_ = 0;
  Ms lastTick_ = -1;
  Msg line_;             // what the robot does now, or why it waits
  std::vector<Msg> notes_;  // what it leaves as it is, with the reason (RAT-037)
  Ms retryAt_ = 0;       // Wait and Hold: when it starts anew; 0 for a wait without end

  // Flow
  Ms flowSince_ = 0;     // flow proven since (0: none)
  Ms noFlowSince_ = 0;
  Ms flushedMs_ = 0;
  double flowSum_ = 0;
  int flowN_ = 0;
  bool noFlowLogged_ = false;

  // Measuring
  struct Sample3 {
    Ms at;
    double ph, ec, temp;
  };
  std::deque<Sample3> window_;
  Measured measured_, last_;
  Ms evaluateAt_ = 0;    // pH rests after an EC dose (RAT-046)

  // Mixing
  Ms mixedMs_ = 0, mixTargetMs_ = 0;
  Ms mixPendingMs_ = 0;  // doses not mixed in yet: mix first, learn nothing
  bool mixingFirst_ = false;

  // The round
  Kind kind_ = Kind::None;
  std::vector<DoseStep> queue_;
  std::string roundRecipe_;
  Measured before_;
  double mlRound_ = 0, volRound_ = kNaN, expectedRise_ = kNaN, expectedTest_ = kNaN;
  bool limited_ = false, testDose_ = false, clean_ = true, doseFailed_ = false;
  Epoch lastEcDoseAt_ = 0;
  int ecRounds_ = 0, phDoses_ = 0, seq_ = 0;
  int noEffectEc_ = 0, noEffectPh_ = 0;
  // Rounds of one recipe made smaller to fit the partial-run limit, each too
  // small to judge alone: summed until they should raise EC by 0.06 (RAT-055)
  struct SmallRounds {
    double rise = 0, expected = 0, mlPerL = 0;
    std::string recipe;
  } small_;
  bool anyDosed_ = false;  // this check dosed: its end is logged
};

}  // namespace doseph
