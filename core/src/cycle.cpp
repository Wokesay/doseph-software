// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/cycle.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/messages.hpp"

namespace doseph {

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Off: return "off";
    case Phase::Stopped: return "stopped";
    case Phase::PumpOn: return "pump_on";
    case Phase::ProveFlow: return "prove_flow";
    case Phase::Flush: return "flush";
    case Phase::Measure: return "measure";
    case Phase::Dose: return "dose";
    case Phase::Mix: return "mix";
    case Phase::Remeasure: return "remeasure";
    case Phase::Hold: return "hold";
    case Phase::Wait: return "wait";
  }
  return "unknown";
}

namespace {

constexpr double kEcFromPhDown = 1.04;  // mS/cm per ml/L pH− (RAT-053)
constexpr Ms kAnnounceMs = kMinute;     // an announced change explains jumps this much longer (RAT-042)

Ms minutes(double m) { return static_cast<Ms>(std::llround(m * 60.0)) * kSecond; }
std::string mmss(Ms ms) {
  Ms s = std::max<Ms>(0, (ms + 999) / 1000);
  std::string sec = std::to_string(s % 60);
  return std::to_string(s / 60) + ":" + (sec.size() < 2 ? "0" + sec : sec);
}

}  // namespace

CycleSettings cycleSettings(const Ctx& c) {
  CycleSettings s;
  const auto& cfg = c.cfg;
  s.circulation = cfg.tank.circulation;
  s.volumeL = cfg.tank.volumeL;
  s.holdMs = minutes(isNum(cfg.tank.holdMin) ? std::clamp(cfg.tank.holdMin, kHoldMinMin, kHoldMaxMin) : 60.0);
  s.flowLpm = c.rt.flowLpm;
  if (isNum(s.volumeL) && isNum(s.flowLpm) && s.flowLpm > 0) s.turnoverMin = s.volumeL / s.flowLpm;
  // The suggestion: about two turnovers for EC, three for pH (SD-105, RAT-052, RAT-058); the user's value wins
  s.suggestEcMin = suggestedWaitMin(s.volumeL, s.flowLpm, 2.0);
  s.suggestPhMin = suggestedWaitMin(s.volumeL, s.flowLpm, 3.0);
  auto own = [](double v) { return isNum(v) ? std::clamp(v, kWaitMinMin, kWaitMaxMin) : kNaN; };
  s.waitEcMin = isNum(cfg.tank.waitEcMin) ? own(cfg.tank.waitEcMin) : s.suggestEcMin;
  s.waitPhMin = isNum(cfg.tank.waitPhMin) ? own(cfg.tank.waitPhMin) : s.suggestPhMin;
  s.ecTol = std::max(0.05, isNum(cfg.control.ecTolerance) ? cfg.control.ecTolerance : 0.1);  // never 0 (RAT-008)
  s.phTol = std::max(0.05, isNum(cfg.control.phTolerance) ? cfg.control.phTolerance : 0.15);
  const RecipeCfg* r = cfg.active.recipe.empty() ? nullptr : cfg.recipe(cfg.active.recipe);
  if (!r) {
    s.off = say("cycle.off.no_recipe");
  } else if (cfg.active.until != 0 && c.epoch >= cfg.active.until) {
    s.off = say("cycle.off.ended", {{"name", r->name}, {"until", cfg.active.until}});
  } else if (!isNum(s.volumeL) || s.volumeL <= 0) {
    s.off = say("cycle.off.no_volume");
  } else {
    s.recipe = r;
    s.ecTarget = r->ecTarget;
    s.phTarget = phTarget(*r);
  }
  return s;
}

void MixCycle::enter(const Ctx& c, Phase p) {
  phase_ = p;
  since_ = c.now;
  sinceEpoch_ = c.epoch;
  switch (p) {
    case Phase::PumpOn:
      notes_.clear();
      ecRounds_ = phDoses_ = 0;
      evaluateAt_ = 0;
      break;
    case Phase::ProveFlow:
      flowSince_ = 0;
      break;
    case Phase::Flush:
      flushedMs_ = 0;
      flowSum_ = 0;
      flowN_ = 0;
      break;
    case Phase::Measure:
    case Phase::Remeasure:
      window_.clear();
      break;
    case Phase::Mix:
      noFlowSince_ = 0;
      break;
    default:
      break;
  }
}

void MixCycle::circulation(Ctx& c, Actuators& act, bool on) {
  if (c.cfg.tank.circulation == "external") return;
  auto st = act.circulation();
  if (st && *st == on) return;
  Msg err;
  if (!act.setCirculation(c, on, err) && on) line_ = say("cycle.circ_failed", {{"reason", err}});
}

void MixCycle::wait(Ctx& c, Actuators& act, Msg why, Ms retry) {
  enter(c, Phase::Wait);
  line_ = std::move(why);
  retryAt_ = retry > 0 ? c.now + retry : 0;
  if (c.cfg.tank.circulation == "cycle") circulation(c, act, false);
}

void MixCycle::holdNow(Ctx& c, Actuators& act) {
  enter(c, Phase::Hold);
  const CycleSettings s = cycleSettings(c);
  retryAt_ = c.now + s.holdMs;
  line_ = say("cycle.hold", {{"ph", numOrNull(measured_.ph)}, {"ec", numOrNull(measured_.ec)}});
  // Everything it dosed is mixed in and measured: the journal starts empty again (SD-106)
  c.journal.clear();
  if (anyDosed_)
    c.log.add(c.epoch, "control", "info", say("ev.cycle.held"),
              say("ev.cycle.held.text", {{"ph", numOrNull(measured_.ph)}, {"ec", numOrNull(measured_.ec)}}),
              {{"ph", numOrNull(measured_.ph)}, {"ec", numOrNull(measured_.ec)}});
  anyDosed_ = false;
  if (s.circulation == "cycle") circulation(c, act, false);
}

void MixCycle::stop(Ctx& c, Actuators& act, Doser& doser) {
  restart(c, act, doser);
  enter(c, Phase::Stopped);
  line_ = say("cycle.stopped");
}

void MixCycle::restart(Ctx& c, Actuators& act, Doser& doser) {
  // What a round put in and did not mix and measure yet is mixed first; the
  // round learns nothing (RAT-057)
  const bool open = phase_ == Phase::Dose || phase_ == Phase::Mix || phase_ == Phase::Remeasure || mixingFirst_ ||
                    (kind_ != Kind::None && mlRound_ > 0);
  if (phase_ == Phase::Dose && doser.busy()) doser.abort(c, act, say("abort.interrupted"));
  if (open) {
    const CycleSettings s = cycleSettings(c);
    const double m = std::max(isNum(s.waitEcMin) ? s.waitEcMin : 0.0, isNum(s.waitPhMin) ? s.waitPhMin : 0.0);
    mixFirst(minutes(std::max(m, kWaitMinMin)));
  }
  kind_ = Kind::None;
  queue_.clear();
  clean_ = false;
  mixingFirst_ = false;
  // The state says so at once, not only at the next tick
  const CycleSettings s = cycleSettings(c);
  if (c.stopped) {
    enter(c, Phase::Stopped);
    line_ = say("cycle.stopped");
  } else if (!s.off.key.empty()) {
    enter(c, Phase::Off);
    line_ = s.off;
  } else {
    enter(c, Phase::PumpOn);
    line_ = say(s.circulation == "external" ? "cycle.wait_flow_external" : "cycle.pump_on");
  }
}

void MixCycle::doseFinished(Ctx& c, const DoseProgress& p) {
  if (kind_ == Kind::None) return;  // a round ended meanwhile: its journal makes the next cycle mix first
  mlRound_ += p.mlDone;
  if (kind_ == Kind::Ec && p.msDone > 0) lastEcDoseAt_ = c.epoch;
  if (p.state != DoseProgress::State::Done) {
    clean_ = false;  // a cut dose says nothing about the effect (RAT-057)
    doseFailed_ = true;
    notes_.push_back(say("cycle.note.dose_failed", {{"reason", p.error}}));
  }
}

bool MixCycle::measuring(Ctx& c, Actuators& act) {
  const Reading& ph = c.truth.get(Channel::Ph);
  const Reading& ec = c.truth.get(Channel::Ec);
  const Reading& t = c.truth.get(Channel::Temp);
  if (!ec.usable()) {
    window_.clear();
    // A reason waiting does not cure, at once; one that may pass, after a while
    const bool lasting = ec.quality == Quality::Uncalibrated || ec.quality == Quality::Implausible ||
                         ec.quality == Quality::Jump || ec.quality == Quality::Frozen || ec.quality == Quality::Offline;
    if (lasting || c.now - since_ > kFlowWaitMs) {
      wait(c, act, say("cycle.wait.ec", {{"reason", ec.reason}}));
      return false;
    }
    line_ = say("cycle.measure_wait_ec");
    return false;
  }
  window_.push_back({c.now, ph.usable() ? *ph.value : kNaN, *ec.value, t.usable() ? *t.value : kNaN});
  while (window_.size() >= 2 && window_[1].at <= c.now - kStableWindowMs) window_.pop_front();
  double phMin = kNaN, phMax = kNaN, ecMin = kNaN, ecMax = kNaN, phSum = 0, ecSum = 0, tSum = 0;
  int tN = 0;
  bool phAll = true;
  for (const auto& s : window_) {
    ecMin = isNum(ecMin) ? std::min(ecMin, s.ec) : s.ec;
    ecMax = isNum(ecMax) ? std::max(ecMax, s.ec) : s.ec;
    ecSum += s.ec;
    if (isNum(s.ph)) {
      phMin = isNum(phMin) ? std::min(phMin, s.ph) : s.ph;
      phMax = isNum(phMax) ? std::max(phMax, s.ph) : s.ph;
      phSum += s.ph;
    } else {
      phAll = false;
    }
    if (isNum(s.temp)) {
      tSum += s.temp;
      tN++;
    }
  }
  const Ms spanned = c.now - window_.front().at;
  const bool steady = ecMax - ecMin <= kStableEc && (!phAll || phMax - phMin <= kStablePh);
  if (spanned < kStableWindowMs || !steady) {
    if (c.now - since_ > kMeasureMaxMs) {
      c.log.add(c.epoch, "control", "warn", say("ev.cycle.unsteady"),
                say("ev.cycle.unsteady.text", {{"ec", ecMax - ecMin}, {"ph", phAll ? json(phMax - phMin) : json(nullptr)}}));
      wait(c, act, say("cycle.wait.unsteady"));
      return false;
    }
    line_ = say(phase_ == Phase::Remeasure ? "cycle.remeasure" : "cycle.measure",
                {{"left", mmss(std::max<Ms>(0, kStableWindowMs - spanned))}, {"moving", !steady}});
    return false;
  }
  const double n = static_cast<double>(window_.size());
  measured_.ec = ecSum / n;
  measured_.ph = phAll ? phSum / n : kNaN;  // a pH not usable all through the window is none (R5)
  measured_.temp = tN > 0 ? tSum / tN : kNaN;
  measured_.at = c.epoch;
  last_ = measured_;
  if (c.now < evaluateAt_) {
    line_ = say("cycle.ph_rest", {{"left", mmss(evaluateAt_ - c.now)}});
    return false;
  }
  return true;
}

void MixCycle::evaluate(Ctx& c, Actuators& act) {
  const CycleSettings s = cycleSettings(c);
  if (!s.recipe) return;
  const RecipeCfg& r = *s.recipe;
  notes_.clear();
  // A test dose that does not fit the entered volume holds every dose until a person looks (SD-105)
  if (auto v = c.rt.latches.find("tank.volume"); v != c.rt.latches.end()) {
    wait(c, act, say("cycle.wait.volume", {{"implied", jnum(v->second, "impliedL")}, {"entered", jnum(v->second, "enteredL")}}), 0);
    return;
  }
  const double ec = measured_.ec, ph = measured_.ph;
  const CanisterCfg* down = c.cfg.phDown();
  const bool phLatched = c.rt.latches.count("ph.no_effect") > 0;
  const bool phHigh = isNum(s.phTarget) && isNum(ph) && ph > s.phTarget + s.phTol;
  // EC aims below its target by what the pH− dose that follows adds (RAT-053)
  double aim = s.ecTarget;
  if (isNum(aim) && phHigh && down && !phLatched) {
    const double eff = clampEffect(c.rt.phEffect, kPhStartEffect);
    aim -= std::min(0.3, (ph - s.phTarget) / eff * kEcFromPhDown);
  }

  // EC first: nutrients move pH, so pH is corrected last (RAT-004, RAT-046)
  if (!isNum(s.ecTarget)) {
    notes_.push_back(say("cycle.note.no_ec_target", {{"name", r.name}}));
  } else if (c.rt.latches.count("ec.no_effect")) {
    notes_.push_back(say("cycle.note.ec_latched"));
  } else if (ec < aim - s.ecTol) {
    if (ecRounds_ >= c.cfg.control.maxEcRounds) {
      notes_.push_back(say("cycle.note.ec_max", {{"n", ecRounds_}}));
    } else if (c.pumpJob) {
      wait(c, act, say("cycle.wait.pump_job"), kMinute);
      return;
    } else {
      startEc(c, act, s, ec, aim);
      return;
    }
  } else if (ec > s.ecTarget + s.ecTol) {
    // The robot cannot dilute: fresh water lowers EC (RAT-012)
    notes_.push_back(say("cycle.note.ec_above", {{"ec", ec}, {"target", s.ecTarget}}));
  }

  // pH: lowering only, never a counter-correction (RAT-041); only above the EC floor (RAT-046)
  if (!isNum(s.phTarget)) {
    notes_.push_back(say("cycle.note.no_ph_range", {{"name", r.name}}));
  } else if (!isNum(ph)) {
    notes_.push_back(say("cycle.note.ph_invalid", {{"reason", c.truth.get(Channel::Ph).reason}}));
  } else if (phHigh) {
    if (phLatched) {
      notes_.push_back(say("cycle.note.ph_latched"));
    } else if (!down) {
      notes_.push_back(say("cycle.note.no_ph_down"));
    } else if (ec < c.cfg.control.ecFloor) {
      notes_.push_back(say("cycle.note.ph_gate", {{"ec", ec}, {"floor", c.cfg.control.ecFloor}}));
    } else if (phDoses_ >= c.cfg.control.maxPhDoses) {
      notes_.push_back(say("cycle.note.ph_max", {{"n", phDoses_}}));
    } else if (lastEcDoseAt_ > 0 && c.epoch - lastEcDoseAt_ < kEcRestS) {
      // pH waits after an EC dose until the EC reading no longer holds samples from before it (RAT-046)
      evaluateAt_ = c.now + (kEcRestS - (c.epoch - lastEcDoseAt_)) * kSecond;
      enter(c, Phase::Measure);
      line_ = say("cycle.ph_rest", {{"left", mmss(evaluateAt_ - c.now)}});
      return;
    } else if (c.pumpJob) {
      wait(c, act, say("cycle.wait.pump_job"), kMinute);
      return;
    } else {
      startPh(c, act, s, ph);
      return;
    }
  } else if (ph < s.phTarget - s.phTol) {
    notes_.push_back(say("cycle.note.ph_below", {{"ph", ph}, {"target", s.phTarget}}));
  }
  holdNow(c, act);
}

void MixCycle::startEc(Ctx& c, Actuators& act, const CycleSettings& s, double ec, double aim) {
  const RecipeCfg& r = *s.recipe;
  const RecipeEffect eff = recipeEffect(r, learnedEcEffect(c.rt, r.id));
  // The first dose of a new tank is a small test dose (SD-105)
  const bool test = c.rt.newTank;
  auto plan = planEcDose(c.cfg, r, s.volumeL, aim - ec, eff.dose, eff.largest, c.cfg.control.maxEcStep, test ? kTestRiseEc : kNaN);
  if (!plan.ok) {
    wait(c, act, say("cycle.wait.ec_plan", {{"reason", plan.reason}}));
    return;
  }
  kind_ = Kind::Ec;
  queue_ = plan.steps;
  roundRecipe_ = r.id;
  before_ = measured_;
  mlRound_ = 0;
  volRound_ = s.volumeL;
  clean_ = true;
  doseFailed_ = false;
  limited_ = plan.limited;
  testDose_ = test;
  // At the effect the round is planned on, and at the best guess of the true one for the test (SD-105)
  expectedRise_ = eff.dose * plan.totalMl / s.volumeL;
  expectedTest_ = eff.expected * plan.totalMl / s.volumeL;
  ecRounds_++;
  anyDosed_ = true;
  enter(c, Phase::Dose);
  line_ = say(test ? "cycle.dose_ec_test" : "cycle.dose_ec",
              {{"from", ec}, {"aim", aim}, {"round", ecRounds_}, {"ml", plan.totalMl}, {"limited", plan.limited}});
}

void MixCycle::startPh(Ctx& c, Actuators& act, const CycleSettings& s, double ph) {
  const CanisterCfg* down = c.cfg.phDown();
  const double eff = clampEffect(c.rt.phEffect, kPhStartEffect);
  const bool test = c.rt.newTank;
  auto plan = planPhDose(ph, s.phTarget, eff, s.volumeL, c.cfg.control.phMaxMlPerDose, test ? kTestDropPh : kNaN);
  if (!plan.ok) {
    wait(c, act, say("cycle.wait.ph_plan", {{"reason", plan.reason}}));
    return;
  }
  DoseStep step;
  step.canister = down->id;
  step.name = down->name;
  step.pump = down->pump;
  step.color = down->color;
  step.ml = plan.ml;
  step.flowMlPerMin = c.cfg.pumpFlow(down->pump);
  Msg err;
  const Limits lim = c.cfg.limits.bounded();
  step.runs = splitRuns(plan.ml, step.flowMlPerMin, c.cfg.limits, lim.maxPartialRuns, err);
  if (!err.key.empty()) {
    if (err.key == "dose.too_small") {
      // A run under 1 s is not dosed: the robot rests visibly (RAT-050)
      notes_.push_back(say("cycle.note.ph_too_small", {{"ml", plan.ml}}));
      holdNow(c, act);
      return;
    }
    wait(c, act, say("cycle.wait.ph_plan", {{"reason", err}}));
    return;
  }
  kind_ = Kind::Ph;
  queue_ = {step};
  roundRecipe_ = s.recipe->id;
  before_ = measured_;
  mlRound_ = 0;
  volRound_ = s.volumeL;
  clean_ = true;
  doseFailed_ = false;
  limited_ = false;
  testDose_ = test;
  expectedTest_ = plan.ml / s.volumeL * eff;
  expectedRise_ = kNaN;
  phDoses_++;
  anyDosed_ = true;
  enter(c, Phase::Dose);
  line_ = say(test ? "cycle.dose_ph_test" : "cycle.dose_ph", {{"from", ph}, {"target", s.phTarget}, {"n", phDoses_}, {"ml", plan.ml}});
}

void MixCycle::learn(Ctx& c, Actuators& act) {
  const Kind kind = kind_;
  kind_ = Kind::None;
  const double v = volRound_;
  const double mlPerL = isNum(v) && v > 0 ? mlRound_ / v : kNaN;
  auto volumeLatch = [&](double q, const char* which, double observed) {
    const double implied = v / q;
    c.rt.latches["tank.volume"] = {{"at", c.epoch}, {"enteredL", v}, {"impliedL", std::round(implied)},
                                   {"kind", which}, {"recipe", roundRecipe_}, {"effect", numOrNull(observed)}};
    c.log.add(c.epoch, "alarm", "alarm", say("ev.cycle.volume"),
              say("ev.cycle.volume.text", {{"implied", std::round(implied)}, {"entered", v}}));
  };
  if (kind == Kind::Ec) {
    const double rise = measured_.ec - before_.ec;
    c.log.add(c.epoch, "control", "info", say("ev.cycle.ec_round", {{"n", ecRounds_}}),
              say("ev.cycle.ec_round.text", {{"from", before_.ec}, {"to", measured_.ec}, {"ml", mlRound_}}),
              {{"from", before_.ec}, {"to", measured_.ec}, {"ml", mlRound_}, {"test", testDose_}, {"clean", clean_}});
    const RecipeCfg* dosed = c.cfg.recipe(roundRecipe_);
    // The test dose: the rise against the one expected at the entered volume (SD-105). A rise
    // too small to tell is counted as "no effect" below and teaches nothing, so the next dose is a
    // test dose again: learned from it, the volume's error would hide in the effect.
    bool learnIt = true;
    if (testDose_) {
      learnIt = false;
      if (clean_ && isNum(expectedTest_) && expectedTest_ > 0 && rise >= kNoEffectEc) {
        const double q = rise / expectedTest_;
        if (q > kVolumeHigh || q < kVolumeLow) {
          volumeLatch(q, "ec", isNum(mlPerL) && mlPerL > 0 ? rise / mlPerL : kNaN);
          wait(c, act, say("cycle.wait.volume", {{"implied", std::round(v / q)}, {"entered", v}}), 0);
          return;
        }
        c.rt.newTank = false;
        learnIt = true;
        c.log.add(c.epoch, "control", "info", say("ev.cycle.test_ok"), say("ev.cycle.test_ok.text", {{"entered", v}, {"implied", std::round(v / q)}}));
      }
    }
    // A round made smaller for slow pumps that should raise EC by less than 0.06 says too little on
    // its own: clean ones of one recipe are judged and learned together (RAT-055)
    const bool tiny = limited_ && isNum(expectedRise_) && expectedRise_ < kSmallRoundEc;
    double judged = kNaN, judgedMlPerL = kNaN;
    if (small_.recipe != roundRecipe_ || (clean_ && !tiny)) small_ = {};
    if (clean_ && !tiny) {
      judged = rise;
      judgedMlPerL = mlPerL;
    } else if (clean_ && tiny) {
      small_.recipe = roundRecipe_;
      small_.rise += rise;
      small_.expected += expectedRise_;
      small_.mlPerL += isNum(mlPerL) ? mlPerL : 0;
      if (small_.expected >= kSmallRoundEc) {
        judged = small_.rise;
        judgedMlPerL = small_.mlPerL;
        small_ = {};
      }
    }
    // Learned for the recipe the round dosed, within the band (RAT-057)
    if (learnIt && dosed && isNum(judged) && isNum(judgedMlPerL) && judgedMlPerL > 0)
      c.rt.ecEffects[roundRecipe_] = clampEcEffect(judged / judgedMlPerL, recipeEffect(*dosed, kNaN).start);
    if (isNum(judged)) noEffectEc_ = judged < kNoEffectEc ? noEffectEc_ + 1 : 0;
    if (noEffectEc_ >= kNoEffectLatch) {
      c.rt.latches["ec.no_effect"] = {{"at", c.epoch}};
      noEffectEc_ = 0;
      small_ = {};
      c.log.add(c.epoch, "alarm", "alarm", say("ev.ec.no_effect"), say("ev.ec.no_effect.text"));
    }
  } else if (kind == Kind::Ph) {
    const double drop = before_.ph - measured_.ph;
    c.log.add(c.epoch, "control", "info", say("ev.cycle.ph_dose", {{"n", phDoses_}}),
              say("ev.cycle.ph_dose.text", {{"from", numOrNull(before_.ph)}, {"to", numOrNull(measured_.ph)}, {"ml", mlRound_}}),
              {{"from", numOrNull(before_.ph)}, {"to", numOrNull(measured_.ph)}, {"ml", mlRound_}, {"test", testDose_}, {"clean", clean_}});
    if (!isNum(drop)) {
      // pH not usable after the dose: nothing judged or learned
    } else if (clean_) {
      // The test dose checks one side only: pH− buffers differ widely, so a weaker fall is normal;
      // a fall far stronger than at the strongest measured effect means a smaller tank (SD-105)
      if (testDose_ && isNum(expectedTest_) && expectedTest_ > 0 && drop > kVolumeHigh * expectedTest_ && drop >= 0.05) {
        volumeLatch(drop / expectedTest_, "ph", isNum(mlPerL) && mlPerL > 0 ? drop / mlPerL : kNaN);
        wait(c, act, say("cycle.wait.volume", {{"implied", std::round(v * expectedTest_ / drop)}, {"entered", v}}), 0);
        return;
      }
      if (testDose_ && drop >= kNoEffectPh) {
        c.rt.newTank = false;
        c.log.add(c.epoch, "control", "info", say("ev.cycle.test_ok_ph"), say("ev.cycle.test_ok_ph.text", {{"entered", v}}));
      }
      // A dose without a measurable fall learns only towards the smaller dose (RAT-041)
      if (drop < kNoEffectPh) {
        c.rt.phEffect = std::max(clampEffect(c.rt.phEffect, kPhStartEffect), kPhStartEffect);
        noEffectPh_++;
      } else {
        if (isNum(mlPerL) && mlPerL > 0) c.rt.phEffect = clampEffect(drop / mlPerL, kPhStartEffect);
        noEffectPh_ = 0;
      }
      if (noEffectPh_ >= kNoEffectLatch) {
        c.rt.latches["ph.no_effect"] = {{"at", c.epoch}};
        noEffectPh_ = 0;
        c.log.add(c.epoch, "alarm", "alarm", say("ev.ph.no_effect"), say("ev.ph.no_effect.text"));
      }
    }
  }
  evaluate(c, act);
}

void MixCycle::tick(Ctx& c, Actuators& act, Doser& doser) {
  const Ms dt = lastTick_ < 0 ? 0 : std::clamp<Ms>(c.now - lastTick_, 0, 5 * kSecond);
  lastTick_ = c.now;
  if (c.stopped) {
    if (phase_ != Phase::Stopped) {
      enter(c, Phase::Stopped);
      line_ = say("cycle.stopped");
    }
    return;
  }
  const CycleSettings s = cycleSettings(c);
  // A probe in a buffer: its readings are not the tank's. The cycle waits until the calibration ends.
  if (c.probeCalibrating) {
    if (!(phase_ == Phase::Wait && line_.key == "cycle.wait.calibrating")) {
      restart(c, act, doser);
      wait(c, act, say("cycle.wait.calibrating"), 0);
    }
    return;
  }
  if (!s.off.key.empty()) {
    if (phase_ != Phase::Off) {
      restart(c, act, doser);  // a round going on is cut: what went in is mixed first later
      if (s.off.key == "cycle.off.ended")
        c.log.add(c.epoch, "control", "notice", say("ev.cycle.recipe_ended"), s.off);
    }
    line_ = s.off;
    if (s.circulation == "always") circulation(c, act, true);
    else if (s.circulation == "cycle") circulation(c, act, false);
    return;
  }
  if (phase_ == Phase::Off || phase_ == Phase::Stopped) enter(c, Phase::PumpOn);
  // The flow is averaged while it is proven: volume ÷ flow is the turnover (SD-105)
  const Reading& flow = c.truth.get(Channel::Flow);
  auto noteFlow = [&] {
    if (!flow.usable()) return;
    flowSum_ += *flow.value;
    flowN_++;
  };
  auto announceChamber = [&] {  // the chamber's water changes from standing to the tank's (RAT-042)
    for (Channel ch : {Channel::Ph, Channel::Ec, Channel::Temp}) c.truth.expectChange(ch, c.now + kAnnounceMs);
  };

  switch (phase_) {
    case Phase::PumpOn:
      announceChamber();
      circulation(c, act, true);
      enter(c, Phase::ProveFlow);
      line_ = say(s.circulation == "external" ? "cycle.wait_flow_external" : "cycle.pump_on");
      break;
    case Phase::ProveFlow:
      announceChamber();
      if (s.circulation == "always" || s.circulation == "cycle") circulation(c, act, true);
      if (c.flowOk) {
        noFlowLogged_ = false;
        if (flowSince_ == 0) flowSince_ = c.now;
        line_ = say("cycle.prove_flow", {{"flow", numOrNull(flow.value)}});
        if (c.now - flowSince_ >= kFlowProveMs) enter(c, Phase::Flush);
      } else {
        flowSince_ = 0;
        line_ = say(s.circulation == "external" ? "cycle.wait_flow_external" : "cycle.wait_flow");
        if (c.now - since_ > kFlowWaitMs) {
          const Msg why = say(s.circulation == "external" ? "cycle.wait.no_flow_external" : "cycle.wait.no_flow");
          if (!noFlowLogged_) {
            c.log.add(c.epoch, "alarm", "alarm", say("ev.cycle.no_flow"), why);
            noFlowLogged_ = true;
          }
          if (s.circulation != "external") circulation(c, act, false);  // a pump without flow may run dry
          wait(c, act, why);
        }
      }
      break;
    case Phase::Flush:
      announceChamber();
      if (!c.flowOk) {
        enter(c, Phase::ProveFlow);
        break;
      }
      flushedMs_ += dt;
      noteFlow();
      // The measuring chamber is flushed with tank water before every measurement: no decision on an old reading (RAT-043)
      line_ = say("cycle.flush", {{"left", mmss(kFlushMs - flushedMs_)}});
      if (flushedMs_ >= kFlushMs) {
        if (flowN_ > 0) {
          c.rt.flowLpm = flowSum_ / flowN_;
          c.rt.flowAt = c.epoch;
        }
        if (kind_ != Kind::None) {
          enter(c, Phase::Remeasure);  // flow came back during a round: measure its result
          line_ = say("cycle.remeasure", {{"left", mmss(kStableWindowMs)}});
        } else if (mixPendingMs_ > 0) {
          // Doses not mixed in yet (a restart, a Stop): mix first, learn nothing (SD-106)
          mixingFirst_ = true;
          mixTargetMs_ = mixPendingMs_;
          mixPendingMs_ = 0;
          mixedMs_ = 0;
          enter(c, Phase::Mix);
          line_ = say("cycle.mix_first", {{"left", mmss(mixTargetMs_)}});
        } else {
          enter(c, Phase::Measure);
          line_ = say("cycle.measure", {{"left", mmss(kStableWindowMs)}});
        }
      }
      break;
    case Phase::Measure:
    case Phase::Remeasure:
      if (!c.flowOk) {
        enter(c, Phase::ProveFlow);
        break;
      }
      if (measuring(c, act)) {
        if (phase_ == Phase::Remeasure) learn(c, act);
        else if (phase_ == Phase::Measure) evaluate(c, act);
      }
      break;
    case Phase::Dose: {
      if (kind_ == Kind::Ec) {
        // An EC round moves EC and pH: announced while it doses and mixes (RAT-042)
        c.truth.expectChange(Channel::Ec, c.now + kAnnounceMs);
        c.truth.expectChange(Channel::Ph, c.now + kAnnounceMs);
      }
      if (doser.busy()) break;
      if (!queue_.empty() && !doseFailed_) {
        DoseOrder o;
        o.id = std::string(kind_ == Kind::Ec ? "ec-" : "ph-") + std::to_string(++seq_);
        o.purpose = kind_ == Kind::Ec ? "ec" : "ph";
        o.step = queue_.front();
        Msg err;
        if (doser.start(c, act, o, err)) {
          queue_.erase(queue_.begin());
          break;
        }
        doseFailed_ = true;
        clean_ = false;
        notes_.push_back(say("cycle.note.dose_failed", {{"reason", err}}));
      }
      if (mlRound_ > 0) {
        mixedMs_ = 0;
        const double m = kind_ == Kind::Ec ? s.waitEcMin : s.waitPhMin;
        mixTargetMs_ = minutes(isNum(m) ? m : kWaitMinMin);
        enter(c, Phase::Mix);
        line_ = say("cycle.mix", {{"left", mmss(mixTargetMs_)}});
      } else {
        kind_ = Kind::None;
        wait(c, act, notes_.empty() ? say("cycle.wait.nothing_dosed") : notes_.back());
      }
      break;
    }
    case Phase::Mix: {
      if (kind_ == Kind::Ec || mixingFirst_) {
        c.truth.expectChange(Channel::Ec, c.now + kAnnounceMs);
        c.truth.expectChange(Channel::Ph, c.now + kAnnounceMs);
      }
      // The mixing clock runs only with flow (SD-105)
      if (c.flowOk) {
        mixedMs_ += dt;
        noFlowSince_ = 0;
      } else {
        if (noFlowSince_ == 0) noFlowSince_ = c.now;
        circulation(c, act, true);
        if (c.now - noFlowSince_ > kNoFlowMixMaxMs) {
          // What went in still needs mixing: the next cycle mixes first, the round learns nothing
          mixPendingMs_ = std::max(mixPendingMs_, mixTargetMs_);
          kind_ = Kind::None;
          mixingFirst_ = false;
          const Msg why = say("cycle.wait.no_flow");
          c.log.add(c.epoch, "alarm", "alarm", say("ev.cycle.no_flow"), why);
          if (s.circulation != "external") circulation(c, act, false);
          wait(c, act, why);
          break;
        }
      }
      line_ = say(c.flowOk ? (mixingFirst_ ? "cycle.mix_first" : "cycle.mix") : "cycle.mix_paused",
                  {{"left", mmss(mixTargetMs_ - mixedMs_)}});
      if (mixedMs_ >= mixTargetMs_) {
        if (mixingFirst_) {
          mixingFirst_ = false;
          enter(c, Phase::Measure);
        } else {
          enter(c, Phase::Remeasure);
        }
      }
      break;
    }
    case Phase::Hold:
      if (s.circulation == "always") circulation(c, act, true);
      else if (s.circulation == "cycle") circulation(c, act, false);
      if (c.now >= retryAt_) enter(c, Phase::PumpOn);
      break;
    case Phase::Wait:
      if (s.circulation == "always") circulation(c, act, true);
      if (retryAt_ > 0 && c.now >= retryAt_) enter(c, Phase::PumpOn);
      break;
    case Phase::Off:
    case Phase::Stopped:
      break;
  }
}

json MixCycle::status(const Ctx& c) const {
  const CycleSettings s = cycleSettings(c);
  json j = {{"phase", phaseName(phase_)}, {"since", sinceEpoch_}, {"line", line_}, {"notes", notes_}};
  json progress = nullptr;
  auto secs = [](Ms ms) { return std::max<Ms>(0, ms) / 1000; };
  if (phase_ == Phase::Flush) progress = {{"doneS", secs(flushedMs_)}, {"totalS", secs(kFlushMs)}};
  if (phase_ == Phase::Mix) progress = {{"doneS", secs(mixedMs_)}, {"totalS", secs(mixTargetMs_)}, {"paused", !c.flowOk}, {"first", mixingFirst_}};
  if ((phase_ == Phase::Hold || phase_ == Phase::Wait) && retryAt_ > 0) progress = {{"nextAt", c.epoch + secs(retryAt_ - c.now)}};
  j["progress"] = progress;
  if (kind_ != Kind::None)
    j["round"] = {{"kind", kind_ == Kind::Ec ? "ec" : "ph"}, {"n", kind_ == Kind::Ec ? ecRounds_ : phDoses_},
                  {"test", testDose_}, {"ml", mlRound_}, {"from", numOrNull(kind_ == Kind::Ec ? before_.ec : before_.ph)}};
  else
    j["round"] = nullptr;
  j["last"] = last_.at > 0 ? json{{"ph", numOrNull(last_.ph)}, {"ec", numOrNull(last_.ec)}, {"temp", numOrNull(last_.temp)}, {"at", last_.at}}
                           : json(nullptr);
  j["mixPending"] = mixPendingMs_ > 0;
  j["settings"] = {{"recipe", s.recipe ? json(s.recipe->id) : json(nullptr)},
                   {"ecTarget", numOrNull(s.ecTarget)},
                   {"ecTolerance", numOrNull(s.ecTol)},
                   {"phTarget", numOrNull(s.phTarget)},
                   {"phTolerance", numOrNull(s.phTol)},
                   {"volumeL", numOrNull(s.volumeL)},
                   {"flowLpm", numOrNull(s.flowLpm)},
                   {"turnoverMin", numOrNull(s.turnoverMin)},
                   {"waitEcMin", numOrNull(s.waitEcMin)},
                   {"waitPhMin", numOrNull(s.waitPhMin)},
                   {"suggestEcMin", numOrNull(s.suggestEcMin)},
                   {"suggestPhMin", numOrNull(s.suggestPhMin)},
                   {"holdMin", s.holdMs / kMinute},
                   {"circulation", s.circulation}};
  return j;
}

}  // namespace doseph
