// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/pumpcal.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/messages.hpp"

namespace doseph {

namespace {

bool agree(double a, double b) { return std::fabs(a - b) <= kCalAgree * (a + b) / 2.0; }
double round2(double v) { return std::round(v * 100.0) / 100.0; }

}  // namespace

bool PumpCal::start(Ctx& c, Actuators& act, int channel, const std::string& method, Msg& err) {
  const Profile& prof = act.profile();
  if (channel < 1 || channel > static_cast<int>(prof.channels.size())) {
    err = say("act.no_channel", {{"channel", channel}});
    return false;
  }
  if (method != "cylinder" && method != "scale") {
    err = say("pumpcal.method");
    return false;
  }
  const ChannelCaps& caps = prof.channels[static_cast<size_t>(channel - 1)];
  if (caps.head.empty()) {
    err = say("act.no_head", {{"channel", channel}});
    return false;
  }
  const auto nominal = prof.headMlPerMin.find(caps.head);
  if (nominal == prof.headMlPerMin.end() || !isNum(nominal->second) || nominal->second <= 0) {
    err = say("pumpcal.no_nominal");
    return false;
  }
  // The concentrate in the line goes back into its canister first: no pump that cannot run
  // backwards is calibrated this way (SD-143)
  if (!act.canPullBack(channel)) {
    err = say("pumpcal.no_reverse", {{"channel", channel}});
    return false;
  }
  const int seq = seq_;  // order IDs stay unique across calibrations
  *this = PumpCal{};
  seq_ = seq;
  channel_ = channel;
  method_ = method;
  head_ = caps.head;
  nominal_ = nominal->second;
  lineMl_ = caps.lineMl;
  drops_ = act.drops(channel).has_value();
  if (const CanisterCfg* b = c.cfg.canisterOnChannel(channel)) {
    canister_ = b->id;
    name_ = b->name;
  }
  at_ = c.epoch;
  // Kept in the state: a restart in the middle locks the channel (R6)
  c.rt.pumpCal = {{"channel", channel}, {"at", c.epoch}};
  act.setChannelLight(channel, true);
  c.log.add(c.epoch, "calibration", "info", say("ev.pumpcal.start", {{"channel", channel}}), Msg{}, {{"channel", channel}});
  go(c, "reverse", say("pumpcal.reverse", {{"channel", channel}}));
  return true;
}

void PumpCal::go(Ctx& c, const std::string& step, Msg message) {
  step_ = step;
  message_ = std::move(message);
  lastStep_ = c.now;
  pending_ = step == "reverse" || step == "prime" || step == "run" || step == "empty" || step == "fill";
}

bool PumpCal::intakeOut() const {
  return step_ == "to_water" || step_ == "prime" || step_ == "reading" || step_ == "run" || step_ == "empty" || step_ == "to_canister";
}

bool PumpCal::next(Ctx& c, Actuators& act, const json& body, Msg& err) {
  if (!active() || !waiting()) {
    err = say(active() ? "pumpcal.not_now" : "pumpcal.none");
    return false;
  }
  if (step_ == "to_water") {
    go(c, "prime", say("pumpcal.prime", {{"channel", channel_}}));
    return true;
  }
  if (step_ == "to_canister") {
    go(c, "fill", say("pumpcal.fill", {{"channel", channel_}}));
    return true;
  }
  // A reading: ml on the cylinder, or grams on the scale (water: 1 g is 1 ml)
  const double reading = jnum(body, "reading");
  if (!isNum(reading) || reading < 0 || reading > 10000) {
    err = say("pumpcal.reading");
    return false;
  }
  if (readings_.empty()) {
    readings_.push_back(reading);
    go(c, "run", say("pumpcal.run", {{"channel", channel_}, {"run", 1}}));
    return true;
  }
  const double last = readings_.back();
  if (reading >= last) {
    err = say("pumpcal.reading_up", {{"last", last}});
    return false;
  }
  Run& run = runs_.at(readings_.size() - 1);
  const double pumped = (last - reading) / (method_ == "cylinder" ? kCylinderShift : 1.0);
  const double flow = pumped / static_cast<double>(std::max<Ms>(run.ms, 1)) * 60000.0;
  // Drops are a coarse check, never the rate: none at all means nothing reached the outlet
  if (drops_ && run.drops == 0) {
    err = say("pumpcal.no_drops");
    return false;
  }
  if (flow < nominal_ * (1 - kCalWindow)) {
    err = say("pumpcal.too_low", {{"flow", round2(flow)}, {"nominal", nominal_}});
    return false;
  }
  if (flow > nominal_ * (1 + kCalWindow)) {
    err = say("pumpcal.too_high", {{"flow", round2(flow)}, {"nominal", nominal_}});
    return false;
  }
  readings_.push_back(reading);
  run.flow = flow;
  judge(c, act);
  return true;
}

// Two runs that agree give the rate; two that do not, a third; of three, the two that agree best
void PumpCal::judge(Ctx& c, Actuators& act) {
  const size_t n = runs_.size();
  double rate = kNaN;
  if (n == 2 && agree(runs_[0].flow, runs_[1].flow)) {
    rate = (runs_[0].flow + runs_[1].flow) / 2.0;
  } else if (n == 3) {
    double best = kNaN;
    for (size_t i = 0; i < 3; ++i)
      for (size_t j = i + 1; j < 3; ++j) {
        const double a = runs_[i].flow, b = runs_[j].flow;
        const double d = std::fabs(a - b) / ((a + b) / 2.0);
        if (!isNum(best) || d < best) {
          best = d;
          rate = agree(a, b) ? (a + b) / 2.0 : kNaN;
        }
      }
    if (!isNum(rate)) return fail(c, act, say("pumpcal.disagree", {{"share", kCalAgree * 100}}));
  }
  if (!isNum(rate)) {
    go(c, "run", say("pumpcal.run", {{"channel", channel_}, {"run", n + 1}}));
    return;
  }
  flow_ = round2(rate);
  result_ = flow_;
  go(c, "empty", say("pumpcal.emptying", {{"channel", channel_}, {"flow", flow_}}));
}

void PumpCal::tick(Ctx& c, Actuators& act, Doser& doser) {
  if (!active()) return;
  if (c.now - lastStep_ > kCalIdleMs) return cancel(c, act, doser, say("pumpcal.idle", {{"min", kCalIdleMs / kMinute}}));
  if (pending_ && order_.empty() && !doser.busy()) launch(c, act, doser);
}

bool PumpCal::launch(Ctx& c, Actuators& act, Doser& doser) {
  const Limits lim = c.cfg.limits.bounded();
  const Ms maxRun = static_cast<Ms>(lim.maxRunS * 1000);
  DoseOrder o;
  o.id = "pcal-" + std::to_string(channel_) + "-" + std::to_string(++seq_);
  o.step.channel = channel_;
  o.step.canister = canister_;
  o.step.name = name_.empty() ? say("channel.n", {{"channel", channel_}}).text : name_;
  o.step.flowMlPerMin = nominal_;  // for the pull-back after a run, as for a dose
  // The line and a margin, backwards: more than it holds, so nothing of it stays
  const Ms lineMs = isNum(lineMl_) && lineMl_ > 0 ? static_cast<Ms>(lineMl_ * (1 + kCalLineMargin) / nominal_ * 60000.0) : kCalLineUnknownMs;
  auto split = [&](Ms total) {
    std::vector<Ms> runs;
    for (Ms left = std::max<Ms>(total, kSecond); left > 0; left -= maxRun) runs.push_back(std::min(left, maxRun));
    return runs;
  };
  if (step_ == "reverse" || step_ == "empty") {
    o.purpose = "empty";
    o.step.runs = split(lineMs);
  } else if (step_ == "prime") {
    // Until it drips steadily; without a drop sensor twice what the line holds
    o.purpose = drops_ ? "calprime" : "calrun";
    o.step.runs = drops_ ? std::vector<Ms>(kCalPrimeRuns, std::min(kCalPrimeRunMs, maxRun)) : split(2 * lineMs);
  } else if (step_ == "run") {
    o.purpose = "calrun";
    o.step.runs = {std::clamp<Ms>(static_cast<Ms>(kCalRunMl / nominal_ * 60000.0), 3 * kSecond, maxRun)};
    const auto d = act.drops(channel_);
    dropsFrom_ = d ? d->count : 0;
  } else if (step_ == "fill") {
    // Until its first drop: what falls after it is the first dose, booked as one (SD-111)
    o.purpose = "fill";
    o.step.flowMlPerMin = c.cfg.channelFlow(channel_);
    const Ms run = std::min<Ms>(60 * kSecond, maxRun);
    o.step.runs.assign(static_cast<size_t>((5 * kMinute + run - 1) / run), run);
  } else {
    return false;
  }
  Msg err;
  if (!doser.start(c, act, o, err)) {
    message_ = err;  // tried again at the next tick, until the step is left idle too long
    return false;
  }
  order_ = o.id;
  pending_ = false;
  return true;
}

void PumpCal::orderDone(Ctx& c, Actuators& act, const DoseProgress& p) {
  order_.clear();
  if (p.state != DoseProgress::State::Done) return fail(c, act, say("pumpcal.run_failed", {{"reason", p.error}}));
  if (step_ == "reverse") {
    go(c, "to_water", say(method_ == "cylinder" ? "pumpcal.to_water.cylinder" : "pumpcal.to_water.scale", {{"channel", channel_}}));
  } else if (step_ == "prime") {
    go(c, "reading", say(method_ == "cylinder" ? "pumpcal.start.cylinder" : "pumpcal.start.scale"));
  } else if (step_ == "run") {
    Run run;
    run.ms = p.msDone;  // the actual run time the driver reported (RAT-070)
    const auto d = act.drops(channel_);
    run.drops = d && d->count >= dropsFrom_ ? d->count - dropsFrom_ : 0;
    runs_.push_back(run);
    go(c, "reading", say(method_ == "cylinder" ? "pumpcal.after.cylinder" : "pumpcal.after.scale", {{"run", runs_.size()}}));
  } else if (step_ == "empty") {
    go(c, "to_canister", say("pumpcal.to_canister", {{"channel", channel_}}));
  } else if (step_ == "fill") {
    c.rt.filled.insert(channel_);
    c.rt.pumpCal = nullptr;
    act.setChannelLight(channel_, false);
    go(c, "done", say("pumpcal.done", {{"channel", channel_}, {"flow", flow_}}));
    c.log.add(c.epoch, "calibration", "info", say("ev.pumpcal.end", {{"channel", channel_}}), message_, {{"channel", channel_}, {"flowMlPerMin", flow_}});
  }
}

void PumpCal::fail(Ctx& c, Actuators& act, Msg why) {
  const bool out = intakeOut();
  if (out) {
    // The intake may be in the water: nothing doses on this channel until a person has put it back
    setLatch(c.cfg, c.rt, cutLatch(channel_), {{"at", c.epoch}, {"channel", channel_}, {"key", "channel.cal_cut"}});
  }
  c.rt.pumpCal = nullptr;
  act.setChannelLight(channel_, false);
  pending_ = false;
  step_ = "failed";
  message_ = std::move(why);
  c.log.add(c.epoch, "calibration", out ? "warn" : "info", say("ev.pumpcal.end", {{"channel", channel_}}), message_, {{"channel", channel_}});
}

void PumpCal::cancel(Ctx& c, Actuators& act, Doser& doser, const Msg& why) {
  if (!active()) return;
  if (!order_.empty() && doser.busy()) doser.abort(c, act, why);
  order_.clear();
  fail(c, act, why);
  step_ = "cancelled";
}

json PumpCal::toJson() const {
  if (channel_ <= 0) return nullptr;
  json runs = json::array();
  for (const auto& r : runs_) runs.push_back({{"s", r.ms / 1000.0}, {"drops", r.drops}, {"flowMlPerMin", numOrNull(r.flow)}});
  return {{"channel", channel_},
          {"method", method_},
          {"step", step_},
          {"waiting", active() && waiting()},
          {"readings", readings_},
          {"runs", runs},
          {"flowMlPerMin", numOrNull(flow_)},
          {"nominalMlPerMin", numOrNull(nominal_)},
          {"message", message_},
          {"at", at_}};
}

}  // namespace doseph
