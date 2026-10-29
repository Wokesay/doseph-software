// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/climate.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/config.hpp"
#include "doseph/devices.hpp"
#include "doseph/gateway.hpp"
#include "doseph/lighting.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {

// The bands around a target, and the shortest time between two switchings of one output (SD-136)
constexpr double kTempBandC = 1, kRhBandPct = 5, kCo2BandPpm = 100;
constexpr Ms kSwitchGapMs = kMinute;
// A compressor dehumidifier starts again only this long after it went off (RAT-034)
constexpr Ms kDehumidifierRestMs = 5 * kMinute;
// The CO2 valve opens for a minute at most, then rests a minute; its device closes it on its own (RAT-019)
constexpr Ms kCo2PulseMs = kMinute;
// Readings outside these are no value (R5): an assumption of what a room can hold
constexpr double kTempMinC = -20, kTempMaxC = 60, kCo2MaxPpm = 10000;
// Two readings of the robot's own sensors further apart than this give no VPD (RAT-017)
constexpr Ms kVpdPairMs = kMinute;

// A sensor of a room: whether it is bound, and its value where there is one that can be (R5)
struct Air {
  bool bound = false;
  std::optional<double> v;
  Ms at = 0;
  bool mqtt = false;
};

Air air(const Ctx& c, const NetDevices& dev, const std::string& role, double lo, double hi) {
  Air a;
  const auto b = c.cfg.roles.find(role);
  if (b == c.cfg.roles.end()) return a;
  a.bound = true;
  a.mqtt = b->second.source == "mqtt";
  const RoleValue rv = dev.value(c.cfg, role, c.now);
  if (rv.state != "ok" || !rv.value || !rv.value->is_number()) return a;
  const double x = rv.value->get<double>();
  if (!std::isfinite(x) || x < lo || x > hi) return a;
  a.v = x;
  a.at = rv.at;
  return a;
}

// Whether a room's output is off at its device, or has none (SD-136, RAT-034): an output that does
// not answer may be on
bool knownOff(const Ctx& c, const NetDevices& dev, const std::string& role) {
  const auto b = c.cfg.roles.find(role);
  if (b == c.cfg.roles.end()) return true;
  return dev.output(b->second.device, b->second.channel, c.now) == false;
}

std::optional<double> vpdOf(const Air& t, const Air& h) {
  if (!t.v || !h.v) return std::nullopt;
  // A sensor that reports on change has no age by its report: its own freshness counts (RAT-017)
  if (!t.mqtt && !h.mqtt && std::llabs(t.at - h.at) > kVpdPairMs) return std::nullopt;
  return airVpdKpa(*t.v, *h.v);
}

json orNull(const std::optional<double>& v, int decimals) {
  if (!v) return nullptr;
  const double f = std::pow(10.0, decimals);
  return std::round(*v * f) / f;
}

}  // namespace

double saturationKpa(double tempC) { return 0.6108 * std::exp(17.27 * tempC / (tempC + 237.3)); }

double airVpdKpa(double tempC, double rhPct) { return saturationKpa(tempC) * (1.0 - rhPct / 100.0); }

double rhForVpd(double vpdKpa, double tempC) { return std::clamp(100.0 * (1.0 - vpdKpa / saturationKpa(tempC)), 0.0, 100.0); }

const PhaseCfg* followedClimatePhase(const Config& cfg, const ClimateRoomCfg& r) {
  if (r.plant.empty()) return nullptr;
  const PlantCfg* p = cfg.plant(r.plant);
  if (!p || !p->running() || p->phase < 0 || p->phase >= static_cast<int>(p->phases.size())) return nullptr;
  return &p->phases[static_cast<size_t>(p->phase)];
}

ClimateRoomCfg effectiveRoom(const Config& cfg, const ClimateRoomCfg& r) {
  ClimateRoomCfg e = r;
  if (const PhaseCfg* ph = followedClimatePhase(cfg, r)) {
    if (ph->dayC) e.dayC = *ph->dayC;
    if (ph->nightC) e.nightC = *ph->nightC;
    // The phase's humidity or VPD leads, whichever it has (RAT-009)
    if (ph->vpdKpa) {
      e.byVpd = true;
      e.vpdKpa = *ph->vpdKpa;
    } else if (ph->rhPct) {
      e.byVpd = false;
      e.rhPct = *ph->rhPct;
    }
    if (ph->co2Ppm) e.co2Ppm = *ph->co2Ppm;
  }
  return e;
}

ClimateTargets climateTargets(const Config& cfg, const ClimateRoomCfg& r, int secondOfDay) {
  const ClimateRoomCfg e = effectiveRoom(cfg, r);
  ClimateTargets t;
  // The room's day: its first light circuit's, as its plan lights it; else its own
  const LightCircuitCfg* l = r.lights.empty() ? nullptr : cfg.lighting.circuit(r.lights.front());
  if (l) {
    t.day = lightAt(effectiveLight(cfg, *l), secondOfDay).day;
  } else {
    LightCircuitCfg own;
    own.startMin = e.startMin;
    own.hours = e.hours;
    t.day = lightAt(own, secondOfDay).day;
  }
  t.tempC = t.day ? e.dayC : e.nightC;
  // A VPD target gives the humidity from the target temperature, not the measured one (RAT-009)
  if (e.byVpd && isNum(t.tempC)) {
    t.vpdKpa = e.vpdKpa;
    t.rhPct = rhForVpd(e.vpdKpa, t.tempC);
  } else {
    t.rhPct = e.rhPct;
  }
  if (t.day) t.co2Ppm = e.co2Ppm;
  return t;
}

void Climate::start(Ms now) {
  startMs_ = now;
  rooms_.clear();
}

void Climate::tick(Ctx& c, int secondOfDay, const NetDevices& dev, NetOutputs& out) {
  // Rooms gone
  for (auto it = rooms_.begin(); it != rooms_.end();) it = c.cfg.climate.room(it->first) ? std::next(it) : rooms_.erase(it);
  const bool maintenance = c.maintenanceUntil > c.epoch;
  for (const auto& r : c.cfg.climate.rooms) {
    const ClimateTargets t = climateTargets(c.cfg, r, secondOfDay);
    const bool always = effectiveRoom(c.cfg, r).circulateAlways;
    const Air temp = air(c, dev, climateRole(r.id, "temp"), kTempMinC, kTempMaxC);
    const Air rh = air(c, dev, climateRole(r.id, "rh"), 0, 100);
    const Air co2 = air(c, dev, climateRole(r.id, "co2"), 0, kCo2MaxPpm);
    const bool fresh = rooms_.find(r.id) == rooms_.end();
    Room& st = rooms_[r.id];
    // After a start the dehumidifier rests its 5 min first (RAT-034)
    if (fresh) st.outs["dehumidifier"].changedAt = startMs_;
    // A regulated switching waits its gap; one to the safe side never does
    auto set = [&](const char* kind, bool on, bool now = false) {
      Out& o = st.outs[kind];
      if (o.on == on) return;
      if (!now && c.now - o.changedAt < kSwitchGapMs) return;
      o.on = on;
      o.changedAt = c.now;
      o.onSince = on ? c.now : -1;
    };
    auto on = [&](const char* kind) { return st.outs[kind].on; };

    // Exhaust: too warm or too humid; on where a value it needs is missing, which does no harm
    if ((temp.bound && !temp.v) || (rh.bound && !rh.v)) {
      set("exhaust", true, true);
    } else {
      const bool hot = temp.v && *temp.v >= t.tempC + kTempBandC, humid = rh.v && *rh.v >= t.rhPct + kRhBandPct;
      const bool cool = !temp.v || *temp.v <= t.tempC, dry = !rh.v || *rh.v <= t.rhPct;
      if (hot || humid) set("exhaust", true);
      else if (cool && dry) set("exhaust", false);
    }
    // Circulation: by day, or always where set
    set("circulation", t.day || always);
    // The heater, which has its own thermostat, gets power when the room is too cold (RAT-069)
    if (!temp.v || c.stopped) set("heater", false, true);
    else if (*temp.v <= t.tempC - kTempBandC) set("heater", true);
    else if (*temp.v >= t.tempC) set("heater", false);
    // Humidity: never both, and neither while the other's output may be on (RAT-034)
    if (!rh.v || c.stopped) {
      set("humidifier", false, true);
      set("dehumidifier", false, true);
    } else {
      const bool wet = *rh.v >= t.rhPct + kRhBandPct, dried = *rh.v <= t.rhPct;
      const bool parched = *rh.v <= t.rhPct - kRhBandPct, wetted = *rh.v >= t.rhPct;
      if (dried || parched) set("dehumidifier", false, true);
      if (wetted || wet) set("humidifier", false, true);
      // The other one found on at its device, as by a hand there: off at once
      if (!knownOff(c, dev, climateRole(r.id, "dehumidifier"))) set("humidifier", false, true);
      if (!knownOff(c, dev, climateRole(r.id, "humidifier"))) set("dehumidifier", false, true);
      Out& de = st.outs["dehumidifier"];
      if (wet && !on("humidifier") && knownOff(c, dev, climateRole(r.id, "humidifier")) && c.now - de.changedAt >= kDehumidifierRestMs)
        set("dehumidifier", true);
      if (parched && !on("dehumidifier") && knownOff(c, dev, climateRole(r.id, "dehumidifier"))) set("humidifier", true);
    }
    // CO2: above 2,000 ppm the valve closes, with an alarm, until it is back below the highest target
    if (co2.v && *co2.v > kCo2CutPpm && !st.co2High) {
      st.co2High = true;
      c.log.add(c.epoch, "alarm", "alarm", say("ev.climate.co2_high", {{"name", r.name}, {"max", kCo2CutPpm}}),
                say("ev.climate.co2_high.text", {{"target", kCo2MaxTargetPpm}}), {{"room", r.id}, {"ppm", *co2.v}});
    } else if (st.co2High && co2.v && *co2.v <= kCo2MaxTargetPpm) {
      st.co2High = false;
    }
    // Only by day, with a target, while the exhaust fan is off, no one works in the room, nothing
    // stops and a value read since the start says it is low
    const bool co2Ok = t.day && isNum(t.co2Ppm) && !on("exhaust") && !c.stopped && !maintenance && !st.co2High && co2.v && co2.at > startMs_;
    Out& valve = st.outs["co2_valve"];
    if (!co2Ok || *co2.v >= t.co2Ppm || (valve.on && c.now - valve.onSince >= kCo2PulseMs)) set("co2_valve", false, true);
    else if (*co2.v <= t.co2Ppm - kCo2BandPpm) set("co2_valve", true);

    for (const auto& kind : climateOutputs()) out.want(climateRole(r.id, kind), on(kind.c_str()));
  }
}

json Climate::status(const Ctx& c, int secondOfDay, const NetDevices& dev, const json& outputs) const {
  json out = json::array();
  for (const auto& r : c.cfg.climate.rooms) {
    const ClimateTargets t = climateTargets(c.cfg, r, secondOfDay);
    const ClimateRoomCfg e = effectiveRoom(c.cfg, r);
    const Air temp = air(c, dev, climateRole(r.id, "temp"), kTempMinC, kTempMaxC);
    const Air rh = air(c, dev, climateRole(r.id, "rh"), 0, 100);
    const Air co2 = air(c, dev, climateRole(r.id, "co2"), 0, kCo2MaxPpm);
    const auto st = rooms_.find(r.id);
    json outs = json::object();
    for (const auto& kind : climateOutputs()) {
      const std::string role = climateRole(r.id, kind);
      bool want = false;
      if (st != rooms_.end())
        if (const auto o = st->second.outs.find(kind); o != st->second.outs.end()) want = o->second.on;
      outs[kind] = {{"want", want}, {"output", outputs.contains(role) ? outputs[role] : json(nullptr)}};
    }
    json missing = json::array();
    for (const auto& [kind, a] : {std::pair{"temp", temp}, std::pair{"rh", rh}, std::pair{"co2", co2}})
      if (a.bound && !a.v) missing.push_back(kind);
    json plant = nullptr;
    if (!r.plant.empty())
      if (const PlantCfg* pl = c.cfg.plant(r.plant)) {
        const PhaseCfg* ph = followedClimatePhase(c.cfg, r);
        plant = {{"id", pl->id}, {"name", pl->name}, {"phase", ph ? json(ph->name) : json(nullptr)}};
      }
    out.push_back({{"id", r.id},
                   {"day", t.day},
                   {"byVpd", e.byVpd},
                   {"targets", {{"tempC", orNull(isNum(t.tempC) ? std::optional(t.tempC) : std::nullopt, 1)},
                                {"rhPct", orNull(isNum(t.rhPct) ? std::optional(t.rhPct) : std::nullopt, 0)},
                                {"vpdKpa", orNull(isNum(t.vpdKpa) ? std::optional(t.vpdKpa) : std::nullopt, 2)},
                                {"co2Ppm", orNull(isNum(t.co2Ppm) ? std::optional(t.co2Ppm) : std::nullopt, 0)}}},
                   {"air", {{"tempC", orNull(temp.v, 1)}, {"rhPct", orNull(rh.v, 0)}, {"vpdKpa", orNull(vpdOf(temp, rh), 2)}, {"co2Ppm", orNull(co2.v, 0)}}},
                   {"missing", missing},
                   {"co2High", st != rooms_.end() && st->second.co2High},
                   {"outputs", outs},
                   {"plant", plant}});
  }
  return out;
}

}  // namespace doseph
