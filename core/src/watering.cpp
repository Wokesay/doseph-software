// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/watering.hpp"

#include <cmath>

#include "doseph/config.hpp"
#include "doseph/gateway.hpp"
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
  return key == "water.no.other" || key == "water.no.output_busy" || key == "water.no.mixing";
}

json& section(RuntimeState& rt, const char* key) {
  if (!rt.watering.is_object()) rt.watering = json::object();
  json& s = rt.watering[key];
  if (!s.is_object()) s = json::object();
  return s;
}

double givenL(const RuntimeState& rt) { return rt.watering.is_object() ? jnum(rt.watering, "givenL", 0) : 0; }

double dayRunS(const RuntimeState& rt, const std::string& date, const std::string& circuit) {
  if (!rt.watering.is_object() || !rt.watering.contains("day")) return 0;
  const json& d = rt.watering["day"];
  if (jstr(d, "date") != date || !d.contains("runS") || !d["runS"].is_object()) return 0;
  return jnum(d["runS"], circuit.c_str(), 0);
}

double reserveL(const Config& cfg, const WaterIn& in) {
  return isNum(in.volumeL) ? in.volumeL * cfg.watering.reservePct / 100.0 : kNaN;
}

double litres(const WaterCircuitCfg& w, double s) { return isNum(w.rateLpm) ? w.rateLpm * s / 60.0 : kNaN; }

// Why the tank cannot give `takeL` more litres now: an empty float, a level or the count below the
// reserve (RAT-067, RAT-068); nothing where it can
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
  if (counts && isNum(reserve)) {
    if (in.volumeL - givenL(c.rt) - take < reserve) return say("water.no.reserve", {{"reserve", round1(reserve)}});
    return std::nullopt;
  }
  if (!in.emptyAssigned) return say("water.no.count");  // nothing tells how much water there is
  return std::nullopt;
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
  if (auto why = blocked(c, in, *w, s, override)) return why;
  const auto notFit = in.mixing ? std::optional<Msg>(say("water.no.mixing")) : unfit(in);
  start(c, *w, s, override && notFit.has_value(), "hand", out);
  // Watering anyway is a warning: the water was not fit, and a person chose to (SD-134)
  if (shot_->override) c.log.add(c.epoch, "control", "warn", say("ev.water.override", {{"name", w->name}}), *notFit, {{"circuit", w->id}, {"s", s}});
  return std::nullopt;
}

bool Watering::stop(Ctx& c, NetOutputs& out, const std::string& circuit, Msg why) {
  if (!shot_ || shot_->circuit != circuit) return false;
  end(c, out, std::move(why), false);
  return true;
}

void Watering::start(Ctx& c, const WaterCircuitCfg& w, double s, bool override, const std::string& why, NetOutputs& out) {
  shot_ = Shot{w.id, c.now, -1, s, override, why};
  due_.erase(w.id);
  skipped_.erase(w.id);
  section(c.rt, "shot") = {{"circuit", w.id}, {"since", c.epoch}, {"s", s}, {"override", override}, {"why", why}};
  out.want(waterRole(w.id), true);
}

void Watering::end(Ctx& c, NetOutputs& out, Msg how, bool warn) {
  const Shot shot = *shot_;
  shot_.reset();
  out.want(waterRole(shot.circuit), false);
  const WaterCircuitCfg* w = c.cfg.watering.circuit(shot.circuit);
  const double ran = shot.openedAt >= 0 ? static_cast<double>(c.now - shot.openedAt) / 1000.0 : 0;
  const double l = w ? litres(*w, ran) : kNaN;
  if (isNum(l)) c.rt.watering["givenL"] = givenL(c.rt) + l;
  // The day's run time, by local day (SD-134)
  json& day = section(c.rt, "day");
  if (jstr(day, "date") != date_) day = {{"date", date_}, {"runS", json::object()}};
  day["runS"][shot.circuit] = jnum(day["runS"], shot.circuit.c_str(), 0) + ran;
  section(c.rt, "last")[shot.circuit] = {{"at", c.epoch}, {"s", round1(ran)}, {"l", isNum(l) ? json(round1(l)) : json(nullptr)}, {"end", how}};
  c.rt.watering["shot"] = nullptr;
  const json name = w ? json(w->name) : json(shot.circuit);
  const json data = {{"circuit", shot.circuit}, {"s", round1(ran)}, {"l", isNum(l) ? json(round1(l)) : json(nullptr)}, {"override", shot.override}};
  if (warn) c.log.add(c.epoch, "control", "warn", say("ev.water.cut", {{"name", name}, {"s", round1(ran)}}), std::move(how), data);
  else c.log.add(c.epoch, "control", "info", say("ev.water.shot", {{"name", name}, {"s", round1(ran)}}), std::move(how), data);
}

void Watering::skip(Ctx& c, const WaterCircuitCfg& w, const Msg& why) {
  // "Below the reserve": water is to be topped up, which a refill valve answers (SD-132)
  if (why.key == "water.no.reserve" && !c.rt.waterLow) {
    c.rt.waterLow = true;
    c.log.add(c.epoch, "control", "warn", say("ev.water.low"), why);
  }
  // Each reason once, until the circuit waters again: a schedule of short intervals is no flood
  if (skipped_[w.id] == why.key) return;
  skipped_[w.id] = why.key;
  c.log.add(c.epoch, "control", "warn", say("ev.water.skipped", {{"name", w.name}}), why, {{"circuit", w.id}});
}

void Watering::tick(Ctx& c, const WaterIn& in, NetOutputs& out) {
  date_ = in.date;
  // The schedules: each local minute passed brings the shots due in it. After a start no shot
  // missed is made up (R6); one that still waits when its circuit's next comes is skipped.
  if (in.minuteOfDay >= 0) {
    if (lastMinute_ < 0) {
      lastMinute_ = in.minuteOfDay;
    } else if (in.minuteOfDay != lastMinute_) {
      int m = lastMinute_;
      for (int i = 0; i < 24 * 60 && m != in.minuteOfDay; ++i) {
        m = (m + 1) % (24 * 60);
        for (const auto& w : c.cfg.watering.circuits) {
          if (w.paused) continue;
          if (const auto s = shotAt(w, m)) {
            if (due_.count(w.id)) skip(c, w, say("water.no.superseded"));
            due_[w.id] = *s;
          }
        }
      }
      lastMinute_ = in.minuteOfDay;
    }
  }
  for (auto it = due_.begin(); it != due_.end();) {
    const WaterCircuitCfg* w = c.cfg.watering.circuit(it->first);
    it = !w || w->paused ? due_.erase(it) : std::next(it);
  }

  if (shot_) {
    const WaterCircuitCfg* w = c.cfg.watering.circuit(shot_->circuit);
    const auto o = in.outputs.find(shot_->circuit);
    const json state = o == in.outputs.end() ? json::object() : o->second;
    const std::string st = jstr(state, "state", "not_assigned");
    if (shot_->openedAt < 0 && (st == "on" || jbool(state, "switchedOn", false))) shot_->openedAt = c.now;
    const double ran = shot_->openedAt >= 0 ? static_cast<double>(c.now - shot_->openedAt) / 1000.0 : 0;
    if (!w) return end(c, out, say("water.end.removed"), true);
    // What it gave so far counts against the reserve; a level probe sees it itself
    const auto shortOf = tankShort(c, in, in.levelAssigned ? 0 : litres(*w, ran), isNum(w->rateLpm));
    if (c.stopped) return end(c, out, say("water.end.stopped"), true);
    if (c.maintenanceUntil > 0) return end(c, out, say("water.end.maintenance"), true);
    if (!c.hold.empty()) return end(c, out, say("water.end.hold"), true);
    if (in.dosing) return end(c, out, say("water.end.dose"), true);  // a dose that starts stops it (RAT-086)
    if (in.flowDry) return end(c, out, say("water.no.flow"), true);
    if (st == "not_assigned" || st == "offline" || st == "refused") return end(c, out, say("water.end.output"), true);
    if (!shot_->override)
      if (auto why = unfit(in)) return end(c, out, *why, true);
    if (shortOf) {
      if (shortOf->key == "water.no.reserve" && !c.rt.waterLow) {
        c.rt.waterLow = true;
        c.log.add(c.epoch, "control", "warn", say("ev.water.low"), *shortOf);
      }
      return end(c, out, *shortOf, true);
    }
    if (shot_->openedAt < 0 && c.now - shot_->since >= kWaterOpenMs) return end(c, out, say("water.end.not_open"), true);
    if (shot_->openedAt >= 0 && ran >= shot_->s) return end(c, out, say("water.end.done"), false);
    return;
  }

  // One circuit at a time, in the order of the configuration (SD-134)
  for (const auto& w : c.cfg.watering.circuits) {
    const auto d = due_.find(w.id);
    if (d == due_.end()) continue;
    const auto why = blocked(c, in, w, d->second, false);
    if (!why) {
      start(c, w, d->second, false, "schedule", out);
      return;
    }
    if (waits(why->key)) continue;
    skip(c, w, *why);
    due_.erase(d);
  }
}

bool Watering::reportCut(Ctx& c) {
  if (!c.rt.watering.is_object() || !c.rt.watering.contains("shot") || !c.rt.watering["shot"].is_object()) return false;
  const json shot = c.rt.watering["shot"];
  c.rt.watering["shot"] = nullptr;
  const WaterCircuitCfg* w = c.cfg.watering.circuit(jstr(shot, "circuit"));
  c.log.add(c.epoch, "control", "warn", say("ev.water.cut_restart", {{"name", w ? json(w->name) : json(jstr(shot, "circuit"))}}),
            say("ev.water.cut_restart.text"));
  return true;
}

std::optional<double> Watering::available(const Ctx& c, const WaterIn& in) {
  const double reserve = reserveL(c.cfg, in);
  if (!isNum(reserve)) return std::nullopt;
  if (in.levelAssigned) return in.levelL ? std::optional<double>(*in.levelL - reserve) : std::nullopt;
  return in.volumeL - givenL(c.rt) - reserve;
}

json Watering::status(const Ctx& c, const WaterIn& in) const {
  json circuits = json::array();
  for (const auto& w : c.cfg.watering.circuits) {
    const auto d = due_.find(w.id);
    const double s = d != due_.end() ? d->second : w.mode == "manual" || w.mode == "times" ? w.longestS : w.shotS;
    const auto why = blocked(c, in, w, std::min(s, w.longestS), false);
    json last = nullptr;
    if (c.rt.watering.is_object() && c.rt.watering.contains("last") && c.rt.watering["last"].contains(w.id)) last = c.rt.watering["last"][w.id];
    circuits.push_back({{"id", w.id},
                        {"running", shot_ && shot_->circuit == w.id},
                        {"due", d != due_.end()},
                        {"dayS", round1(dayRunS(c.rt, in.date, w.id))},
                        {"last", last},
                        {"blocked", why ? json(*why) : json(nullptr)}});
  }
  const auto avail = available(c, in);
  const auto notFit = unfit(in);
  const auto warn = notFit ? std::nullopt : warning(in);
  return {{"running", shot_ ? json(shot_->circuit) : json(nullptr)},
          {"warning", warn ? json(*warn) : json(nullptr)},
          {"override", shot_ && shot_->override},
          {"givenL", round1(givenL(c.rt))},
          {"reserveL", isNum(reserveL(c.cfg, in)) ? json(round1(reserveL(c.cfg, in))) : json(nullptr)},
          {"availableL", avail ? json(round1(*avail)) : json(nullptr)},
          {"low", c.rt.waterLow},
          {"fit", notFit ? json(*notFit) : json(nullptr)},
          {"mixing", in.mixing},
          {"circuits", circuits}};
}

}  // namespace doseph
