// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/lighting.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/config.hpp"
#include "doseph/gateway.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {

constexpr int kDayS = 24 * 3600;

// What was switched by hand and still holds: {on, day, until}; nothing otherwise. It holds until the
// plan's next change: the plan's day or night, as it is now, is no longer the one it was switched in
// (also after a change of the plan or the phase), or its time has come.
std::optional<json> handOf(const RuntimeState& rt, const std::string& id, Epoch now, bool dayNow) {
  if (!rt.lighting.is_object() || !rt.lighting.contains(id)) return std::nullopt;
  const json& h = rt.lighting[id];
  if (!h.is_object() || !h.contains("on") || !h["on"].is_boolean()) return std::nullopt;
  if (now >= static_cast<Epoch>(jnum(h, "until", 0))) return std::nullopt;
  if (h.contains("day") && h["day"].is_boolean() && h["day"].get<bool>() != dayNow) return std::nullopt;
  return h;
}

// A light on by hand when its plan has it off: at its brightness by day, the driver's threshold kept
double handPct(const LightCircuitCfg& e, const LightPoint& p) {
  return p.pct > 0 ? p.pct : std::max({e.pct, isNum(e.thresholdPct) ? e.thresholdPct : 0.0, 1.0});
}

}  // namespace

LightPoint lightAt(const LightCircuitCfg& l, int secondOfDay) {
  LightPoint p;
  // Hours without a value are refused by the validation; were one here, no day is the safe side
  const int length = static_cast<int>(std::lround(std::clamp(isNum(l.hours) ? l.hours : 0.0, 0.0, 24.0) * 3600.0));
  if (length <= 0) {
    p.changeInS = kDayS;
    return p;
  }
  const int t = ((secondOfDay - l.startMin * 60) % kDayS + kDayS) % kDayS;
  p.day = length >= kDayS || t < length;
  p.changeInS = length >= kDayS ? kDayS : p.day ? length - t : kDayS - t;
  if (!p.day) return p;
  if (!l.dimmed) {
    p.on = true;
    return p;
  }
  // The ramps lie within the day: up from its start, down to its end
  double frac = 1;
  if (length < kDayS) {
    const int rise = l.sunriseMin * 60, set = l.sunsetMin * 60;
    if (rise > 0 && t < rise) frac = std::min(frac, (t + 1.0) / rise);
    if (set > 0 && length - t <= set) frac = std::min(frac, static_cast<double>(length - t) / set);
  }
  double pct = std::clamp(l.pct, 0.0, 100.0) * frac;
  // Never below the driver's threshold, where the lamps stay dark (RAT-013), nor below 1 %
  pct = std::max({pct, isNum(l.thresholdPct) ? l.thresholdPct : 0.0, 1.0});
  p.on = true;
  p.pct = std::round(pct * 10.0) / 10.0;
  return p;
}

const PhaseCfg* followedLightPhase(const Config& cfg, const LightCircuitCfg& l) {
  if (l.plant.empty()) return nullptr;
  const PlantCfg* p = cfg.plant(l.plant);
  if (!p || !p->running() || p->phase < 0 || p->phase >= static_cast<int>(p->phases.size())) return nullptr;
  return &p->phases[static_cast<size_t>(p->phase)];
}

LightCircuitCfg effectiveLight(const Config& cfg, const LightCircuitCfg& l) {
  LightCircuitCfg e = l;
  if (const PhaseCfg* ph = followedLightPhase(cfg, l)) {
    if (ph->lightHours) e.hours = *ph->lightHours;
    if (ph->lightPct) e.pct = *ph->lightPct;
  }
  return e;
}

void Lighting::tick(Ctx& c, int secondOfDay, NetOutputs& out, const LightCaps& caps) {
  if (!c.rt.lighting.is_object()) c.rt.lighting = json::object();
  // The hand switches of circuits gone
  for (auto it = c.rt.lighting.begin(); it != c.rt.lighting.end();) {
    if (!c.cfg.lighting.circuit(it.key())) it = c.rt.lighting.erase(it);
    else ++it;
  }
  for (const auto& l : c.cfg.lighting.circuits) {
    const LightCircuitCfg e = effectiveLight(c.cfg, l);
    const LightPoint p = lightAt(e, secondOfDay);
    bool on = p.on;
    double pct = p.pct;
    if (const auto h = handOf(c.rt, l.id, c.epoch, p.day)) {
      on = (*h)["on"].get<bool>();
      if (on) pct = handPct(e, p);
    } else {
      c.rt.lighting.erase(l.id);  // past the plan's next change: by the plan again
    }
    // Heat protection, also over a switch by hand: dimmed, never below the driver's threshold, or off
    if (const auto cap = caps.find(l.id); cap != caps.end()) {
      if (cap->second.off) on = false;
      else if (l.dimmed && on) pct = std::max({std::min(pct, cap->second.pct), isNum(e.thresholdPct) ? e.thresholdPct : 0.0, 1.0});
    }
    out.want(lightRole(l.id), on);
    if (l.dimmed && on) out.wantLevel(lightRole(l.id), pct);
  }
}

bool Lighting::hand(Ctx& c, const std::string& circuit, std::optional<bool> on, int secondOfDay) {
  const LightCircuitCfg* l = c.cfg.lighting.circuit(circuit);
  if (!l) return false;
  if (!c.rt.lighting.is_object()) c.rt.lighting = json::object();
  if (!on) {
    c.rt.lighting.erase(circuit);
    c.log.add(c.epoch, "control", "info", say("ev.light.plan", {{"name", l->name}}));
    return true;
  }
  const LightPoint p = lightAt(effectiveLight(c.cfg, *l), secondOfDay);
  c.rt.lighting[circuit] = {{"on", *on}, {"day", p.day}, {"until", c.epoch + p.changeInS}};
  // A day of 0 or 24 h has no change: a hand switch then holds a day
  const char* until = p.changeInS >= kDayS ? "ev.light.for_a_day" : p.day ? "ev.light.until_end" : "ev.light.until_start";
  c.log.add(c.epoch, "control", "info", say(*on ? "ev.light.hand_on" : "ev.light.hand_off", {{"name", l->name}}), say(until),
            {{"circuit", circuit}, {"until", c.epoch + p.changeInS}});
  return true;
}

void Lighting::forget(RuntimeState& rt, const std::string& circuit) {
  if (rt.lighting.is_object()) rt.lighting.erase(circuit);
}

json Lighting::status(const Ctx& c, int secondOfDay, const json& outputs, const LightCaps& caps) {
  json out = json::array();
  for (const auto& l : c.cfg.lighting.circuits) {
    const LightCircuitCfg e = effectiveLight(c.cfg, l);
    const LightPoint p = lightAt(e, secondOfDay);
    const auto h = handOf(c.rt, l.id, c.epoch, p.day);
    const auto cap = caps.find(l.id);
    const bool capped = cap != caps.end();
    const bool on = (h ? (*h)["on"].get<bool>() : p.on) && !(capped && cap->second.off);
    double pct = !on ? 0.0 : h ? handPct(e, p) : p.pct;
    if (capped && on) pct = std::max({std::min(pct, cap->second.pct), isNum(e.thresholdPct) ? e.thresholdPct : 0.0, 1.0});
    json plant = nullptr;
    if (!l.plant.empty())
      if (const PlantCfg* pl = c.cfg.plant(l.plant)) {
        const PhaseCfg* ph = followedLightPhase(c.cfg, l);
        plant = {{"id", pl->id}, {"name", pl->name}, {"phase", ph ? json(ph->name) : json(nullptr)}, {"hours", e.hours}, {"pct", e.pct}};
      }
    const std::string role = lightRole(l.id);
    const json output = outputs.contains(role) ? outputs[role] : json(nullptr);
    out.push_back({{"id", l.id},
                   {"day", p.day},
                   {"on", on},
                   {"pct", !l.dimmed ? json(nullptr) : json(pct)},
                   {"hours", e.hours},
                   {"nextChangeAt", p.changeInS < kDayS ? json(c.epoch + p.changeInS) : json(nullptr)},
                   {"hand", h ? json{{"on", (*h)["on"]}, {"until", (*h)["until"]}} : json(nullptr)},
                   {"darkOn", h && on && !p.day},
                   {"plant", plant},
                   {"heat", capped ? json{{"off", cap->second.off}, {"pct", cap->second.pct}} : json(nullptr)},
                   {"output", output}});
  }
  return out;
}

}  // namespace doseph
