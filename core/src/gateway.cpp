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
  return say("purpose.other");
}

bool intoTank(const std::string& purpose) { return purpose == "ec" || purpose == "ph"; }

}  // namespace

// ---------------------------------------------------------------- Actuators

bool Actuators::startRun(const Ctx& c, int pump, Ms ms, const std::string& purpose, const std::string& jobId, Msg& err) {
  auto fail = [&](Msg m) {
    err = std::move(m);
    return false;
  };
  if (c.stopped) return fail(say("act.stopped"));
  if (runningPump_ != 0) return fail(say("act.busy"));  // one run at a time (RAT-018)
  if (pump < 1 || pump > hw_.pumpCount()) return fail(say("act.no_pump", {{"pump", pump}}));
  const Limits lim = c.cfg.limits.bounded();  // fixed limits; settings only tighten (R7)
  if (ms <= 0 || ms > static_cast<Ms>(lim.maxRunS * 1000)) return fail(say("act.too_long", {{"s", lim.maxRunS}}));
  if (intoTank(purpose)) {
    // No dose without a measured calibration (RAT-015), no run under the minimum (RAT-050)
    const double flow = c.cfg.pumpFlow(pump);
    if (!(isNum(flow) && flow > 0)) return fail(say("act.uncalibrated"));
    if (ms < static_cast<Ms>(lim.minRunS * 1000)) return fail(say("act.too_short", {{"s", lim.minRunS}}));
    // No dose without flow through the device: it would not reach the tank (SD-105)
    if (!c.flowOk) return fail(say("act.no_flow"));
  } else if (purpose != "calibration" && purpose != "prime") {
    return fail(say("act.purpose"));
  }
  std::string e;
  if (!hw_.startRun(pump, ms, jobId, e)) return fail(say("act.hw", {{"error", e}}));
  runningPump_ = pump;
  runningPurpose_ = purpose;
  cutLogged_ = false;
  return true;
}

void Actuators::clearRun(int pump) {
  if (runningPump_ == pump) {
    runningPump_ = 0;
    runningPurpose_.clear();
  }
}

bool Actuators::setCirculation(const Ctx& c, bool on, Msg& err) {
  if (on && c.stopped) {
    err = say("act.stopped");
    return false;
  }
  std::string e;
  if (!hw_.setCirculation(on, e)) {
    err = say("act.hw", {{"error", e}});
    return false;
  }
  return true;
}

void Actuators::stopAll() {
  hw_.stopAllPumps();
  std::string e;
  hw_.setCirculation(false, e);  // also when its state is unknown: the command costs nothing
}

void Actuators::enforce(const Ctx& c) {
  if (runningPump_ == 0) return;
  // A Stop ends every run; a dose without flow stops: what it pumps would stay in the device (SD-105)
  if (c.stopped || (intoTank(runningPurpose_) && !c.flowOk)) {
    hw_.stopAllPumps();
    if (!cutLogged_ && !c.stopped) {
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
  const Ms ms = o.step.runs[progress_.run];
  const int run = static_cast<int>(progress_.run) + 1;
  // Written before the pump starts: a restart while it runs is then reported (SD-106)
  JournalRun jr;
  jr.order = o.id;
  jr.run = run;
  jr.purpose = o.purpose;
  jr.bottle = o.step.bottle;
  jr.name = o.step.name;
  jr.pump = o.step.pump;
  jr.ml = isNum(o.step.flowMlPerMin) ? o.step.flowMlPerMin * static_cast<double>(ms) / 60000.0 : kNaN;
  jr.ms = ms;
  jr.at = c.epoch;
  if (!c.journal.begin(jr)) {
    progress_.error = say("store.failed");
    return false;
  }
  Msg e;
  if (!act.startRun(c, o.step.pump, ms, o.purpose, busJob(), e)) {
    c.journal.end(o.id, run, 0, 0);  // nothing ran
    progress_.error = e;
    return false;
  }
  running_ = true;
  runStartedAt_ = c.now;
  runRequestedMs_ = ms;
  return true;
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
  order.id = order.id.empty() ? "d" + std::to_string(++seq_) : order.id;
  active_ = order;
  progress_ = DoseProgress{};
  progress_.state = DoseProgress::State::Running;
  progress_.orderId = active_->id;
  running_ = false;
  pauseUntil_ = 0;
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
  c.journal.end(o.id, static_cast<int>(progress_.run) + 1, ms, ml);
}

void Doser::logOrder(const Ctx& c) {
  const auto& o = *active_;
  if (!intoTank(o.purpose)) return;  // calibration and priming are no dose (RAT-070)
  if (progress_.state == DoseProgress::State::Aborted && progress_.msDone <= 0) return;  // nothing ran
  const bool ok = progress_.state == DoseProgress::State::Done;
  c.log.add(c.epoch, "dose", ok ? "info" : "warn", say(ok ? "ev.dose" : "ev.dose_incomplete", {{"name", o.step.name}, {"ml", progress_.mlDone}}),
            purposeLabel(o.purpose),
            {{"bottle", o.step.bottle},
             {"pump", o.step.pump},
             {"ml", progress_.mlDone},
             {"mlPlanned", numOrNull(o.step.ml)},
             {"ms", progress_.msDone},
             {"runs", progress_.run},
             {"purpose", o.purpose},
             {"order", o.id}});
}

void Doser::tick(const Ctx& c, Actuators& act) {
  if (!active_) return;
  const auto& o = *active_;
  if (running_) {
    RunStatus st = act.runStatus(o.step.pump);
    const bool mine = st.jobId == busJob();
    // A failure without a job ID: the pump driver disconnected or not answering
    const bool lost = !mine && st.state == RunStatus::State::Failed && st.jobId.empty();
    if (!lost && (!mine || st.state == RunStatus::State::Running)) {
      const Ms deadline = runStartedAt_ + runRequestedMs_ + std::max<Ms>(5 * kSecond, runRequestedMs_ / 5);
      if (c.now <= deadline) return;
      act.stopPumps();
      act.clearRun(o.step.pump);
      running_ = false;
      book(c, mine && st.actualMs > 0 ? st.actualMs : runRequestedMs_);
      fail(c, say("dose.no_response", {{"name", o.step.name}}));
      return;
    }
    act.clearRun(o.step.pump);
    running_ = false;
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
        progress_.state = DoseProgress::State::Done;
        logOrder(c);
        finished_ = progress_;
        active_.reset();
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
  act.stopPumps();
  if (running_) {
    RunStatus st = act.runStatus(active_->step.pump);
    const Ms ran = st.jobId == busJob() ? st.actualMs : std::clamp<Ms>(c.now - runStartedAt_, 0, runRequestedMs_);
    book(c, std::max<Ms>(0, ran));
  }
  act.clearRun(active_->step.pump);
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
