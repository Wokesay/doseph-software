// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/gateway.hpp"

#include <algorithm>

#include "doseph/messages.hpp"

namespace doseph {

namespace {

Msg purposeLabel(const std::string& p) {
  if (p == "ec") return say("purpose.ec");
  if (p == "ph") return say("purpose.ph");
  if (p == "calibration") return say("purpose.calibration");
  if (p == "prime") return say("purpose.prime");
  if (p == "fill") return say("purpose.fill");
  return say("purpose.other");
}

bool intoTank(const std::string& purpose) { return purpose == "ec" || purpose == "ph"; }
// What counts against a channel's daily amount: what goes into the tank (SD-113)
bool counted(const std::string& purpose) { return intoTank(purpose) || purpose == "fill"; }
constexpr Epoch kDayS = 24 * 3600;

constexpr double kDropMl = 0.05;         // one drop at the outlet (SD-111)
constexpr double kPullBackMl = 0.2;      // pulled back after a dose, so nothing dries at the tip
constexpr Ms kPullBackMaxMs = kSecond;
constexpr Ms kDropWaitMinMs = 5 * kSecond;  // the shortest wait for a drop
constexpr Ms kAfterRunMs = 10 * kSecond;    // drops this long after a run are still its own

}  // namespace

std::string channelLatch(int channel, const std::string& what) { return "channel." + std::to_string(channel) + "." + what; }

bool channelLocked(const RuntimeState& rt, int channel) {
  return holds(rt, channelLatch(channel, "no_drops")) || holds(rt, channelLatch(channel, "siphon"));
}

// ---------------------------------------------------------------- Actuators

bool Actuators::allowed(const Ctx& c, int channel, Ms ms, const std::string& purpose, Msg& err) const {
  auto fail = [&](Msg m) {
    err = std::move(m);
    return false;
  };
  if (c.stopped) return fail(say("act.stopped"));
  if (!c.hold.empty()) return fail(say("act.hold." + c.hold));  // the hardware release holds every pump (SD-110)
  if (runningChannel_ != 0) return fail(say("act.busy"));  // one run at a time (RAT-018)
  if (channel < 1 || channel > hw_.channelCount()) return fail(say("act.no_channel", {{"channel", channel}}));
  // A channel without a pump head moves nothing (SD-108)
  const std::string& head = hw_.profile().channels[static_cast<size_t>(channel - 1)].head;
  if (head.empty()) return fail(say("act.no_head", {{"channel", channel}}));
  const Limits lim = c.cfg.limits.bounded();  // fixed limits; settings only tighten (R7)
  if (ms <= 0 || ms > static_cast<Ms>(lim.maxRunS * 1000)) return fail(say("act.too_long", {{"s", lim.maxRunS}}));
  // Nothing runs towards the tank before the owner confirmed where pump and canisters stand
  // (SD-118); into a cup it may
  if ((intoTank(purpose) || purpose == "fill" || purpose == "refill") && !placementConfirmed(c.cfg)) return fail(say("act.placement"));
  if (intoTank(purpose)) {
    // No dose without a measured calibration (RAT-015), no run under the minimum (RAT-050)
    const double flow = c.cfg.channelFlow(channel);
    if (!(isNum(flow) && flow > 0)) return fail(say("act.uncalibrated"));
    // A calibration of another head is void (SD-108); the robot drops it, this holds meanwhile
    if (c.cfg.channels.at(channel).head != head) return fail(say("act.head_changed"));
    if (ms < static_cast<Ms>(lim.minRunS * 1000)) return fail(say("act.too_short", {{"s", lim.minRunS}}));
    // No dose without flow through the device: it would not reach the tank (SD-105)
    if (!c.flowOk) return fail(say("act.no_flow"));
    if (channelLocked(c.rt, channel)) return fail(say("act.channel_locked", {{"channel", channel}}));
  } else if (purpose == "pullback") {
    if (!canPullBack(channel)) return fail(say("act.no_reverse", {{"channel", channel}}));
  } else if (purpose != "calibration" && purpose != "prime" && purpose != "fill" && purpose != "refill") {
    return fail(say("act.purpose"));
  }
  if (counted(purpose))
    if (auto why = dailyCap(c, channel, ms)) return fail(*why);
  return true;
}

double Actuators::dailyCapMin(int channel) const {
  if (channel < 1 || channel > hw_.channelCount()) return kDailyMaxMin;
  const double head = hw_.profile().channels[static_cast<size_t>(channel - 1)].dailyMaxMin;
  return isNum(head) && head > 0 ? std::min(head, kDailyMaxMin) : kDailyMaxMin;
}

std::optional<Msg> Actuators::dailyCap(const Ctx& c, int channel, Ms ms) const {
  if (channel < 1 || channel > hw_.channelCount()) return std::nullopt;
  const double capMin = dailyCapMin(channel);
  const Ms used = c.rt.dayRunMs(channel, c.epoch - kDayS);
  if (static_cast<double>(used + ms) > capMin * 60000.0) return say("act.daily_cap", {{"channel", channel}, {"min", capMin}});
  // The user's amount per day, below the cap, from the calibrated flow
  const CanisterCfg* b = c.cfg.canisterOnChannel(channel);
  const double flow = c.cfg.channelFlow(channel);
  if (b && isNum(b->maxMlPerDay) && isNum(flow) && flow > 0 &&
      flow * static_cast<double>(used + ms) / 60000.0 > b->maxMlPerDay + 1e-9)
    return say("act.daily_ml", {{"name", b->name}, {"ml", b->maxMlPerDay}});
  return std::nullopt;
}

bool Actuators::startRun(const Ctx& c, int channel, Ms ms, const std::string& purpose, const std::string& jobId, Msg& err) {
  if (!allowed(c, channel, ms, purpose, err)) return false;
  std::string e;
  if (!hw_.startRun(channel, ms, jobId, e, purpose == "pullback")) {
    err = say("act.hw", {{"error", e}});
    return false;
  }
  runningChannel_ = channel;
  runningPurpose_ = purpose;
  cutLogged_ = false;
  // Counted at its start for the full time asked: the safe side if it is cut (SD-113)
  if (counted(purpose)) {
    auto& runs = c.rt.dayRuns[channel];
    runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const auto& r) { return r.first <= c.epoch - kDayS; }), runs.end());
    runs.emplace_back(c.epoch, ms);
  }
  // A run into the tank must drip: a drop at the calibrated flow is expected within a quarter of
  // the wait. A run after one that has not dripped yet waits from where that one left off.
  if (intoTank(purpose)) {
    if (const auto d = hw_.drops(channel)) {
      auto [w, fresh] = watch_.try_emplace(channel);
      if (fresh) w->second.since = c.now;
      w->second.window = std::max<Ms>(kDropWaitMinMs, static_cast<Ms>(4 * kDropMl / c.cfg.channelFlow(channel) * 60000.0));
      w->second.ended = -1;
      w->second.dripped = false;  // this run has to drip itself
      seen_[channel] = d->count;
    }
  }
  return true;
}

void Actuators::clearRun(int channel) {
  if (runningChannel_ == channel) {
    runningChannel_ = 0;
    runningPurpose_.clear();
  }
}

bool Actuators::setFeedPump(const Ctx& c, bool on, Msg& err) {
  if (on && c.stopped) {
    err = say("act.stopped");
    return false;
  }
  if (on && !c.hold.empty()) {
    err = say("act.hold." + c.hold);
    return false;
  }
  std::string e;
  if (!hw_.setFeedPump(on, e)) {
    err = say("act.hw", {{"error", e}});
    return false;
  }
  return true;
}

void Actuators::stopAll() {
  hw_.stopAllPumps();
  watch_.clear();  // what had not dripped by a Stop is not judged (SD-111)
  std::string e;
  hw_.setFeedPump(false, e);  // also when its state is unknown: the command costs nothing
}

void Actuators::lockChannel(const Ctx& c, int channel, const std::string& what) {
  const json latch = {{"at", c.epoch}, {"key", "channel." + what}, {"channel", channel}};
  const bool again = reportStands(c.rt, channelLatch(channel, what));
  if (setLatch(c.cfg, c.rt, channelLatch(channel, what), latch, "channel." + what))
    c.log.add(c.epoch, "alarm", "alarm", say("ev.channel." + what, {{"channel", channel}}), say("ev.channel." + what + ".text"),
              {{"channel", channel}});
  else if (!again)  // reported only, once while its report stands (SD-133)
    c.log.add(c.epoch, "alarm", "alarm", reportedEvent("channel." + what, latch), say("ev.lock.reported.text"), {{"channel", channel}});
}

void Actuators::watchDrops(const Ctx& c) {
  if (runningChannel_ != 0) lastRun_[runningChannel_] = c.now;
  // A Stop or the hardware release cut every run: what had not dripped by then is not judged
  if (c.stopped || !c.hold.empty()) watch_.clear();  // also stopAll()
  for (int n = 1; n <= hw_.channelCount(); ++n) {
    const auto d = hw_.drops(n);
    if (!d) continue;
    const auto seen = seen_.find(n);
    const bool fell = seen != seen_.end() && d->count > seen->second;
    seen_[n] = d->count;
    const bool runs = runningChannel_ == n;
    if (auto it = watch_.find(n); it != watch_.end()) {
      DropWatch& w = it->second;
      if (fell) {
        w.since = c.now;
        w.dripped = true;
      }
      if (!runs && w.ended < 0) w.ended = c.now;
      if (runs) {
        if (c.now - w.since <= w.window) continue;
        // The doser books what ran and ends the dose; reported only, the run goes to its end (SD-133)
        if (!c.cfg.reportsOnly("channel.no_drops")) hw_.stopAllPumps();
      } else if (w.dripped) {
        watch_.erase(it);  // it dripped: done
        continue;
      } else if (c.now - w.ended <= w.window) {
        continue;  // a short run is judged at its end plus the wait
      }
      watch_.erase(it);
      lockChannel(c, n, "no_drops");
      continue;
    }
    // Drops while the pump stands, past the run's own: a canister above the device siphons (PD-455)
    const auto last = lastRun_.find(n);
    if (fell && !runs && (last == lastRun_.end() || c.now - last->second > kAfterRunMs) &&
        !latched(c.cfg, c.rt, channelLatch(n, "siphon"), "channel.siphon")) {
      lockChannel(c, n, "siphon");
      unplanned_ = true;
    }
  }
}

void Actuators::enforce(const Ctx& c) {
  watchDrops(c);
  if (runningChannel_ == 0) return;
  // A Stop or the hardware release ends every run (SD-110); a dose without flow stops: what it
  // pumps would stay in the device (SD-105)
  if (c.stopped || !c.hold.empty() || (intoTank(runningPurpose_) && !c.flowOk)) {
    hw_.stopAllPumps();
    if (!cutLogged_ && !c.stopped && c.hold.empty()) {
      c.log.add(c.epoch, "block", "warn", say("ev.flow_cut"), say("ev.flow_cut.text"));
      cutLogged_ = true;
    }
  }
}

// ---------------------------------------------------------------- Doser

std::string Doser::busJob() const {
  std::string job = active_->id + "#" + std::to_string(progress_.run + 1);
  return bootTag_.empty() ? job : job + "." + bootTag_;
}

bool Doser::launch(const Ctx& c, Actuators& act) {
  const auto& o = *active_;
  if (remote_) {
    // The other device journals it before its pump starts, refills and pulls back its own line
    if (!c.remote || !c.remote->linked()) {
      progress_.error = say("coupling.no_link");
      return false;
    }
    leg_ = Leg::Dose;
    job_ = busJob();
    c.remote->send(job_, o.step.channel, o.step.runs[progress_.run], o.purpose);
    running_ = true;
    runStartedAt_ = c.now;
    runRequestedMs_ = o.step.runs[progress_.run];
    return true;
  }
  // A line pulled back after the last dose is filled first: the same time forwards, not booked,
  // so the dose stays the planned amount (SD-111)
  if (auto back = c.rt.pulledBackMs.find(o.step.channel); back != c.rt.pulledBackMs.end()) {
    Msg e;
    // Only for a run that may start: a refused one is refused as without a refill
    if (!act.allowed(c, o.step.channel, o.step.runs[progress_.run], o.purpose, e)) {
      progress_.error = e;
      return false;
    }
    job_ = busJob() + "f";
    if (!act.startRun(c, o.step.channel, back->second, "refill", job_, e)) {
      progress_.error = e;
      return false;
    }
    leg_ = Leg::Refill;
    running_ = true;
    runStartedAt_ = c.now;
    runRequestedMs_ = back->second;
    return true;
  }
  leg_ = Leg::Dose;
  const Ms ms = o.step.runs[progress_.run];
  const int run = static_cast<int>(progress_.run) + 1;
  // Written before the pump starts: a restart while it runs is then reported (SD-106)
  JournalRun jr;
  jr.order = o.id;
  jr.run = run;
  jr.purpose = o.purpose;
  jr.canister = o.step.canister;
  jr.name = o.step.name;
  jr.channel = o.step.channel;
  jr.ml = isNum(o.step.flowMlPerMin) ? o.step.flowMlPerMin * static_cast<double>(ms) / 60000.0 : kNaN;
  jr.ms = ms;
  jr.at = c.epoch;
  if (!c.journal.begin(jr)) {
    progress_.error = say("store.failed");
    return false;
  }
  Msg e;
  job_ = busJob();
  if (!act.startRun(c, o.step.channel, ms, o.purpose, job_, e)) {
    c.journal.end(o.id, run, 0, 0);  // nothing ran
    progress_.error = e;
    return false;
  }
  running_ = true;
  runStartedAt_ = c.now;
  runRequestedMs_ = ms;
  return true;
}

// After the last run of a dose into the tank the pump runs backwards briefly, where it can (SD-111)
bool Doser::pullBack(const Ctx& c, Actuators& act) {
  const auto& o = *active_;
  const double flow = o.step.flowMlPerMin;
  if (!intoTank(o.purpose) || !act.canPullBack(o.step.channel) || !(isNum(flow) && flow > 0)) return false;
  const Ms ms = std::clamp<Ms>(std::llround(kPullBackMl / flow * 60000.0), 1, kPullBackMaxMs);
  Msg e;
  job_ = busJob() + "b";
  if (!act.startRun(c, o.step.channel, ms, "pullback", job_, e)) return false;  // nothing pulled back: nothing to fill
  leg_ = Leg::PullBack;
  running_ = true;
  runStartedAt_ = c.now;
  runRequestedMs_ = ms;
  return true;
}

bool Doser::dripped(Actuators& act) const {
  const auto d = act.drops(active_->step.channel);
  return d && d->count > fillFrom_;
}

void Doser::finish(const Ctx& c, DoseProgress::State state) {
  progress_.state = state;
  logOrder(c);
  finished_ = progress_;
  active_.reset();
}

// A refill or a pull back ended: neither is booked
void Doser::legEnded(const Ctx& c, Actuators& act, bool done, Ms ran, const std::string& error) {
  const auto& o = *active_;
  if (leg_ == Leg::PullBack) {
    if (ran > 0) c.rt.pulledBackMs[o.step.channel] = std::min(ran, kPullBackMaxMs);
    finish(c, DoseProgress::State::Done);  // the dose itself is complete
    return;
  }
  // The refill: done, the dose follows at once; cut, how full the line is is unknown, and the
  // next run starts without a refill (the safe direction: less into the tank)
  c.rt.pulledBackMs.erase(o.step.channel);
  if (done) {
    if (!launch(c, act)) finish(c, DoseProgress::State::Failed);
    return;
  }
  progress_.error = say("dose.failed", {{"name", o.step.name}, {"reason", error.empty() ? json(say("dose.run_cancelled")) : json(error)}});
  finish(c, DoseProgress::State::Failed);
}

void Doser::fail(const Ctx& c, Msg error) {
  progress_.state = DoseProgress::State::Failed;
  progress_.error = std::move(error);
  logOrder(c);
  finished_ = progress_;
  active_.reset();
}

bool Doser::start(const Ctx& c, Actuators& act, DoseOrder order, Msg& err) {
  if (active_) {
    err = say("dose.busy");
    return false;
  }
  if (order.step.runs.empty()) {
    err = say("dose.empty");
    return false;
  }
  // A run on the coupled device's channel (SD-131): its own gateway decides there; here only what
  // holds every dose of this device holds it too, and the link must be up
  if (!order.step.device.empty()) {
    Msg e;
    if (c.stopped) e = say("act.stopped");
    else if (!c.hold.empty()) e = say("act.hold." + c.hold);
    else if (!placementConfirmed(c.cfg)) e = say("act.placement");
    else if (!intoTank(order.purpose)) e = say("act.purpose");
    else if (!c.remote || !c.remote->linked()) e = say("coupling.no_link");
    if (!e.key.empty()) {
      err = e;
      return false;
    }
  }
  order.id = order.id.empty() ? "d" + std::to_string(++seq_) : order.id;
  active_ = order;
  remote_ = !order.step.device.empty();
  progress_ = DoseProgress{};
  progress_.state = DoseProgress::State::Running;
  progress_.orderId = active_->id;
  progress_.channel = active_->step.channel;
  running_ = false;
  pauseUntil_ = 0;
  filled_ = false;
  const auto d = remote_ ? std::nullopt : act.drops(order.step.channel);
  fillFrom_ = d ? d->count : 0;
  if (!launch(c, act)) {
    err = progress_.error;
    active_.reset();
    progress_.state = DoseProgress::State::Idle;
    return false;
  }
  return true;
}

void Doser::book(const Ctx& c, Ms ms) {
  const auto& o = *active_;
  const double ml = isNum(o.step.flowMlPerMin) ? o.step.flowMlPerMin * static_cast<double>(ms) / 60000.0 : kNaN;
  progress_.msDone += ms;
  if (isNum(ml)) progress_.mlDone += ml;
  if (!remote_) c.journal.end(o.id, static_cast<int>(progress_.run) + 1, ms, ml);
  // The canister's level, run by run: a dose and a line fill take from it alike; a calibration is
  // booked from the cup when its amount is entered (SD-119). Kept below zero, never hidden as 0 (RAT-015).
  if (o.purpose != "calibration") takeStock(c, o.step.canister, o.step.name, ml);
}

void takeStock(const Ctx& c, const std::string& canister, const std::string& name, double ml) {
  auto st = c.rt.stockMl.find(canister);
  if (st == c.rt.stockMl.end() || !isNum(st->second) || !isNum(ml)) return;  // unknown stays unknown
  const bool covered = st->second > kStockBelowZeroMl;
  st->second -= ml;
  if (covered && st->second <= kStockBelowZeroMl)
    c.log.add(c.epoch, "dose", "warn", say("ev.stock.below_zero", {{"name", name}}), say("ev.stock.below_zero.text", {{"name", name}}),
              {{"canister", canister}, {"stockMl", st->second}});
}

void Doser::logOrder(const Ctx& c) {
  const auto& o = *active_;
  // A line fill drips into the tank: it is logged with the doses (SD-111)
  if (o.purpose == "fill") {
    const bool ok = progress_.state == DoseProgress::State::Done;
    c.log.add(c.epoch, "dose", ok ? "info" : "warn", say(ok ? "ev.fill" : "ev.fill_incomplete", {{"name", o.step.name}, {"channel", o.step.channel}}),
              ok ? say("ev.fill.text", {{"channel", o.step.channel}, {"s", progress_.msDone / 1000.0}}) : progress_.error,
              {{"canister", o.step.canister}, {"channel", o.step.channel}, {"ms", progress_.msDone}, {"purpose", o.purpose}, {"order", o.id}});
    return;
  }
  if (!intoTank(o.purpose)) return;  // calibration and priming are no dose (RAT-070)
  if (progress_.state == DoseProgress::State::Aborted && progress_.msDone <= 0) return;  // nothing ran
  const bool ok = progress_.state == DoseProgress::State::Done;
  c.log.add(c.epoch, "dose", ok ? "info" : "warn", say(ok ? "ev.dose" : "ev.dose_incomplete", {{"name", o.step.name}, {"ml", progress_.mlDone}}),
            purposeLabel(o.purpose),
            {{"canister", o.step.canister},
             {"channel", o.step.channel},
             {"ml", progress_.mlDone},
             {"mlPlanned", numOrNull(o.step.ml)},
             {"ms", progress_.msDone},
             {"runs", progress_.run},
             {"purpose", o.purpose},
             {"order", o.id}});
}

// A run on the coupled device: its result comes with the link. One that does not come back in
// time is booked as run (the safe direction: no dosing again) and the order fails.
void Doser::remoteTick(const Ctx& c) {
  const auto& o = *active_;
  if (!running_) {
    if (c.now < pauseUntil_) return;
    if (!c.remote || !c.remote->linked()) {
      fail(c, say("coupling.no_link"));
      return;
    }
    c.remote->send(job_ = busJob(), o.step.channel, o.step.runs[progress_.run], o.purpose);
    running_ = true;
    runStartedAt_ = c.now;
    runRequestedMs_ = o.step.runs[progress_.run];
    return;
  }
  const auto r = c.remote ? c.remote->result(job_) : std::nullopt;
  if (!r) {
    const Ms deadline = runStartedAt_ + runRequestedMs_ + std::max<Ms>(5 * kSecond, runRequestedMs_ / 5) + kRemoteGraceMs;
    if (c.now <= deadline) return;
    if (c.remote) c.remote->abort(job_);
    running_ = false;
    book(c, runRequestedMs_);
    fail(c, say("dose.no_response", {{"name", o.step.name}}));
    return;
  }
  running_ = false;
  book(c, std::max<Ms>(0, r->ms));
  if (r->state == "done") {
    progress_.run++;
    if (progress_.run >= o.step.runs.size()) finish(c, DoseProgress::State::Done);
    else pauseUntil_ = c.now + 3 * kSecond;  // pause between partial runs (RAT-055)
    return;
  }
  progress_.state = DoseProgress::State::Failed;
  const json reason = r->error.key.empty() ? json(say("dose.run_cancelled")) : json(r->error);
  progress_.error = say("dose.failed", {{"name", o.step.name}, {"reason", reason}});
  logOrder(c);
  finished_ = progress_;
  active_.reset();
}

void Doser::tick(const Ctx& c, Actuators& act) {
  if (!active_) return;
  if (remote_) {
    remoteTick(c);
    return;
  }
  const auto& o = *active_;
  // A line fill stops at its first drops (SD-111)
  if (running_ && leg_ == Leg::Dose && o.purpose == "fill" && !filled_ && dripped(act)) {
    act.stopPumps();
    filled_ = true;
  }
  if (running_) {
    RunStatus st = act.runStatus(o.step.channel);
    const bool mine = st.jobId == job_;
    // A failure without a job ID: the pump driver disconnected or not answering
    const bool lost = !mine && st.state == RunStatus::State::Failed && st.jobId.empty();
    if (!lost && (!mine || st.state == RunStatus::State::Running)) {
      const Ms deadline = runStartedAt_ + runRequestedMs_ + std::max<Ms>(5 * kSecond, runRequestedMs_ / 5);
      if (c.now <= deadline) return;
      act.stopPumps();
      act.clearRun(o.step.channel);
      running_ = false;
      if (leg_ != Leg::Dose) {
        legEnded(c, act, false, 0, "");
        return;
      }
      book(c, mine && st.actualMs > 0 ? st.actualMs : runRequestedMs_);
      fail(c, say("dose.no_response", {{"name", o.step.name}}));
      return;
    }
    act.clearRun(o.step.channel);
    running_ = false;
    if (leg_ != Leg::Dose) {
      legEnded(c, act, mine && st.state == RunStatus::State::Done, mine ? st.actualMs : 0, mine ? st.error : "");
      return;
    }
    if (filled_ && mine) {
      book(c, std::max<Ms>(0, st.actualMs));
      progress_.run++;
      finish(c, DoseProgress::State::Done);
      return;
    }
    if (lost) {
      act.stopPumps();
      book(c, std::clamp<Ms>(c.now - runStartedAt_, 0, runRequestedMs_));
      const json reason = st.error.empty() ? json(say("dose.disconnected")) : json(st.error);
      fail(c, say("dose.lost", {{"name", o.step.name}, {"reason", reason}}));
      return;
    }
    if (st.state == RunStatus::State::Done) {
      book(c, st.actualMs);
      progress_.run++;
      if (progress_.run >= o.step.runs.size()) {
        if (pullBack(c, act)) return;  // the order ends after it
        if (o.purpose == "fill" && !dripped(act)) {
          // Every run ran and nothing dripped: the line is not full (SD-111)
          progress_.error = say("dose.fill_no_drops", {{"name", o.step.name}, {"s", progress_.msDone / 1000.0}});
          finish(c, DoseProgress::State::Failed);
          return;
        }
        finish(c, DoseProgress::State::Done);
        return;
      }
      pauseUntil_ = c.now + 3 * kSecond;  // pause between partial runs (RAT-055)
      return;
    }
    book(c, std::max<Ms>(0, st.actualMs));
    progress_.state = DoseProgress::State::Failed;
    const json reason = st.error.empty() ? json(say("dose.run_cancelled")) : json(st.error);
    progress_.error = say("dose.failed", {{"name", o.step.name}, {"reason", reason}});
    logOrder(c);
    finished_ = progress_;
    active_.reset();
    return;
  }
  if (o.purpose == "fill" && dripped(act)) {
    finish(c, DoseProgress::State::Done);  // it dripped between two runs
    return;
  }
  if (c.now < pauseUntil_) return;
  if (!launch(c, act)) {
    progress_.state = DoseProgress::State::Failed;
    logOrder(c);
    finished_ = progress_;
    active_.reset();
  }
}

void Doser::abort(const Ctx& c, Actuators& act, const Msg& reason) {
  if (!active_) return;
  if (remote_) {
    // The other device stops it and books there; here what ran is booked as far as it may have run
    if (running_) {
      if (c.remote) c.remote->abort(job_);
      book(c, std::clamp<Ms>(c.now - runStartedAt_, 0, runRequestedMs_));
    }
    running_ = false;
    progress_.state = DoseProgress::State::Aborted;
    progress_.error = say("dose.aborted", {{"name", active_->step.name}, {"reason", reason}});
    logOrder(c);
    finished_ = progress_;
    active_.reset();
    return;
  }
  act.stopPumps();
  if (running_) {
    RunStatus st = act.runStatus(active_->step.channel);
    const Ms ran = std::max<Ms>(0, st.jobId == job_ ? st.actualMs : std::clamp<Ms>(c.now - runStartedAt_, 0, runRequestedMs_));
    if (leg_ == Leg::PullBack) {
      act.clearRun(active_->step.channel);
      running_ = false;
      legEnded(c, act, false, ran, "");  // keeps what it pulled back; the dose itself is complete
      return;
    }
    if (leg_ == Leg::Refill) c.rt.pulledBackMs.erase(active_->step.channel);  // how full the line is is unknown
    else book(c, ran);
  }
  act.clearRun(active_->step.channel);
  running_ = false;
  progress_.state = DoseProgress::State::Aborted;
  progress_.error = say("dose.aborted", {{"name", active_->step.name}, {"reason", reason}});
  logOrder(c);
  finished_ = progress_;
  active_.reset();
}

std::optional<DoseProgress> Doser::takeFinished() {
  auto f = finished_;
  finished_.reset();
  return f;
}

}  // namespace doseph
