// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/cycle.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/messages.hpp"
#include "doseph/plants.hpp"

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
constexpr double kPhReserveEffect = kPhStartEffect / 2;  // the pH− effect the reserve assumes until one is learned (SD-121)
constexpr Ms kAnnounceMs = kMinute;     // an announced change explains jumps this much longer (RAT-042)

Ms minutes(double m) { return static_cast<Ms>(std::llround(m * 60.0)) * kSecond; }
std::string mmss(Ms ms) {
  Ms s = std::max<Ms>(0, (ms + 999) / 1000);
  std::string sec = std::to_string(s % 60);
  return std::to_string(s / 60) + ":" + (sec.size() < 2 ? "0" + sec : sec);
}

// A dose needs at least its amount in the canister where the level is known; an unknown level is
// no reason to hold (SD-119)
// Why a dose cannot start now: its channel is locked (SD-111), it would pass the channel's daily
// amount (SD-113), or its canister holds too little (SD-119)
std::optional<Msg> shortOf(const Ctx& c, const Actuators& act, const DoseStep& d) {
  const RuntimeState& rt = c.rt;
  if (channelLocked(rt, d.channel)) return say("cycle.wait.channel_locked", {{"name", d.name}, {"channel", d.channel}});
  Ms total = 0;
  for (Ms r : d.runs) total += r;
  if (auto why = act.dailyCap(c, d.channel, total)) return say("cycle.wait.daily", {{"name", d.name}, {"reason", *why}});
  auto it = rt.stockMl.find(d.canister);
  if (it == rt.stockMl.end() || !isNum(it->second) || !isNum(d.ml) || it->second >= d.ml) return std::nullopt;
  return say(it->second <= kStockBelowZeroMl ? "cycle.wait.stock_empty" : "cycle.wait.stock",
             {{"name", d.name}, {"need", d.ml}, {"have", it->second}});
}

}  // namespace

CycleSettings cycleSettings(const Ctx& c) {
  CycleSettings s;
  const auto& cfg = c.cfg;
  s.volumeL = cfg.tank.volumeL;
  s.holdMs = minutes(isNum(cfg.tank.holdMin) ? std::clamp(cfg.tank.holdMin, kHoldMinMin, kHoldMaxMin) : 60.0);
  s.flowLpm = c.rt.flowLpm;
  if (isNum(s.volumeL) && isNum(s.flowLpm) && s.flowLpm > 0) s.turnoverMin = s.volumeL / s.flowLpm;
  // The mixing wait (SD-112): one turnover, at least 2 min, then stable; the user may only lengthen it
  s.mixOwnMin = suggestedWaitMin(s.volumeL, s.flowLpm, 1.0);
  s.mixUserMin = isNum(cfg.tank.mixMinMin) ? std::clamp(cfg.tank.mixMinMin, 0.0, kWaitMaxMin) : kNaN;
  s.mixMin = isNum(s.mixOwnMin) ? s.mixOwnMin : kWaitMinMin;
  s.mixWhy = isNum(s.mixOwnMin) ? "turnover" : "minimum";
  // A large tank mixes by the customer's pump: its own wait is the settling learned from its doses,
  // 60 min until one is learned (SD-112)
  if (isNum(s.volumeL) && s.volumeL > kLargeTankL) {
    const double learned = learnedSettleMin(c.rt.settleMin);
    s.mixOwnMin = isNum(learned) ? learned : kSettleDefaultMin;
    s.mixWhy = isNum(learned) ? "settling" : "settling_default";
    s.mixMin = s.mixOwnMin;
  }
  if (isNum(s.mixUserMin) && s.mixUserMin > s.mixMin) {
    s.mixMin = s.mixUserMin;
    s.mixWhy = "user";
  }
  if (isNum(s.turnoverMin))
    s.stableMs = std::clamp<Ms>(minutes(s.turnoverMin / 5), kStableWindowMs, kStableWindowMaxMs);
  s.ecTol = std::max(0.05, isNum(cfg.control.ecTolerance) ? cfg.control.ecTolerance : 0.1);  // never 0 (RAT-008)
  s.phTol = std::max(0.05, isNum(cfg.control.phTolerance) ? cfg.control.phTolerance : 0.15);
  // The water plan: the leading plant's phase, else the recipe chosen by hand (SD-128)
  const WaterPlan w = waterPlan(cfg, c.epoch);
  if (!placementConfirmed(cfg)) {
    s.off = say("cycle.off.placement");  // where pump and canisters stand is confirmed first (SD-118)
  } else if (!w.recipe) {
    s.off = say("cycle.off.no_recipe");
  } else if (w.ended) {
    s.off = say("cycle.off.ended", {{"name", w.recipe->name}, {"until", w.until}});
  } else if (!isNum(s.volumeL) || s.volumeL <= 0) {
    s.off = say("cycle.off.no_volume");
  } else {
    s.recipe = w.recipe;
    s.ecTarget = w.ecTarget;
    s.phTarget = w.phTarget;
  }
  return s;
}

void MixCycle::enter(const Ctx& c, Phase p) {
  if (p != Phase::Hold) cable_ = {};
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
    default:
      break;
  }
}

// The device's own feed pump runs for a check and is off in Hold, Wait and Off (SD-109)
void MixCycle::feedPump(Ctx& c, Actuators& act, bool on) {
  auto st = act.feedPump();
  // After a cut for no flow the hardware holds the pump off until it is switched off once more
  if (st && *st == on && !(c.pumpCut && !on)) return;
  if (on) {
    flowSeen_ = c.now;  // the window without flow starts with the pump
    flowedThisRun_ = false;
    lowAmpsSince_ = 0;
  }
  Msg err;
  if (!act.setFeedPump(c, on, err) && on) line_ = say("cycle.pump_failed", {{"reason", err}});
}

void MixCycle::noFlow(Ctx& c, Actuators& act, Msg why) {
  if (!noFlowLogged_) {
    c.log.add(c.epoch, "alarm", "alarm", say("ev.cycle.no_flow"), why);
    noFlowLogged_ = true;
  }
  const int tries = c.pumpInTank ? kDryTriesTank : kDryTries;
  if (++noFlowTries_ >= tries) {
    noFlowTries_ = 0;
    c.rt.latches["flow.dry"] = {{"at", c.epoch}, {"tries", tries}};
    c.log.add(c.epoch, "alarm", "alarm", say("ev.flow.dry"), say("ev.flow.dry.text", {{"tries", tries}}));
    wait(c, act, say("cycle.wait.dry"), 0);
  } else {
    wait(c, act, std::move(why));  // the pump off: without flow it may run dry, or spill
  }
}

void MixCycle::wait(Ctx& c, Actuators& act, Msg why, Ms retry) {
  enter(c, Phase::Wait);
  line_ = std::move(why);
  retryAt_ = retry > 0 ? c.now + retry : 0;
  feedPump(c, act, false);
}

void MixCycle::holdNow(Ctx& c, Actuators& act) {
  enter(c, Phase::Hold);
  // The last 10 s with the pump running, to compare once it stands (SD-127)
  cable_ = {};
  for (const auto& w : window_) {
    if (w.at < c.now - kCableMs) continue;
    if (isNum(w.ph)) {
      cable_.phSum += w.ph;
      cable_.phN++;
    }
    if (isNum(w.ec)) {
      cable_.ecSum += w.ec;
      cable_.ecN++;
    }
  }
  if (cable_.phN > 0 || cable_.ecN > 0) {
    cable_.stage = 1;
    cable_.at = c.now;
    cable_.phOn = cable_.phN ? cable_.phSum / cable_.phN : kNaN;
    cable_.ecOn = cable_.ecN ? cable_.ecSum / cable_.ecN : kNaN;
    cable_.phSum = cable_.ecSum = 0;
    cable_.phN = cable_.ecN = 0;
  }
  feedPump(c, act, false);
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
}

bool MixCycle::cableCheck(Ctx& c, Actuators& act) {
  if (cable_.stage == 0) return false;
  const Ms t = c.now - cable_.at;
  const Reading& ph = c.truth.get(Signal::Ph);
  const Reading& ec = c.truth.get(Signal::Ec);
  const bool sampling = t >= kCableMs && t <= 2 * kCableMs;
  if (sampling) {
    if (ph.usable()) {
      cable_.phSum += *ph.value;
      cable_.phN++;
    }
    if (ec.usable()) {
      cable_.ecSum += *ec.value;
      cable_.ecN++;
    }
  }
  if (cable_.stage == 2) {
    // Running again: the chamber takes tank water, and its readings of 10–20 s count
    feedPump(c, act, true);
    if (!c.flowOk && c.now - flowSeen_ > kFlowStartMs) {
      cable_ = {};  // no flow: the comparison says nothing; the next check tries again
      return false;
    }
    if (t <= 2 * kCableMs) return true;
    cable_.phOn = cable_.phN ? cable_.phSum / cable_.phN : kNaN;
    cable_.ecOn = cable_.ecN ? cable_.ecSum / cable_.ecN : kNaN;
    cable_.phSum = cable_.ecSum = 0;
    cable_.phN = cable_.ecN = 0;
    cable_.stage = 3;
    cable_.at = c.now;
    feedPump(c, act, false);
    return true;
  }
  // Standing: its readings of 10–20 s after the stop against those running
  feedPump(c, act, false);
  if (t <= 2 * kCableMs) return true;
  const double phOff = cable_.phN ? cable_.phSum / cable_.phN : kNaN;
  const double ecOff = cable_.ecN ? cable_.ecSum / cable_.ecN : kNaN;
  const bool shifted = (isNum(phOff) && isNum(cable_.phOn) && std::fabs(phOff - cable_.phOn) > kCablePh) ||
                       (isNum(ecOff) && isNum(cable_.ecOn) && std::fabs(ecOff - cable_.ecOn) > kCableEc);
  if (!shifted) {
    cable_ = {};
    return false;
  }
  if (cable_.stage == 1) {
    // Once more at once, so that one stray reading never locks the pump
    const Cable again{2, c.now};
    cable_ = again;
    feedPump(c, act, true);
    return true;
  }
  const json args = {{"phOn", numOrNull(cable_.phOn)}, {"phOff", numOrNull(phOff)}, {"ecOn", numOrNull(cable_.ecOn)}, {"ecOff", numOrNull(ecOff)}};
  json latch = args;
  latch["at"] = c.epoch;
  c.rt.latches["pump.cable"] = latch;
  c.log.add(c.epoch, "alarm", "alarm", say("ev.pump.cable"), say("ev.pump.cable.text", args), args);
  cable_ = {};
  wait(c, act, say("cycle.wait.cable"), 0);
  return true;
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
    mixFirst(minutes(s.mixMin));
  }
  kind_ = Kind::None;
  queue_.clear();
  clean_ = false;
  mixingFirst_ = false;
  settlePairs(c);  // the round is over; a part still dosing is settled when its result comes (SD-114)
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
    line_ = say("cycle.pump_on");
  }
}

void MixCycle::doseFinished(Ctx& c, const DoseProgress& p) {
  // A pair part's amount, also for a round that ended meanwhile (SD-114)
  json& pr = c.rt.pairRound;
  if (pr.is_object() && jstr(pr, "order") == p.orderId) {
    json& part = pr["parts"][jstr(pr, "canister")];
    part["done"] = jnum(part, "done", 0) + p.mlDone;
    pr["order"] = "";
    if (kind_ == Kind::None) settlePairs(c);  // its round was ended meanwhile
  }
  if (kind_ == Kind::None) return;  // a round ended meanwhile: its journal makes the next cycle mix first
  mlRound_ += p.mlDone;
  if (kind_ == Kind::Ec && p.msDone > 0) lastEcDoseAt_ = c.epoch;
  // Nutrients went in, or pH−, which adds a little EC too (RAT-053): a rise is theirs (SD-115)
  if (p.msDone > 0) c.rt.ecInBand = nullptr;
  if (p.state != DoseProgress::State::Done) {
    clean_ = false;  // a cut dose says nothing about the effect (RAT-057)
    doseFailed_ = true;
    notes_.push_back(say("cycle.note.dose_failed", {{"reason", p.error}}));
  }
}

bool MixCycle::measuring(Ctx& c, Actuators& act) {
  const Reading& ph = c.truth.get(Signal::Ph);
  const Reading& ec = c.truth.get(Signal::Ec);
  const Reading& t = c.truth.get(Signal::Temp);
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
  // Steady over 60 s or a fifth of a turnover, at most 10 min (SD-112)
  const Ms stable = cycleSettings(c).stableMs;
  window_.push_back({c.now, ph.usable() ? *ph.value : kNaN, *ec.value, t.usable() ? *t.value : kNaN});
  while (window_.size() >= 2 && window_[1].at <= c.now - stable) window_.pop_front();
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
  if (spanned < stable || !steady) {
    if (c.now - since_ > stable + kMeasureMaxMs) {
      c.log.add(c.epoch, "control", "warn", say("ev.cycle.unsteady"),
                say("ev.cycle.unsteady.text", {{"ec", ecMax - ecMin}, {"ph", phAll ? json(phMax - phMin) : json(nullptr)}}));
      wait(c, act, say("cycle.wait.unsteady"));
      return false;
    }
    line_ = say(phase_ == Phase::Remeasure ? "cycle.remeasure" : "cycle.measure",
                {{"left", mmss(std::max<Ms>(0, stable - spanned))}, {"moving", !steady}});
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

// EC that rose above its band with no dose in since a check found it within means water went missing
// (SD-115): "top up water" is raised once, and goes when EC is back within the band. pH− counts as a
// dose too: it adds a little EC (RAT-053).
void MixCycle::watchWater(Ctx& c, const RecipeCfg& r, double ec, const CycleSettings& s) {
  if (ec >= s.ecTarget - s.ecTol && ec <= s.ecTarget + s.ecTol) {
    c.rt.ecInBand = {{"recipe", r.id}, {"target", s.ecTarget}};
    if (c.rt.topUp) {
      c.rt.topUp = false;
      c.log.add(c.epoch, "control", "info", say("ev.water.ok"), say("ev.water.ok.text", {{"ec", ec}}));
    }
    return;
  }
  const json& b = c.rt.ecInBand;
  const bool held = b.is_object() && jstr(b, "recipe") == r.id && jnum(b, "target") == s.ecTarget;
  if (ec > s.ecTarget + s.ecTol && held && !c.rt.topUp) {
    c.rt.topUp = true;
    c.log.add(c.epoch, "control", "warn", say("ev.water.top_up"), say("ev.water.top_up.text", {{"ec", ec}, {"target", s.ecTarget}}));
  }
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
  if (isNum(s.ecTarget) && isNum(ec)) watchWater(c, r, ec, s);
  const CanisterCfg* down = c.cfg.phDown();
  const bool phLatched = c.rt.latches.count("ph.no_effect") > 0;
  const bool phHigh = isNum(s.phTarget) && isNum(ph) && ph > s.phTarget + s.phTol;
  // EC aims below its target by what the pH− dose that follows adds (RAT-053). Until the pH− effect
  // is learned it is planned at half the start effect: too little EC is topped up once pH is in its
  // range, too much cannot be taken back. Never below what lets pH be dosed at all (RAT-046, SD-121)
  double aim = s.ecTarget;
  if (isNum(aim) && phHigh && down && !phLatched) {
    const double eff = isNum(c.rt.phEffect) ? clampEffect(c.rt.phEffect, kPhStartEffect) : kPhReserveEffect;
    aim = std::max(aim - (ph - s.phTarget) / eff * kEcFromPhDown, std::min(aim, c.cfg.control.ecFloor + s.ecTol));
  }

  // EC first: nutrients move pH, so pH is corrected last (RAT-004, RAT-046)
  if (!isNum(s.ecTarget)) {
    notes_.push_back(say("cycle.note.no_ec_target", {{"name", r.name}}));
  } else if (c.rt.latches.count("ec.no_effect")) {
    notes_.push_back(say("cycle.note.ec_latched"));
  } else if (ec < aim - s.ecTol) {
    if (ecRounds_ >= c.cfg.control.maxEcRounds) {
      notes_.push_back(say("cycle.note.ec_max", {{"n", ecRounds_}}));
    } else if (startEc(c, act, s, ec, aim)) {
      return;
    }
  } else if (ec > s.ecTarget + s.ecTol) {
    // The robot cannot dilute: fresh water lowers EC (RAT-012)
    notes_.push_back(say("cycle.note.ec_above", {{"ec", ec}, {"target", s.ecTarget}}));
    if (c.rt.topUp) notes_.push_back(say("cycle.note.top_up"));
  }

  // pH: lowering only, never a counter-correction (RAT-041); only above the EC floor (RAT-046)
  if (!isNum(s.phTarget)) {
    notes_.push_back(say("cycle.note.no_ph_range", {{"name", r.name}}));
  } else if (!isNum(ph)) {
    notes_.push_back(say("cycle.note.ph_invalid", {{"reason", c.truth.get(Signal::Ph).reason}}));
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
    } else {
      startPh(c, act, s, ph);
      return;
    }
  } else if (ph < s.phTarget - s.phTol) {
    notes_.push_back(say("cycle.note.ph_below", {{"ph", ph}, {"target", s.phTarget}}));
  }
  holdNow(c, act);
}

bool MixCycle::startEc(Ctx& c, Actuators& act, const CycleSettings& s, double ec, double aim) {
  const RecipeCfg& r = *s.recipe;
  const RecipeEffect eff = recipeEffect(r, learnedEcEffect(c.rt, r.id));
  // The first dose of a new tank is a small test dose (SD-105)
  // A pair that misses a part from a round cut short gets it first, alone (SD-114)
  settlePairs(c);
  if (c.rt.catchUp.is_object() && startCatchUp(c, act, s, ec)) return true;
  const bool test = c.rt.newTank;
  auto plan = planEcDose(c.cfg, r, s.volumeL, aim - ec, eff.dose, eff.largest, c.cfg.control.maxEcStep, test ? kTestRiseEc : kNaN);
  if (!plan.ok) {
    if (plan.tooSmall) {
      // A round under 1 s for a part is not dosed: the robot goes on to pH and rests visibly (RAT-050, SD-122)
      notes_.push_back(say("cycle.note.ec_too_small", {{"reason", plan.reason}}));
      return false;
    }
    wait(c, act, say("cycle.wait.ec_plan", {{"reason", plan.reason}}));
    return true;
  }
  for (const auto& st : plan.steps)
    if (auto why = shortOf(c, act, st)) {
      wait(c, act, *why);
      return true;
    }
  recordPairs(c, r.id, plan.steps, false);
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
  return true;
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
  step.channel = down->channel;
  step.color = down->color;
  step.ml = plan.ml;
  step.flowMlPerMin = c.cfg.channelFlow(down->channel);
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
  if (auto why = shortOf(c, act, step)) {
    wait(c, act, *why);
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

// ---------------------------------------------------------------- pairs (SD-114)

namespace {

// "Part A, Part B" and the sum of their amounts
std::pair<std::string, double> namesOf(const Config& cfg, const json& parts) {
  std::string names;
  double total = 0;
  for (const auto& [id, ml] : parts.items()) {
    const CanisterCfg* b = cfg.canister(id);
    names += (names.empty() ? "" : ", ") + (b ? b->name : id);
    total += ml.is_number() ? ml.get<double>() : 0;
  }
  return {names, total};
}

}  // namespace

void dropCatchUp(Ctx& c, const Msg& why) {
  if (!c.rt.catchUp.is_object()) return;
  const json parts = c.rt.catchUp.value("parts", json::object());
  const auto [names, total] = namesOf(c.cfg, parts);
  c.rt.catchUp = nullptr;
  c.log.add(c.epoch, "dose", "notice", say("ev.cycle.catchup_dropped", {{"names", names}}),
            say("ev.cycle.catchup_dropped.text", {{"names", names}, {"ml", total}, {"reason", why}}), {{"parts", parts}});
}

void pairAfterRestart(RuntimeState& rt, const std::vector<JournalRun>& runs) {
  json& pr = rt.pairRound;
  if (!pr.is_object() || jstr(pr, "order").empty()) return;
  double ml = 0;
  for (const auto& r : runs)
    if (r.order == jstr(pr, "order")) ml += r.cut || !isNum(r.mlDone) ? (isNum(r.ml) ? r.ml : 0) : r.mlDone;
  json& part = pr["parts"][jstr(pr, "canister")];
  part["done"] = jnum(part, "done", 0) + ml;
  pr["order"] = "";
}

void MixCycle::recordPairs(Ctx& c, const std::string& recipe, const std::vector<DoseStep>& steps, bool catchUp) {
  json parts = json::object();
  for (const auto& st : steps)
    if (!st.pair.empty()) parts[st.canister] = {{"planned", st.ml}, {"done", 0.0}};
  c.rt.pairRound = parts.empty() ? json(nullptr)
                                 : json{{"recipe", recipe}, {"order", ""}, {"canister", ""}, {"catchUp", catchUp}, {"parts", parts}};
}

// Once a round's parts are all done or given up: each part of a pair is brought to the share of
// its plan the furthest part reached, so the pair keeps its ratio (RAT-054); a catch-up round
// owes its whole amounts. A part still dosing is settled when it ends.
void MixCycle::settlePairs(Ctx& c) {
  json& pr = c.rt.pairRound;
  if (!pr.is_object() || !jstr(pr, "order").empty()) return;
  const bool catchUp = jbool(pr, "catchUp", false);
  const json parts = pr.value("parts", json::object());
  double top = 0;
  for (const auto& [id, p] : parts.items()) {
    const double planned = jnum(p, "planned"), done = jnum(p, "done", 0);
    if (isNum(planned) && planned > 0) top = std::max(top, done / planned);
  }
  json missing = json::object();
  for (const auto& [id, p] : parts.items()) {
    const double planned = jnum(p, "planned"), done = jnum(p, "done", 0);
    if (!isNum(planned) || planned <= 0) continue;
    const double m = (catchUp ? planned : std::min(1.0, top) * planned) - done;
    if (m >= kCatchUpMinMl) missing[id] = m;
  }
  const std::string recipe = jstr(pr, "recipe");
  pr = nullptr;
  if (missing.empty()) return;
  c.rt.catchUp = {{"recipe", recipe}, {"parts", missing}};
  const auto [names, total] = namesOf(c.cfg, missing);
  c.log.add(c.epoch, "dose", "warn", say("ev.cycle.pair_short", {{"names", names}}),
            say("ev.cycle.pair_short.text", {{"names", names}, {"ml", total}}), {{"recipe", recipe}, {"parts", missing}});
}

// The missing parts alone, in the recipe's order; the round learns nothing (RAT-057). A part that
// cannot be dosed holds the others of its pair. False when the catch-up no longer fits and was dropped.
bool MixCycle::startCatchUp(Ctx& c, Actuators& act, const CycleSettings& s, double ec) {
  const RecipeCfg& r = *s.recipe;
  const json parts = c.rt.catchUp.value("parts", json::object());
  std::vector<DoseStep> steps;
  bool fits = jstr(c.rt.catchUp, "recipe") == r.id && !parts.empty();
  for (const auto& rs : r.steps) {
    if (!fits || !parts.contains(rs.canister)) continue;
    const CanisterCfg* b = c.cfg.canister(rs.canister);
    if (!b || rs.fixed || b->pair.empty() || !parts[rs.canister].is_number()) {
      fits = false;
      break;
    }
    DoseStep d;
    d.canister = b->id;
    d.name = b->name;
    d.channel = b->channel;
    d.pair = b->pair;
    d.color = b->color;
    d.mlPerL = rs.mlPerL;
    d.ml = parts[rs.canister].get<double>();
    d.flowMlPerMin = c.cfg.channelFlow(b->channel);
    steps.push_back(d);
  }
  if (fits && steps.size() != parts.size()) fits = false;  // a part the recipe no longer has
  if (!fits) {
    dropCatchUp(c, say("catchup.recipe_changed"));
    return false;
  }
  const Limits lim = c.cfg.limits.bounded();
  double total = 0;
  for (auto& d : steps) {
    if (d.channel <= 0 || !isNum(d.flowMlPerMin)) {
      wait(c, act, say("cycle.wait.catchup_part", {{"name", d.name}}));
      return true;
    }
    Msg err;
    d.runs = splitRuns(d.ml, d.flowMlPerMin, c.cfg.limits, lim.maxPartialRuns, err);
    if (!err.key.empty()) {
      dropCatchUp(c, err);
      return false;
    }
    if (auto why = shortOf(c, act, d)) {
      wait(c, act, *why);
      return true;
    }
    total += d.ml;
  }
  c.rt.catchUp = nullptr;
  recordPairs(c, r.id, steps, true);
  kind_ = Kind::Ec;
  queue_ = steps;
  roundRecipe_ = r.id;
  before_ = measured_;
  mlRound_ = 0;
  volRound_ = s.volumeL;
  clean_ = false;
  doseFailed_ = false;
  limited_ = false;
  testDose_ = false;
  expectedRise_ = expectedTest_ = kNaN;
  ecRounds_++;
  anyDosed_ = true;
  enter(c, Phase::Dose);
  line_ = say("cycle.dose_catchup", {{"from", ec}, {"ml", total}, {"names", namesOf(c.cfg, parts).first}});
  return true;
}

void MixCycle::learn(Ctx& c, Actuators& act) {
  const Kind kind = kind_;
  kind_ = Kind::None;
  // A clean round's settling time is read from the history by the robot (SD-112, RAT-057)
  if (clean_ && doseEndAt_ > 0 && (kind == Kind::Ec || kind == Kind::Ph))
    settled_ = Settled{kind == Kind::Ec ? "ec" : "ph", doseEndAt_, kind == Kind::Ec ? measured_.ec : measured_.ph};
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
    settlePairs(c);
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
  // The hardware release holds every pump (SD-110): a dose going on is cut, what went in is mixed
  // first later; the cycle waits with the reason until the release is back
  if (!c.hold.empty()) {
    const std::string key = "cycle.wait.hold." + c.hold;
    if (!(phase_ == Phase::Wait && line_.key == key)) {
      restart(c, act, doser);
      wait(c, act, say(key), 0);
    }
    feedPump(c, act, false);
    return;
  }
  const CycleSettings s = cycleSettings(c);
  // A probe in a buffer, or a channel calibrated or its line filled: the check and the feed pump
  // stop first and start anew once it ends (SD-109)
  if (c.probeCalibrating || c.channelJob) {
    const char* key = c.probeCalibrating ? "cycle.wait.calibrating" : "cycle.wait.channel_job";
    if (!(phase_ == Phase::Wait && line_.key == key)) {
      restart(c, act, doser);
      wait(c, act, say(key), 0);
    }
    return;
  }
  // Maintenance: someone works at tank or device. The cycle holds and the feed pump is off until it ends.
  if (c.maintenanceUntil > 0) {
    if (!(phase_ == Phase::Wait && line_.key == "cycle.wait.maintenance")) {
      restart(c, act, doser);
      wait(c, act, say("cycle.wait.maintenance", {{"until", c.maintenanceUntil}}), 0);
    }
    feedPump(c, act, false);
    return;
  }
  if (!s.off.key.empty()) {
    if (phase_ != Phase::Off) {
      restart(c, act, doser);  // a round going on is cut: what went in is mixed first later
      if (s.off.key == "cycle.off.ended")
        c.log.add(c.epoch, "control", "notice", say("ev.cycle.recipe_ended"), s.off);
    }
    line_ = s.off;
    feedPump(c, act, false);
    return;
  }
  // Tries without flow in a row: the feed pump may run dry; a person releases it (SD-109)
  if (c.rt.latches.count("flow.dry")) {
    if (!(phase_ == Phase::Wait && line_.key == "cycle.wait.dry")) {
      restart(c, act, doser);
      wait(c, act, say("cycle.wait.dry"), 0);
    }
    return;
  }
  // pH or EC shifted while the feed pump ran: its cable may leak into the water; the pump and
  // dosing stay off until a person releases it (SD-127)
  if (c.rt.latches.count("pump.cable")) {
    if (!(phase_ == Phase::Wait && line_.key == "cycle.wait.cable")) {
      restart(c, act, doser);
      wait(c, act, say("cycle.wait.cable"), 0);
    }
    return;
  }
  if (phase_ == Phase::Off || phase_ == Phase::Stopped) enter(c, Phase::PumpOn);
  // The flow is averaged while it is proven: volume ÷ flow is the turnover (SD-105)
  const Reading& flow = c.truth.get(Signal::Flow);
  auto noteFlow = [&] {
    if (!flow.usable()) return;
    flowSum_ += *flow.value;
    flowN_++;
  };
  auto announceChamber = [&] {  // the chamber's water changes from standing to the tank's (RAT-042)
    for (Signal ch : {Signal::Ph, Signal::Ec, Signal::Temp}) c.truth.expectChange(ch, c.now + kAnnounceMs);
  };

  // The hardware switched the feed pump off: no flow within its window (PD-477). It counts as a try
  // without flow; switching the pump off re-arms the hardware (SD-127)
  if (c.pumpCut && phase_ != Phase::Hold && phase_ != Phase::Wait) {
    restart(c, act, doser);  // a round going on is cut: what went in is mixed first later
    noFlow(c, act, say("cycle.wait.pump_cut"));
    return;
  }

  // The window without flow counts from the pump's start or the last flow seen, in every phase
  if (c.flowOk) flowSeen_ = c.now;
  // The feed pump's current, where measured (SD-127, RAT-049): learned while it moves water. Once
  // it has moved water in this run, far less for 2 s is a dry run, and it goes off at once as a
  // try without flow; at its start the window without flow protects it
  if (act.feedPump().value_or(false) && c.pumpA && isNum(*c.pumpA)) {
    const double a = *c.pumpA, normal = c.rt.pumpNormalA;
    const bool low = isNum(normal) && c.rt.pumpLearnedS >= kPumpLearnS && a < kDryShare * normal;
    if (c.flowOk && !low && a > 0) {
      const double dts = static_cast<double>(dt) / 1000.0;
      c.rt.pumpNormalA = isNum(normal) ? normal + (a - normal) * std::min(1.0, dts / 60.0) : a;
      c.rt.pumpLearnedS += dts;
    }
    if (c.flowOk) flowedThisRun_ = true;
    if (low && flowedThisRun_) {
      if (lowAmpsSince_ == 0) lowAmpsSince_ = c.now;
      if (c.now - lowAmpsSince_ >= kDryShareMs) {
        lowAmpsSince_ = 0;
        restart(c, act, doser);  // a round going on is cut: what went in is mixed first later
        noFlow(c, act, say("cycle.wait.dry_run", {{"amps", a}, {"normal", c.rt.pumpNormalA}}));
        return;
      }
    } else {
      lowAmpsSince_ = 0;
    }
  } else {
    lowAmpsSince_ = 0;
  }

  switch (phase_) {
    case Phase::PumpOn:
      announceChamber();
      feedPump(c, act, true);
      enter(c, Phase::ProveFlow);
      line_ = say("cycle.pump_on");
      break;
    case Phase::ProveFlow:
      announceChamber();
      feedPump(c, act, true);
      if (c.flowOk) {
        noFlowLogged_ = false;
        noFlowTries_ = 0;
        if (flowSince_ == 0) flowSince_ = c.now;
        line_ = say("cycle.prove_flow", {{"flow", numOrNull(flow.value)}});
        if (c.now - flowSince_ >= kFlowProveMs) enter(c, Phase::Flush);
      } else {
        flowSince_ = 0;
        line_ = say("cycle.wait_flow");
        // No flow within the window, at the start or after it was lost: the pump goes off and the
        // check tries again in 5 min. In the tank 6 s, before the hardware's own cut; a pump in the
        // device primes for up to 2 min (SD-109, SD-127)
        if (c.now - flowSeen_ > (c.pumpInTank ? kFlowStartMs : kFlowWaitMs)) noFlow(c, act, say("cycle.wait.no_flow"));
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
          line_ = say("cycle.remeasure", {{"left", mmss(s.stableMs)}});
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
          line_ = say("cycle.measure", {{"left", mmss(s.stableMs)}});
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
        c.truth.expectChange(Signal::Ec, c.now + kAnnounceMs);
        c.truth.expectChange(Signal::Ph, c.now + kAnnounceMs);
      }
      if (doser.busy()) break;
      if (!queue_.empty() && !doseFailed_) {
        DoseOrder o;
        o.id = std::string(kind_ == Kind::Ec ? "ec-" : "ph-") + std::to_string(++seq_);
        o.purpose = kind_ == Kind::Ec ? "ec" : "ph";
        o.step = queue_.front();
        Msg err;
        if (doser.start(c, act, o, err)) {
          json& pr = c.rt.pairRound;
          if (pr.is_object() && pr["parts"].contains(o.step.canister)) {
            pr["order"] = o.id;
            pr["canister"] = o.step.canister;
          }
          queue_.erase(queue_.begin());
          break;
        }
        doseFailed_ = true;
        clean_ = false;
        notes_.push_back(say("cycle.note.dose_failed", {{"reason", err}}));
      }
      if (mlRound_ > 0) {
        mixedMs_ = 0;
        doseEndAt_ = c.epoch;
        mixTargetMs_ = minutes(s.mixMin);  // pH waits like EC (SD-112)
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
        c.truth.expectChange(Signal::Ec, c.now + kAnnounceMs);
        c.truth.expectChange(Signal::Ph, c.now + kAnnounceMs);
      }
      // The mixing clock runs only with flow (SD-105); without it for the window, the pump goes off
      // as when proving flow (SD-127)
      if (c.flowOk) {
        mixedMs_ += dt;
      } else {
        feedPump(c, act, true);
        if (c.now - flowSeen_ > (c.pumpInTank ? kFlowStartMs : kFlowWaitMs)) {
          // What went in still needs mixing: the next cycle mixes first, the round learns nothing
          mixPendingMs_ = std::max(mixPendingMs_, mixTargetMs_);
          kind_ = Kind::None;
          mixingFirst_ = false;
          noFlow(c, act, say("cycle.wait.no_flow"));
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
      if (cableCheck(c, act)) break;
      feedPump(c, act, false);
      if (c.now >= retryAt_) enter(c, Phase::PumpOn);
      break;
    case Phase::Wait:
      feedPump(c, act, false);
      if (retryAt_ > 0 && c.now >= retryAt_) enter(c, Phase::PumpOn);
      break;
    case Phase::Off:
    case Phase::Stopped:
      break;
  }
}

json MixCycle::checks(const Ctx& c) const {
  const CycleSettings s = cycleSettings(c);
  const WaterPlan w = waterPlan(c.cfg, c.epoch);
  const RecipeCfg* r = w.recipe;
  auto item = [](bool ok, const Msg& m) {
    json j = m;
    j["ok"] = ok;
    return j;
  };
  auto reading = [&](Signal ch, const char* good, const char* bad) {
    const Reading& x = c.truth.get(ch);
    return item(x.usable(), x.usable() ? say(good) : say(bad, {{"reason", x.reason}}));
  };
  // A canister that can dose: on a channel, with a calibration (RAT-015)
  auto ready = [&](const CanisterCfg* b) { return b && b->channel > 0 && isNum(c.cfg.channelFlow(b->channel)) && !channelLocked(c.rt, b->channel); };

  json all = json::array();
  all.push_back(item(!c.stopped, say(c.stopped ? "check.stopped" : "check.not_stopped")));
  const bool placed = placementConfirmed(c.cfg);
  all.push_back(item(placed, say(placed ? "check.placement" : "cycle.off.placement")));
  if (s.off.key == "cycle.off.ended") all.push_back(item(false, s.off));  // its time is over
  else all.push_back(item(r != nullptr, r ? say("check.recipe", {{"name", r->name}}) : say("cycle.off.no_recipe")));
  const bool volume = isNum(s.volumeL) && s.volumeL > 0;
  all.push_back(item(volume, volume ? say("check.volume", {{"volume", s.volumeL}}) : say("cycle.off.no_volume")));
  const bool volumeLatch = c.rt.latches.count("tank.volume") > 0;
  all.push_back(item(!volumeLatch, say(volumeLatch ? "check.volume_latched" : "check.volume_fits")));
  all.push_back(item(!noFlowLogged_, say(noFlowLogged_ ? "check.no_flow" : "check.flow")));
  const bool busy = c.channelJob || c.probeCalibrating || c.maintenanceUntil > 0;
  all.push_back(item(!busy, c.maintenanceUntil > 0 ? say("cycle.wait.maintenance", {{"until", c.maintenanceUntil}})
                            : say(c.probeCalibrating ? "check.probe_job" : c.channelJob ? "check.channel_job" : "check.no_job")));

  json ec = json::array();
  if (r) {
    ec.push_back(item(isNum(w.ecTarget), isNum(w.ecTarget) ? say("check.ec_target", {{"target", w.ecTarget}})
                                                               : say("cycle.note.no_ec_target", {{"name", r->name}})));
    std::string missing;
    for (const auto& st : r->steps)
      if (!st.fixed && !ready(c.cfg.canister(st.canister))) {
        const CanisterCfg* b = c.cfg.canister(st.canister);
        missing = b ? b->name : st.canister;
        break;
      }
    ec.push_back(item(missing.empty(), missing.empty() ? say("check.parts_ready") : say("check.part_not_ready", {{"name", missing}})));
    std::string empty;
    for (const auto& st : r->steps)
      if (const CanisterCfg* b = c.cfg.canister(st.canister); b && !st.fixed && stockStatus(*b, c.rt) == "empty") {
        empty = b->name;
        break;
      }
    ec.push_back(item(empty.empty(), empty.empty() ? say("check.stock_ok") : say("check.stock_empty", {{"name", empty}})));
  }
  ec.push_back(reading(Signal::Ec, "check.ec_ok", "check.ec_bad"));
  const bool ecLatch = c.rt.latches.count("ec.no_effect") > 0;
  ec.push_back(item(!ecLatch, say(ecLatch ? "cycle.note.ec_latched" : "check.no_latch")));

  json ph = json::array();
  if (r) {
    const double target = w.phTarget;
    ph.push_back(item(isNum(target), isNum(target) ? say("check.ph_target", {{"target", target}})
                                                   : say("cycle.note.no_ph_range", {{"name", r->name}})));
  }
  ph.push_back(reading(Signal::Ph, "check.ph_ok", "check.ph_bad"));
  const CanisterCfg* down = c.cfg.phDown();
  ph.push_back(item(ready(down), ready(down) ? say("check.ph_down", {{"name", down->name}}) : say("check.no_ph_down")));
  if (down) {
    const bool empty = stockStatus(*down, c.rt) == "empty";
    ph.push_back(item(!empty, empty ? say("check.stock_empty", {{"name", down->name}}) : say("check.stock_ok")));
  }
  // pH needs ions to be read: only above the EC floor (RAT-046)
  const Reading& ecNow = c.truth.get(Signal::Ec);
  if (ecNow.usable()) {
    const bool gate = *ecNow.value >= c.cfg.control.ecFloor;
    ph.push_back(item(gate, say(gate ? "check.gate_ok" : "check.gate_low", {{"ec", *ecNow.value}, {"floor", c.cfg.control.ecFloor}})));
  }
  const Epoch since = lastEcDoseAt_ > 0 ? c.epoch - lastEcDoseAt_ : kEcRestS;  // no EC dose on record: nothing to wait for
  ph.push_back(item(since >= kEcRestS, since >= kEcRestS ? say("check.rest_over")
                                                        : say("check.rest_left", {{"left", mmss((kEcRestS - since) * kSecond)}})));
  ph.push_back(item(kind_ != Kind::Ec, say(kind_ == Kind::Ec ? "check.ec_first" : "check.ec_done")));
  const bool phLatch = c.rt.latches.count("ph.no_effect") > 0;
  ph.push_back(item(!phLatch, say(phLatch ? "cycle.note.ph_latched" : "check.no_latch")));
  return {{"all", all}, {"ec", ec}, {"ph", ph}};
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
  j["checks"] = checks(c);
  j["settings"] = {{"recipe", s.recipe ? json(s.recipe->id) : json(nullptr)},
                   {"ecTarget", numOrNull(s.ecTarget)},
                   {"ecTolerance", numOrNull(s.ecTol)},
                   {"phTarget", numOrNull(s.phTarget)},
                   {"phTolerance", numOrNull(s.phTol)},
                   {"volumeL", numOrNull(s.volumeL)},
                   {"flowLpm", numOrNull(s.flowLpm)},
                   {"turnoverMin", numOrNull(s.turnoverMin)},
                   {"mixMin", numOrNull(s.mixMin)},
                   {"mixOwnMin", numOrNull(s.mixOwnMin)},
                   {"mixUserMin", numOrNull(s.mixUserMin)},
                   {"mixWhy", s.mixWhy},
                   {"stableS", s.stableMs / kSecond},
                   {"holdMin", s.holdMs / kMinute}};
  return j;
}

}  // namespace doseph
