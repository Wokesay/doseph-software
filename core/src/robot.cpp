// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/robot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "doseph/embedded.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {

constexpr const char* kConfigFile = "config.json";
constexpr const char* kStateFile = "state.json";
constexpr const char* kAuthFile = "auth.json";
constexpr const char* kEventsFile = "events.json";
constexpr const char* kEventsLog = "events.log";
constexpr const char* kHistoryFile = "history.bin";
constexpr const char* kHistoryLog = "history.log";
constexpr Epoch kSampleS = 10;           // history: one sample per 10 s (finest tier)
constexpr Epoch kSnapshotS = 6 * 3600;   // history and events written whole, their journals emptied
constexpr Epoch kStateSaveS = 60;        // the clock in the state, at most this often
constexpr Ms kProbeIdle = 30 * kMinute;  // a probe calibration explains jumps this long after its last step (RAT-042)
const std::set<std::string> kSeries = {"ph", "ec", "temp", "flow"};

Msg changed(const char* what) { return say("ev.config.changed", {{"what", say(what)}}); }

}  // namespace

Result Result::fail(int st, const Msg& m, json extra) {
  json body = {{"error", m}};
  for (auto& [k, v] : extra.items()) body[k] = v;
  return {st, std::move(body)};
}

Robot::Robot(IHardware& hw, IStorage& storage, const IClock& clock, RandomFn rng)
    : hw_(hw), store_(storage), clock_(clock), rng_(std::move(rng)), auth_(rng_), act_(hw), journal_(storage) {}

json Robot::templates() { return json::parse(embedded::kTemplatesJson); }

Ctx Robot::ctx() {
  Ctx c{cfg_, rt_, truth_, log_, journal_};
  c.now = clock_.nowMs();
  c.epoch = clock_.epoch();
  c.stopped = rt_.stopped;
  c.flowOk = flowOk();
  c.probeCalibrating = probe_.has_value();
  c.pumpJob = pumpJobActive();
  return c;
}

bool Robot::flowOk() const {
  const Reading& f = truth_.get(Channel::Flow);
  return f.usable() && *f.value >= kFlowMinLpm;
}

void Robot::markPasswordSet() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  next.system.passwordSet = true;
  saveConfig(next, std::nullopt);
}

std::string Robot::armSetupCode(Ms validFor) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (auth_.hasPassword() || credentialsLost()) return "";
  return auth_.armSetupCode(clock_.nowMs(), validFor);
}

bool Robot::saveAuth() { return store_.write(kAuthFile, auth_.toJson().dump()); }

void Robot::logEvent(const std::string& type, const std::string& sev, Msg title, Msg text) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  log_.add(clock_.epoch(), type, sev, std::move(title), std::move(text));
  saveEvents();
}

// ---------------------------------------------------------------- start

// After a restart the outputs are off, while the recorded facts come back:
// latches, jump locks, learned effects, the Stop, an open dose (RAT-028, R6)
void Robot::boot() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  bool stateUnknown = false;
  if (auto s = store_.read(kConfigFile)) {
    try {
      Config c = configFromJson(json::parse(*s));
      if (validateConfig(c, hw_.pumpCount()).empty()) cfg_ = c;
      else stateUnknown = true;
    } catch (...) {
      stateUnknown = true;
    }
    // A configuration that cannot be read is kept as a copy and the robot stays stopped (R6)
    if (stateUnknown) store_.write("config.broken.json", *s);
  }
  if (auto s = store_.read(kStateFile)) {
    try {
      rt_ = runtimeFromJson(json::parse(*s));
    } catch (...) {
      // Whether a latch stood is unknown: the robot starts stopped, a person looks (RAT-063)
      rt_ = RuntimeState{};
      stateUnknown = true;
    }
  }
  if (auto s = store_.read(kAuthFile)) {
    const json j = json::parse(*s, nullptr, false);
    if (j.is_object()) auth_.load(j);
  }
  if (auto s = store_.read(kEventsFile)) {
    const json j = json::parse(*s, nullptr, false);
    if (j.is_array() || j.is_object()) log_.load(j);
  }
  if (auto s = store_.read(kEventsLog)) log_.replay(*s);
  savedEventId_ = log_.lastId();
  if (auto s = store_.read(kHistoryFile)) history_.load(*s);
  if (auto s = store_.read(kHistoryLog)) history_.replay(*s, kSeries);

  rt_.bootCount++;
  clock_.start(rt_.clock, log_.newestTs(), rt_.bootCount);
  bootEpoch_ = clock_.epoch();
  bootMs_ = clock_.nowMs();
  lastSnapshot_ = bootEpoch_;
  doser_.setBootTag(std::to_string(rt_.bootCount));
  // After a start everything is off; interrupted runs are reported, not resumed (R6, RAT-007)
  act_.stopAll();
  if (stateUnknown) {
    rt_.stopped = true;
    log_.add(bootEpoch_, "alarm", "alarm", say("ev.boot.state_unknown"), say("ev.boot.state_unknown.text"));
  }
  journal_.load();
  for (const auto& r : journal_.closeOpen()) {
    if (r.intoTank())
      log_.add(bootEpoch_, "dose", "alarm", say("ev.boot.cut_dose", {{"name", r.name}}),
               say("ev.boot.cut_dose.text", {{"name", r.name}, {"ml", numOrNull(r.ml)}}), {{"canister", r.canister}, {"ml", numOrNull(r.ml)}});
    else
      log_.add(bootEpoch_, "dose", "notice", say("ev.boot.cut_cup", {{"pump", r.pump}}), say("ev.boot.cut_cup.text"));
  }
  if (journal_.anyIntoTank()) {
    // Doses not mixed in and measured yet: the first cycle mixes first and learns nothing (SD-106)
    Ctx c = ctx();
    const CycleSettings s = cycleSettings(c);
    const double m = std::max({kWaitMinMin, isNum(s.waitEcMin) ? s.waitEcMin : 0.0, isNum(s.waitPhMin) ? s.waitPhMin : 0.0});
    cycle_.mixFirst(static_cast<Ms>(m * 60) * kSecond);
    log_.add(bootEpoch_, "control", "notice", say("ev.boot.mix_first"), say("ev.boot.mix_first.text", {{"min", m}}));
  } else {
    journal_.clear();
  }
  log_.add(bootEpoch_, "system", rt_.stopped ? "warn" : "info", say("ev.boot"),
           rt_.stopped ? say("ev.boot.stopped") : Msg{});
  saveState(true);
  saveEvents();
}

// ---------------------------------------------------------------- tick

void Robot::tick() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  clock_.poll();
  const Ms now = clock_.nowMs();
  const Epoch epoch = clock_.epoch();
  hw_.poll(now);
  truth_.update(cfg_, hw_, rt_, now, epoch);
  // A probe calibration explains jumps while someone works on it (RAT-042)
  if (probe_ && now - probe_->lastStep < kProbeIdle)
    for (Channel ch : {Channel::Ph, Channel::Ec, Channel::Temp}) truth_.expectChange(ch, now + 10 * kSecond);
  Ctx c = ctx();
  act_.enforce(c);
  doser_.tick(c, act_);
  if (auto p = doser_.takeFinished()) {
    if (job_ && p->orderId == job_->id) pumpJobDone(*p);
    else cycle_.doseFinished(c, *p);
  }
  c.pumpJob = pumpJobActive();
  cycle_.tick(c, act_, doser_);

  if (epoch - lastSample_ >= kSampleS) {
    lastSample_ = epoch;
    sampleHistory(epoch);
  }
  if (epoch - lastWatch_ >= 5 || lastWatch_ == 0) {
    lastWatch_ = epoch;
    WatchInput in{cfg_, rt_, {}};
    for (Channel ch : kChannels) in.readings.push_back(truth_.get(ch));
    const Phase ph = cycle_.phase();
    in.needsFlow = ph == Phase::ProveFlow || ph == Phase::Flush || ph == Phase::Measure || ph == Phase::Remeasure ||
                   ph == Phase::Dose || ph == Phase::Mix;
    in.lastPh = cycle_.last().ph;
    in.lastEc = cycle_.last().ec;
    in.lastAt = cycle_.last().at;
    in.epoch = epoch;
    in.bootEpoch = bootEpoch_;
    in.stopped = rt_.stopped;
    in.pumpCount = hw_.pumpCount();
    watch_ = evaluate(in);
  }
  saveState();
  saveEvents();
  if (epoch - lastSnapshot_ >= kSnapshotS) snapshot(epoch);
}

void Robot::sampleHistory(Epoch epoch) {
  // pH, EC and temperature only with flow: without it the measuring chamber holds standing water, not the tank's;
  // a gap stays a gap (RAT-016)
  const bool flow = flowOk();
  std::vector<std::pair<std::string, double>> samples;
  for (Channel ch : kChannels) {
    const Reading& r = truth_.get(ch);
    if (!r.usable() || (ch != Channel::Flow && !flow)) continue;
    samples.emplace_back(channelName(ch), *r.value);
    history_.add(channelName(ch), epoch, *r.value);
  }
  if (!samples.empty()) store_.append(kHistoryLog, History::record(epoch, samples));
}

void Robot::snapshot(Epoch epoch) {
  lastSnapshot_ = epoch;
  // Snapshot first, then the empty journal: a journal that outlives its snapshot is not counted twice
  if (store_.write(kHistoryFile, history_.dump())) store_.write(kHistoryLog, "");
  saveEvents();
  if (store_.write(kEventsFile, log_.toJson().dump())) store_.write(kEventsLog, "");
}

void Robot::flush() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  saveState(true);
  saveEvents();
  snapshot(clock_.epoch());
}

void Robot::saveState(bool force) {
  rt_.clock = clock_.stamp();
  json j = rt_;
  json cmp = j;
  cmp.erase("clock");
  const std::string s = cmp.dump();
  const Epoch epoch = clock_.epoch();
  // A latch or a learned value is saved in the tick it changes; the clock once a minute
  if (!force && s == savedState_ && epoch - lastStateSave_ < kStateSaveS) return;
  if (store_.write(kStateFile, j.dump())) {
    savedState_ = s;
    lastStateSave_ = epoch;
  }
}

void Robot::saveEvents() {
  const std::uint64_t last = log_.lastId();
  if (last == savedEventId_) return;
  if (store_.append(kEventsLog, log_.journalSince(savedEventId_))) savedEventId_ = last;
}

// ---------------------------------------------------------------- reading

json Robot::info() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return {{"product", "Doseph"},
          {"version", embedded::kVersion},
          {"api", 1},
          {"schemaVersion", kSchemaVersion},
          {"platform", platform_},
          {"hasPassword", auth_.hasPassword()},
          {"setupCode", auth_.setupCodeRequired()},
          {"name", cfg_.system.name},
          {"language", cfg_.system.language},
          {"pumpCount", hw_.pumpCount()}};
}

json Robot::configJson() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return cfg_;
}

json Robot::state() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Ctx c = ctx();
  json readings = json::object();
  for (Channel ch : kChannels) {
    const Reading& r = truth_.get(ch);
    readings[channelName(ch)] = {{"value", numOrNull(r.value)},
                                 {"quality", qualityName(r.quality)},
                                 {"reason", r.reason},
                                 {"ageS", r.ageMs >= 0 ? json(r.ageMs / 1000) : json(nullptr)},
                                 {"unit", r.unit},
                                 {"decimals", r.decimals},
                                 {"usable", r.usable()}};
  }
  json watch = watch_;
  watch["stale"] = clock_.epoch() - watch_.evaluatedAt > kWatchStaleS;  // an old assessment is none (RAT-073)
  json dosing = nullptr;
  if (const auto& a = doser_.active())
    dosing = {{"order", a->id},
              {"purpose", a->purpose},
              {"canister", a->step.canister},
              {"name", a->step.name},
              {"ml", numOrNull(a->step.ml)},
              {"mlDone", doser_.progress().mlDone},
              {"run", doser_.progress().run + 1},
              {"runs", a->step.runs.size()}};
  json pumps = json::array();
  for (int n = 1; n <= hw_.pumpCount(); ++n) {
    const CanisterCfg* b = cfg_.canisterOnPump(n);
    auto it = cfg_.pumps.find(n);
    pumps.push_back({{"pump", n},
                     {"canister", b ? json(b->id) : json(nullptr)},
                     {"flowMlPerMin", it == cfg_.pumps.end() ? json(nullptr) : numOrNull(it->second.flowMlPerMin)},
                     {"at", it == cfg_.pumps.end() ? json(nullptr) : json(it->second.at)}});
  }
  json job = nullptr;
  if (job_)
    job = {{"id", job_->id}, {"type", job_->type}, {"pump", job_->pump}, {"state", job_->state},
           {"seconds", job_->ms / 1000.0}, {"actualS", job_->actualMs / 1000.0}, {"ml", numOrNull(job_->ml)},
           {"message", job_->message}, {"at", job_->at}};
  json probe = nullptr;
  if (probe_) {
    auto raw = [&](Channel ch) {
      auto s = hw_.sample(ch);
      return s ? numOrNull(s->raw) : json(nullptr);
    };
    probe = {{"kind", probe_->kind}, {"points", probe_->points}, {"rawPh", raw(Channel::Ph)}, {"rawEc", raw(Channel::Ec)},
             {"temp", numOrNull(truth_.get(Channel::Temp).value)}};
  }
  json ecEffects = json::object();
  for (const auto& [k, v] : rt_.ecEffects) ecEffects[k] = numOrNull(v);
  return {{"now", c.epoch},
          {"uptimeS", (c.now - bootMs_) / 1000},
          {"clock", clock_.source()},
          {"stopped", rt_.stopped},
          {"online", hw_.online()},
          {"circulation", [&] {
             auto st = act_.circulation();
             return st ? json(*st) : json(nullptr);
           }()},
          {"flowOk", c.flowOk},
          {"readings", readings},
          {"cycle", cycle_.status(c)},
          {"watchdog", watch},
          {"latches", rt_.latches},
          {"jumpLocks", rt_.jumpLocks},
          {"newTank", rt_.newTank},
          {"effects", {{"ph", numOrNull(rt_.phEffect)}, {"ecByRecipe", ecEffects}}},
          {"dosing", dosing},
          {"pumps", pumps},
          {"pumpJob", job},
          {"probe", probe},
          {"eventId", log_.lastId()}};
}

json Robot::history(const std::string& series, Epoch from, Epoch to, size_t points) const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  SeriesPoints p = history_.query(series, from, to, points);
  auto arr = [](const std::vector<double>& v) {
    json a = json::array();
    for (double x : v) a.push_back(numOrNull(x));
    return a;
  };
  return {{"series", series}, {"stepS", p.stepS}, {"t", p.t}, {"avg", arr(p.avg)}, {"min", arr(p.min)}, {"max", arr(p.max)}};
}

std::string Robot::historyCsv(const std::vector<std::string>& series, Epoch from, Epoch to, const std::string& lang) const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const bool de = lang == "de";
  const char sep = de ? ';' : ',';
  std::string out = de ? "zeit_utc" : "time_utc";
  std::map<Epoch, std::vector<double>> rows;
  std::vector<int> decimals;
  for (size_t i = 0; i < series.size(); ++i) {
    out += sep;
    out += series[i];
    int d = 3;
    for (Channel ch : kChannels)
      if (series[i] == channelName(ch)) d = channelDef(ch).decimals + 1;  // a mean carries one digit more
    decimals.push_back(d);
    const SeriesPoints p = history_.query(series[i], from, to, 0);
    for (size_t k = 0; k < p.t.size(); ++k) {
      auto& row = rows[p.t[k]];
      row.resize(series.size(), kNaN);
      row[i] = p.avg[k];
    }
  }
  out += '\n';
  char buf[64];
  for (const auto& [t, row] : rows) {
    out += utcText(t);
    for (size_t i = 0; i < row.size(); ++i) {
      out += sep;
      if (!isNum(row[i])) continue;  // a gap stays empty, never 0 (RAT-016, RAT-006)
      std::snprintf(buf, sizeof buf, "%.*f", decimals[i], row[i]);
      std::string n(buf);
      if (de) std::replace(n.begin(), n.end(), '.', ',');
      out += n;
    }
    out += '\n';
  }
  return out;
}

json Robot::events(Epoch from, Epoch to, const std::string& type, size_t limit) const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  json a = json::array();
  for (const auto& e : log_.query(from, to, type, limit)) a.push_back(e);
  return a;
}

// ---------------------------------------------------------------- settings

Result Robot::saveConfig(Config next, const std::optional<Msg>& what) {
  auto errs = validateConfig(next, hw_.pumpCount());
  if (!errs.empty()) return Result::fail(422, errs.front(), {{"errors", errs}});
  next.revision = cfg_.revision + 1;
  if (!store_.write(kConfigFile, json(next).dump())) return Result::fail(500, say("store.failed"));
  cfg_ = std::move(next);
  if (what) log_.add(clock_.epoch(), "config", "info", *what);
  return Result::ok({{"revision", cfg_.revision}});
}

void Robot::restartCycle() {
  Ctx c = ctx();
  cycle_.restart(c, act_, doser_);
}

Result Robot::setSystem(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  next.system.name = utf8Prefix(jstr(j, "name", next.system.name), 60);
  next.system.language = jstr(j, "language", next.system.language);
  next.system.timezone = jstr(j, "timezone", next.system.timezone);
  return saveConfig(next, changed("cfg.what.system"));
}

Result Robot::putTank(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  // A field left out stays; null clears it (a mixing wait back to the suggestion); anything else is refused
  auto num = [&](const char* key, double& into) {
    if (!j.contains(key)) return true;
    if (j[key].is_null()) into = kNaN;
    else if (j[key].is_number()) into = j[key].get<double>();
    else return false;
    return true;
  };
  if (!num("volumeL", next.tank.volumeL) || !num("waitEcMin", next.tank.waitEcMin) || !num("waitPhMin", next.tank.waitPhMin) ||
      !num("holdMin", next.tank.holdMin))
    return Result::fail(422, say("api.bad_input"));
  next.tank.circulation = jstr(j, "circulation", next.tank.circulation);
  const bool volumeChanged = !(std::isnan(next.tank.volumeL) && std::isnan(cfg_.tank.volumeL)) && next.tank.volumeL != cfg_.tank.volumeL;
  Result r = saveConfig(next, changed("cfg.what.tank"));
  if (r.status != 200) return r;
  if (volumeChanged) {
    // Another volume: the next dose checks it again (SD-105)
    rt_.newTank = true;
    rt_.latches.erase("tank.volume");
  }
  restartCycle();
  saveState(true);
  return r;
}

Result Robot::putControl(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  auto num = [&](const char* key, double& into) {
    if (j.contains(key) && j[key].is_number()) into = j[key].get<double>();
  };
  num("ecTolerance", next.control.ecTolerance);
  num("phTolerance", next.control.phTolerance);
  num("maxEcStep", next.control.maxEcStep);
  num("ecFloor", next.control.ecFloor);
  num("phMaxMlPerDose", next.control.phMaxMlPerDose);
  if (j.contains("maxEcRounds") && j["maxEcRounds"].is_number_integer()) next.control.maxEcRounds = j["maxEcRounds"].get<int>();
  if (j.contains("maxPhDoses") && j["maxPhDoses"].is_number_integer()) next.control.maxPhDoses = j["maxPhDoses"].get<int>();
  return saveConfig(next, changed("cfg.what.control"));
}

Result Robot::newTank() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  // A person announces a new fill: its jumps are explained, and the next dose is a test dose (SD-105, RAT-042)
  rt_.newTank = true;
  rt_.latches.erase("tank.volume");
  rt_.jumpLocks.clear();
  const Ms now = clock_.nowMs();
  for (Channel ch : {Channel::Ph, Channel::Ec, Channel::Temp}) truth_.expectChange(ch, now + 10 * kMinute);
  log_.add(clock_.epoch(), "control", "info", say("ev.tank.new"), say("ev.tank.new.text"));
  restartCycle();
  saveState(true);
  return Result::ok();
}

Result Robot::putCanister(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  std::string id = jstr(j, "id");
  CanisterCfg* b = nullptr;
  for (auto& x : next.canisters)
    if (x.id == id) b = &x;
  if (!b) {
    if (id.empty()) {
      do id = "b" + std::to_string(++idSeq_);
      while (next.canister(id));
    }
    next.canisters.push_back(CanisterCfg{});
    b = &next.canisters.back();
    b->id = id;
  }
  b->name = utf8Prefix(jstr(j, "name", b->name), 60);
  b->role = jstr(j, "role", b->role);
  if (j.contains("pump")) {
    if (!j["pump"].is_number_integer()) return Result::fail(422, say("cfg.canister.pump", {{"name", b->name}, {"max", hw_.pumpCount()}}));
    b->pump = j["pump"].get<int>();
  }
  b->pair = jstr(j, "pair", b->pair);
  b->color = jstr(j, "color", b->color);
  Result r = saveConfig(next, changed("cfg.what.canisters"));
  if (r.status == 200) r.body["id"] = id;
  return r;
}

Result Robot::deleteCanister(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!cfg_.canister(id)) return Result::fail(404, say("api.not_found"));
  for (const auto& r : cfg_.recipes)
    for (const auto& s : r.steps)
      if (s.canister == id) return Result::fail(409, say("canister.in_recipe", {{"name", r.name}}));
  Config next = cfg_;
  next.canisters.erase(std::remove_if(next.canisters.begin(), next.canisters.end(), [&](const CanisterCfg& b) { return b.id == id; }),
                     next.canisters.end());
  return saveConfig(next, changed("cfg.what.canisters"));
}

Result Robot::putRecipe(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  std::string id = jstr(j, "id");
  RecipeCfg r;
  std::optional<RecipeCfg> before;
  if (const RecipeCfg* was = id.empty() ? nullptr : cfg_.recipe(id)) before = *was;
  if (!before) {
    if (id.empty()) {
      do id = "r" + std::to_string(++idSeq_);
      while (next.recipe(id));
    }
  }
  r.id = id;
  r.name = utf8Prefix(jstr(j, "name"), 80);
  r.note = utf8Prefix(jstr(j, "note"), 500);
  r.ecTarget = j.contains("ecTarget") && j["ecTarget"].is_number() ? j["ecTarget"].get<double>() : kNaN;
  if (j.contains("ph") && j["ph"].is_array() && j["ph"].size() == 2 && j["ph"][0].is_number() && j["ph"][1].is_number()) {
    r.phLow = j["ph"][0].get<double>();
    r.phHigh = j["ph"][1].get<double>();
  } else if (j.contains("ph") && !j["ph"].is_null()) {
    return Result::fail(422, say("cfg.recipe.ph_range", {{"name", r.name}, {"min", 4.5}, {"max", 7.5}}));
  }
  if (j.contains("steps") && j["steps"].is_array())
    for (const auto& s : j["steps"]) {
      if (!s.is_object()) return Result::fail(422, say("api.bad_input"));
      r.steps.push_back(RecipeStep{jstr(s, "canister"), jnum(s, "mlPerL"), jbool(s, "fixed", false)});
    }
  bool replaced = false;
  for (auto& x : next.recipes)
    if (x.id == id) {
      x = r;
      replaced = true;
    }
  if (!replaced) next.recipes.push_back(r);
  Result res = saveConfig(next, changed("cfg.what.recipes"));
  if (res.status != 200) return res;
  // What the recipe learned stays only where its parts and ratio allow (RAT-085)
  if (before) {
    const EffectEdit e = editEcEffect(*before, r, learnedEcEffect(rt_, id));
    if (isNum(e.learned)) rt_.ecEffects[id] = e.learned;
    else rt_.ecEffects.erase(id);
    if (e.changed) cycle_.recipeChanged(id);
  }
  saveState(true);
  res.body["id"] = id;
  return res;
}

Result Robot::deleteRecipe(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!cfg_.recipe(id)) return Result::fail(404, say("api.not_found"));
  if (cfg_.active.recipe == id) return Result::fail(409, say("recipe.in_force"));
  Config next = cfg_;
  next.recipes.erase(std::remove_if(next.recipes.begin(), next.recipes.end(), [&](const RecipeCfg& r) { return r.id == id; }),
                     next.recipes.end());
  Result r = saveConfig(next, changed("cfg.what.recipes"));
  if (r.status == 200) {
    rt_.ecEffects.erase(id);
    saveState(true);
  }
  return r;
}

Result Robot::setActive(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  const Epoch now = clock_.epoch();
  const std::string recipe = jstr(j, "recipe");
  next.active = ActiveCfg{};
  if (!recipe.empty()) {
    const RecipeCfg* r = cfg_.recipe(recipe);
    if (!r) return Result::fail(422, say("cfg.active.recipe"));
    next.active.recipe = recipe;
    next.active.from = now;
    // How long it is in force: until a time, for a number of days, or without an end (SD-105)
    const double days = jnum(j, "days");
    const double until = jnum(j, "until");
    if (isNum(days)) {
      if (days < 1 || days > 366) return Result::fail(422, say("active.days", {{"min", 1}, {"max", 366}}));
      next.active.until = now + static_cast<Epoch>(std::llround(days * 86400));
    } else if (isNum(until) && until > 0) {
      if (until <= static_cast<double>(now) || until > static_cast<double>(now) + 366.0 * 86400) return Result::fail(422, say("active.until"));
      next.active.until = static_cast<Epoch>(until);
    }
  }
  const std::string old = cfg_.active.recipe;
  Result r = saveConfig(next, std::nullopt);
  if (r.status != 200) return r;
  if (!recipe.empty())
    log_.add(now, "control", "info", say("ev.active.set", {{"name", cfg_.recipe(recipe)->name}}),
             next.active.until ? say("ev.active.days", {{"days", std::round(static_cast<double>(next.active.until - now) / 86400.0 * 10) / 10}})
                               : say("ev.active.open"),
             {{"recipe", recipe}, {"until", next.active.until}});
  else
    log_.add(now, "control", "info", say("ev.active.cleared"));
  if (old != recipe) cycle_.recipeChanged(old);
  restartCycle();
  return r;
}

// ---------------------------------------------------------------- pumps

Result Robot::startPumpJob(const std::string& type, int pump, double seconds) {
  if (rt_.stopped) return Result::fail(409, say("act.stopped"));
  if (pump < 1 || pump > hw_.pumpCount()) return Result::fail(404, say("act.no_pump", {{"pump", pump}}));
  if (pumpJobActive()) return Result::fail(409, say("job.busy"));
  if (doser_.busy() || cycle_.dosing()) return Result::fail(409, say("job.cycle_dosing"));
  const Limits lim = cfg_.limits.bounded();
  if (!isNum(seconds) || seconds < 1 || seconds > lim.maxRunS) return Result::fail(422, say("job.seconds", {{"max", lim.maxRunS}}));
  const CanisterCfg* b = cfg_.canisterOnPump(pump);
  PumpJob job;
  job.id = (type == "calibration" ? "cal-" : "prime-") + std::to_string(++idSeq_);
  job.type = type;
  job.pump = pump;
  job.ms = static_cast<Ms>(std::llround(seconds * 1000));
  job.at = clock_.epoch();
  DoseOrder o;
  o.id = job.id;
  o.purpose = type;
  o.step.pump = pump;
  o.step.canister = b ? b->id : "";
  o.step.name = b ? b->name : say("pump.n", {{"pump", pump}}).text;
  o.step.runs = {job.ms};
  Ctx c = ctx();
  Msg err;
  if (!doser_.start(c, act_, o, err)) return Result::fail(409, err);
  job_ = job;
  return Result::ok({{"id", job.id}});
}

Result Robot::prime(int pump, double seconds) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return startPumpJob("prime", pump, seconds);
}

Result Robot::startCalibration(int pump, double seconds) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return startPumpJob("calibration", pump, seconds);
}

void Robot::pumpJobDone(const DoseProgress& p) {
  job_->actualMs = p.msDone;
  if (p.state != DoseProgress::State::Done) {
    job_->state = "failed";
    job_->message = p.error;
    return;
  }
  if (job_->type == "calibration") {
    job_->state = "waiting_result";
    job_->message = say("job.measure_cup", {{"s", job_->actualMs / 1000.0}});
  } else {
    job_->state = "done";
    job_->message = say("job.primed");
  }
}

Result Robot::calibrationResult(double ml) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!job_ || job_->type != "calibration" || job_->state != "waiting_result") return Result::fail(409, say("job.no_result_wanted"));
  // A result that is missing, zero, negative or above 1000 ml is refused; the previous value stays (RAT-003)
  if (!isNum(ml) || ml <= 0 || ml > 1000) return Result::fail(422, say("job.ml", {{"max", 1000}}));
  if (job_->actualMs <= 0) return Result::fail(409, say("job.no_run"));
  // From the actual run time the driver reported, not the requested one (RAT-070)
  const double flow = ml / static_cast<double>(job_->actualMs) * 60000.0;
  Config next = cfg_;
  next.pumps[job_->pump] = PumpCal{std::round(flow * 100) / 100, clock_.epoch()};
  Result r = saveConfig(next, std::nullopt);
  if (r.status != 200) return r;
  job_->ml = ml;
  job_->state = "done";
  job_->message = say("job.calibrated", {{"pump", job_->pump}, {"flow", flow}});
  log_.add(clock_.epoch(), "calibration", "info", say("ev.pump.calibrated", {{"pump", job_->pump}}),
           say("ev.pump.calibrated.text", {{"flow", flow}, {"ml", ml}, {"s", job_->actualMs / 1000.0}}));
  return Result::ok({{"flowMlPerMin", flow}});
}

Result Robot::cancelPumpJob() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!job_) return Result::fail(404, say("api.not_found"));
  if (job_->state == "running") {
    Ctx c = ctx();
    doser_.abort(c, act_, say("abort.user"));
    if (auto p = doser_.takeFinished()) pumpJobDone(*p);
  }
  job_.reset();
  return Result::ok();
}

// ---------------------------------------------------------------- probes

Result Robot::probe(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const std::string action = jstr(j, "action");
  const Ms now = clock_.nowMs();
  if (action == "start") {
    const std::string kind = jstr(j, "kind");
    if (kind != "ph" && kind != "ec") return Result::fail(422, say("probe.kind"));
    probe_ = ProbeSession{kind, json::array(), kNaN, now};
    log_.add(clock_.epoch(), "calibration", "info", say(kind == "ph" ? "ev.probe.start_ph" : "ev.probe.start_ec"));
    return Result::ok();
  }
  if (!probe_) return Result::fail(409, say("probe.none"));
  if (action == "cancel") {
    probe_.reset();
    restartCycle();
    return Result::ok();
  }
  if (action == "point") {
    const double ref = jnum(j, "reference");
    const Channel ch = probe_->kind == "ph" ? Channel::Ph : Channel::Ec;
    auto s = hw_.sample(ch);
    // A point takes a fresh raw reading, never a stand-in (RAT-006)
    if (!hw_.online() || !s || !isNum(s->raw) || now - s->ts > 10 * kSecond) return Result::fail(409, say("probe.no_raw"));
    probe_->lastStep = now;
    if (probe_->kind == "ph") {
      if (!isNum(ref) || ref < 1 || ref > 13) return Result::fail(422, say("probe.ph_ref"));
      if (probe_->points.size() >= 2) probe_->points = json::array();
      for (const auto& p : probe_->points)
        if (std::fabs(p[1].get<double>() - ref) < 1.0) return Result::fail(422, say("probe.ph_same"));
      probe_->points.push_back({s->raw, ref});
      return Result::ok({{"points", probe_->points}});
    }
    // EC: one reference solution, its value at 25 °C as printed on the canister; the water
    // temperature is needed to compensate (RAT-089)
    const Reading& t = truth_.get(Channel::Temp);
    if (!isNum(ref) || ref < 0.1 || ref > 20) return Result::fail(422, say("probe.ec_ref"));
    if (!t.usable()) return Result::fail(409, say("probe.no_temp", {{"reason", t.reason}}));
    if (s->raw <= 0) return Result::fail(422, say("probe.ec_zero"));
    probe_->points = json::array({json::array({s->raw, ref})});
    probe_->tempC = *t.value;
    return Result::ok({{"points", probe_->points}});
  }
  if (action == "save") {
    json data;
    const Epoch at = clock_.epoch();
    Config next = cfg_;
    if (probe_->kind == "ph") {
      if (probe_->points.size() != 2) return Result::fail(422, say("probe.ph_two"));
      data = {{"points", probe_->points}, {"at", at}};
      if (auto e = SensorTruth::checkPh(data)) return Result::fail(422, *e);
      next.phCal = data;
    } else {
      if (probe_->points.size() != 1) return Result::fail(422, say("probe.ec_one"));
      const double raw = probe_->points[0][0].get<double>(), ref = probe_->points[0][1].get<double>();
      const double factor = ref * (1.0 + kEcTempCoeff * (probe_->tempC - 25.0)) / raw;
      data = {{"factor", factor}, {"reference", ref}, {"tempC", probe_->tempC}, {"at", at}};
      if (auto e = SensorTruth::checkEc(data)) return Result::fail(422, *e);
      next.ecCal = data;
    }
    Result r = saveConfig(next, std::nullopt);
    if (r.status != 200) return r;
    log_.add(at, "calibration", "info", say(probe_->kind == "ph" ? "ev.probe.saved_ph" : "ev.probe.saved_ec"));
    probe_.reset();
    restartCycle();
    return r;
  }
  return Result::fail(422, say("probe.action"));
}

// ---------------------------------------------------------------- operation

Result Robot::ackLatch(const std::string& id, const json& body) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  auto it = rt_.latches.find(id);
  if (it == rt_.latches.end()) return Result::fail(404, say("latch.none"));
  if (id == "tank.volume") {
    // Only a person who confirms the entered volume releases it; a new volume releases it too (putTank)
    if (!jbool(body, "confirm", false)) return Result::fail(422, say("latch.volume_confirm"));
    const json l2 = it->second;
    const double effect = jnum(l2, "effect");
    if (jstr(l2, "kind") == "ec") {
      if (const RecipeCfg* r = cfg_.recipe(jstr(l2, "recipe")); r && isNum(effect))
        rt_.ecEffects[r->id] = clampEcEffect(effect, recipeEffect(*r, kNaN).start);
    } else if (isNum(effect)) {
      rt_.phEffect = clampEffect(effect, kPhStartEffect);
    }
    rt_.newTank = false;
  }
  const json args = it->second.is_object() ? it->second : json::object();
  rt_.latches.erase(it);
  log_.add(clock_.epoch(), "unblock", "notice", say("ev.latch.released"), say("latch." + id, args));
  restartCycle();
  saveState(true);
  return Result::ok();
}

Result Robot::stop(const Msg& who) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  // A Stop switches everything off actively, books what has run and holds until Resume (RAT-036)
  act_.stopAll();
  const bool was = rt_.stopped;
  rt_.stopped = true;
  Ctx c = ctx();
  if (job_ && job_->state == "running") {
    doser_.abort(c, act_, say("abort.stop"));
    if (auto p = doser_.takeFinished()) pumpJobDone(*p);
  }
  cycle_.stop(c, act_, doser_);
  if (auto p = doser_.takeFinished()) cycle_.doseFinished(c, *p);
  act_.stopAll();
  saveState(true);
  if (!was) log_.add(clock_.epoch(), "system", "warn", say("ev.stop"), who);
  saveEvents();
  return Result::ok();
}

Result Robot::resume() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!rt_.stopped) return Result::ok();
  rt_.stopped = false;
  log_.add(clock_.epoch(), "system", "info", say("ev.resume"));
  restartCycle();
  saveState(true);
  return Result::ok();
}

}  // namespace doseph
