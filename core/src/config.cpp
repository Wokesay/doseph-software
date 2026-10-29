// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <stdexcept>

#include "doseph/coupling.hpp"
#include "doseph/messages.hpp"
#include "doseph/service.hpp"

namespace doseph {

namespace {

double numOrNaN(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || j[key].is_null()) return kNaN;
  if (!j[key].is_number()) throw std::invalid_argument(std::string("not a number: ") + key);
  return j[key].get<double>();
}

int intOr(const json& j, const char* key, int fallback) {
  if (!j.is_object() || !j.contains(key) || j[key].is_null()) return fallback;
  if (!j[key].is_number_integer()) throw std::invalid_argument(std::string("not a whole number: ") + key);
  return j[key].get<int>();
}

Epoch epochOr(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || j[key].is_null()) return 0;
  if (!j[key].is_number_integer()) throw std::invalid_argument(std::string("not a time: ") + key);
  return j[key].get<Epoch>();
}

const json& objectAt(const json& j, const char* key) {
  static const json empty = json::object();
  if (!j.contains(key) || j[key].is_null()) return empty;
  if (!j[key].is_object()) throw std::invalid_argument(std::string("not an object: ") + key);
  return j[key];
}

const json& arrayAt(const json& j, const char* key) {
  static const json empty = json::array();
  if (!j.contains(key) || j[key].is_null()) return empty;
  if (!j[key].is_array()) throw std::invalid_argument(std::string("not a list: ") + key);
  return j[key];
}

bool inRange(double v, double lo, double hi) { return isNum(v) && v >= lo && v <= hi; }

// Characters, not bytes, of a UTF-8 text
size_t chars(const std::string& s) {
  return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; }));
}

// The circuits (SD-134): each shot within its circuit's longest, which stays within what the device
// takes; a reserve of at least 10 % of the tank
template <class Err>
void validateWatering(const Config& cfg, Err&& err) {
  const WateringCfg& w = cfg.watering;
  if (!inRange(w.reservePct, 10, 90)) err(say("cfg.water.reserve", {{"min", 10}, {"max", 90}}));
  if (w.circuits.size() > kMaxCircuits) err(say("cfg.water.too_many", {{"max", kMaxCircuits}}));
  std::set<std::string> ids;
  auto minute = [](int m) { return m >= 0 && m < 24 * 60; };
  for (const auto& c : w.circuits) {
    const bool idOk = c.id.size() >= 2 && c.id.size() <= 4 && c.id[0] == 'w' &&
                      std::all_of(c.id.begin() + 1, c.id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    if (!idOk || !ids.insert(c.id).second) err(say("cfg.water.id", {{"id", c.id}}));
    if (c.name.empty() || chars(c.name) > 40) err(say("cfg.water.name", {{"max", 40}}));
    const json name = c.name;
    if (!inRange(c.longestS, 1, kMaxShotS)) err(say("cfg.water.longest", {{"name", name}, {"max", kMaxShotS / 3600}}));
    auto shot = [&](double s) { return inRange(s, 1, isNum(c.longestS) ? c.longestS : kMaxShotS); };
    if (c.mode == "window") {
      if (!minute(c.fromMin) || !minute(c.toMin)) err(say("cfg.water.window", {{"name", name}}));
      if (c.shotsPerDay < 1 || c.shotsPerDay > 24 * 60) err(say("cfg.water.shots", {{"name", name}, {"max", 24 * 60}}));
      if (!shot(c.shotS)) err(say("cfg.water.shot", {{"name", name}}));
    } else if (c.mode == "times") {
      if (c.times.empty() || c.times.size() > kMaxWaterTimes) err(say("cfg.water.times", {{"name", name}, {"max", kMaxWaterTimes}}));
      std::set<int> at;
      for (const auto& t : c.times)
        if (!minute(t.atMin) || !at.insert(t.atMin).second || !shot(t.s)) {
          err(say("cfg.water.time", {{"name", name}}));
          break;
        }
    } else if (c.mode == "interval") {
      if (c.everyMin < 1 || c.everyMin > 24 * 60) err(say("cfg.water.every", {{"name", name}}));
      if (!shot(c.shotS)) err(say("cfg.water.shot", {{"name", name}}));
    } else if (c.mode != "manual") {
      err(say("cfg.water.mode", {{"name", name}}));
    }
    if (isNum(c.rateLpm) && !inRange(c.rateLpm, 0.01, 1000)) err(say("cfg.water.rate", {{"name", name}}));
    if (isNum(c.dailyMaxS) && !inRange(c.dailyMaxS, 1, 24 * 3600)) err(say("cfg.water.daily", {{"name", name}}));
    // A plant sets a window's shots (SD-134, RAT-076): only a window follows one, and one that is there
    if (!c.plant.empty() && (c.mode != "window" || !cfg.plant(c.plant))) err(say("cfg.water.plant", {{"name", name}}));
  }
}

// Light (SD-135): a name, a day within 24 h whose ramps fit in it, a dimmer's brightness and the
// driver's threshold in bounds, and a plant that is there
template <class Err>
void validateLighting(const Config& cfg, Err&& err) {
  const LightingCfg& l = cfg.lighting;
  if (l.circuits.size() > kMaxLights) err(say("cfg.light.too_many", {{"max", kMaxLights}}));
  std::set<std::string> ids;
  for (const auto& c : l.circuits) {
    const bool idOk = c.id.size() >= 2 && c.id.size() <= 3 && c.id[0] == 'l' &&
                      std::all_of(c.id.begin() + 1, c.id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    if (!idOk || !ids.insert(c.id).second) err(say("cfg.light.id", {{"id", c.id}}));
    if (c.name.empty() || chars(c.name) > 40) err(say("cfg.light.name", {{"max", 40}}));
    const json name = c.name;
    if (c.startMin < 0 || c.startMin >= 24 * 60) err(say("cfg.light.start", {{"name", name}}));
    if (!inRange(c.hours, 0, 24)) err(say("cfg.light.hours", {{"name", name}}));
    if (!inRange(c.pct, 1, 100)) err(say("cfg.light.pct", {{"name", name}}));
    if (!inRange(c.thresholdPct, 0, 50)) err(say("cfg.light.threshold", {{"name", name}, {"max", 50}}));
    // The ramps fit in a dimmer's day; a switch has none, and a day of 0 h no light to ramp
    if (c.sunriseMin < 0 || c.sunsetMin < 0 || c.sunriseMin > kMaxRampMin || c.sunsetMin > kMaxRampMin ||
        (c.dimmed && isNum(c.hours) && c.hours > 0 && c.sunriseMin + c.sunsetMin > c.hours * 60))
      err(say("cfg.light.ramps", {{"name", name}, {"max", kMaxRampMin}}));
    if (!c.plant.empty() && !cfg.plant(c.plant)) err(say("cfg.light.plant", {{"name", name}}));
  }
}

template <class Err>
void validateClimate(const Config& cfg, Err&& err) {
  const ClimateCfg& k = cfg.climate;
  if (k.rooms.size() > kMaxRooms) err(say("cfg.climate.too_many", {{"max", kMaxRooms}}));
  std::set<std::string> ids, lights;
  for (const auto& r : k.rooms) {
    const bool idOk = r.id.size() >= 2 && r.id.size() <= 3 && r.id[0] == 'r' &&
                      std::all_of(r.id.begin() + 1, r.id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    if (!idOk || !ids.insert(r.id).second) err(say("cfg.climate.id", {{"id", r.id}}));
    if (r.name.empty() || chars(r.name) > 40) err(say("cfg.climate.name", {{"max", 40}}));
    const json name = r.name;
    // A light circuit lies in one room at most: its heat protection is that room's (SD-136)
    for (const auto& l : r.lights)
      if (!cfg.lighting.circuit(l) || !lights.insert(l).second) err(say("cfg.climate.light", {{"name", name}}));
    if (r.startMin < 0 || r.startMin >= 24 * 60 || !inRange(r.hours, 0, 24)) err(say("cfg.climate.day", {{"name", name}}));
    if (!inRange(r.dayC, 5, 40) || !inRange(r.nightC, 5, 40)) err(say("cfg.climate.temp", {{"name", name}, {"min", 5}, {"max", 40}}));
    if (!inRange(r.rhPct, 20, 95)) err(say("cfg.climate.rh", {{"name", name}, {"min", 20}, {"max", 95}}));
    if (!inRange(r.vpdKpa, 0.2, 2.5)) err(say("cfg.climate.vpd", {{"name", name}, {"min", 0.2}, {"max", 2.5}}));
    if (!std::isnan(r.co2Ppm) && !inRange(r.co2Ppm, 400, kCo2MaxTargetPpm))
      err(say("cfg.climate.co2", {{"name", name}, {"min", 400}, {"max", kCo2MaxTargetPpm}}));
    // The heat limit above both targets: light is not dimmed for a room kept as it should be
    if (!inRange(r.heatC, 20, 45) || (isNum(r.dayC) && r.heatC <= r.dayC) || (isNum(r.nightC) && r.heatC <= r.nightC))
      err(say("cfg.climate.heat", {{"name", name}, {"min", 20}, {"max", 45}}));
    if (!std::isnan(r.heaterW) && !inRange(r.heaterW, 50, 3500)) err(say("cfg.climate.heater", {{"name", name}, {"min", 50}, {"max", 3500}}));
    if (!r.plant.empty() && !cfg.plant(r.plant)) err(say("cfg.climate.plant", {{"name", name}}));
  }
}

}  // namespace

WaterCircuitCfg waterCircuitFromJson(const json& x) {
  if (!x.is_object()) throw std::invalid_argument("a watering circuit is not an object");
  WaterCircuitCfg w;
  w.id = jstr(x, "id");
  w.name = jstr(x, "name");
  w.mode = jstr(x, "mode", w.mode);
  w.fromMin = intOr(x, "fromMin", w.fromMin);
  w.toMin = intOr(x, "toMin", w.toMin);
  w.shotsPerDay = intOr(x, "shotsPerDay", w.shotsPerDay);
  if (x.contains("shotS")) w.shotS = numOrNaN(x, "shotS");
  for (const auto& t : arrayAt(x, "times")) {
    if (!t.is_object()) throw std::invalid_argument("a watering time is not an object");
    w.times.push_back({intOr(t, "atMin", -1), numOrNaN(t, "s")});
  }
  w.everyMin = intOr(x, "everyMin", w.everyMin);
  if (x.contains("longestS")) w.longestS = numOrNaN(x, "longestS");
  w.rateLpm = numOrNaN(x, "rateLpm");
  w.dailyMaxS = numOrNaN(x, "dailyMaxS");
  if (x.contains("paused") && !x["paused"].is_boolean()) throw std::invalid_argument("paused is not true or false");
  w.paused = jbool(x, "paused", false);
  w.plant = jstr(x, "plant");
  return w;
}

LightCircuitCfg lightCircuitFromJson(const json& x) {
  if (!x.is_object()) throw std::invalid_argument("a light circuit is not an object");
  LightCircuitCfg l;
  l.id = jstr(x, "id");
  l.name = jstr(x, "name");
  if (x.contains("dimmed") && !x["dimmed"].is_boolean()) throw std::invalid_argument("dimmed is not true or false");
  l.dimmed = jbool(x, "dimmed", false);
  l.startMin = intOr(x, "startMin", l.startMin);
  if (x.contains("hours")) l.hours = numOrNaN(x, "hours");
  if (x.contains("pct")) l.pct = numOrNaN(x, "pct");
  if (x.contains("thresholdPct")) l.thresholdPct = numOrNaN(x, "thresholdPct");
  l.sunriseMin = intOr(x, "sunriseMin", l.sunriseMin);
  l.sunsetMin = intOr(x, "sunsetMin", l.sunsetMin);
  l.plant = jstr(x, "plant");
  return l;
}

const WaterCircuitCfg* WateringCfg::circuit(const std::string& id) const {
  for (const auto& c : circuits)
    if (c.id == id) return &c;
  return nullptr;
}

std::string waterRole(const std::string& circuit) { return "water." + circuit; }
std::string lightRole(const std::string& circuit) { return "light." + circuit; }

ClimateRoomCfg climateRoomFromJson(const json& x) {
  if (!x.is_object()) throw std::invalid_argument("a room is not an object");
  ClimateRoomCfg r;
  r.id = jstr(x, "id");
  r.name = jstr(x, "name");
  for (const auto& l : arrayAt(x, "lights")) {
    if (!l.is_string()) throw std::invalid_argument("a room's light circuit is not a string");
    r.lights.push_back(l.get<std::string>());
  }
  r.startMin = intOr(x, "startMin", r.startMin);
  for (const auto& [key, field] : {std::pair{"hours", &r.hours}, std::pair{"dayC", &r.dayC}, std::pair{"nightC", &r.nightC},
                                   std::pair{"rhPct", &r.rhPct}, std::pair{"vpdKpa", &r.vpdKpa}, std::pair{"heatC", &r.heatC}})
    if (x.contains(key)) *field = numOrNaN(x, key);
  r.co2Ppm = numOrNaN(x, "co2Ppm");
  r.heaterW = numOrNaN(x, "heaterW");
  if ((x.contains("byVpd") && !x["byVpd"].is_boolean()) || (x.contains("circulateAlways") && !x["circulateAlways"].is_boolean()))
    throw std::invalid_argument("not true or false");
  r.byVpd = jbool(x, "byVpd", false);
  r.circulateAlways = jbool(x, "circulateAlways", false);
  r.plant = jstr(x, "plant");
  return r;
}

const ClimateRoomCfg* ClimateCfg::room(const std::string& id) const {
  for (const auto& r : rooms)
    if (r.id == id) return &r;
  return nullptr;
}

std::string climateRole(const std::string& room, const std::string& kind) { return "climate." + room + "." + kind; }

const std::vector<std::string>& climateOutputs() {
  static const std::vector<std::string> k = {"exhaust", "circulation", "heater", "humidifier", "dehumidifier", "co2_valve"};
  return k;
}

const std::vector<std::string>& climateSensors() {
  static const std::vector<std::string> k = {"temp", "rh", "co2"};
  return k;
}

const LightCircuitCfg* LightingCfg::circuit(const std::string& id) const {
  for (const auto& c : circuits)
    if (c.id == id) return &c;
  return nullptr;
}

Limits Limits::bounded() const {
  Limits b;
  b.minRunS = isNum(minRunS) ? std::clamp(minRunS, kHardMinRunS, 10.0) : kHardMinRunS;
  b.maxRunS = isNum(maxRunS) ? std::clamp(maxRunS, b.minRunS, kHardMaxRunS) : kHardMaxRunS;
  b.maxPartialRuns = std::clamp(maxPartialRuns, 1, kHardMaxPartialRuns);
  return b;
}

const CanisterCfg* Config::canister(const std::string& id) const {
  for (const auto& b : canisters)
    if (b.id == id) return &b;
  return nullptr;
}

const CanisterCfg* Config::canisterOnChannel(int channel) const {
  for (const auto& b : canisters)
    if (channel > 0 && b.channel == channel && b.device.empty()) return &b;
  return nullptr;
}

const RecipeCfg* Config::recipe(const std::string& id) const {
  for (const auto& r : recipes)
    if (r.id == id) return &r;
  return nullptr;
}

json phasesJson(const std::vector<PhaseCfg>& phases) {
  json out = json::array();
  for (const auto& ph : phases)
    out.push_back({{"name", ph.name},
                   {"days", ph.days},
                   {"confirm", ph.confirm},
                   {"recipe", ph.recipe},
                   {"ecTarget", numOrNull(ph.ecTarget)},
                   {"ph", isNum(ph.phLow) && isNum(ph.phHigh) ? json::array({ph.phLow, ph.phHigh}) : json(nullptr)},
                   {"shotsPerDay", ph.shotsPerDay ? json(*ph.shotsPerDay) : json(nullptr)},
                   {"shotS", numOrNull(ph.shotS)},
                   {"lightHours", ph.lightHours ? json(*ph.lightHours) : json(nullptr)},
                   {"lightPct", ph.lightPct ? json(*ph.lightPct) : json(nullptr)},
                   {"dayC", ph.dayC ? json(*ph.dayC) : json(nullptr)},
                   {"nightC", ph.nightC ? json(*ph.nightC) : json(nullptr)},
                   {"rhPct", ph.rhPct ? json(*ph.rhPct) : json(nullptr)},
                   {"vpdKpa", ph.vpdKpa ? json(*ph.vpdKpa) : json(nullptr)},
                   {"co2Ppm", ph.co2Ppm ? json(*ph.co2Ppm) : json(nullptr)}});
  return out;
}

std::vector<PhaseCfg> phasesFromJson(const json& j) {
  std::vector<PhaseCfg> out;
  for (const auto& p : arrayAt(j, "phases")) {
    if (!p.is_object()) throw std::invalid_argument("phase is not an object");
    PhaseCfg x;
    x.name = jstr(p, "name");
    x.days = intOr(p, "days", 14);
    x.confirm = jbool(p, "confirm", false);
    x.recipe = jstr(p, "recipe");
    x.ecTarget = numOrNaN(p, "ecTarget");
    if (p.contains("ph") && !p["ph"].is_null()) {
      const json& ph = p["ph"];
      if (!ph.is_array() || ph.size() != 2 || !ph[0].is_number() || !ph[1].is_number())
        throw std::invalid_argument("pH range is not two numbers");
      x.phLow = ph[0].get<double>();
      x.phHigh = ph[1].get<double>();
    }
    if (p.contains("shotsPerDay") && !p["shotsPerDay"].is_null()) {
      if (!p["shotsPerDay"].is_number_integer()) throw std::invalid_argument("shots a day is not a whole number");
      const long long n = p["shotsPerDay"].get<long long>();
      x.shotsPerDay = n < 0 || n > 100000 ? -1 : static_cast<int>(n);  // out of bounds stays so
    }
    x.shotS = numOrNaN(p, "shotS");
    // Light (SD-135) and climate (SD-136): a number or nothing
    for (const auto& [key, field] : {std::pair{"lightHours", &x.lightHours}, std::pair{"lightPct", &x.lightPct}, std::pair{"dayC", &x.dayC},
                                     std::pair{"nightC", &x.nightC}, std::pair{"rhPct", &x.rhPct}, std::pair{"vpdKpa", &x.vpdKpa},
                                     std::pair{"co2Ppm", &x.co2Ppm}}) {
      if (!p.contains(key) || p[key].is_null()) continue;
      if (!p[key].is_number()) throw std::invalid_argument(std::string(key) + " is not a number");
      *field = p[key].get<double>();
    }
    out.push_back(std::move(x));
  }
  return out;
}

const PlantRecipeCfg* Config::plantRecipe(const std::string& id) const {
  for (const auto& r : plantRecipes)
    if (r.id == id) return &r;
  return nullptr;
}

const PlantCfg* Config::plant(const std::string& id) const {
  for (const auto& p : plants)
    if (p.id == id) return &p;
  return nullptr;
}

const CanisterCfg* Config::phDown() const {
  for (const auto& b : canisters)
    if (b.role == "ph_down" && b.channel > 0) return &b;
  return nullptr;
}

const SetupStep* setupStep(const std::string& id) {
  for (const auto& st : kSetupSteps)
    if (id == st.id) return &st;
  return nullptr;
}

bool placementConfirmed(const Config& cfg) {
  const auto it = cfg.setup.find("placement");
  return it != cfg.setup.end() && it->second.state == "done";
}

double Config::canisterFlow(const CanisterCfg& b) const {
  if (b.device.empty()) return channelFlow(b.channel);
  if (b.device != peerId) return kNaN;
  const auto it = peerFlows.find(b.channel);
  return it == peerFlows.end() ? kNaN : it->second;
}

const NetDeviceCfg* Config::device(const std::string& id) const {
  for (const auto& d : devices)
    if (d.id == id) return &d;
  return nullptr;
}

bool Config::reportsOnly(const std::string& kind) const {
  // Only a lock a person releases, whatever a configuration says: never leak or high level
  const auto& all = reportableLocks();
  return std::find(all.begin(), all.end(), kind) != all.end() &&
         std::find(locks.reportOnly.begin(), locks.reportOnly.end(), kind) != locks.reportOnly.end();
}

double Config::channelFlow(int channel) const {
  auto it = channels.find(channel);
  return it == channels.end() ? kNaN : it->second.flowMlPerMin;
}

double phTarget(const RecipeCfg& r) {
  if (!isNum(r.phLow) || !isNum(r.phHigh)) return kNaN;
  return std::round((r.phLow + r.phHigh) / 2.0 * 100.0) / 100.0;
}

bool isTimeZoneName(std::string_view tz) {
  if (tz.empty() || tz.size() > 64) return false;
  const auto letter = [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); };
  if (!letter(tz.front()) || tz.back() == '/') return false;
  char prev = 0;
  for (char ch : tz) {
    const bool ok = letter(ch) || (ch >= '0' && ch <= '9') || ch == '_' || ch == '+' || ch == '-' || ch == '/';
    if (!ok || (ch == '/' && prev == '/')) return false;
    prev = ch;
  }
  return true;
}

void to_json(json& j, const Config& c) {
  j = json::object();
  j["schemaVersion"] = c.schemaVersion;
  j["revision"] = c.revision;
  j["system"] = {{"name", c.system.name},
                 {"language", c.system.language},
                 {"timezone", c.system.timezone},
                 {"passwordSet", c.system.passwordSet}};
  j["notify"] = {{"quietFromMin", c.notify.quietFromMin}, {"quietToMin", c.notify.quietToMin}};
  j["service"] = {{"days", c.service.days}};
  j["tank"] = {{"volumeL", numOrNull(c.tank.volumeL)},
               {"mixMinMin", numOrNull(c.tank.mixMinMin)},
               {"holdMin", numOrNull(c.tank.holdMin)},
               {"system", c.tank.system}};
  j["control"] = {{"ecTolerance", c.control.ecTolerance},       {"phTolerance", c.control.phTolerance},
                  {"maxEcStep", c.control.maxEcStep},           {"ecFloor", c.control.ecFloor},
                  {"phMaxMlPerDose", c.control.phMaxMlPerDose}, {"maxEcRounds", c.control.maxEcRounds},
                  {"maxPhDoses", c.control.maxPhDoses}};
  j["locks"] = {{"reportOnly", c.locks.reportOnly}};
  j["refill"] = {{"maxFillMin", numOrNull(c.refill.maxFillMin)}};
  j["circulation"] = {{"mode", c.circulation.mode}};
  json circuits = json::array();
  for (const auto& w : c.watering.circuits) {
    json times = json::array();
    for (const auto& t : w.times) times.push_back({{"atMin", t.atMin}, {"s", t.s}});
    circuits.push_back({{"id", w.id},           {"name", w.name},          {"mode", w.mode},
                        {"fromMin", w.fromMin}, {"toMin", w.toMin},        {"shotsPerDay", w.shotsPerDay},
                        {"shotS", w.shotS},     {"times", times},          {"everyMin", w.everyMin},
                        {"longestS", w.longestS}, {"rateLpm", numOrNull(w.rateLpm)}, {"dailyMaxS", numOrNull(w.dailyMaxS)},
                        {"paused", w.paused},       {"plant", w.plant}});
  }
  j["watering"] = {{"reservePct", c.watering.reservePct}, {"circuits", circuits}};
  json lights = json::array();
  for (const auto& l : c.lighting.circuits)
    lights.push_back({{"id", l.id},           {"name", l.name},           {"dimmed", l.dimmed},
                      {"startMin", l.startMin}, {"hours", numOrNull(l.hours)}, {"pct", numOrNull(l.pct)},
                      {"thresholdPct", numOrNull(l.thresholdPct)}, {"sunriseMin", l.sunriseMin}, {"sunsetMin", l.sunsetMin},
                      {"plant", l.plant}});
  j["lighting"] = {{"circuits", lights}};
  json rooms = json::array();
  for (const auto& r : c.climate.rooms)
    rooms.push_back({{"id", r.id},
                     {"name", r.name},
                     {"lights", r.lights},
                     {"startMin", r.startMin},
                     {"hours", numOrNull(r.hours)},
                     {"dayC", numOrNull(r.dayC)},
                     {"nightC", numOrNull(r.nightC)},
                     {"byVpd", r.byVpd},
                     {"rhPct", numOrNull(r.rhPct)},
                     {"vpdKpa", numOrNull(r.vpdKpa)},
                     {"co2Ppm", numOrNull(r.co2Ppm)},
                     {"heatC", numOrNull(r.heatC)},
                     {"heaterW", numOrNull(r.heaterW)},
                     {"circulateAlways", r.circulateAlways},
                     {"plant", r.plant}});
  j["climate"] = {{"rooms", rooms}};
  j["limits"] = {{"minRunS", c.limits.minRunS}, {"maxRunS", c.limits.maxRunS}, {"maxPartialRuns", c.limits.maxPartialRuns}};
  json canisters = json::array();
  for (const auto& b : c.canisters)
    canisters.push_back({{"id", b.id}, {"name", b.name}, {"role", b.role}, {"channel", b.channel}, {"pair", b.pair}, {"color", b.color},
                         {"capacityMl", numOrNull(b.capacityMl)},
                         {"maxMlPerDay", numOrNull(b.maxMlPerDay)},
                         {"device", b.device}});
  j["canisters"] = canisters;
  json recipes = json::array();
  for (const auto& r : c.recipes) {
    json steps = json::array();
    for (const auto& s : r.steps) steps.push_back({{"canister", s.canister}, {"mlPerL", numOrNull(s.mlPerL)}, {"fixed", s.fixed}});
    recipes.push_back({{"id", r.id},
                       {"name", r.name},
                       {"note", r.note},
                       {"steps", steps},
                       {"ecTarget", numOrNull(r.ecTarget)},
                       {"ph", isNum(r.phLow) && isNum(r.phHigh) ? json::array({r.phLow, r.phHigh}) : json(nullptr)}});
  }
  j["recipes"] = recipes;
  j["active"] = {{"recipe", c.active.recipe}, {"from", c.active.from}, {"until", c.active.until}};
  json plantRecipes = json::array();
  for (const auto& r : c.plantRecipes) plantRecipes.push_back({{"id", r.id}, {"name", r.name}, {"note", r.note}, {"phases", phasesJson(r.phases)}});
  j["plantRecipes"] = plantRecipes;
  json plants = json::array();
  for (const auto& p : c.plants)
    plants.push_back({{"id", p.id},
                      {"name", p.name},
                      {"from", p.from},
                      {"phases", phasesJson(p.phases)},
                      {"start", p.start},
                      {"phase", p.phase},
                      {"phaseStart", p.phaseStart},
                      {"harvestAt", p.harvestAt},
                      {"completedAt", p.completedAt}});
  j["plants"] = plants;
  j["lead"] = c.lead;
  json channels = json::object();
  for (const auto& [n, p] : c.channels) channels[std::to_string(n)] = {{"flowMlPerMin", numOrNull(p.flowMlPerMin)}, {"at", p.at}, {"head", p.head}};
  j["channels"] = channels;
  json setup = json::object();
  for (const auto& [id, m] : c.setup) setup[id] = {{"state", m.state}, {"at", m.at}};
  j["setup"] = setup;
  json devices = json::array();
  for (const auto& d : c.devices) devices.push_back(deviceToJson(d));
  j["devices"] = devices;
  json roles = json::object();
  for (const auto& [id, b] : c.roles) roles[id] = bindingToJson(b);
  j["roles"] = roles;
  j["probes"] = {{"ph", c.phCal}, {"ec", c.ecCal}};
}

json migrateConfig(json j) {
  if (!j.is_object()) throw std::invalid_argument("configuration is not an object");
  int v = intOr(j, "schemaVersion", kSchemaVersion);
  if (v < 1 || v > kSchemaVersion) throw std::invalid_argument("unknown schema version");
  if (v == 1) {
    // Doseph's terms (SD-107): a bottle is a canister, its kind is its role
    if (j.contains("bottles")) {
      json canisters = j["bottles"];
      if (canisters.is_array())
        for (auto& b : canisters)
          if (b.is_object() && b.contains("kind")) {
            b["role"] = b["kind"];
            b.erase("kind");
          }
      j["canisters"] = canisters;
      j.erase("bottles");
    }
    if (j.contains("recipes") && j["recipes"].is_array())
      for (auto& r : j["recipes"])
        if (r.is_object() && r.contains("steps") && r["steps"].is_array())
          for (auto& st : r["steps"])
            if (st.is_object() && st.contains("bottle")) {
              st["canister"] = st["bottle"];
              st.erase("bottle");
            }
    v = 2;
  }
  if (v == 2) {
    // A dosing pump is a channel (SD-107): the canister's pump and the pumps' calibrations
    if (j.contains("canisters") && j["canisters"].is_array())
      for (auto& b : j["canisters"])
        if (b.is_object() && b.contains("pump")) {
          b["channel"] = b["pump"];
          b.erase("pump");
        }
    if (j.contains("pumps")) {
      j["channels"] = j["pumps"];
      j.erase("pumps");
    }
    v = 3;
  }
  j["schemaVersion"] = v;
  return j;
}

Config configFromJson(const json& in) {
  const json j = migrateConfig(in);
  Config c;
  c.schemaVersion = kSchemaVersion;
  c.revision = intOr(j, "revision", 0);
  const json& s = objectAt(j, "system");
  c.system.name = jstr(s, "name", c.system.name);
  c.system.language = jstr(s, "language", c.system.language);
  c.system.timezone = jstr(s, "timezone", c.system.timezone);
  c.system.passwordSet = jbool(s, "passwordSet", false);
  const json& nt = objectAt(j, "notify");
  c.notify.quietFromMin = intOr(nt, "quietFromMin", c.notify.quietFromMin);
  c.notify.quietToMin = intOr(nt, "quietToMin", c.notify.quietToMin);
  const json& sv = objectAt(j, "service");
  if (sv.contains("days") && sv["days"].is_object())
    for (const auto& [k, v] : sv["days"].items())
      if (v.is_number_integer()) c.service.days[k] = v.get<int>();
  const json& t = objectAt(j, "tank");
  c.tank.volumeL = numOrNaN(t, "volumeL");
  // "circulation" from before SD-109 is read over: the device's own feed pump runs for a check
  if (t.contains("mixMinMin")) {
    c.tank.mixMinMin = numOrNaN(t, "mixMinMin");
  } else {
    // The two waits from before SD-112: the longer one, as the least wait the user wanted
    const double ec = numOrNaN(t, "waitEcMin"), ph = numOrNaN(t, "waitPhMin");
    c.tank.mixMinMin = isNum(ec) && isNum(ph) ? std::max(ec, ph) : isNum(ec) ? ec : ph;
  }
  if (t.contains("holdMin")) c.tank.holdMin = numOrNaN(t, "holdMin");
  c.tank.system = jstr(t, "system");
  const json& k = objectAt(j, "control");
  auto num = [&](const char* key, double& into) {
    if (k.contains(key)) into = numOrNaN(k, key);
  };
  num("ecTolerance", c.control.ecTolerance);
  num("phTolerance", c.control.phTolerance);
  num("maxEcStep", c.control.maxEcStep);
  num("ecFloor", c.control.ecFloor);
  num("phMaxMlPerDose", c.control.phMaxMlPerDose);
  c.control.maxEcRounds = intOr(k, "maxEcRounds", c.control.maxEcRounds);
  c.control.maxPhDoses = intOr(k, "maxPhDoses", c.control.maxPhDoses);
  for (const auto& x : arrayAt(objectAt(j, "locks"), "reportOnly")) {
    if (!x.is_string()) throw std::invalid_argument("a lock is not a string");
    c.locks.reportOnly.push_back(x.get<std::string>());
  }
  if (const json& f = objectAt(j, "refill"); f.contains("maxFillMin")) c.refill.maxFillMin = numOrNaN(f, "maxFillMin");
  if (const json& w = objectAt(j, "watering"); !w.empty()) {
    if (w.contains("reservePct")) c.watering.reservePct = numOrNaN(w, "reservePct");
    for (const auto& x : arrayAt(w, "circuits")) c.watering.circuits.push_back(waterCircuitFromJson(x));
  }
  if (const json& l = objectAt(j, "lighting"); !l.empty()) {
    for (const auto& x : arrayAt(l, "circuits")) c.lighting.circuits.push_back(lightCircuitFromJson(x));
  }
  if (const json& cl = objectAt(j, "climate"); !cl.empty()) {
    for (const auto& x : arrayAt(cl, "rooms")) c.climate.rooms.push_back(climateRoomFromJson(x));
  }
  if (const json& f = objectAt(j, "circulation"); f.contains("mode")) {
    if (!f["mode"].is_string()) throw std::invalid_argument("the circulation's mode is not a string");
    c.circulation.mode = f["mode"].get<std::string>();
  }
  const json& l = objectAt(j, "limits");
  if (l.contains("minRunS")) c.limits.minRunS = numOrNaN(l, "minRunS");
  if (l.contains("maxRunS")) c.limits.maxRunS = numOrNaN(l, "maxRunS");
  c.limits.maxPartialRuns = intOr(l, "maxPartialRuns", c.limits.maxPartialRuns);
  for (const auto& b : arrayAt(j, "canisters")) {
    if (!b.is_object()) throw std::invalid_argument("canister is not an object");
    CanisterCfg x;
    x.id = jstr(b, "id");
    x.name = jstr(b, "name");
    x.role = jstr(b, "role", x.role);
    x.channel = intOr(b, "channel", 0);
    x.pair = jstr(b, "pair");
    x.color = jstr(b, "color", x.color);
    x.capacityMl = numOrNaN(b, "capacityMl");
    x.maxMlPerDay = numOrNaN(b, "maxMlPerDay");
    x.device = jstr(b, "device");
    c.canisters.push_back(x);
  }
  for (const auto& r : arrayAt(j, "recipes")) {
    if (!r.is_object()) throw std::invalid_argument("recipe is not an object");
    RecipeCfg x;
    x.id = jstr(r, "id");
    x.name = jstr(r, "name");
    x.note = jstr(r, "note");
    x.ecTarget = numOrNaN(r, "ecTarget");
    if (r.contains("ph") && !r["ph"].is_null()) {
      const json& ph = r["ph"];
      if (!ph.is_array() || ph.size() != 2 || !ph[0].is_number() || !ph[1].is_number())
        throw std::invalid_argument("pH range is not two numbers");
      x.phLow = ph[0].get<double>();
      x.phHigh = ph[1].get<double>();
    }
    for (const auto& st : arrayAt(r, "steps")) {
      if (!st.is_object()) throw std::invalid_argument("recipe step is not an object");
      RecipeStep y;
      y.canister = jstr(st, "canister");
      y.mlPerL = numOrNaN(st, "mlPerL");
      y.fixed = jbool(st, "fixed", false);
      x.steps.push_back(y);
    }
    c.recipes.push_back(x);
  }
  const json& a = objectAt(j, "active");
  c.active.recipe = jstr(a, "recipe");
  c.active.from = epochOr(a, "from");
  c.active.until = epochOr(a, "until");
  for (const auto& r : arrayAt(j, "plantRecipes")) {
    if (!r.is_object()) throw std::invalid_argument("plant recipe is not an object");
    c.plantRecipes.push_back({jstr(r, "id"), jstr(r, "name"), jstr(r, "note"), phasesFromJson(r)});
  }
  for (const auto& p : arrayAt(j, "plants")) {
    if (!p.is_object()) throw std::invalid_argument("plant is not an object");
    PlantCfg x;
    x.id = jstr(p, "id");
    x.name = jstr(p, "name");
    x.from = jstr(p, "from");
    x.phases = phasesFromJson(p);
    x.start = epochOr(p, "start");
    x.phase = intOr(p, "phase", 0);
    x.phaseStart = epochOr(p, "phaseStart");
    x.harvestAt = epochOr(p, "harvestAt");
    x.completedAt = epochOr(p, "completedAt");
    c.plants.push_back(std::move(x));
  }
  c.lead = jstr(j, "lead");
  // A mark that does not fit its step is left out: the step is open again, nothing else changes
  for (const auto& [id, m] : objectAt(j, "setup").items()) {
    const SetupStep* st = setupStep(id);
    const std::string state = m.is_object() ? jstr(m, "state") : "";
    if (st && ((state == "done" && std::string(st->how) == "confirm") || (state == "skipped" && st->optional)))
      c.setup[id] = SetupMark{state, epochOr(m, "at")};
  }
  for (const auto& d : arrayAt(j, "devices")) c.devices.push_back(deviceFromJson(d));
  for (const auto& [id, b] : objectAt(j, "roles").items()) c.roles[id] = bindingFromJson(b);
  for (const auto& [key, v] : objectAt(j, "channels").items()) {
    int n = 0;
    try {
      n = std::stoi(key);
    } catch (...) {
      throw std::invalid_argument("channel number");
    }
    if (std::to_string(n) != key) throw std::invalid_argument("channel number");
    if (!v.is_object()) throw std::invalid_argument("channel calibration is not an object");
    c.channels[n] = ChannelCal{numOrNaN(v, "flowMlPerMin"), epochOr(v, "at"), jstr(v, "head")};
  }
  const json& p = objectAt(j, "probes");
  if (p.contains("ph")) c.phCal = p["ph"];
  if (p.contains("ec")) c.ecCal = p["ec"];
  return c;
}

std::vector<Msg> validateConfig(const Config& c, int channelCount) {
  std::vector<Msg> out;
  auto err = [&](Msg m) { out.push_back(std::move(m)); };

  if (c.system.name.empty() || c.system.name.size() > 40) err(say("cfg.system.name"));
  if (c.system.language != "en" && c.system.language != "de") err(say("cfg.system.language"));
  if (!isTimeZoneName(c.system.timezone)) err(say("cfg.system.timezone"));
  auto minute = [](int m) { return m >= 0 && m < 24 * 60; };
  if (!minute(c.notify.quietFromMin) || !minute(c.notify.quietToMin)) err(say("cfg.notify.quiet"));
  for (const auto& [kind, days] : c.service.days)
    if (serviceDefaultDays(kind) == 0 || days < kServiceMinDays || days > kServiceMaxDays)
      err(say("cfg.service.days", {{"min", kServiceMinDays}, {"max", kServiceMaxDays}}));

  // Limits only ever tighten (R7)
  const auto& L = c.limits;
  if (!isNum(L.minRunS) || L.minRunS < kHardMinRunS || L.minRunS > 10) err(say("cfg.limits.min_run"));
  if (!isNum(L.maxRunS) || L.maxRunS > kHardMaxRunS || L.maxRunS < L.minRunS) err(say("cfg.limits.max_run"));
  if (L.maxPartialRuns < 1 || L.maxPartialRuns > kHardMaxPartialRuns) err(say("cfg.limits.runs"));

  // A tolerance is never 0 (RAT-008); the rest within what control can handle
  const auto& k = c.control;
  if (!inRange(k.ecTolerance, 0.05, 1.0)) err(say("cfg.control.ec_tolerance", {{"min", 0.05}, {"max", 1.0}}));
  if (!inRange(k.phTolerance, 0.05, 1.0)) err(say("cfg.control.ph_tolerance", {{"min", 0.05}, {"max", 1.0}}));
  if (!inRange(k.maxEcStep, 0.1, 1.5)) err(say("cfg.control.max_ec_step", {{"min", 0.1}, {"max", 1.5}}));
  if (!inRange(k.ecFloor, 0.3, 3.0)) err(say("cfg.control.ec_floor", {{"min", 0.3}, {"max", 3.0}}));
  if (!inRange(k.phMaxMlPerDose, 0.5, 20)) err(say("cfg.control.ph_max_ml", {{"min", 0.5}, {"max", 20}}));
  if (k.maxEcRounds < 1 || k.maxEcRounds > 20) err(say("cfg.control.ec_rounds", {{"min", 1}, {"max", 20}}));
  if (k.maxPhDoses < 1 || k.maxPhDoses > 12) err(say("cfg.control.ph_doses", {{"min", 1}, {"max", 12}}));
  // Only a lock a person releases may report only, each once; never leak, high level or the cable
  // check (SD-133)
  std::set<std::string> reportOnly;
  for (const auto& kind : c.locks.reportOnly) {
    const auto& all = reportableLocks();
    if (kind == "hw.leak" || kind == "hw.high_level" || kind == "pump.cable") err(say("cfg.locks.always"));
    else if (std::find(all.begin(), all.end(), kind) == all.end()) err(say("cfg.locks.unknown", {{"lock", kind}}));
    else if (!reportOnly.insert(kind).second) err(say("cfg.locks.twice", {{"lock", say("lock." + kind)}}));
  }
  // The refill valve's longest fill (RAT-079)
  if (!inRange(c.refill.maxFillMin, kRefillMinMin, kRefillMaxMin))
    err(say("cfg.refill.max_fill", {{"min", kRefillMinMin}, {"max", kRefillMaxMin}}));
  if (c.circulation.mode != "mixing" && c.circulation.mode != "always") err(say("cfg.circulation.mode"));
  validateWatering(c, err);
  validateLighting(c, err);
  validateClimate(c, err);

  // Network devices and their roles (SD-132): an output only on a Shelly, one role per output
  std::set<std::string> deviceIds;
  if (c.devices.size() > kMaxNetDevices) err(say("cfg.device.too_many", {{"max", kMaxNetDevices}}));
  std::set<std::string> addresses;
  for (const auto& d : c.devices) {
    std::string addr = d.address;
    std::transform(addr.begin(), addr.end(), addr.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (!addr.empty() && !addresses.insert(addr).second) err(say("cfg.device.twice", {{"name", d.name}}));
    const bool idOk = d.id.size() >= 2 && d.id.size() <= 4 && d.id[0] == 'd' &&
                      std::all_of(d.id.begin() + 1, d.id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    if (!idOk || !deviceIds.insert(d.id).second) err(say("cfg.device.id", {{"id", d.id}}));
    if (d.kind != "shelly" && d.kind != "esphome") err(say("cfg.device.kind", {{"name", d.name}}));
    if (!peerAddress(d.address)) err(say("cfg.device.address", {{"name", d.name}}));
    if (d.name.empty() || chars(d.name) > 40) err(say("cfg.device.name", {{"max", 40}}));
  }
  std::set<std::string> switches;
  auto topicOk = [](const std::string& t) {
    return !t.empty() && t.size() <= 200 && t.find_first_of("+#") == std::string::npos;
  };
  for (const auto& [role, b] : c.roles) {
    const auto def = roleDef(c, role);
    if (!def) {
      err(say("cfg.role.unknown", {{"role", role}}));
      continue;
    }
    const Msg name = roleLabel(c, role);
    const NetDeviceCfg* d = c.device(b.device);
    if (def->kind == "output" && b.source != "shelly") err(say("cfg.role.output_shelly", {{"role", name}}));
    if (b.source == "shelly" || b.source == "esphome") {
      if (!d || d->kind != b.source) err(say("cfg.role.device", {{"role", name}}));
    } else if (b.source == "mqtt") {
      if (!topicOk(b.entity) || (!b.availability.empty() && !topicOk(b.availability)) || chars(b.field) > 60)
        err(say("cfg.role.topic", {{"role", name}}));
      // A float switch over MQTT says "not full" only while its sender is seen: a value kept on the
      // broker outlives a sender that stopped, and the float is the valve's limit (RAT-032)
      // The same holds for the limits of watering (SD-134)
      // And for a room's CO2, which opens the valve while it reads low (SD-136)
      else if ((role == "tank_full" || role == "tank_empty" || role == "tank_level" || def->kind == "co2") && b.availability.empty())
        err(say("cfg.role.availability", {{"role", name}}));
    } else {
      err(say("cfg.role.source", {{"role", name}}));
    }
    if (b.source == "shelly" && (b.channel < 0 || b.channel > 15)) err(say("cfg.role.channel", {{"role", name}, {"max", 15}}));
    if (b.source == "esphome") {
      const size_t slash = b.entity.find('/');
      const std::string domain = b.entity.substr(0, slash), object = slash == std::string::npos ? "" : b.entity.substr(slash + 1);
      const bool objOk = !object.empty() && object.size() <= 64 &&
                         std::all_of(object.begin(), object.end(), [](unsigned char ch) { return std::islower(ch) || std::isdigit(ch) || ch == '_'; });
      if ((domain != "binary_sensor" && domain != "sensor") || !objOk) err(say("cfg.role.entity", {{"role", name}}));
    }
    if (def->kind == "output" && b.source == "shelly" && !switches.insert(b.device + ":" + std::to_string(b.channel)).second)
      err(say("cfg.role.shared", {{"role", name}}));
  }

  const auto& t = c.tank;
  if (isNum(t.volumeL) && !inRange(t.volumeL, 1, 100000)) err(say("cfg.tank.volume", {{"min", 1}, {"max", 100000}}));
  if (isNum(t.mixMinMin) && !inRange(t.mixMinMin, 0, kWaitMaxMin)) err(say("cfg.tank.mix_min", {{"min", 0}, {"max", kWaitMaxMin}}));
  if (!inRange(t.holdMin, kHoldMinMin, kHoldMaxMin)) err(say("cfg.tank.hold", {{"min", kHoldMinMin}, {"max", kHoldMaxMin}}));

  std::set<std::string> ids;
  std::set<int> channels;
  std::map<std::string, int> pairCount;
  int phDowns = 0;
  for (const auto& b : c.canisters) {
    if (b.id.empty() || b.id.size() > 40 || !ids.insert(b.id).second) err(say("cfg.canister.id"));
    if (b.name.empty() || b.name.size() > 40) err(say("cfg.canister.name"));
    if (b.role != "nutrient" && b.role != "ph_down") err(say("cfg.canister.role", {{"name", b.name}}));
    // One pH− canister at most (SD-108)
    if (b.role == "ph_down" && ++phDowns == 2) err(say("cfg.canister.ph_down_twice", {{"name", b.name}}));
    // On the coupled device (SD-131): its ID, and a channel there; one canister per channel there too
    const bool remote = !b.device.empty();
    if (remote && (b.device.size() != 16 || b.device.find_first_not_of("0123456789abcdef") != std::string::npos))
      err(say("cfg.canister.device", {{"name", b.name}}));
    if (b.channel < 0 || b.channel > (remote ? kMaxChannels : std::min(channelCount, kMaxChannels)))
      err(say("cfg.canister.channel", {{"name", b.name}, {"max", remote ? kMaxChannels : channelCount}}));
    else if (b.channel > 0 && !channels.insert(remote ? 1000 + b.channel : b.channel).second)
      err(say("cfg.canister.channel_twice", {{"name", b.name}, {"channel", b.channel}}));
    if (!b.pair.empty() && b.role != "nutrient") err(say("cfg.canister.pair_role", {{"name", b.name}}));
    if (b.pair.size() > 8) err(say("cfg.canister.pair_name", {{"name", b.name}}));
    if (!std::isnan(b.maxMlPerDay) && !inRange(b.maxMlPerDay, 0.1, kCapacityMaxMl))
      err(say("cfg.canister.max_per_day", {{"name", b.name}, {"min", 0.1}, {"max", kCapacityMaxMl}}));
    if (!std::isnan(b.capacityMl) && !inRange(b.capacityMl, kCapacityMinMl, kCapacityMaxMl))
      err(say("cfg.canister.capacity", {{"name", b.name}, {"min", kCapacityMinMl}, {"max", kCapacityMaxMl}}));
    if (!b.pair.empty()) pairCount[b.pair]++;
  }

  std::set<std::string> recipeIds;  // what each recipe learned is kept by its ID
  for (const auto& r : c.recipes) {
    if (r.id.empty() || r.id.size() > 40 || !recipeIds.insert(r.id).second) err(say("cfg.recipe.id", {{"name", r.name}}));
    if (r.name.empty() || r.name.size() > 60) err(say("cfg.recipe.name"));
    if (r.steps.empty()) err(say("cfg.recipe.empty", {{"name", r.name}}));
    std::set<std::string> pairsIn, seen;
    double scaled = 0;
    for (const auto& s : r.steps) {
      const CanisterCfg* b = c.canister(s.canister);
      if (!b) {
        err(say("cfg.recipe.canister", {{"name", r.name}}));
        continue;
      }
      if (!seen.insert(b->id).second) err(say("cfg.recipe.twice", {{"name", r.name}, {"part", b->name}}));
      // pH correction is never part of a recipe; it always comes last (RAT-004)
      if (b->role != "nutrient") err(say("cfg.recipe.ph", {{"name", r.name}, {"part", b->name}}));
      if (!inRange(s.mlPerL, 0.001, 50)) err(say("cfg.recipe.amount", {{"name", r.name}, {"part", b->name}}));
      if (!b->pair.empty()) pairsIn.insert(b->pair);
      // A fixed part of a pair would leave its partner scaled alone (RAT-054, RAT-085)
      if (s.fixed && !b->pair.empty()) err(say("cfg.recipe.fixed_pair", {{"name", r.name}, {"part", b->name}}));
      if (!s.fixed && isNum(s.mlPerL) && s.mlPerL > 0) scaled += s.mlPerL;
    }
    // A pair goes into a recipe with all its parts, and with two at least (RAT-054); canisters are
    // added one at a time, so a pair of one is refused only where it is used
    for (const auto& p : pairsIn) {
      int in = 0;
      for (const auto& s : r.steps)
        if (const CanisterCfg* b = c.canister(s.canister); b && b->pair == p) in++;
      if (in != pairCount[p]) err(say("cfg.recipe.pair", {{"name", r.name}, {"pair", p}}));
      else if (pairCount[p] < 2) err(say("cfg.pair.single", {{"pair", p}}));
    }
    // The EC target lies in the range the robot controls, and the effect it
    // implies within a quarter to four times the start effect (RAT-056). The
    // message names the EC that would fit the amounts.
    if (!std::isnan(r.ecTarget)) {
      if (!inRange(r.ecTarget, 0.2, 4.0)) err(say("cfg.recipe.ec_range", {{"name", r.name}, {"min", 0.2}, {"max", 4.0}}));
      else if (scaled > 0) {
        const double e = r.ecTarget / scaled;
        if (e < kRefEffectMin * kEcStartEffect || e > kRefEffectMax * kEcStartEffect)
          err(say("cfg.recipe.ec_fit", {{"name", r.name},
                                        {"ec", r.ecTarget},
                                        {"low", std::max(0.2, kRefEffectMin * kEcStartEffect * scaled)},
                                        {"high", std::min(4.0, kRefEffectMax * kEcStartEffect * scaled)}}));
      }
    }
    if (std::isnan(r.phLow) != std::isnan(r.phHigh) || (isNum(r.phLow) && (!inRange(r.phLow, 4.5, 7.5) || !inRange(r.phHigh, 4.5, 7.5) || r.phLow >= r.phHigh)))
      err(say("cfg.recipe.ph_range", {{"name", r.name}, {"min", 4.5}, {"max", 7.5}}));
  }
  // Plant recipes and plants (SD-128): phases with a name, days, a recipe there is and targets in bounds
  auto checkPhases = [&](const std::string& name, const std::vector<PhaseCfg>& phases) {
    if (phases.empty() || phases.size() > kMaxPhases) err(say("cfg.plant.phases", {{"name", name}, {"max", kMaxPhases}}));
    for (const auto& ph : phases) {
      if (ph.name.empty() || chars(ph.name) > 40) err(say("cfg.phase.name", {{"name", name}}));
      if (ph.days < 1 || ph.days > 999) err(say("cfg.phase.days", {{"name", name}, {"phase", ph.name}, {"min", 1}, {"max", 999}}));
      if (!c.recipe(ph.recipe)) err(say("cfg.phase.recipe", {{"name", name}, {"phase", ph.name}}));
      if (isNum(ph.ecTarget) && !inRange(ph.ecTarget, 0.2, 4.0))
        err(say("cfg.recipe.ec_range", {{"name", ph.name}, {"min", 0.2}, {"max", 4.0}}));
      if (std::isnan(ph.phLow) != std::isnan(ph.phHigh) ||
          (isNum(ph.phLow) && (!inRange(ph.phLow, 4.5, 7.5) || !inRange(ph.phHigh, 4.5, 7.5) || ph.phLow >= ph.phHigh)))
        err(say("cfg.recipe.ph_range", {{"name", ph.name}, {"min", 4.5}, {"max", 7.5}}));
      if (ph.shotsPerDay && (*ph.shotsPerDay < 1 || *ph.shotsPerDay > 24 * 60))
        err(say("cfg.phase.shots", {{"name", name}, {"phase", ph.name}, {"max", 24 * 60}}));
      if (!std::isnan(ph.shotS) && !inRange(ph.shotS, 1, kMaxShotS))
        err(say("cfg.phase.shot", {{"name", name}, {"phase", ph.name}, {"max", kMaxShotS / 3600}}));
      if (ph.lightHours && !inRange(*ph.lightHours, 0, 24)) err(say("cfg.phase.light_hours", {{"name", name}, {"phase", ph.name}}));
      if (ph.lightPct && !inRange(*ph.lightPct, 1, 100)) err(say("cfg.phase.light_pct", {{"name", name}, {"phase", ph.name}}));
      // Climate (SD-136): within the room's bounds
      if ((ph.dayC && !inRange(*ph.dayC, 5, 40)) || (ph.nightC && !inRange(*ph.nightC, 5, 40)) || (ph.rhPct && !inRange(*ph.rhPct, 20, 95)) ||
          (ph.vpdKpa && !inRange(*ph.vpdKpa, 0.2, 2.5)) || (ph.co2Ppm && !inRange(*ph.co2Ppm, 400, kCo2MaxTargetPpm)))
        err(say("cfg.phase.climate", {{"name", name}, {"phase", ph.name}}));
    }
  };
  std::set<std::string> plantIds;
  for (const auto& r : c.plantRecipes) {
    if (r.id.empty() || !plantIds.insert("r:" + r.id).second) err(say("cfg.plant.id"));
    if (r.name.empty() || chars(r.name) > 40) err(say("cfg.plant.name"));
    checkPhases(r.name, r.phases);
  }
  for (const auto& p : c.plants) {
    if (p.id.empty() || !plantIds.insert("p:" + p.id).second) err(say("cfg.plant.id"));
    if (p.name.empty() || chars(p.name) > 40) err(say("cfg.plant.name"));
    checkPhases(p.name, p.phases);
    if (p.phase < 0 || p.phase >= static_cast<int>(p.phases.size())) err(say("cfg.plant.phase", {{"name", p.name}}));
  }
  if (!c.lead.empty() && !c.plant(c.lead)) err(say("cfg.plant.lead"));
  if (!c.tank.system.empty() &&
      std::none_of(std::begin(kGrowSystems), std::end(kGrowSystems), [&](const GrowSystem& g) { return c.tank.system == g.id; }))
    err(say("cfg.tank.system"));
  if (!c.active.recipe.empty()) {
    if (!c.recipe(c.active.recipe)) err(say("cfg.active.recipe"));
    if (c.active.until != 0 && c.active.until <= c.active.from) err(say("cfg.active.until"));
  }
  for (const auto& [n, p] : c.channels) {
    if (n < 1 || n > std::min(channelCount, kMaxChannels)) err(say("cfg.channel.number", {{"channel", n}}));
    // A calibration result that is missing, zero, negative or senseless is refused (RAT-003)
    if (!isNum(p.flowMlPerMin) || p.flowMlPerMin <= 0 || p.flowMlPerMin > 2000) err(say("cfg.channel.flow", {{"channel", n}}));
  }
  return out;
}

void to_json(json& j, const RuntimeState& s) {
  j = json::object();
  j["latches"] = s.latches;
  j["jumpLocks"] = s.jumpLocks;
  j["phEffect"] = numOrNull(s.phEffect);
  j["ecEffects"] = s.ecEffects;
  j["stockMl"] = s.stockMl;
  j["pairRound"] = s.pairRound;
  j["catchUp"] = s.catchUp;
  json pulled = json::object();
  for (const auto& [n, ms] : s.pulledBackMs) pulled[std::to_string(n)] = ms;
  j["pulledBackMs"] = pulled;
  json day = json::object();
  for (const auto& [n, runs] : s.dayRuns) {
    json list = json::array();
    for (const auto& [at, ms] : runs) list.push_back({at, ms});
    day[std::to_string(n)] = list;
  }
  j["dayRuns"] = day;
  j["ecInBand"] = s.ecInBand;
  j["topUp"] = s.topUp;
  j["refill"] = s.refill;
  j["watering"] = s.watering;
  j["waterLow"] = s.waterLow;
  j["lighting"] = s.lighting;
  j["settleMin"] = s.settleMin;
  j["alarms"] = s.alarms;
  j["lastReport"] = s.lastReport;
  j["serviceSince"] = s.serviceSince;
  j["serviceDone"] = s.serviceDone;
  j["stopped"] = s.stopped;
  j["newTank"] = s.newTank;
  j["flowLpm"] = numOrNull(s.flowLpm);
  j["flowAt"] = s.flowAt;
  j["pumpNormalA"] = numOrNull(s.pumpNormalA);
  j["filled"] = s.filled;
  j["pumpLearnedS"] = s.pumpLearnedS;
  if (s.clock) j["clock"] = *s.clock;
  j["bootCount"] = s.bootCount;
}

bool holds(const RuntimeState& rt, const std::string& id) {
  const auto it = rt.latches.find(id);
  if (it == rt.latches.end()) return false;
  const json& l = it->second;
  if (!l.is_object() || !jbool(l, "reportOnly", false)) return true;
  // A damaged state may say so of any latch: only a kind that may report only is taken at its word
  const std::string kind = jstr(l, "key", id);
  const auto& all = reportableLocks();
  return std::find(all.begin(), all.end(), kind) == all.end();
}

bool latched(const Config& cfg, const RuntimeState& rt, const std::string& id, const std::string& kind) {
  return holds(rt, id) || (rt.latches.count(id) && cfg.reportsOnly(kind));
}

bool reportStands(const RuntimeState& rt, const std::string& id) { return rt.latches.count(id) && !holds(rt, id); }

bool setLatch(const Config& cfg, RuntimeState& rt, const std::string& id, json entry, const std::string& kind) {
  const bool report = cfg.reportsOnly(kind.empty() ? id : kind);
  if (report) entry["reportOnly"] = true;
  rt.latches[id] = std::move(entry);
  return !report;
}

Msg reportedEvent(const std::string& kind, const json& latch) {
  const Msg lock = say("lock." + kind);
  if (latch.is_object() && latch.contains("channel") && latch["channel"].is_number_integer())
    return say("ev.lock.reported_channel", {{"channel", latch["channel"]}, {"lock", lock}});
  return say("ev.lock.reported", {{"lock", lock}});
}

Ms RuntimeState::dayRunMs(int channel, Epoch since) const {
  Ms sum = 0;
  if (auto it = dayRuns.find(channel); it != dayRuns.end())
    for (const auto& [at, ms] : it->second)
      if (at > since) sum += ms;
  return sum;
}

std::string stockStatus(const CanisterCfg& b, const RuntimeState& rt) {
  auto it = rt.stockMl.find(b.id);
  if (it == rt.stockMl.end() || !isNum(it->second)) return "unknown";
  if (it->second <= kStockBelowZeroMl) return "empty";
  return it->second < (b.role == "ph_down" ? kStockLowPhDownMl : kStockLowNutrientMl) ? "low" : "ok";
}

RuntimeState runtimeFromJson(const json& j) {
  RuntimeState s;
  if (!j.is_object()) return s;
  // Read as strictly as the latches need: a damaged entry throws, and the
  // robot treats the whole state as unknown (RAT-063)
  if (j.contains("latches")) s.latches = j["latches"].get<std::map<std::string, json>>();
  if (j.contains("jumpLocks")) s.jumpLocks = j["jumpLocks"].get<std::map<std::string, json>>();
  const double ph = jnum(j, "phEffect");
  if (isNum(ph) && ph > 0) s.phEffect = ph;
  // An entry that is no positive number counts as nothing learned (R5)
  if (j.contains("ecEffects") && j["ecEffects"].is_object())
    for (const auto& [k, v] : j["ecEffects"].items())
      if (v.is_number() && isNum(v.get<double>()) && v.get<double>() > 0) s.ecEffects[k] = v.get<double>();
  // A level is any finite number, below zero too (SD-119); anything else is unknown
  if (j.contains("stockMl") && j["stockMl"].is_object())
    for (const auto& [k, v] : j["stockMl"].items())
      if (v.is_number() && isNum(v.get<double>())) s.stockMl[k] = v.get<double>();
  if (j.contains("pairRound") && j["pairRound"].is_object()) s.pairRound = j["pairRound"];
  if (j.contains("catchUp") && j["catchUp"].is_object()) s.catchUp = j["catchUp"];
  // A line pulled back for 1 ms to 1 s; anything else counts as not pulled back
  if (j.contains("pulledBackMs") && j["pulledBackMs"].is_object())
    for (const auto& [k, v] : j["pulledBackMs"].items()) {
      const int n = std::atoi(k.c_str());
      if (n > 0 && std::to_string(n) == k && v.is_number_integer() && v.get<Ms>() > 0 && v.get<Ms>() <= 1000) s.pulledBackMs[n] = v.get<Ms>();
    }
  // The runs that count against the daily cap: a damaged entry is dropped, the rest kept (SD-113)
  if (j.contains("dayRuns") && j["dayRuns"].is_object())
    for (const auto& [k, v] : j["dayRuns"].items()) {
      const int n = std::atoi(k.c_str());
      if (n <= 0 || std::to_string(n) != k || !v.is_array()) continue;
      for (const auto& r : v)
        if (r.is_array() && r.size() == 2 && r[0].is_number_integer() && r[1].is_number_integer() && r[1].get<Ms>() > 0)
          s.dayRuns[n].emplace_back(r[0].get<Epoch>(), r[1].get<Ms>());
    }
  if (j.contains("ecInBand") && j["ecInBand"].is_object()) s.ecInBand = j["ecInBand"];
  s.topUp = jbool(j, "topUp", false);
  if (j.contains("refill") && j["refill"].is_object()) s.refill = j["refill"];
  if (j.contains("watering") && j["watering"].is_object()) s.watering = j["watering"];
  s.waterLow = jbool(j, "waterLow", false);
  if (j.contains("lighting") && j["lighting"].is_object()) s.lighting = j["lighting"];
  if (j.contains("settleMin") && j["settleMin"].is_array())
    for (const auto& m : j["settleMin"])
      if (m.is_number() && isNum(m.get<double>()) && m.get<double>() >= 0 && s.settleMin.size() < 3) s.settleMin.push_back(m.get<double>());
  // An alarm's state that is no object is dropped: at worst it is pushed once more (SD-123)
  if (j.contains("alarms") && j["alarms"].is_object())
    for (const auto& [k, v] : j["alarms"].items())
      if (v.is_object()) s.alarms[k] = v;
  s.lastReport = j.contains("lastReport") && j["lastReport"].is_number_integer() ? j["lastReport"].get<Epoch>() : 0;
  // A time that is no whole number is dropped: the item counts from now (SD-125)
  for (const char* key : {"serviceSince", "serviceDone"})
    if (j.contains(key) && j[key].is_object())
      for (const auto& [k, v] : j[key].items())
        if (v.is_number_integer()) (std::string(key) == "serviceSince" ? s.serviceSince : s.serviceDone)[k] = v.get<Epoch>();
  s.stopped = jbool(j, "stopped", false);
  s.newTank = jbool(j, "newTank", true);
  const double flow = jnum(j, "flowLpm");
  if (isNum(flow) && flow > 0) s.flowLpm = flow;
  s.flowAt = j.contains("flowAt") && j["flowAt"].is_number_integer() ? j["flowAt"].get<Epoch>() : 0;
  const double amps = jnum(j, "pumpNormalA");
  if (isNum(amps) && amps > 0) s.pumpNormalA = amps;
  const double learned = jnum(j, "pumpLearnedS");
  if (isNum(learned) && learned > 0 && isNum(s.pumpNormalA)) s.pumpLearnedS = learned;
  if (j.contains("filled") && j["filled"].is_array())
    for (const auto& n : j["filled"])
      if (n.is_number_integer()) s.filled.insert(n.get<int>());
  if (j.contains("clock")) s.clock = stampFromJson(j["clock"]);
  if (j.contains("bootCount") && j["bootCount"].is_number_unsigned()) s.bootCount = j["bootCount"].get<std::uint32_t>();
  return s;
}

}  // namespace doseph
