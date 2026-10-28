// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <stdexcept>

#include "doseph/messages.hpp"

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

}  // namespace

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
    if (channel > 0 && b.channel == channel) return &b;
  return nullptr;
}

const RecipeCfg* Config::recipe(const std::string& id) const {
  for (const auto& r : recipes)
    if (r.id == id) return &r;
  return nullptr;
}

const CanisterCfg* Config::phDown() const {
  for (const auto& b : canisters)
    if (b.role == "ph_down" && b.channel > 0) return &b;
  return nullptr;
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
  j["tank"] = {{"volumeL", numOrNull(c.tank.volumeL)},
               {"mixMinMin", numOrNull(c.tank.mixMinMin)},
               {"holdMin", numOrNull(c.tank.holdMin)}};
  j["control"] = {{"ecTolerance", c.control.ecTolerance},       {"phTolerance", c.control.phTolerance},
                  {"maxEcStep", c.control.maxEcStep},           {"ecFloor", c.control.ecFloor},
                  {"phMaxMlPerDose", c.control.phMaxMlPerDose}, {"maxEcRounds", c.control.maxEcRounds},
                  {"maxPhDoses", c.control.maxPhDoses}};
  j["limits"] = {{"minRunS", c.limits.minRunS}, {"maxRunS", c.limits.maxRunS}, {"maxPartialRuns", c.limits.maxPartialRuns}};
  json canisters = json::array();
  for (const auto& b : c.canisters)
    canisters.push_back({{"id", b.id}, {"name", b.name}, {"role", b.role}, {"channel", b.channel}, {"pair", b.pair}, {"color", b.color},
                         {"capacityMl", numOrNull(b.capacityMl)}});
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
  json channels = json::object();
  for (const auto& [n, p] : c.channels) channels[std::to_string(n)] = {{"flowMlPerMin", numOrNull(p.flowMlPerMin)}, {"at", p.at}, {"head", p.head}};
  j["channels"] = channels;
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
  const json& t = objectAt(j, "tank");
  c.tank.volumeL = numOrNaN(t, "volumeL");
  // "circulation" from before SD-109 is read over: the device's own suction pump runs for a check
  if (t.contains("mixMinMin")) {
    c.tank.mixMinMin = numOrNaN(t, "mixMinMin");
  } else {
    // The two waits from before SD-112: the longer one, as the least wait the user wanted
    const double ec = numOrNaN(t, "waitEcMin"), ph = numOrNaN(t, "waitPhMin");
    c.tank.mixMinMin = isNum(ec) && isNum(ph) ? std::max(ec, ph) : isNum(ec) ? ec : ph;
  }
  if (t.contains("holdMin")) c.tank.holdMin = numOrNaN(t, "holdMin");
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
    if (b.channel < 0 || b.channel > std::min(channelCount, kMaxChannels)) err(say("cfg.canister.channel", {{"name", b.name}, {"max", channelCount}}));
    else if (b.channel > 0 && !channels.insert(b.channel).second) err(say("cfg.canister.channel_twice", {{"name", b.name}, {"channel", b.channel}}));
    if (!b.pair.empty() && b.role != "nutrient") err(say("cfg.canister.pair_role", {{"name", b.name}}));
    if (b.pair.size() > 8) err(say("cfg.canister.pair_name", {{"name", b.name}}));
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
  j["stopped"] = s.stopped;
  j["newTank"] = s.newTank;
  j["flowLpm"] = numOrNull(s.flowLpm);
  j["flowAt"] = s.flowAt;
  if (s.clock) j["clock"] = *s.clock;
  j["bootCount"] = s.bootCount;
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
  s.stopped = jbool(j, "stopped", false);
  s.newTank = jbool(j, "newTank", true);
  const double flow = jnum(j, "flowLpm");
  if (isNum(flow) && flow > 0) s.flowLpm = flow;
  s.flowAt = j.contains("flowAt") && j["flowAt"].is_number_integer() ? j["flowAt"].get<Epoch>() : 0;
  if (j.contains("clock")) s.clock = stampFromJson(j["clock"]);
  if (j.contains("bootCount") && j["bootCount"].is_number_unsigned()) s.bootCount = j["bootCount"].get<std::uint32_t>();
  return s;
}

}  // namespace doseph
