// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/gateway.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

#include "doseph/faults.hpp"
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
  if (!c.hold.empty()) return fail(holdText("act.hold.", c.hold));  // the hardware release holds every pump (SD-110)
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
    // Not before a person checked the protection by hand after the start, where the device does not
    // test it itself (SD-141)
    if (c.protectionCheck) return fail(say("act.protection_check"));
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
    err = holdText("act.hold.", c.hold);
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
    else if (!c.hold.empty()) e = holdText("act.hold.", c.hold);
    else if (c.protectionCheck) e = say("act.protection_check");
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

void Doser::book(const Ctx& c, Ms ms, bool known) {
  const auto& o = *active_;
  const double ml = isNum(o.step.flowMlPerMin) ? o.step.flowMlPerMin * static_cast<double>(ms) / 60000.0 : kNaN;
  progress_.msDone += ms;
  if (isNum(ml)) progress_.mlDone += ml;
  if (!remote_) c.journal.end(o.id, static_cast<int>(progress_.run) + 1, ms, ml);
  // The canister's level, run by run: a dose and a line fill take from it alike; a calibration is
  // booked from the cup when its amount is entered (SD-119). Kept below zero, never hidden as 0 (RAT-015).
  if (o.purpose != "calibration") takeStock(c, o.step.canister, o.step.name, ml);
  // What went into the tank, for the plants' records (RAT-064): an amount not known is counted as
  // such, never as nothing (R5)
  if (counted(o.purpose) && ms > 0) {
    if (known && isNum(ml)) c.rt.count.ml[o.step.canister] += ml;
    else c.rt.count.unknown[o.step.canister]++;
  }
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
    book(c, runRequestedMs_, false);
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
      book(c, mine && st.actualMs > 0 ? st.actualMs : runRequestedMs_, mine && st.actualMs > 0);
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
      book(c, std::clamp<Ms>(c.now - runStartedAt_, 0, runRequestedMs_), false);
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
      book(c, std::clamp<Ms>(c.now - runStartedAt_, 0, runRequestedMs_), false);
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
    else book(c, ran, st.jobId == job_);
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

// ---------------------------------------------------------------- NetOutputs

namespace {

// The Shelly component an output role is: a dimmer is a "Light", any other a "Switch"
std::string component(const Config& cfg, const std::string& role) {
  const auto def = roleDef(cfg, role);
  return def && def->dimmer ? "Light" : "Switch";
}

// What an output is bound to: another component on the same channel is another output
std::string outputKey(const Config& cfg, const std::string& role, const RoleBinding& b) {
  const NetDeviceCfg* d = cfg.device(b.device);
  return component(cfg, role) + "|" + b.device + "@" + (d ? d->address : "") + ":" + std::to_string(b.channel);
}

// A dimmer takes whole percent: up, so a brightness at the driver's threshold stays at it (RAT-013)
long sentLevel(double pct) { return static_cast<long>(std::ceil(std::clamp(pct, 1.0, 100.0) - 1e-9)); }

const RoleBinding* shellyOutput(const Config& cfg, const std::string& role) {
  const auto def = roleDef(cfg, role);
  const auto it = cfg.roles.find(role);
  if (!def || def->kind != "output" || it == cfg.roles.end() || it->second.source != "shelly" || !cfg.device(it->second.device)) return nullptr;
  return &it->second;
}

}  // namespace

// The refill valve's longest fill within its hard limit (R7, RAT-079)
Ms NetOutputs::longestFill(const Config& cfg) {
  const double min = isNum(cfg.refill.maxFillMin) ? std::min(cfg.refill.maxFillMin, kRefillMaxMin) : kRefillMaxMin;
  return static_cast<Ms>(std::llround(min * 60.0)) * kSecond;
}

// The refill valve without a float that says "not full" (RAT-032)
bool NetOutputs::limited(const Config& cfg, const NetDevices& dev, const std::string& role, Ms now) {
  if (role != "refill_valve") return false;
  const RoleValue full = dev.value(cfg, "tank_full", now);
  return !(full.state == "ok" && full.value->is_boolean() && !full.value->get<bool>());
}

bool NetOutputs::want(const std::string& role, bool on) {
  // A fixed output, a watering circuit's (SD-134), a light's (SD-135) or a room's (SD-136); the tick
  // drops what the configuration lacks
  const RoleDef* def = roleDef(role);
  if (!(def ? def->kind == "output" : role.rfind("water.", 0) == 0 || role.rfind("light.", 0) == 0 || role.rfind("climate.", 0) == 0))
    return false;
  outs_[role].want = on;
  return true;
}

void NetOutputs::wantLevel(const std::string& role, double pct) {
  if (role.rfind("light.", 0) != 0 || !isNum(pct)) return;
  outs_[role].level = std::clamp(pct, 1.0, 100.0);
}

void NetOutputs::allOff() {
  // Light follows its plan, and a room's fans their rule, whatever stops (SD-135, SD-136)
  for (auto& [role, o] : outs_)
    if (!throughStop(role)) o.want = false;
  forceOff_ = true;
}

void NetOutputs::send(const Config& cfg, NetDevices& dev, const std::string& role, Out& o, const std::string& stage, const std::string& method,
                      const json& params, Ms now) {
  const RoleBinding& b = cfg.roles.at(role);
  o.stage = stage;
  o.pending = dev.rpc(cfg, b.device, method, params, role + "|" + stage, now);
  if (o.pending == 0) {  // no such device: nothing to wait for
    o.stage.clear();
    o.retryAt = now + kNetPollMs;
  }
}

void NetOutputs::off(const Config& cfg, NetDevices& dev, const std::string& role, Out& o, Ms now) {
  const RoleBinding& b = cfg.roles.at(role);
  dev.rpc(cfg, b.device, component(cfg, role) + ".Set", {{"id", b.channel}, {"on", false}}, role + "|off", now);
  o.lastOff = now;
  o.onSent = false;
  o.onAt = -1;
}

void NetOutputs::tick(const Config& cfg, NetDevices& dev, bool stopped, bool hold, Ms now) {
  stopped_ = stopped;
  hold_ = hold;
  const bool force = forceOff_;
  forceOff_ = false;
  tickReleases(dev, now);
  // Outputs of circuits no longer in the configuration are dropped with their wants
  for (auto it = outs_.begin(); it != outs_.end();) it = roleDef(cfg, it->first) ? std::next(it) : outs_.erase(it);
  for (const auto& def : roleDefs(cfg)) {
    if (def.kind != "output") continue;
    Out& o = outs_[def.id];
    const RoleBinding* b = shellyOutput(cfg, def.id);
    if (!b) {
      o = Out{};  // nothing wanted of a role without an output: a later binding starts with nothing
      continue;
    }
    const std::string key = outputKey(cfg, def.id, *b);
    if (o.key != key) {  // bound anew: off first, then its settings are checked; nothing wanted yet
      o = Out{};
      o.key = key;
      // An off still going out for this very output, let go before, would undo what it does now. One
      // for the other component on its channel (a switch made a dimmer) goes on until answered.
      const NetDeviceCfg* d = cfg.device(b->device);
      const std::string comp = component(cfg, def.id);
      releasing_.erase(std::remove_if(releasing_.begin(), releasing_.end(),
                                      [&](const Release& r) { return d && r.device.address == d->address && r.channel == b->channel && r.comp == comp; }),
                       releasing_.end());
    }
    if (!dev.online(b->device, now)) {
      // Away: its settings may be gone with it (a reset, a power loss); checked again once back. A
      // pulse output (the refill valve, a watering circuit) switches on again only when wanted anew,
      // with its longest run from then (RAT-032).
      if (def.pulse) o.want = false;
      o.onAt = -1;
      o.verified = false;
      o.wasOnline = false;
      o.onSent = false;
      o.stage.clear();
      o.pending = 0;
      o.problem = say("device.offline");
      continue;
    }
    if (!o.wasOnline) {
      o.wasOnline = true;
      o.problem = {};
      o.lastOff = std::numeric_limits<Ms>::min() / 2;
    }
    const auto isOn = dev.output(b->device, b->channel, now, def.dimmer);
    // A light bound to the other kind of output (a dimmer on a relay, or the reverse): said, as
    // nothing sent to it would switch it (SD-135)
    if (def.light && !isOn && dev.output(b->device, b->channel, now, !def.dimmer)) {
      o.problem = say(def.dimmer ? "device.not_dimmer" : "device.is_dimmer");
      continue;
    }
    const json id = b->channel;
    const std::string comp = def.dimmer ? "Light" : "Switch";
    // Light follows its plan, and a room's fans their rule, through a Stop and a hold (SD-135); a
    // room's other outputs are off at a Stop and go on through a hold of the pumps (SD-136)
    const bool held = heldBy(def), forced = force && !throughStop(def.id);
    // The refill valve's own limits, whatever wants it open (RAT-032): never open without a float
    // that says "not full", and closed once it has been on for the longest fill. Closed so, it
    // opens again only when wanted anew.
    const bool limit = o.want && limited(cfg, dev, def.id, now);
    if (limit) o.want = false;
    if (def.id == "refill_valve" && o.want && o.onAt >= 0 && now - o.onAt >= longestFill(cfg)) o.want = false;
    // A watering circuit past its longest shot, whatever wants it on (SD-134)
    if (def.pulse && def.maxRunMs > 0 && o.want && o.onAt >= 0 && now - o.onAt >= def.maxRunMs) o.want = false;
    // A continuous output the device says is off, though the robot switched it on a while ago (a short
    // power cut, a hand at the device): checked again, and on again while wanted. A pulse output an
    // auto-off switched off stays off.
    if (!def.pulse && o.onSent && isOn == false && o.onAt >= 0 && now - o.onAt > 2 * kNetPollMs) {
      o.onSent = false;
      o.verified = false;
      o.onAt = -1;
    }
    // On without the robot having switched it after a check: off first, whatever is wanted (RAT-019).
    // A switch-on whose answer is not in yet is the robot's own. A light wanted on that is on (after
    // a restart) is checked while it stays on (SD-135).
    // One whose settings were written and still differ is switched off as any other (RAT-019).
    const bool unchecked = isOn == true && !(o.verified && (o.onSent || o.stage == "on")) && !(def.light && o.want && !o.refused);
    if (forced || !o.want || held || unchecked || limit) {
      // Off at once, and again while the device says on or has not said; a Stop and a start always
      if ((forced || isOn != false || o.onSent) && (forced || now - o.lastOff >= kNetPollMs)) off(cfg, dev, def.id, o, now);
      // A bound output shows whether it could switch: its settings are checked while it is off
      if (!o.verified && o.stage.empty() && now >= o.retryAt) {
        o.wrote = false;
        send(cfg, dev, def.id, o, "read", comp + ".GetConfig", {{"id", id}}, now);
      }
      continue;
    }
    // On wanted: every switch-on reads the settings back first (RAT-019). Once switched on it is
    // not switched on again until it was off: an auto-off that acted stays off
    if (!o.stage.empty() || now < o.retryAt) continue;
    if (isOn != false || o.onSent) {
      if (def.light && isOn == true && !o.onSent) {
        // A light on already, not switched by the robot since it started (or whose switch-on was
        // not answered): checked while it stays on
        o.wrote = false;
        send(cfg, dev, def.id, o, "read", comp + ".GetConfig", {{"id", id}}, now);
      } else if (def.dimmer && o.onSent && isOn == true && now - o.lastLevel >= kNetPollMs) {
        // A dimmer at another brightness than wanted: set again
        const auto br = dev.brightness(b->device, b->channel, now);
        if (!br || std::fabs(*br - static_cast<double>(sentLevel(o.level))) > 0.5) {
          // The brightness alone, and only while on: it is no switch-on (RAT-019)
          dev.rpc(cfg, b->device, "Light.Set", {{"id", id}, {"brightness", sentLevel(o.level)}}, def.id + "|level", now);
          o.lastLevel = now;
        }
      }
      continue;
    }
    o.wrote = false;
    send(cfg, dev, def.id, o, "read", comp + ".GetConfig", {{"id", id}}, now);
  }
}

void NetOutputs::answer(const Config& cfg, NetDevices& dev, const NetAnswer& a, Ms now) {
  if (a.tag == "release") {
    for (auto it = releasing_.begin(); it != releasing_.end(); ++it)
      if (it->pending == a.id) {
        if (a.ok) {
          releasing_.erase(it);  // the device switched it off
        } else if (a.other) {
          // Another device answers at its address: nothing more goes there, and the alarm says so
          failedReleases_.push_back(it->device.name);
          releasing_.erase(it);
        } else {
          it->pending = 0;  // again at the next poll
        }
        return;
      }
    return;
  }
  const size_t bar = a.tag.find('|');
  if (bar == std::string::npos) return;
  const std::string role = a.tag.substr(0, bar), stage = a.tag.substr(bar + 1);
  const auto def = roleDef(cfg, role);
  const RoleBinding* b = shellyOutput(cfg, role);
  const auto oit = outs_.find(role);
  if (!def || !b || oit == outs_.end()) return;
  Out& o = oit->second;
  if (o.key != outputKey(cfg, role, *b) || b->device != a.device) return;  // for a binding gone since
  if (stage == "off" || stage == "level") {
    const char* failed = stage == "off" ? "device.switch_failed" : "device.level_failed";
    if (!a.ok) o.problem = say(failed);
    else if (o.problem.key == failed) o.problem = {};
    return;
  }
  // Only the answer to the request the stage waits for: a late one of an older check counts for nothing
  if (stage != o.stage || a.id != o.pending) return;
  o.pending = 0;
  const json id = b->channel;
  const std::string comp = def->dimmer ? "Light" : "Switch";
  // Switched on: a dimmer at its brightness
  const json onParams = def->dimmer ? json{{"id", id}, {"on", true}, {"brightness", sentLevel(o.level)}} : json{{"id", id}, {"on", true}};
  auto giveUp = [&](Msg why, Ms retry) {
    o.stage.clear();
    o.problem = std::move(why);
    o.retryAt = now + retry;
  };
  if (!a.ok) return giveUp(say(stage == "on" ? "device.switch_on_failed" : "device.check_failed"), kNetPollMs);
  if (stage == "read" || stage == "reread") {
    // The settings of this very output: a read-back that names another one says nothing
    if (!a.result.is_object() || !a.result.contains("id") || a.result["id"] != id) return giveUp(say("device.check_failed"), kNetPollMs);
    const json wanted = shellySwitchConfig(*def, a.result);
    if (shellyConfigFits(wanted, a.result)) {
      o.verified = true;
      o.refused = false;
      o.problem = {};
      if (o.want && !heldBy(*def) && !limited(cfg, dev, role, now)) {
        o.onAt = now;
        o.lastLevel = now;
        send(cfg, dev, role, o, "on", comp + ".Set", onParams, now);
      } else {
        o.stage.clear();
      }
      return;
    }
    if (!o.wrote) {
      o.wrote = true;
      send(cfg, dev, role, o, "write", comp + ".SetConfig", {{"id", id}, {"config", wanted}}, now);
      return;
    }
    // Written and still not so: it does not switch on (RAT-019)
    o.verified = false;
    o.refused = true;
    return giveUp(say("device.settings_differ"), kMinute);
  }
  if (stage == "write") return send(cfg, dev, role, o, "reread", comp + ".GetConfig", {{"id", id}}, now);
  if (stage == "on") {
    o.stage.clear();
    o.onSent = true;
    // Not wanted any more while it switched on: off at once
    if (limited(cfg, dev, role, now)) o.want = false;
    if (!o.want || heldBy(*def)) off(cfg, dev, role, o, now);
  }
}

void NetOutputs::release(const Config& cfg, NetDevices& dev, const std::string& role, Ms now) {
  outs_.erase(role);
  const RoleBinding* b = shellyOutput(cfg, role);
  if (!b) return;
  Release r;
  r.device = *cfg.device(b->device);
  if (dev.isOther(b->device)) {
    // Another device answers at its address: an off would reach the wrong one
    failedReleases_.push_back(r.device.name);
    return;
  }
  std::tie(r.user, r.password) = dev.credentials(b->device);
  r.ownId = dev.ownId(b->device);
  r.channel = b->channel;
  r.comp = component(cfg, role);
  r.lastSent = now;
  r.until = now + kMinute;
  r.pending = dev.releaseRpc(r.device, r.user, r.password, r.ownId, r.comp + ".Set", {{"id", r.channel}, {"on", false}}, now);
  releasing_.push_back(std::move(r));
}

void NetOutputs::tickReleases(NetDevices& dev, Ms now) {
  // Outputs let go: off again until a device answers, for a minute at most
  for (auto it = releasing_.begin(); it != releasing_.end();) {
    if (now > it->until) {
      failedReleases_.push_back(it->device.name);
      it = releasing_.erase(it);
      continue;
    }
    if (it->pending == 0 && now - it->lastSent >= kNetPollMs) {
      it->pending = dev.releaseRpc(it->device, it->user, it->password, it->ownId, it->comp + ".Set", {{"id", it->channel}, {"on", false}}, now);
      it->lastSent = now;
    }
    ++it;
  }
}

void NetOutputs::releaseAll(const Config& cfg, NetDevices& dev, Ms now) {
  for (const auto& [role, b] : cfg.roles) release(cfg, dev, role, now);
}

std::vector<std::string> NetOutputs::takeFailedReleases() {
  std::vector<std::string> out;
  out.swap(failedReleases_);
  return out;
}

Msg NetOutputs::problem(const std::string& role) const {
  const auto it = outs_.find(role);
  return it == outs_.end() ? Msg{} : it->second.problem;
}

json NetOutputs::status(const Config& cfg, const NetDevices& dev, Ms now) const {
  json out = json::object();
  for (const auto& def : roleDefs(cfg)) {
    if (def.kind != "output") continue;
    const RoleBinding* b = shellyOutput(cfg, def.id);
    const auto oit = outs_.find(def.id);
    const Out none;
    const Out& o = oit == outs_.end() || !b || oit->second.key != outputKey(cfg, def.id, *b) ? none : oit->second;
    json j = {{"want", o.want},
              {"verified", o.verified},
              {"switchedOn", o.onSent || o.stage == "on"},
              {"problem", o.problem.key.empty() ? json(nullptr) : json(o.problem)}};
    if (def.dimmer) {
      j["level"] = o.level;
      const auto br = b ? dev.brightness(b->device, b->channel, now) : std::nullopt;
      j["brightness"] = br ? json(*br) : json(nullptr);
    }
    if (!b) j["state"] = "not_assigned";
    else if (!dev.online(b->device, now)) j["state"] = "offline";
    else if (o.refused) j["state"] = "refused";
    else {
      const auto on = dev.output(b->device, b->channel, now, def.dimmer);
      if (on == true) j["state"] = o.verified && (o.onSent || o.stage == "on") ? "on" : "unchecked";
      else if (!o.stage.empty()) j["state"] = "checking";
      else if (!on) j["state"] = "waiting";  // its status not read yet, or stale
      else j["state"] = o.verified ? "off" : "unchecked";
    }
    out[def.id] = j;
  }
  return out;
}

}  // namespace doseph
