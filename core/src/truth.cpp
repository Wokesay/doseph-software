// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/truth.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/messages.hpp"

namespace doseph {

const char* qualityName(Quality q) {
  switch (q) {
    case Quality::Offline: return "offline";
    case Quality::NoData: return "no_data";
    case Quality::Stale: return "stale";
    case Quality::Frozen: return "frozen";
    case Quality::Implausible: return "implausible";
    case Quality::Jump: return "jump";
    case Quality::Uncalibrated: return "uncalibrated";
    case Quality::Ok: return "ok";
  }
  return "unknown";
}

namespace {

constexpr Ms kJumpWindow = 5 * kMinute;    // a jump within 5 min (RAT-039)
constexpr Ms kFrozenAfter = 15 * kMinute;  // raw value standing still (RAT-023)

// Plausible bands and jump thresholds (RAT-020, RAT-039); flow has no jump
// check: it jumps whenever the circulation pump starts or stops.
constexpr SignalDef kDefs[] = {
    {Signal::Ph, "", 2, 3.0, 9.0, 60 * kSecond, 1.0, true},
    {Signal::Ec, "mS/cm", 2, 0.0, 8.0, 60 * kSecond, 0.5, true},
    {Signal::Temp, "°C", 1, 0.0, 45.0, 120 * kSecond, 3.0, false},
    {Signal::Flow, "L/min", 1, 0.0, 500.0, 15 * kSecond, kNaN, false},
};

const Reading& emptyReading() {
  static const Reading r;
  return r;
}

struct PhLine {
  double r1, s1, slope;
};

// Two buffer points (raw, reference). The slope must be plausible (a healthy
// probe, RAT-025). Reads stored data only checked: a broken calibration means
// "no value", never a crash.
std::optional<PhLine> phLine(const json& calib) {
  if (!calib.is_object()) return std::nullopt;
  auto it = calib.find("points");
  if (it == calib.end() || !it->is_array() || it->size() != 2) return std::nullopt;
  auto ok = [](const json& p) { return p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number(); };
  const json& a = (*it)[0];
  const json& b = (*it)[1];
  if (!ok(a) || !ok(b)) return std::nullopt;
  double r1 = a[0].get<double>(), s1 = a[1].get<double>();
  double r2 = b[0].get<double>(), s2 = b[1].get<double>();
  if (!isNum(r1) || !isNum(r2) || !isNum(s1) || !isNum(s2) || std::fabs(r2 - r1) < 0.5) return std::nullopt;
  double slope = (s2 - s1) / (r2 - r1);
  if (slope < 0.8 || slope > 1.25) return std::nullopt;
  return PhLine{r1, s1, slope};
}

double ecFactor(const json& calib) {
  double f = jnum(calib, "factor");
  return isNum(f) && f >= 0.5 && f <= 2.0 ? f : kNaN;
}

}  // namespace

const SignalDef& signalDef(Signal c) {
  for (const auto& d : kDefs)
    if (d.signal == c) return d;
  return kDefs[0];
}

std::optional<Msg> SensorTruth::checkPh(const json& data) {
  auto line = phLine(data);
  if (!line) return say("calib.ph_slope");
  // The probe's zero point: the raw value at which the line reads pH 7, as
  // the electrode's voltage, within ±30 mV (RAT-026)
  const double raw7 = line->r1 + (7.0 - line->s1) / line->slope;
  const double mv = (7.0 - raw7) * kNernstMvPerPh;
  if (std::fabs(mv) > 30.0) return say("calib.ph_zero", {{"mv", std::round(mv)}});
  return std::nullopt;
}

std::optional<Msg> SensorTruth::checkEc(const json& data) {
  if (!isNum(ecFactor(data))) return say("calib.ec_factor");
  return std::nullopt;
}

std::optional<double> SensorTruth::calibrate(Signal c, double raw, const Config& cfg, double tempC) {
  if (!isNum(raw)) return std::nullopt;
  if (c == Signal::Ph) {
    auto line = phLine(cfg.phCal);
    if (!line) return std::nullopt;
    return line->s1 + (raw - line->r1) * line->slope;
  }
  if (c == Signal::Ec) {
    const double f = ecFactor(cfg.ecCal);
    if (!isNum(f) || !isNum(tempC)) return std::nullopt;
    return compensate(raw * f, tempC);
  }
  return raw;
}

void SensorTruth::expectChange(Signal c, Ms until) {
  auto& t = tracks_[c];
  t.explainedUntil = std::max(t.explainedUntil, until);
}

const Reading& SensorTruth::get(Signal c) const {
  auto it = readings_.find(c);
  return it == readings_.end() ? emptyReading() : it->second;
}

void SensorTruth::update(const Config& cfg, const IHardware& hw, RuntimeState& rt, Ms now, Epoch epoch) {
  // Temperature first: EC is compensated with it (RAT-089)
  for (Signal ch : {Signal::Temp, Signal::Ph, Signal::Ec, Signal::Flow}) {
    const SignalDef& def = signalDef(ch);
    const std::string name = signalName(ch);
    Reading r;
    r.signal = name;
    r.unit = def.unit;
    r.decimals = def.decimals;
    auto& tr = tracks_[ch];
    auto done = [&](Quality q, Msg reason) {
      r.quality = q;
      r.reason = std::move(reason);
      readings_[ch] = r;
    };
    const std::string unit = std::string(def.unit).empty() ? std::string() : " " + std::string(def.unit);
    auto shown = [&](double v) {
      const double p = std::pow(10.0, def.decimals);
      return numOrNull(std::round(v * p) / p);
    };
    if (!hw.online()) {
      done(Quality::Offline, say("truth.offline"));
      continue;
    }
    auto s = hw.sample(ch);
    if (!s || !isNum(s->raw)) {
      done(Quality::NoData, say("truth.no_data"));
      continue;
    }
    r.ts = s->ts;
    r.ageMs = std::max<Ms>(0, now - s->ts);
    if (s->raw != tr.lastRaw) {
      tr.lastRaw = s->raw;
      tr.lastRawChange = now;
    }

    // The value: pH from its calibration, EC calibrated and compensated,
    // temperature and flow as they come. Without one, the raw value is shown
    // but nothing regulates on it (RAT-025).
    std::optional<double> value;
    Msg uncal;
    if (ch == Signal::Ph) {
      value = calibrate(ch, s->raw, cfg, kNaN);
      if (!value) {
        r.value = s->raw;
        uncal = say("truth.uncalibrated");
      }
    } else if (ch == Signal::Ec) {
      const double f = ecFactor(cfg.ecCal);
      const Reading& t = get(Signal::Temp);
      if (!isNum(f)) {
        r.value = s->raw;
        uncal = say("truth.uncalibrated");
      } else if (!t.usable()) {
        // Never taken as if at 25 °C (R5): shown, marked, not for control (RAT-089)
        r.value = s->raw * f;
        uncal = say("truth.ec_no_temp", {{"reason", t.reason}});
      } else {
        value = compensate(s->raw * f, *t.value);
      }
    } else {
      value = s->raw;
    }
    if (value) r.value = value;

    // Stale after its limit, by the age of its last measurement, never by a change of value (RAT-027)
    if (r.ageMs > def.maxAgeMs) {
      done(Quality::Stale, say("truth.stale", {{"ageS", r.ageMs / 1000}, {"limitS", def.maxAgeMs / 1000}}));
      continue;
    }
    if (!value) {
      done(Quality::Uncalibrated, uncal);
      continue;
    }
    // Standstill of the raw value apart from freshness (RAT-023, RAT-059)
    if (def.expectsNoise && now - tr.lastRawChange > kFrozenAfter) {
      done(Quality::Frozen, say("truth.frozen"));
      continue;
    }
    if (*value < def.plausMin || *value > def.plausMax) {
      done(Quality::Implausible, say("truth.implausible", {{"signal", name}, {"value", shown(*value)}, {"unit", unit},
                                                           {"min", def.plausMin}, {"max", def.plausMax}}));
      continue;
    }
    // Jump lock: a change above the threshold within 5 min that no announced
    // intervention explains locks the signal until 15 min of calm (RAT-039,
    // RAT-042, RAT-044). It survives a restart in the run-time state.
    if (isNum(def.jump) && s->ts != tr.lastTs) {
      tr.lastTs = s->ts;
      while (!tr.window.empty() && s->ts - tr.window.front().first > kJumpWindow) tr.window.pop_front();
      const bool explained = now < tr.explainedUntil;
      if (!explained && !tr.window.empty()) {
        auto [mn, mx] = std::minmax_element(tr.window.begin(), tr.window.end(),
                                            [](const auto& a, const auto& b) { return a.second < b.second; });
        const double from = std::fabs(*value - mn->second) > std::fabs(*value - mx->second) ? mn->second : mx->second;
        if (std::fabs(*value - from) > def.jump) {
          rt.jumpLocks[name] = {{"until", epoch + kJumpHoldS}, {"from", from}, {"to", *value}, {"at", epoch}};
          tr.window.clear();  // a new reference; free 15 min after the last jump
        }
      }
      if (explained) tr.window.clear();
      tr.window.emplace_back(s->ts, *value);
    }
    auto lock = rt.jumpLocks.find(name);
    const Epoch until = lock == rt.jumpLocks.end() ? 0 : static_cast<Epoch>(jnum(lock->second, "until", 0));
    if (lock != rt.jumpLocks.end() && epoch < until) {
      // The reason names only causes that can be true: the robot lowers pH
      // only, so a fall of pH names its own dose as one possibility (RAT-041)
      const double from = jnum(lock->second, "from"), to = jnum(lock->second, "to");
      const json args = {{"signal", name}, {"until", until}, {"from", shown(from)}, {"to", shown(to)}, {"unit", unit}};
      const bool fell = isNum(from) && isNum(to) && to < from;
      done(Quality::Jump, say(ch == Signal::Ph && fell ? "truth.jump_ph_fall" : "truth.jump", args));
      continue;
    }
    if (lock != rt.jumpLocks.end()) {
      rt.jumpLocks.erase(lock);
      tr.window.clear();
      tr.window.emplace_back(s->ts, *value);
    }
    done(Quality::Ok, say("truth.ok"));
  }
}

}  // namespace doseph
