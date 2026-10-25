// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/watchdog.hpp"

#include <cmath>

#include "doseph/messages.hpp"

namespace doseph {

void to_json(json& j, const WatchResult& w) {
  json items = json::array();
  for (const auto& a : w.items) items.push_back({{"id", a.id}, {"label", a.label}, {"status", a.status}, {"text", a.text}});
  j = {{"evaluatedAt", w.evaluatedAt}, {"overall", w.overall}, {"ok", w.ok}, {"problems", w.problems},
       {"neutral", w.neutral},         {"headline", w.headline}, {"items", items}};
}

namespace {

const Reading* find(const WatchInput& in, const char* channel) {
  for (const auto& r : in.readings)
    if (r.channel == channel) return &r;
  return nullptr;
}

}  // namespace

WatchResult evaluate(const WatchInput& in) {
  WatchResult w;
  w.evaluatedAt = in.epoch;
  auto add = [&](std::string id, Msg label, std::string status, Msg text) {
    w.items.push_back({std::move(id), std::move(label), std::move(status), std::move(text)});
  };
  const RecipeCfg* recipe = in.cfg.active.recipe.empty() ? nullptr : in.cfg.recipe(in.cfg.active.recipe);
  const bool ended = recipe && in.cfg.active.until != 0 && in.epoch >= in.cfg.active.until;
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
    if (!inForce) return add(id, label, "neutral", say("watch.no_recipe"));
    if (!isNum(want)) return add(id, label, "neutral", say("watch.no_target"));
    if (grace) return add(id, label, "neutral", say("watch.grace"));
    if (in.lastAt == 0 || in.epoch - in.lastAt > maxAge || !isNum(value)) return add(id, label, "neutral", say("watch.no_measurement"));
    const double band = tol + kAlarmMargin;
    if (std::fabs(value - want) <= band)
      return add(id, label, "ok", say("watch.target_ok", {{"value", value}, {"target", want}, {"d", decimals}}));
    add(id, label, "problem", say(value > want ? "watch.target_high" : "watch.target_low", {{"value", value}, {"target", want}, {"band", band}}));
  };
  checkTarget("target.ec", "watch.target_ec", in.lastEc, recipe ? recipe->ecTarget : kNaN, std::max(0.05, in.cfg.control.ecTolerance), 2);
  checkTarget("target.ph", "watch.target_ph", in.lastPh, recipe ? phTarget(*recipe) : kNaN, std::max(0.05, in.cfg.control.phTolerance), 2);

  // Latches hold until a person releases them (RAT-062, RAT-063)
  for (const auto& [id, l] : in.rt.latches) add("latch." + id, say("watch.latch"), "problem", say("latch." + id, l.is_object() ? l : json::object()));

  // The recipe in force, the volume and the pumps it needs (RAT-015)
  if (!recipe) add("recipe", say("watch.recipe"), "neutral", say("watch.no_recipe"));
  else if (ended) add("recipe", say("watch.recipe"), "problem", say("watch.recipe_ended", {{"name", recipe->name}, {"until", in.cfg.active.until}}));
  else add("recipe", say("watch.recipe"), "ok", say("watch.recipe_ok", {{"name", recipe->name}}));
  if (inForce) {
    if (!isNum(in.cfg.tank.volumeL)) add("tank.volume", say("watch.volume"), "problem", say("watch.no_volume"));
    std::vector<const BottleCfg*> needed;
    for (const auto& s : recipe->steps)
      if (const BottleCfg* b = in.cfg.bottle(s.bottle)) needed.push_back(b);
    if (isNum(phTarget(*recipe))) {
      const BottleCfg* down = in.cfg.phDown();
      if (!down) add("bottle.ph_down", say("watch.pumps"), "problem", say("watch.no_ph_down"));
      else needed.push_back(down);
    }
    for (const BottleCfg* b : needed) {
      if (b->pump <= 0 || b->pump > in.pumpCount) add("bottle." + b->id, say("watch.pumps"), "problem", say("watch.no_pump", {{"name", b->name}}));
      else if (!isNum(in.cfg.pumpFlow(b->pump))) add("bottle." + b->id, say("watch.pumps"), "problem", say("watch.pump_uncalibrated", {{"name", b->name}, {"pump", b->pump}}));
    }
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
