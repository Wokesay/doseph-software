// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/watering.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/config.hpp"
#include "doseph/devices.hpp"
#include "doseph/gateway.hpp"
#include "doseph/localtime.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {

// The water's limits for watering (RAT-086): a wide band that catches a gross overdose, not a missed
// recipe
constexpr double kWaterPhLow = 4.0, kWaterPhHigh = 8.5;
constexpr double kWaterEcAbove = 1.0;  // mS/cm above the EC target
constexpr double kWaterEcMax = 5.0;    // mS/cm, always

double round1(double v) { return std::round(v * 10.0) / 10.0; }

// A shot that waits rather than being skipped: it starts once this has passed
bool waits(const std::string& key) {
  return key == "water.no.other" || key == "water.no.output_busy" || key == "water.no.mixing" || key == "water.no.dosing";
}

// The tank lacks water: a refill valve is asked for some (SD-132)
bool low(const std::string& key) { return key == "water.no.reserve" || key == "water.no.empty"; }

json& section(RuntimeState& rt, const char* key) {
  if (!rt.watering.is_object()) rt.watering = json::object();
  json& s = rt.watering[key];
  if (!s.is_object()) s = json::object();
  return s;
}

// The litres given since the tank was last new or full; none before either (R5)
std::optional<double> givenL(const RuntimeState& rt) {
  if (!rt.watering.is_object() || !rt.watering.contains("givenL") || !rt.watering["givenL"].is_number()) return std::nullopt;
  return rt.watering["givenL"].get<double>();
}

double dayRunS(const RuntimeState& rt, const std::string& date, const std::string& circuit) {
  if (!rt.watering.is_object() || !rt.watering.contains("day")) return 0;
  const json& d = rt.watering["day"];
  if (jstr(d, "date") != date || !d.contains("runS") || !d["runS"].is_object()) return 0;
  return jnum(d["runS"], circuit.c_str(), 0);
}

// A shot's litres and run time, booked to the count and to the local day it began (SD-134). Water
// of a circuit whose flow is not known leaves the count without a value (R5).
void book(RuntimeState& rt, const std::string& circuit, const std::string& date, double s, double l) {
  if (const auto g = givenL(rt)) {
    if (isNum(l)) rt.watering["givenL"] = *g + l;
    else if (s > 0) rt.watering["givenL"] = nullptr;
  }
  json& day = section(rt, "day");
  if (jstr(day, "date") != date) {
    // A shot from the day before leaves today's run time alone
    if (jstr(day, "date") > date) return;
    day = {{"date", date}, {"runS", json::object()}};
  }
  if (!day["runS"].is_object()) day["runS"] = json::object();
  day["runS"][circuit] = jnum(day["runS"], circuit.c_str(), 0) + s;
}

double reserveL(const Config& cfg, const WaterIn& in) {
  return isNum(in.volumeL) ? in.volumeL * cfg.watering.reservePct / 100.0 : kNaN;
}

double litres(double rateLpm, double s) { return isNum(rateLpm) ? rateLpm * s / 60.0 : kNaN; }
double litres(const WaterCircuitCfg& w, double s) { return litres(w.rateLpm, s); }

// Why the tank cannot give `takeL` more litres now: an empty float, a level or the count below the
// reserve (RAT-067, RAT-068); nothing where it can. `counts`: the circuit's litres are known.
std::optional<Msg> tankShort(const Ctx& c, const WaterIn& in, double takeL, bool counts) {
  if (in.emptyAssigned) {
    if (!in.empty) return say("water.no.empty_float");
    if (*in.empty) return say("water.no.empty");
  }
  const double reserve = reserveL(c.cfg, in);
  const double take = isNum(takeL) ? takeL : 0;
  if (in.levelAssigned) {
    if (!in.levelL) return say("water.no.level");
    if (!isNum(reserve)) return say("water.no.volume");
    if (*in.levelL - take < reserve) return say("water.no.reserve", {{"reserve", round1(reserve)}});
    return std::nullopt;
  }
  if (counts && !isNum(reserve) && !in.emptyAssigned) return say("water.no.volume");
  if (counts && isNum(reserve)) {
    // The count starts with a new tank or the float "full"; before, it tells nothing (R5)
    const auto given = givenL(c.rt);
    if (!given) return in.emptyAssigned ? std::nullopt : std::optional<Msg>(say("water.no.count_start"));
    if (in.volumeL - *given - take < reserve) return say("water.no.reserve", {{"reserve", round1(reserve)}});
    return std::nullopt;
  }
  if (!in.emptyAssigned) return say("water.no.count");  // nothing tells how much water there is
  return std::nullopt;
}

// Water is low: a refill valve is asked to top up (SD-132), once until the tank has enough again
void raiseLow(Ctx& c, const Msg& why) {
  if (!low(why.key) || c.rt.waterLow) return;
  c.rt.waterLow = true;
  c.log.add(c.epoch, "control", "warn", say("ev.water.low"), why);
}

}  // namespace

std::optional<double> shotAt(const WaterCircuitCfg& w, int m) {
  if (w.mode == "window") {
    // Spread evenly from the window's start to its end; over the whole day without an end
    const int span = ((w.toMin - w.fromMin) % 1440 + 1440) % 1440;
    const int n = w.shotsPerDay;
    for (int k = 0; k < n; ++k) {
      const long long off = span == 0 ? static_cast<long long>(k) * 1440 / n : n == 1 ? 0 : static_cast<long long>(k) * span / (n - 1);
      if ((w.fromMin + off) % 1440 == m) return w.shotS;
    }
    return std::nullopt;
  }
  if (w.mode == "times") {
    for (const auto& t : w.times)
      if (t.atMin == m) return t.s;
    return std::nullopt;
  }
  if (w.mode == "interval" && w.everyMin > 0 && m % w.everyMin == 0) return w.shotS;
  return std::nullopt;
}

const PhaseCfg* followedPhase(const Config& cfg, const WaterCircuitCfg& w) {
  if (w.plant.empty() || w.mode != "window") return nullptr;
  const PlantCfg* p = cfg.plant(w.plant);
  if (!p || !p->running() || p->phase < 0 || p->phase >= static_cast<int>(p->phases.size())) return nullptr;
  return &p->phases[static_cast<size_t>(p->phase)];
}

WaterCircuitCfg effectiveCircuit(const Config& cfg, const WaterCircuitCfg& w) {
  WaterCircuitCfg e = w;
  if (const PhaseCfg* ph = followedPhase(cfg, w)) {
    if (ph->shotsPerDay > 0) e.shotsPerDay = ph->shotsPerDay;
    if (isNum(ph->shotS)) e.shotS = std::min(ph->shotS, w.longestS);
  }
  return e;
}

std::optional<NextShot> nextShot(const WaterCircuitCfg& w, int m) {
  // In how many minutes from this one, 1 to a day; the shot of this minute was taken already
  auto ahead = [m](int at) { return (at - m + 2 * 1440 - 1) % 1440 + 1; };
  std::optional<NextShot> next;
  auto take = [&](int at, double s) {
    const int a = ahead(at);
    if (!next || a < next->inMin) next = NextShot{a, s};
  };
  if (w.mode == "window") {
    const int span = ((w.toMin - w.fromMin) % 1440 + 1440) % 1440;
    const int n = w.shotsPerDay;
    for (int k = 0; k < n; ++k) {
      const long long off = span == 0 ? static_cast<long long>(k) * 1440 / n : n == 1 ? 0 : static_cast<long long>(k) * span / (n - 1);
      take(static_cast<int>((w.fromMin + off) % 1440), w.shotS);
    }
  } else if (w.mode == "times") {
    for (const auto& t : w.times) take(t.atMin, t.s);
  } else if (w.mode == "interval" && w.everyMin > 0) {
    // The next multiple of the interval, or midnight, where the count starts again
    next = NextShot{std::min(w.everyMin - m % w.everyMin, 1440 - m), w.shotS};
  }
  return next;
}

std::optional<Msg> Watering::unfit(const WaterIn& in) {
  if (in.calibrating) return say("water.no.calibrating");
  if (in.stale || !isNum(in.ph) || !isNum(in.ec)) return say("water.no.reading");  // R5
  if (in.ph < kWaterPhLow || in.ph > kWaterPhHigh) return say("water.no.ph", {{"ph", in.ph}, {"low", kWaterPhLow}, {"high", kWaterPhHigh}});
  if (in.ec > kWaterEcMax) return say("water.no.ec_max", {{"ec", in.ec}, {"max", kWaterEcMax}});
  if (isNum(in.ecTarget) && in.ec > in.ecTarget + kWaterEcAbove)
    return say("water.no.ec", {{"ec", in.ec}, {"target", in.ecTarget}, {"above", kWaterEcAbove}});
  return std::nullopt;
}

std::optional<Msg> Watering::warning(const WaterIn& in) {
  if (in.stale || !isNum(in.ph) || !isNum(in.phTarget) || !isNum(in.phTol)) return std::nullopt;
  if (std::abs(in.ph - in.phTarget) <= in.phTol) return std::nullopt;
  return say("water.warn.ph", {{"ph", in.ph}, {"target", in.phTarget}});
}

std::optional<Msg> Watering::blocked(const Ctx& c, const WaterIn& in, const WaterCircuitCfg& w, double s, bool override) const {
  if (shot_) return say(shot_->circuit == w.id ? "water.no.running" : "water.no.other");
  if (c.stopped) return say("water.no.stopped");
  if (c.maintenanceUntil > 0) return say("water.no.maintenance");
  if (!c.hold.empty()) return say("water.no.hold");
  if (in.flowDry) return say("water.no.flow");
  if (in.dosing) return say("water.no.dosing");  // a dose would end the shot at once (RAT-086)
  const auto o = in.outputs.find(w.id);
  const std::string st = o == in.outputs.end() ? "not_assigned" : jstr(o->second, "state");
  if (st == "not_assigned") return say("water.no.output_unassigned");
  if (st == "offline" || st == "refused") return say("water.no.output");
  if (st != "off") return say("water.no.output_busy");
  if (auto why = tankShort(c, in, litres(w, s), isNum(w.rateLpm))) return why;
  if (isNum(w.dailyMaxS) && dayRunS(c.rt, in.date, w.id) + s > w.dailyMaxS) return say("water.no.daily", {{"s", w.dailyMaxS}});
  if (override) return std::nullopt;
  if (in.mixing) return say("water.no.mixing");
  return unfit(in);
}

std::optional<Msg> Watering::waterNow(Ctx& c, const WaterIn& in, const std::string& circuit, double s, bool override, NetOutputs& out) {
  const WaterCircuitCfg* w = c.cfg.watering.circuit(circuit);
  if (!w) return say("water.none");
  if (!isNum(s) || s < 1 || s > w->longestS) return say("water.no.length", {{"max", w->longestS}});
  if (auto why = blocked(c, in, *w, s, override)) {
    raiseLow(c, *why);
    return why;
  }
  // Watering anyway is a warning that says why: what is wrong with the water rather than that it is
  // still being mixed in, and whether it was (SD-134)
  const auto bad = unfit(in);
  const auto notFit = bad ? bad : in.mixing ? std::optional<Msg>(say("water.no.mixing")) : std::nullopt;
  start(c, *w, s, override && notFit.has_value(), "hand", in.date, out);
  if (shot_->override)
    c.log.add(c.epoch, "control", "warn", say("ev.water.override", {{"name", w->name}}), *notFit, {{"circuit", w->id}, {"s", s}, {"mixing", in.mixing}});
  return std::nullopt;
}

bool Watering::stop(Ctx& c, NetOutputs& out, const std::string& circuit, Msg why) {
  if (!shot_ || shot_->circuit != circuit) return false;
  end(c, out, std::move(why), false);
  return true;
}

void Watering::forget(const std::string& circuit) {
  due_.erase(circuit);
  skipped_.erase(circuit);
}

void Watering::forgetRuntime(RuntimeState& rt, const std::string& circuit) {
  if (!rt.watering.is_object()) return;
  if (rt.watering.contains("last") && rt.watering["last"].is_object()) rt.watering["last"].erase(circuit);
  if (rt.watering.contains("day") && rt.watering["day"].is_object() && rt.watering["day"].contains("runS") &&
      rt.watering["day"]["runS"].is_object())
    rt.watering["day"]["runS"].erase(circuit);
}

void Watering::start(Ctx& c, const WaterCircuitCfg& w, double s, bool override, const std::string& why, const std::string& date,
                     NetOutputs& out) {
  shot_ = Shot{w.id, w.name, w.rateLpm, c.now, -1, false, s, override, why, date};
  due_.erase(w.id);
  skipped_.erase(w.id);
  // The most it may run with the robot down: the device's own auto-off (RAT-019)
  const auto def = roleDef(c.cfg, waterRole(w.id));
  const int offS = def ? autoOffS(*def) : 0;
  section(c.rt, "shot") = {{"circuit", w.id},  {"name", w.name},  {"rateLpm", isNum(w.rateLpm) ? json(w.rateLpm) : json(nullptr)},
                           {"since", c.epoch}, {"s", s},          {"offS", offS > 0 ? json(offS) : json(nullptr)},
                           {"override", override}, {"why", why}, {"date", date}};
  out.want(waterRole(w.id), true);
}

void Watering::end(Ctx& c, NetOutputs& out, Msg how, bool warn) {
  const Shot shot = *shot_;
  shot_.reset();
  out.want(waterRole(shot.circuit), false);
  // From the switch-on, before the device said so: the count errs towards less water left. As the
  // circuit was when the shot began: a change books it as it ran.
  const double ran = shot.sentAt >= 0 ? static_cast<double>(c.now - shot.sentAt) / 1000.0 : 0;
  const double l = litres(shot.rateLpm, ran);
  book(c.rt, shot.circuit, shot.date, ran, l);
  section(c.rt, "last")[shot.circuit] = {{"at", c.epoch}, {"s", round1(ran)}, {"l", isNum(l) ? json(round1(l)) : json(nullptr)}, {"end", how}, {"override", shot.override}};
  c.rt.watering["shot"] = nullptr;
  const json name = shot.name;
  const json data = {{"circuit", shot.circuit}, {"s", round1(ran)}, {"l", isNum(l) ? json(round1(l)) : json(nullptr)}, {"override", shot.override}};
  if (warn) c.log.add(c.epoch, "control", "warn", say("ev.water.cut", {{"name", name}, {"s", round1(ran)}}), std::move(how), data);
  else c.log.add(c.epoch, "control", "info", say("ev.water.shot", {{"name", name}, {"s", round1(ran)}}), std::move(how), data);
}

void Watering::skip(Ctx& c, const WaterCircuitCfg& w, const Msg& why) {
  raiseLow(c, why);
  // Each reason once, until the circuit waters again: a schedule of short intervals is no flood
  if (skipped_[w.id] == why.key) return;
  skipped_[w.id] = why.key;
  c.log.add(c.epoch, "control", "warn", say("ev.water.skipped", {{"name", w.name}}), why, {{"circuit", w.id}});
}

void Watering::tick(Ctx& c, const WaterIn& in, NetOutputs& out) {
  // Water is no longer low once a level probe finds it well above the reserve; the count, by a new
  // tank or the float "full" (robot.cpp). A float "empty" at its line could flap.
  const double reserve = reserveL(c.cfg, in);
  if (c.rt.waterLow && in.levelAssigned && in.levelL && isNum(reserve) && *in.levelL >= reserve + in.volumeL * kWaterLowBackPct / 100.0)
    c.rt.waterLow = false;

  // The schedules, by each minute of UTC passed and the local minute it was. After a start, a clock
  // set or lost, or one that leapt further than kWaterLeapMin either way, the schedules start from
  // the new time and nothing missed is made up (R6); a clock set back less far waits until it is
  // past the last minute taken, so no shot comes twice.
  const long long minute = static_cast<long long>(c.epoch) / 60;
  const bool leapt = lastMinute_ < 0 || in.clockSecured != secured_ || std::llabs(minute - lastMinute_) > kWaterLeapMin;
  secured_ = in.clockSecured;
  if (leapt) {
    lastMinute_ = minute;
  } else if (minute > lastMinute_) {
    std::vector<WaterCircuitCfg> eff;
    for (const auto& w : c.cfg.watering.circuits) eff.push_back(effectiveCircuit(c.cfg, w));
    for (long long m = lastMinute_ + 1; m <= minute; ++m) {
      const int local = localTime(static_cast<Epoch>(m * 60), c.cfg.system.timezone).minuteOfDay;
      for (const auto& w : eff) {
        if (w.paused) continue;
        if (const auto s = shotAt(w, local)) {
          if (due_.count(w.id)) skip(c, w, say("water.no.superseded"));
          due_[w.id] = {*s, static_cast<Epoch>(m * 60)};
        }
      }
    }
    lastMinute_ = minute;
  }
  for (auto it = due_.begin(); it != due_.end();) {
    const WaterCircuitCfg* w = c.cfg.watering.circuit(it->first);
    if (!w || w->paused) {
      it = due_.erase(it);
    } else if (c.epoch - it->second.at > kWaterWaitS) {
      // A shot is no use long after its time; the next one comes by the schedule
      const WaterCircuitCfg& cw = *w;
      it = due_.erase(it);
      skip(c, cw, say("water.no.waited"));
    } else {
      ++it;
    }
  }

  if (shot_) {
    const WaterCircuitCfg* w = c.cfg.watering.circuit(shot_->circuit);
    if (!w) return end(c, out, say("water.end.removed"), true);
    const auto o = in.outputs.find(shot_->circuit);
    const json state = o == in.outputs.end() ? json::object() : o->second;
    const std::string st = jstr(state, "state", "not_assigned");
    // The shot counts from the switch-on; it holds only once the device has said "on"
    if (shot_->sentAt < 0 && (st == "on" || jbool(state, "switchedOn", false))) shot_->sentAt = c.now;
    const bool wasOpen = shot_->opened;
    if (st == "on") shot_->opened = true;
    const double ran = shot_->sentAt >= 0 ? static_cast<double>(c.now - shot_->sentAt) / 1000.0 : 0;
    // What it gave so far counts against the reserve; a level probe sees it itself
    const auto shortOf = tankShort(c, in, in.levelAssigned ? 0 : litres(shot_->rateLpm, ran), isNum(shot_->rateLpm));
    if (c.stopped) return end(c, out, say("water.end.stopped"), true);
    if (c.maintenanceUntil > 0) return end(c, out, say("water.end.maintenance"), true);
    if (!c.hold.empty()) return end(c, out, say("water.end.hold"), true);
    if (in.dosing) return end(c, out, say("water.end.dose"), true);  // a dose that starts stops it (RAT-086)
    if (in.flowDry) return end(c, out, say("water.no.flow"), true);
    if (st == "not_assigned" || st == "offline" || st == "refused") return end(c, out, say("water.end.output"), true);
    // Off at the device once it was on: its own auto-off, or a hand there
    if (wasOpen && st == "off") return end(c, out, say("water.end.output_off"), true);
    if (!shot_->override)
      if (auto why = unfit(in)) return end(c, out, *why, true);
    if (shortOf) {
      raiseLow(c, *shortOf);
      return end(c, out, *shortOf, true);
    }
    // Its length from the switch-on, whether or not the device has said "on" since
    if (shot_->sentAt >= 0 && ran >= shot_->s) return end(c, out, say("water.end.done"), false);
    if (!shot_->opened && c.now - shot_->since >= kWaterOpenMs) return end(c, out, say("water.end.not_open"), true);
    return;
  }

  // One circuit at a time, in the order of the configuration (SD-134)
  for (const auto& w : c.cfg.watering.circuits) {
    const auto d = due_.find(w.id);
    if (d == due_.end()) continue;
    const auto why = blocked(c, in, w, d->second.s, false);
    if (!why) {
      start(c, w, d->second.s, false, "schedule", in.date, out);
      return;
    }
    if (waits(why->key)) continue;
    due_.erase(d);
    skip(c, w, *why);
  }
}

bool Watering::reportCut(Ctx& c, const std::string& date) {
  if (!c.rt.watering.is_object() || !c.rt.watering.contains("shot") || !c.rt.watering["shot"].is_object()) return false;
  const json shot = c.rt.watering["shot"];
  c.rt.watering["shot"] = nullptr;
  const std::string id = jstr(shot, "circuit");
  // With the robot down, the device ran until its own auto-off at most; the clock after a start may
  // not tell how long that was. Booked so, the count errs towards less water left.
  const double ran = jnum(shot, "offS", jnum(shot, "s", 0));
  const double rate = shot.contains("rateLpm") && shot["rateLpm"].is_number() ? shot["rateLpm"].get<double>() : kNaN;
  const std::string day = jstr(shot, "date");
  const double l = litres(rate, ran);
  book(c.rt, id, day.empty() ? date : day, ran, l);
  section(c.rt, "last")[id] = {{"at", c.epoch}, {"s", round1(ran)}, {"l", isNum(l) ? json(round1(l)) : json(nullptr)}, {"end", say("water.end.restart")},
                               {"override", jbool(shot, "override", false)}, {"upTo", true}};
  c.log.add(c.epoch, "control", "warn", say("ev.water.cut_restart", {{"name", jstr(shot, "name", id)}}), say("ev.water.cut_restart.text"));
  return true;
}

std::optional<double> Watering::available(const Ctx& c, const WaterIn& in) {
  const double reserve = reserveL(c.cfg, in);
  if (!isNum(reserve)) return std::nullopt;
  if (in.levelAssigned) return in.levelL ? std::optional<double>(*in.levelL - reserve) : std::nullopt;
  const auto given = givenL(c.rt);
  if (!given) return std::nullopt;
  return in.volumeL - *given - reserve;
}

json Watering::status(const Ctx& c, const WaterIn& in) const {
  json circuits = json::array();
  for (const auto& w : c.cfg.watering.circuits) {
    const WaterCircuitCfg e = effectiveCircuit(c.cfg, w);
    const auto d = due_.find(w.id);
    // Why it cannot water now: its shot that waits, its schedule's next shot, or by hand any shot
    double s = d != due_.end() ? d->second.s : w.mode == "window" || w.mode == "interval" ? e.shotS : 1;
    if (d == due_.end() && w.mode == "times") {
      int next = 24 * 60;
      for (const auto& t : w.times) {
        const int in_ = ((t.atMin - in.minuteOfDay) % (24 * 60) + 24 * 60) % (24 * 60);
        if (in_ < next) next = in_, s = t.s;
      }
    }
    const auto why = blocked(c, in, w, s, false);
    // By hand any shot; whether only the water stands in the way, which a person may water over
    const auto hand = blocked(c, in, w, 1, false);
    const bool anyway = hand && !blocked(c, in, w, 1, true);
    const auto next = w.paused ? std::nullopt : nextShot(e, in.minuteOfDay);
    // The plant it follows, and its phase where that sets the shots
    json plant = nullptr;
    if (!w.plant.empty())
      if (const PlantCfg* p = c.cfg.plant(w.plant)) {
        const PhaseCfg* ph = followedPhase(c.cfg, w);
        plant = {{"id", p->id}, {"name", p->name}, {"phase", ph ? json(ph->name) : json(nullptr)}, {"shotsPerDay", e.shotsPerDay}, {"shotS", e.shotS}};
      }
    json last = nullptr;
    if (c.rt.watering.is_object() && c.rt.watering.contains("last") && c.rt.watering["last"].contains(w.id)) last = c.rt.watering["last"][w.id];
    circuits.push_back({{"id", w.id},
                        {"running", shot_ && shot_->circuit == w.id},
                        {"due", d != due_.end()},
                        {"paused", w.paused},
                        {"plant", plant},
                        {"nextAt", next ? json((c.epoch / 60 + next->inMin) * 60) : json(nullptr)},
                        {"nextS", next ? json(next->s) : json(nullptr)},
                        {"dayS", round1(dayRunS(c.rt, in.date, w.id))},
                        {"last", last},
                        {"blocked", why ? json(*why) : json(nullptr)},
                        {"hand", hand ? json(*hand) : json(nullptr)},
                        {"anyway", anyway}});
  }
  const auto avail = available(c, in);
  const auto given = givenL(c.rt);
  const auto notFit = unfit(in);
  const auto warn = notFit ? std::nullopt : warning(in);
  // What is left of the running shot, from its switch-on
  json left = nullptr;
  if (shot_) left = round1(std::max(0.0, shot_->s - (shot_->sentAt >= 0 ? static_cast<double>(c.now - shot_->sentAt) / 1000.0 : 0)));
  return {{"running", shot_ ? json(shot_->circuit) : json(nullptr)},
          {"leftS", left},
          {"warning", warn ? json(*warn) : json(nullptr)},
          {"override", shot_ && shot_->override},
          {"givenL", given ? json(round1(*given)) : json(nullptr)},
          {"reserveL", isNum(reserveL(c.cfg, in)) ? json(round1(reserveL(c.cfg, in))) : json(nullptr)},
          {"availableL", avail ? json(round1(*avail)) : json(nullptr)},
          {"low", c.rt.waterLow},
          {"fit", notFit ? json(*notFit) : json(nullptr)},
          {"mixing", in.mixing},
          {"circuits", circuits}};
}

}  // namespace doseph
