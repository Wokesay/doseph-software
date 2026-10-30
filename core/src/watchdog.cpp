// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/watchdog.hpp"

#include <cmath>

#include "doseph/coupling.hpp"
#include "doseph/faults.hpp"
#include "doseph/messages.hpp"
#include "doseph/plants.hpp"

namespace doseph {

void to_json(json& j, const WatchResult& w) {
  json items = json::array();
  for (const auto& a : w.items) items.push_back({{"id", a.id}, {"label", a.label}, {"status", a.status}, {"text", a.text}});
  j = {{"evaluatedAt", w.evaluatedAt}, {"overall", w.overall}, {"ok", w.ok}, {"problems", w.problems},
       {"neutral", w.neutral},         {"headline", w.headline}, {"items", items}};
}

namespace {

const Reading* find(const WatchInput& in, const char* signal) {
  for (const auto& r : in.readings)
    if (r.signal == signal) return &r;
  return nullptr;
}

}  // namespace

WatchResult evaluate(const WatchInput& in) {
  WatchResult w;
  w.evaluatedAt = in.epoch;
  auto add = [&](std::string id, Msg label, std::string status, Msg text) {
    w.items.push_back({std::move(id), std::move(label), std::move(status), std::move(text)});
  };
  // The water plan: the leading plant's phase, else the recipe chosen by hand (SD-128)
  const WaterPlan plan = waterPlan(in.cfg, in.epoch);
  const RecipeCfg* recipe = plan.recipe;
  const bool ended = recipe && plan.ended;
  const bool inForce = recipe && !ended;

  // The probes: a reading not delivered is a problem, never neutral (RAT-015); "not
  // calibrated" is one only while a recipe needs it, else neutral (RAT-021)
  for (const char* ch : {"ph", "ec", "temp"}) {
    const Reading* r = find(in, ch);
    const Msg label = say(std::string("watch.") + ch);
    if (!r) continue;
    if (r->quality == Quality::Ok) add(std::string("probe.") + ch, label, "ok", say("watch.reading_ok"));
    else if (r->quality == Quality::Uncalibrated && !inForce) add(std::string("probe.") + ch, label, "neutral", r->reason);
    else add(std::string("probe.") + ch, label, "problem", r->reason);
  }
  // Coupled (SD-131): the link, and the check of the readings against the other device's
  if (in.peer.coupled) {
    const Msg label = say("watch.coupling");
    if (!in.peer.linkOk)
      add("coupling.link", label, "problem", say(in.peer.leader ? "watch.coupling.follower_lost" : "watch.coupling.leader_lost", {{"name", in.peer.name}}));
    else if (in.peer.deviation)
      add("coupling.deviation", label, "problem", say("watch.coupling.deviation", {{"name", in.peer.name}, {"ph", kPeerDevPh}, {"ec", kPeerDevEc}}));
    else
      add("coupling.link", label, "ok", say("watch.coupling.ok", {{"name", in.peer.name}}));
  }
  // Network devices (SD-132): a bound role that does not work is a problem (RAT-024)
  for (const auto& r : in.roles) add("role." + r.id, r.label.key.empty() ? say("role." + r.id) : r.label, r.ok ? "ok" : "problem", r.text);
  // Climate (SD-136): CO2 the valve was closed for, and light heat protection holds back
  for (const auto& r : in.rooms) {
    const Msg label = say("watch.climate", {{"name", r.name}});
    if (r.co2High) add("climate." + r.id + ".co2", label, "problem", say("watch.climate.co2_high", {{"name", r.name}, {"max", kCo2CutPpm}}));
    if (r.heat > 0)
      add("climate." + r.id + ".heat", label, "problem", say(r.heat == 2 ? "watch.climate.heat_off" : "watch.climate.heat_dim", {{"name", r.name}, {"limit", r.limitC}}));
  }
  if (const Reading* f = find(in, "flow")) {
    const Msg label = say("watch.flow");
    if (in.stopped || !in.needsFlow) add("flow", label, "neutral", say("watch.flow_not_needed"));
    else if (f->usable() && *f->value >= 0.5) add("flow", label, "ok", say("watch.flow_ok", {{"flow", *f->value}}));
    else add("flow", label, "problem", f->usable() ? say("watch.flow_none") : f->reason);
  }

  // The targets from the cycle's last stable measurement, with the wider alarm band (RAT-033)
  const bool grace = in.bootEpoch > 0 && in.epoch - in.bootEpoch < kStartupGraceS;
  const Epoch maxAge = static_cast<Epoch>((isNum(in.cfg.tank.holdMin) ? in.cfg.tank.holdMin : 60) * 60) + 30 * 60;
  auto checkTarget = [&](const char* id, const char* labelKey, double value, double want, double tol, int decimals) {
    const Msg label = say(labelKey);
    if (in.stopped) return add(id, label, "neutral", say("watch.stopped"));
    if (in.maintenance) return add(id, label, "neutral", say("watch.maintenance"));
    if (!inForce) return add(id, label, "neutral", say("watch.no_recipe"));
    if (!isNum(want)) return add(id, label, "neutral", say("watch.no_target"));
    if (grace) return add(id, label, "neutral", say("watch.grace"));
    if (in.lastAt == 0 || in.epoch - in.lastAt > maxAge || !isNum(value)) return add(id, label, "neutral", say("watch.no_measurement"));
    const double band = tol + kAlarmMargin;
    if (std::fabs(value - want) <= band)
      return add(id, label, "ok", say("watch.target_ok", {{"value", value}, {"target", want}, {"d", decimals}}));
    add(id, label, "problem", say(value > want ? "watch.target_high" : "watch.target_low", {{"value", value}, {"target", want}, {"band", band}}));
  };
  checkTarget("target.ec", "watch.target_ec", in.lastEc, plan.ecTarget, std::max(0.05, in.cfg.control.ecTolerance), 2);
  checkTarget("target.ph", "watch.target_ph", in.lastPh, plan.phTarget, std::max(0.05, in.cfg.control.phTolerance), 2);

  // Latches hold until a person releases them (RAT-062, RAT-063)
  // A latch's text is its own, or that of its kind where the ID names more (a channel's, SD-111)
  // One set while its lock reported only is shown as reported: it alarms the same, and holds nothing (SD-133)
  for (const auto& [id, l] : in.rt.latches) {
    const std::string kind = l.is_object() ? jstr(l, "key", id) : id;
    if (id == kSafetyRestartNote) add("latch." + id, say("watch.hw"), "problem", say("latch." + kind, l.is_object() ? l : json::object()));
    else if (holds(in.rt, id)) add("latch." + id, say("watch.latch"), "problem", say("latch." + kind, l.is_object() ? l : json::object()));
    else add("latch." + id, say("watch.reported"), "problem", reportedEvent(kind, l));
  }
  // Locks set to report only are a note, not a problem: no alarm, and the problem flag stays free
  // for real faults (decided by the owner, SD-133)
  if (!in.cfg.locks.reportOnly.empty()) {
    json locks = json::array();
    for (const auto& kind : in.cfg.locks.reportOnly) locks.push_back(say("lock." + kind));
    add("locks.report_only", say("watch.locks"), "neutral", say("watch.report_only", {{"locks", locks}}));
  }
  // The device's protection (SD-141): a fault that holds while reported, and a warning; a latched
  // one is listed with the latches
  for (const auto& f : in.faults) {
    const FaultInfo i = classifyFault(f);
    if (i.kind == FaultInfo::Kind::Latch || i.reason == "safety_restart") continue;
    add(i.id(), say("watch.hw"), "problem", say("latch.hw." + i.reason, faultArgs(i, f)));
  }
  // The hardware release, where no latch already says it (SD-110)
  if (in.hold == "stop_button" || in.hold == "release") add("release", say("watch.release"), "problem", say("watch.hold." + in.hold));
  if (in.rt.topUp) add("water", say("watch.water"), "problem", say("watch.top_up"));  // SD-115
  if (in.rt.waterLow) add("water.low", say("watch.watering"), "problem", say("watch.water_low"));  // SD-134

  // The recipe in force, the volume and the channels it needs (RAT-015)
  if (!recipe) add("recipe", say("watch.recipe"), "neutral", say("watch.no_recipe"));
  else if (ended) add("recipe", say("watch.recipe"), "problem", say("watch.recipe_ended", {{"name", recipe->name}, {"until", plan.until}}));
  else add("recipe", say("watch.recipe"), "ok", say("watch.recipe_ok", {{"name", recipe->name}}));
  if (inForce) {
    if (!isNum(in.cfg.tank.volumeL)) add("tank.volume", say("watch.volume"), "problem", say("watch.no_volume"));
    std::vector<const CanisterCfg*> needed;
    for (const auto& s : recipe->steps)
      if (const CanisterCfg* b = in.cfg.canister(s.canister)) needed.push_back(b);
    if (isNum(plan.phTarget)) {
      const CanisterCfg* down = in.cfg.phDown();
      if (!down) add("canister.ph_down", say("watch.channels"), "problem", say("watch.no_ph_down"));
      else needed.push_back(down);
    }
    for (const CanisterCfg* b : needed) {
      if (b->channel <= 0 || (b->device.empty() && b->channel > in.channelCount)) add("canister." + b->id, say("watch.channels"), "problem", say("watch.no_channel", {{"name", b->name}}));
      else if (!b->device.empty() && b->device != in.cfg.peerId)
        add("canister." + b->id, say("watch.channels"), "problem", say("watch.canister_peer", {{"name", b->name}}));
      else if (!isNum(in.cfg.canisterFlow(*b))) add("canister." + b->id, say("watch.channels"), "problem", say("watch.channel_uncalibrated", {{"name", b->name}, {"channel", b->channel}}));
    }
  }
  // Canister levels counted from use, where known (SD-119, RAT-071)
  for (const auto& b : in.cfg.canisters) {
    const std::string st = stockStatus(b, in.rt);
    if (st == "unknown") continue;
    const double ml = in.rt.stockMl.at(b.id);
    const Msg label = say("watch.stock", {{"name", b.name}});
    if (st == "empty") add("stock." + b.id, label, "problem", say("watch.stock_empty", {{"ml", ml}}));
    else if (st == "low") add("stock." + b.id, label, "problem", say("watch.stock_low", {{"ml", ml}}));
    else add("stock." + b.id, label, "ok", say("watch.stock_ok", {{"ml", ml}}));
  }
  if (in.stopped) add("stop", say("watch.stop"), "neutral", say("watch.stopped"));

  for (const auto& a : w.items) {
    if (a.status == "ok") w.ok++;
    else if (a.status == "problem") w.problems++;
    else w.neutral++;
  }
  w.overall = w.problems > 0 ? "problem" : w.ok > 0 ? "ok" : "neutral";
  // Normal states are counted, only deviations are named (RAT-072)
  if (w.problems > 0) {
    for (const auto& a : w.items)
      if (a.status == "problem") {
        w.headline = w.problems == 1 ? a.text : say("watch.headline_more", {{"first", a.text}, {"more", w.problems - 1}});
        break;
      }
  } else {
    w.headline = say("watch.headline_ok", {{"n", w.ok}});
  }
  return w;
}

}  // namespace doseph
