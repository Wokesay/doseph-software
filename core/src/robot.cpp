// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/robot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "doseph/embedded.hpp"
#include "doseph/messages.hpp"
#include "doseph/plants.hpp"
#include "doseph/sha256.hpp"

namespace doseph {

namespace {

constexpr const char* kConfigFile = "config.json";
constexpr const char* kStateFile = "state.json";
constexpr const char* kAuthFile = "auth.json";
constexpr const char* kDeviceFile = "device.json";  // the device's identity, never in a backup (SD-129)
constexpr const char* kPushFile = "push.json";  // the push channels with their secrets, apart from the configuration (SD-124)
constexpr const char* kEventsFile = "events.json";
constexpr const char* kEventsLog = "events.log";
constexpr const char* kHistoryFile = "history.bin";
constexpr const char* kHistoryLog = "history.log";
constexpr Epoch kSampleS = 10;           // history: one sample per 10 s (finest tier)
constexpr Epoch kSnapshotS = 6 * 3600;   // history and events written whole, their journals emptied
constexpr Epoch kStateSaveS = 60;        // the clock in the state, at most this often
constexpr Ms kProbeIdle = 30 * kMinute;  // a probe calibration explains jumps this long after its last step (RAT-042)
constexpr Ms kFillMaxMs = 5 * kMinute;  // a line fill runs at most this long in all (SD-111)
constexpr Ms kReturnMaxMs = 60 * kSecond;  // a return pump running this long without a pause is dry or blocked (SD-109)
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
  c.pumpInTank = hw_.profile().pumpIn != "device";
  c.pumpA = hw_.feedPumpA();
  c.probeCalibrating = probe_.has_value();
  c.maintenanceUntil = maintenanceUntil();
  c.channelJob = channelJobActive();
  // The hardware release (SD-110): a leak and high level hold until a person releases their latch
  const Release r = hw_.release();
  c.pumpCut = r.noFlow;
  c.hold = rt_.latches.count("hw.leak") || r.leak             ? "leak"
           : rt_.latches.count("hw.high_level") || r.highLevel ? "high_level"
           : rt_.latches.count("hw.return")                    ? "return"
           : r.stopButton                                      ? "stop_button"
           : !r.on                                             ? "release"
                                                               : "";
  return c;
}

// Flow by the device's method (SD-109): with an outlet level sensor, it reports water while the
// feed pump runs; with a flow meter, at least 0.5 L/min. The check proves it for 10 s.
bool Robot::flowOk() const {
  if (hw_.profile().flow == "outlet_level") return hw_.waterPath().outletWet == true && hw_.feedPump() == true;
  const Reading& f = truth_.get(Signal::Flow);
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
  if (auth_.hasPassword() || credentialsLost() || authKept_) return "";
  return auth_.armSetupCode(clock_.nowMs(), validFor);
}

bool Robot::saveAuth() {
  if (authKept_) return false;  // never over a password file it could not read
  return store_.write(kAuthFile, auth_.toJson().dump());
}

json Robot::tokens() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const Ms nowMs = clock_.nowMs();
  json out = json::array();
  for (const auto& t : auth_.tokens())
    out.push_back({{"id", t.id},
                   {"name", t.name},
                   {"role", roleName(t.role)},
                   {"created", t.created},
                   {"used", t.used ? json(clock_.epoch() - (nowMs - *t.used) / 1000) : json(nullptr)}});
  return {{"tokens", out}, {"max", Auth::kMaxTokens}};
}

Result Robot::createToken(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const auto role = tokenRole(jstr(j, "role"));
  if (!role) return Result::fail(422, say("auth.token.role"));
  std::string name = jstr(j, "name");
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
  const Auth before = auth_;
  Msg err;
  const auto token = auth_.createToken(name, *role, clock_.epoch(), err);
  if (!token) return Result::fail(422, err);
  if (!saveAuth()) {  // a token the robot would forget at its next start is not handed out
    auth_ = before;
    return Result::fail(500, say("store.failed"));
  }
  const AccessToken& t = auth_.tokens().back();
  logEvent("auth", "notice", say("ev.auth.token_created"), say("ev.auth.token.text", {{"name", t.name}, {"role", say(std::string("auth.role.") + roleName(t.role))}}));
  return Result::ok({{"id", t.id}, {"name", t.name}, {"role", roleName(t.role)}, {"created", t.created}, {"token", *token}});
}

Result Robot::revokeToken(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const Auth before = auth_;
  const auto& all = auth_.tokens();
  auto it = std::find_if(all.begin(), all.end(), [&](const AccessToken& t) { return t.id == id; });
  if (it == all.end()) return Result::fail(404, say("auth.token.unknown"));
  const std::string name = it->name;
  const Role role = it->role;
  auth_.revokeToken(id);
  if (!saveAuth()) {  // revoked only once it is off the disk as well
    auth_ = before;
    return Result::fail(500, say("store.failed"));
  }
  logEvent("auth", "notice", say("ev.auth.token_revoked"), say("ev.auth.token.text", {{"name", name}, {"role", say(std::string("auth.role.") + roleName(role))}}));
  return Result::ok();
}

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
      if (validateConfig(c, hw_.channelCount()).empty()) cfg_ = c;
      else stateUnknown = true;
    } catch (...) {
      stateUnknown = true;
    }
    // A configuration that cannot be read is kept as a copy and the robot stays stopped (R6)
    if (stateUnknown) store_.write("config.broken.json", *s);
  }
  // Push channels that cannot be read are none: nothing is sent to an address no one checked (SD-124)
  if (auto s = store_.read(kPushFile)) {
    try {
      push_ = pushFromJson(json::parse(*s));
    } catch (...) {
      push_ = PushChannels{};
    }
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
    // The robot writes auth.json only with a password in it: one without is damaged, not new
    authKept_ = !auth_.hasPassword();
  }
  if (auto s = store_.read(kDeviceFile)) {
    const json j = json::parse(*s, nullptr, false);
    if (j.is_object()) deviceId_ = jstr(j, "id");
  }
  const bool idOk = deviceId_.size() == 16 && std::all_of(deviceId_.begin(), deviceId_.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)); });
  if (!idOk) {
    std::uint8_t b[8];
    rng_(b, sizeof b);
    deviceId_ = toHex(b, sizeof b);
    store_.write(kDeviceFile, json{{"id", deviceId_}}.dump());
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
  if (rt_.latches.count("hw.return")) act_.allowReturnPump(false);  // the device forgot it (SD-109)
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
      log_.add(bootEpoch_, "dose", "notice", say("ev.boot.cut_cup", {{"channel", r.channel}}), say("ev.boot.cut_cup.text"));
  }
  pairAfterRestart(rt_, journal_.runs());
  if (journal_.anyIntoTank()) {
    // Doses not mixed in and measured yet: the first cycle mixes first and learns nothing (SD-106)
    Ctx c = ctx();
    const CycleSettings s = cycleSettings(c);
    const double m = s.mixMin;
    cycle_.mixFirst(static_cast<Ms>(m * 60) * kSecond);
    log_.add(bootEpoch_, "control", "notice", say("ev.boot.mix_first"), say("ev.boot.mix_first.text", {{"min", m}}));
  } else {
    journal_.clear();
  }
  log_.add(bootEpoch_, "system", rt_.stopped ? "warn" : "info", say("ev.boot"),
           rt_.stopped ? say("ev.boot.stopped") : Msg{});
  if (authKept_) log_.add(bootEpoch_, "auth", "alarm", say("ev.auth.kept"), say("auth.kept"));
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
    for (Signal ch : {Signal::Ph, Signal::Ec, Signal::Temp}) truth_.expectChange(ch, now + 10 * kSecond);
  // Maintenance explains them too, 10 s ahead and never past its end (RAT-042); once over, it is logged
  if (maintenanceUntilMs_ > 0 && now >= maintenanceUntilMs_) {
    maintenanceUntilMs_ = 0;
    log_.add(epoch, "system", "info", say("ev.maintenance_over"));
    restartCycle();
  }
  if (maintenanceUntilMs_ > 0)
    for (Signal ch : {Signal::Ph, Signal::Ec, Signal::Temp})
      truth_.expectChange(ch, std::min(now + 10 * kSecond, maintenanceUntilMs_));
  watchRelease(epoch);
  checkHeads(epoch);
  Ctx c = ctx();
  // The release is back: the check starts anew, nothing resumes on its own (R6)
  if (c.hold.empty() && !lastHold_.empty()) restartCycle();
  lastHold_ = c.hold;
  // A channel job ended: the check starts anew (SD-109)
  if (!c.channelJob && lastChannelJob_) restartCycle();
  lastChannelJob_ = c.channelJob;
  act_.enforce(c);
  // Drops without a running pump went into the tank: the check mixes first and learns nothing,
  // as after a cut dose (SD-111)
  if (act_.takeUnplannedDrops()) mixFirstNow();
  doser_.tick(c, act_);
  if (auto p = doser_.takeFinished()) {
    if (job_ && p->orderId == job_->id) channelJobDone(*p);
    else cycle_.doseFinished(c, *p);
  }
  c.channelJob = channelJobActive();
  cycle_.tick(c, act_, doser_);
  if (auto st = cycle_.takeSettled()) learnSettling(*st, epoch);

  if (epoch - lastSample_ >= kSampleS) {
    lastSample_ = epoch;
    sampleHistory(epoch);
  }
  if (epoch - lastWatch_ >= 5 || lastWatch_ == 0) {
    lastWatch_ = epoch;
    WatchInput in{cfg_, rt_, {}};
    for (Signal ch : kSignals) in.readings.push_back(truth_.get(ch));
    const Phase ph = cycle_.phase();
    in.needsFlow = ph == Phase::ProveFlow || ph == Phase::Flush || ph == Phase::Measure || ph == Phase::Remeasure ||
                   ph == Phase::Dose || ph == Phase::Mix;
    in.lastPh = cycle_.last().ph;
    in.lastEc = cycle_.last().ec;
    in.lastAt = cycle_.last().at;
    in.epoch = epoch;
    in.bootEpoch = bootEpoch_;
    in.stopped = rt_.stopped;
    in.maintenance = maintenanceUntilMs_ > 0;
    in.channelCount = hw_.channelCount();
    in.hold = c.hold;
    watch_ = evaluate(in);
    // Alarms by level and the quiet hours; a target counts only while the check holds or waits (SD-123)
    const bool idle = ph == Phase::Off || ph == Phase::Stopped || ph == Phase::Hold || ph == Phase::Wait;
    // Plants move on at the local midnight after a phase's days, and what they ask of the user joins
    // as notes (SD-128)
    movePlants(epoch);
    // Maintenance items on their date join as notes; the watchdog's own verdict stays as it is (SD-125)
    noteService(cfg_, rt_, epoch);
    alarmWatch_ = watch_;
    for (const auto& i : planService(cfg_, rt_, epoch, cfg_.system.timezone).items)
      if (i.dueNow) alarmWatch_.items.push_back({"maint." + i.id, i.label, "problem", say("service.due")});
    for (const auto& n : plantNotes(cfg_, epoch, cfg_.system.timezone)) alarmWatch_.items.push_back({n.id, n.label, "problem", n.text});
    alarms_.update(alarmWatch_, {cfg_.notify, cfg_.system.timezone, epoch, idle}, rt_);
    // Each notification to every channel set up, in the device's language (SD-124)
    for (const auto& n : alarms_.takeOutbox()) queueDeliveries(deliveriesFor(n, push_, cfg_.system.language, cfg_.system.name, deliveryId_));
  }
  saveState();
  saveEvents();
  if (epoch - lastSnapshot_ >= kSnapshotS) snapshot(epoch);
}

// The hardware release (SD-110): a leak and high level switch everything off and latch until a
// person releases them once the cause is fixed; the stop button is the Stop of the app (RAT-036)
void Robot::watchRelease(Epoch epoch) {
  const Release r = hw_.release();
  auto latch = [&](const char* id, const char* title) {
    if (rt_.latches.count(id)) return;
    act_.stopAll();
    rt_.latches[id] = {{"at", epoch}};
    log_.add(epoch, "alarm", "alarm", say(title), say(std::string(title) + ".text"));
    saveState(true);
  };
  if (r.leak) latch("hw.leak", "ev.hw.leak");
  if (r.highLevel) latch("hw.high_level", "ev.hw.high_level");
  // A return pump that runs on and on is dry or blocked: it is held off and latched (SD-109). The
  // robot sees whether it runs, not the chamber's level; working, it empties the chamber in seconds.
  const Ms now = clock_.nowMs();
  if (hw_.waterPath().returnPump == true) {
    if (returnSince_ < 0) returnSince_ = now;
  } else {
    returnSince_ = -1;
  }
  if (returnSince_ >= 0 && now - returnSince_ >= kReturnMaxMs && !rt_.latches.count("hw.return")) {
    act_.allowReturnPump(false);
    latch("hw.return", "ev.hw.return");
  }
  if (r.stopButton && !stopButtonSeen_) stop(say("stop.button"));
  stopButtonSeen_ = r.stopButton;
}

// A calibration belongs to the pump head it was measured with (SD-108): another head, none fitted
// or none recorded makes it void, and the channel doses nothing until it is calibrated again
void Robot::checkHeads(Epoch epoch) {
  const auto voided = [&](int n, const ChannelCal& cal) {
    return n >= 1 && n <= hw_.channelCount() && (cal.head.empty() || cal.head != hw_.profile().channels[static_cast<size_t>(n - 1)].head);
  };
  std::vector<int> gone;
  for (const auto& [n, cal] : cfg_.channels)
    if (voided(n, cal)) gone.push_back(n);
  if (gone.empty()) return;
  Config next = cfg_;
  for (int n : gone) next.channels.erase(n);
  // Kept while the store fails: the gateway refuses the void calibration meanwhile
  if (saveConfig(next, std::nullopt).status != 200) return;
  for (int n : gone) {
    const std::string& head = hw_.profile().channels[static_cast<size_t>(n - 1)].head;
    if (head.empty())
      log_.add(epoch, "calibration", "warn", say("ev.channel.head_removed", {{"channel", n}}), say("ev.channel.head_removed.text"));
    else
      log_.add(epoch, "calibration", "warn", say("ev.channel.head_changed", {{"channel", n}, {"head", head}}),
               say("ev.channel.head_changed.text"));
  }
}

// A clean dose's settling time from the history: from the end of its dosing until the readings stay
// within the stability band of their final value; the last three are kept (SD-112)
void Robot::learnSettling(const MixCycle::Settled& st, Epoch epoch) {
  const double band = st.kind == "ec" ? kStableEc : kStablePh;
  const SeriesPoints pts = history_.query(st.kind, st.end, epoch, static_cast<size_t>((epoch - st.end) / kSampleS + 10));
  const double s = settleSeconds(pts.t, pts.avg, st.end, st.final, band, pts.stepS);
  if (!isNum(s)) return;
  rt_.settleMin.push_back(s / 60.0);
  while (rt_.settleMin.size() > 3) rt_.settleMin.erase(rt_.settleMin.begin());
}

void Robot::sampleHistory(Epoch epoch) {
  // pH, EC and temperature only with flow: without it the measuring chamber holds standing water, not the tank's;
  // a gap stays a gap (RAT-016)
  const bool flow = flowOk();
  std::vector<std::pair<std::string, double>> samples;
  for (Signal ch : kSignals) {
    const Reading& r = truth_.get(ch);
    if (!r.usable() || (ch != Signal::Flow && !flow)) continue;
    samples.emplace_back(signalName(ch), *r.value);
    history_.add(signalName(ch), epoch, *r.value);
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
          {"deviceId", deviceId_},
          {"hasPassword", auth_.hasPassword()},
          {"setupCode", auth_.setupCodeRequired()},
          // Why neither sign-in nor a first password works, for the sign-in page
          {"passwordProblem", authKept_ ? json(say("auth.kept")) : credentialsLost() ? json(say("auth.lost")) : json(nullptr)},
          {"name", cfg_.system.name},
          {"language", cfg_.system.language},
          {"channelCount", hw_.channelCount()},
          {"profile", profileJson(hw_.profile())}};
}

json Robot::diagnostics() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  std::uint8_t b[4];
  rng_(b, sizeof b);
  const json st = state();
  json evs = json::array();
  for (const auto& e : log_.query(clock_.epoch() - 72 * 3600, clock_.epoch(), "", 300)) evs.push_back(e);
  return {{"reportId", "DP-" + toHex(b, sizeof b)},
          {"createdAt", clock_.epoch()},
          {"info", info()},
          {"config", cfg_},
          {"readings", st["readings"]},
          {"cycle", st["cycle"]},
          {"watchdog", st["watchdog"]},
          {"latches", st["latches"]},
          {"jumpLocks", st["jumpLocks"]},
          {"channels", st["channels"]},
          {"maintenanceUntil", st["maintenanceUntil"]},
          {"events", evs},
          // what the package leaves out, as keys; the app names them in the page's language
          {"redacted", {"password_hash", "sessions", "ip_addresses", "wifi_credentials"}}};
}

json Robot::configJson() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return cfg_;
}

json Robot::state() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Ctx c = ctx();
  json readings = json::object();
  for (Signal ch : kSignals) {
    const Reading& r = truth_.get(ch);
    readings[signalName(ch)] = {{"value", numOrNull(r.value)},
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
  json channels = json::array();
  for (int n = 1; n <= hw_.channelCount(); ++n) {
    const CanisterCfg* b = cfg_.canisterOnChannel(n);
    auto it = cfg_.channels.find(n);
    const std::string& head = hw_.profile().channels[static_cast<size_t>(n - 1)].head;
    channels.push_back({{"channel", n},
                     {"canister", b ? json(b->id) : json(nullptr)},
                     {"head", head.empty() ? json(nullptr) : json(head)},  // the pump head fitted (SD-108)
                     // Its run time into the tank in the last 24 h and its cap (SD-113)
                     {"day", {{"usedS", rt_.dayRunMs(n, c.epoch - 24 * 3600) / 1000.0}, {"capS", act_.dailyCapMin(n) * 60}}},
                     {"flowMlPerMin", it == cfg_.channels.end() ? json(nullptr) : numOrNull(it->second.flowMlPerMin)},
                     {"at", it == cfg_.channels.end() ? json(nullptr) : json(it->second.at)},
                     // What its drop sensor saw (SD-111): null without one
                     {"drops", [&] {
                        const auto d = hw_.drops(n);
                        if (!d) return json(nullptr);
                        return json{{"count", d->count}, {"lastAgoS", d->lastMs < 0 ? json(nullptr) : json((c.now - d->lastMs) / 1000)}};
                      }()}});
  }
  json job = nullptr;
  if (job_)
    job = {{"id", job_->id}, {"type", job_->type}, {"channel", job_->channel}, {"state", job_->state},
           {"seconds", job_->ms / 1000.0}, {"actualS", job_->actualMs / 1000.0}, {"ml", numOrNull(job_->ml)},
           {"message", job_->message}, {"at", job_->at}};
  json probe = nullptr;
  if (probe_) {
    auto raw = [&](Signal ch) {
      auto s = hw_.sample(ch);
      return s ? numOrNull(s->raw) : json(nullptr);
    };
    probe = {{"kind", probe_->kind}, {"points", probe_->points}, {"rawPh", raw(Signal::Ph)}, {"rawEc", raw(Signal::Ec)},
             {"temp", numOrNull(truth_.get(Signal::Temp).value)}};
  }
  json ecEffects = json::object();
  for (const auto& [k, v] : rt_.ecEffects) ecEffects[k] = numOrNull(v);
  json stock = json::object();
  for (const auto& b : cfg_.canisters) {
    auto it = rt_.stockMl.find(b.id);
    stock[b.id] = {{"ml", it == rt_.stockMl.end() ? json(nullptr) : numOrNull(it->second)}, {"status", stockStatus(b, rt_)}};
  }
  return {{"now", c.epoch},
          {"uptimeS", (c.now - bootMs_) / 1000},
          {"clock", clock_.source()},
          {"stopped", rt_.stopped},
          {"online", hw_.online()},
          {"feedPump", [&] {
             auto st = act_.feedPump();
             return st ? json(*st) : json(nullptr);
           }()},
          {"flowOk", c.flowOk},
          // The water plan: the leading plant's phase or the recipe chosen by hand (SD-128)
          {"plan", waterPlanJson(cfg_, c.epoch, cfg_.system.timezone)},
          // The feed pump's current now and as learned while it moves water (SD-127)
          {"pump", {{"amps", c.pumpA ? numOrNull(*c.pumpA) : json(nullptr)},
                    {"normalA", rt_.pumpLearnedS >= kPumpLearnS ? numOrNull(rt_.pumpNormalA) : json(nullptr)}}},
          // What the hardware reports of its release and the water path (SD-109, SD-110)
          {"hardware", [&] {
             const Release r = hw_.release();
             const WaterPath w = hw_.waterPath();
             auto opt = [](const std::optional<bool>& v) { return v ? json(*v) : json(nullptr); };
             return json{{"release", {{"on", r.on}, {"leak", r.leak}, {"highLevel", r.highLevel}, {"stopButton", r.stopButton}, {"noFlow", r.noFlow}}},
                         {"outletWet", opt(w.outletWet)},
                         {"returnPump", opt(w.returnPump)}};
           }()},
          {"readings", readings},
          {"cycle", cycle_.status(c)},
          {"watchdog", watch},
          {"alarms", alarms_.list(alarmWatch_, rt_)},  // raised, with level and pause (SD-123)
          {"latches", rt_.latches},
          {"jumpLocks", rt_.jumpLocks},
          {"newTank", rt_.newTank},
          {"topUp", rt_.topUp},  // "top up water" is raised (SD-115)
          {"effects", {{"ph", numOrNull(rt_.phEffect)}, {"ecByRecipe", ecEffects}}},
          {"dosing", dosing},
          {"channels", channels},
          {"stock", stock},
          {"catchUp", [&] {
             if (!rt_.catchUp.is_object()) return json(nullptr);
             json parts = json::array();
             const json missing = rt_.catchUp.value("parts", json::object());
             for (const auto& [id, ml] : missing.items()) {
               const CanisterCfg* b = cfg_.canister(id);
               parts.push_back({{"canister", id}, {"name", b ? b->name : id}, {"ml", ml}});
             }
             return json{{"recipe", rt_.catchUp.value("recipe", "")}, {"parts", parts}};
           }()},
          {"channelJob", job},
          {"probe", probe},
          {"maintenanceUntil", c.maintenanceUntil > 0 ? json(c.maintenanceUntil) : json(nullptr)},
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
    for (Signal ch : kSignals)
      if (series[i] == signalName(ch)) d = signalDef(ch).decimals + 1;  // a mean carries one digit more
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
  auto errs = validateConfig(next, hw_.channelCount());
  if (!errs.empty()) return Result::fail(422, errs.front(), {{"errors", errs}});
  next.revision = cfg_.revision + 1;
  if (!store_.write(kConfigFile, json(next).dump())) return Result::fail(500, say("store.failed"));
  cfg_ = std::move(next);
  if (what) log_.add(clock_.epoch(), "config", "info", *what);
  return Result::ok({{"revision", cfg_.revision}});
}

// Something went into the tank outside a round: the check starts anew, mixes it in first and learns
// nothing from it
void Robot::mixFirstNow() {
  rt_.ecInBand = nullptr;  // what went in may raise EC: no sign of missing water (SD-115)
  Ctx c = ctx();
  const CycleSettings s = cycleSettings(c);
  const double m = s.mixMin;
  cycle_.mixFirst(static_cast<Ms>(m * 60) * kSecond);
  restartCycle();
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

Result Robot::putNotify(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  auto minute = [&](const char* key, int& into) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer()) return false;
    into = j[key].get<int>();
    return true;
  };
  if (!minute("quietFromMin", next.notify.quietFromMin) || !minute("quietToMin", next.notify.quietToMin))
    return Result::fail(422, say("api.bad_input"));
  return saveConfig(next, changed("cfg.what.notify"));
}

// ---------------------------------------------------------------- plants (SD-128)

json Robot::plants() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return plantsJson(cfg_, clock_.epoch(), cfg_.system.timezone);
}

json Robot::calendar(int days) const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const Epoch now = clock_.epoch();
  const std::string& tz = cfg_.system.timezone;
  const Epoch until = localDaysLater(localMidnight(now, tz), std::clamp(days, 1, 366), tz);
  json entries = plantDates(cfg_, now, until, tz);
  // The maintenance dates with their items, today's included (SD-125)
  const ServicePlan sp = planService(cfg_, rt_, now, tz);
  for (const Epoch d : sp.dates) {
    if (d > until) continue;
    json items = json::array();
    for (const auto& i : sp.items)
      if (i.date == d) items.push_back(i.label);
    entries.push_back({{"date", std::max(d, localMidnight(now, tz))}, {"kind", "maintenance"}, {"items", items}, {"overdue", d < localMidnight(now, tz)}});
  }
  std::stable_sort(entries.begin(), entries.end(), [](const json& a, const json& b) { return a["date"].get<Epoch>() < b["date"].get<Epoch>(); });
  return {{"today", localMidnight(now, tz)}, {"until", until}, {"entries", entries}};
}

namespace {

// The water a plan gives, to tell whether a change of plants changes it
std::string planKey(const WaterPlan& w) {
  if (!w.recipe) return "";
  return w.recipe->id + "|" + std::to_string(w.ecTarget) + "|" + std::to_string(w.phTarget) + "|" + (w.ended ? "ended" : "");
}

}  // namespace

Result Robot::savePlants(Config next, const Msg& what) {
  const Epoch now = clock_.epoch();
  advancePlants(next, now, next.system.timezone);
  // Once no plant runs any more, the robot holds: no recipe chosen by hand long ago comes back
  const bool ran = std::any_of(cfg_.plants.begin(), cfg_.plants.end(), [](const PlantCfg& p) { return p.running(); });
  const bool runs = std::any_of(next.plants.begin(), next.plants.end(), [](const PlantCfg& p) { return p.running(); });
  if (ran && !runs) next.active = ActiveCfg{};
  if (!next.lead.empty() && (!next.plant(next.lead) || !next.plant(next.lead)->running())) next.lead.clear();
  const WaterPlan before = waterPlan(cfg_, now);
  const std::string oldRecipe = before.recipe ? before.recipe->id : "";
  const std::string beforeKey = planKey(before);
  Result r = saveConfig(next, std::nullopt);
  if (r.status != 200) return r;
  log_.add(now, "plant", "info", what);
  if (planKey(waterPlan(cfg_, now)) != beforeKey) {
    const WaterPlan after = waterPlan(cfg_, now);
    if (!oldRecipe.empty() && (!after.recipe || after.recipe->id != oldRecipe)) cycle_.recipeChanged(oldRecipe);
    restartCycle();
  }
  r.body = plantsJson(cfg_, now, cfg_.system.timezone);
  return r;
}

void Robot::movePlants(Epoch epoch) {
  // Most ticks nothing ends: no copy of the configuration then
  const bool due = std::any_of(cfg_.plants.begin(), cfg_.plants.end(), [&](const PlantCfg& p) {
    return p.running() && p.phase >= 0 && p.phase + 1 < static_cast<int>(p.phases.size()) &&
           !p.phases[static_cast<size_t>(p.phase)].confirm && epoch >= phaseEnd(p, cfg_.system.timezone);
  });
  if (!due) return;
  Config next = cfg_;
  const auto moved = advancePlants(next, epoch, cfg_.system.timezone);
  if (moved.empty()) return;
  const WaterPlan before = waterPlan(cfg_, epoch);
  const std::string oldRecipe = before.recipe ? before.recipe->id : "";
  const std::string beforeKey = planKey(before);
  if (saveConfig(next, std::nullopt).status != 200) return;  // a plant that no longer checks out stays
  for (const auto& id : moved)
    if (const PlantCfg* p = cfg_.plant(id))
      log_.add(epoch, "plant", "info", say("ev.plant.phase", {{"name", p->name}, {"phase", p->phases[static_cast<size_t>(p->phase)].name}}),
               Msg{}, {{"plant", id}, {"phase", p->phase}});
  const WaterPlan after = waterPlan(cfg_, epoch);
  if (planKey(after) != beforeKey) {
    if (!oldRecipe.empty() && (!after.recipe || after.recipe->id != oldRecipe)) cycle_.recipeChanged(oldRecipe);
    restartCycle();
  }
}

Result Robot::putPlantRecipe(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.is_object()) return Result::fail(422, say("api.bad_input"));
  Config next = cfg_;
  PlantRecipeCfg r;
  r.id = jstr(j, "id");
  if (r.id.empty()) {
    do r.id = "pr" + std::to_string(++idSeq_);
    while (next.plantRecipe(r.id));
  } else if (!next.plantRecipe(r.id)) {
    return Result::fail(404, say("api.not_found"));
  }
  r.name = utf8Prefix(jstr(j, "name"), 80);
  r.note = utf8Prefix(jstr(j, "note"), 500);
  try {
    r.phases = phasesFromJson(j);
  } catch (const std::exception&) {
    return Result::fail(422, say("api.bad_input"));
  }
  bool replaced = false;
  for (auto& x : next.plantRecipes)
    if (x.id == r.id) {
      x = r;
      replaced = true;
    }
  if (!replaced) next.plantRecipes.push_back(r);
  Result res = saveConfig(next, changed("cfg.what.plants"));
  if (res.status != 200) return res;
  res.body["id"] = r.id;
  return res;
}

Result Robot::deletePlantRecipe(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!cfg_.plantRecipe(id)) return Result::fail(404, say("api.not_found"));
  Config next = cfg_;
  next.plantRecipes.erase(std::remove_if(next.plantRecipes.begin(), next.plantRecipes.end(), [&](const PlantRecipeCfg& r) { return r.id == id; }),
                          next.plantRecipes.end());
  return saveConfig(next, changed("cfg.what.plants"));  // a plant started from it keeps its own copy
}

Result Robot::startPlant(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.is_object()) return Result::fail(422, say("api.bad_input"));
  const Epoch now = clock_.epoch();
  const std::string& tz = cfg_.system.timezone;
  Config next = cfg_;
  PlantCfg p;
  do p.id = "p" + std::to_string(++idSeq_);
  while (next.plant(p.id));
  p.name = utf8Prefix(jstr(j, "name"), 80);
  p.from = jstr(j, "plantRecipe");
  const PlantRecipeCfg* from = p.from.empty() ? nullptr : cfg_.plantRecipe(p.from);
  if (!p.from.empty() && !from) return Result::fail(422, say("api.not_found"));
  // Its phases: given with the start (adjusted in the dialog), else the plant recipe's, copied
  try {
    p.phases = j.contains("phases") ? phasesFromJson(j) : from ? from->phases : std::vector<PhaseCfg>{};
  } catch (const std::exception&) {
    return Result::fail(422, say("api.bad_input"));
  }
  // Day 1 is the day it started: today, or a day of the past year
  const double start = jnum(j, "start");
  const Epoch at = isNum(start) ? static_cast<Epoch>(start) : now;
  if (at > now || at < now - 365 * 86400) return Result::fail(422, say("plant.start"));
  p.start = p.phaseStart = localMidnight(at, tz);
  const bool anyRunning = std::any_of(cfg_.plants.begin(), cfg_.plants.end(), [](const PlantCfg& x) { return x.running(); });
  next.plants.push_back(p);
  // The first running plant leads, and the recipe chosen by hand ends with it
  if (!anyRunning) {
    next.lead = p.id;
    next.active = ActiveCfg{};
  }
  Result r = savePlants(next, say("ev.plant.started", {{"name", p.name}}));
  if (r.status == 200) r.body["id"] = p.id;
  return r;
}

namespace {

PlantCfg* plantIn(Config& c, const std::string& id) {
  for (auto& p : c.plants)
    if (p.id == id) return &p;
  return nullptr;
}

}  // namespace

Result Robot::putPlant(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  PlantCfg* p = plantIn(next, id);
  if (!p) return Result::fail(404, say("api.not_found"));
  if (!j.is_object()) return Result::fail(422, say("api.bad_input"));
  if (j.contains("name")) p->name = utf8Prefix(jstr(j, "name"), 80);
  if (j.contains("phases")) {
    try {
      p->phases = phasesFromJson(j);
    } catch (const std::exception&) {
      return Result::fail(422, say("api.bad_input"));
    }
  }
  return savePlants(next, say("ev.plant.changed", {{"name", p->name}}));
}

Result Robot::plantNext(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  PlantCfg* p = plantIn(next, id);
  if (!p) return Result::fail(404, say("api.not_found"));
  if (!p->running()) return Result::fail(409, say("plant.not_running"));
  if (p->phase + 1 >= static_cast<int>(p->phases.size())) return Result::fail(409, say("plant.last_phase"));
  // Confirmed today: the next phase's day 1 is today
  p->phase++;
  p->phaseStart = localMidnight(clock_.epoch(), cfg_.system.timezone);
  return savePlants(next, say("ev.plant.phase", {{"name", p->name}, {"phase", p->phases[static_cast<size_t>(p->phase)].name}}));
}

Result Robot::plantHarvest(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  PlantCfg* p = plantIn(next, id);
  if (!p) return Result::fail(404, say("api.not_found"));
  if (!p->running()) return Result::fail(409, say("plant.not_running"));
  // Only the user records the harvest, with the time it happened (RAT-002, RAT-077)
  const Epoch now = clock_.epoch();
  const double at = jnum(j, "at");
  const Epoch when = isNum(at) ? static_cast<Epoch>(at) : now;
  if (when > now || when < p->start) return Result::fail(422, say("plant.harvest_at"));
  p->harvestAt = when;
  return savePlants(next, say("ev.plant.harvest", {{"name", p->name}}));
}

Result Robot::plantComplete(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  PlantCfg* p = plantIn(next, id);
  if (!p) return Result::fail(404, say("api.not_found"));
  if (!p->running()) return Result::fail(409, say("plant.not_running"));
  p->completedAt = clock_.epoch();
  return savePlants(next, say("ev.plant.completed", {{"name", p->name}}));
}

Result Robot::plantLead(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  PlantCfg* p = plantIn(next, id);
  if (!p) return Result::fail(404, say("api.not_found"));
  if (!p->running()) return Result::fail(409, say("plant.not_running"));
  next.lead = id;
  return savePlants(next, say("ev.plant.lead", {{"name", p->name}}));
}

Result Robot::deletePlant(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const PlantCfg* was = cfg_.plant(id);
  if (!was) return Result::fail(404, say("api.not_found"));
  const std::string name = was->name;
  Config next = cfg_;
  next.plants.erase(std::remove_if(next.plants.begin(), next.plants.end(), [&](const PlantCfg& p) { return p.id == id; }), next.plants.end());
  return savePlants(next, say("ev.plant.deleted", {{"name", name}}));
}

json Robot::service() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return serviceJson(planService(cfg_, rt_, clock_.epoch(), cfg_.system.timezone));
}

Result Robot::serviceDone(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const auto items = serviceItems(cfg_);
  if (std::none_of(items.begin(), items.end(), [&](const ServiceItem& i) { return i.id == id; })) return Result::fail(404, say("service.none"));
  rt_.serviceDone[id] = clock_.epoch();
  saveState(true);
  return Result::ok(service());
}

Result Robot::putService(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.contains("days") || !j["days"].is_object()) return Result::fail(422, say("api.bad_input"));
  Config next = cfg_;
  // A number sets a kind's interval, null gives it back its own; the configuration checks the bounds
  for (const auto& [kind, v] : j["days"].items()) {
    if (v.is_null()) next.service.days.erase(kind);
    else if (v.is_number_integer()) next.service.days[kind] = v.get<int>();
    else return Result::fail(422, say("api.bad_input"));
  }
  Result r = saveConfig(next, changed("cfg.what.service"));
  if (r.status != 200) return r;
  return Result::ok(service());
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
  if (!num("volumeL", next.tank.volumeL) || !num("mixMinMin", next.tank.mixMinMin) ||
      !num("holdMin", next.tank.holdMin))
    return Result::fail(422, say("api.bad_input"));
  if (j.contains("system")) {
    if (j["system"].is_null()) next.tank.system.clear();
    else if (j["system"].is_string()) next.tank.system = j["system"].get<std::string>();
    else return Result::fail(422, say("api.bad_input"));
  }
  const bool volumeChanged = !(std::isnan(next.tank.volumeL) && std::isnan(cfg_.tank.volumeL)) && next.tank.volumeL != cfg_.tank.volumeL;
  Result r = saveConfig(next, changed("cfg.what.tank"));
  if (r.status != 200) return r;
  if (volumeChanged) {
    // Another volume: the next dose checks it again (SD-105); a pair is not caught up in another tank (SD-114)
    rt_.newTank = true;
    rt_.latches.erase("tank.volume");
    Ctx c = ctx();
    dropCatchUp(c, say("catchup.new_tank"));
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
  rt_.ecInBand = nullptr;  // a new fill (SD-115)
  rt_.topUp = false;
  rt_.latches.erase("tank.volume");
  rt_.jumpLocks.clear();
  {
    Ctx c = ctx();
    dropCatchUp(c, say("catchup.new_tank"));  // a new fill has no missing part (SD-114)
  }
  const Ms now = clock_.nowMs();
  for (Signal ch : {Signal::Ph, Signal::Ec, Signal::Temp}) truth_.expectChange(ch, now + 10 * kMinute);
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
  if (j.contains("channel")) {
    if (!j["channel"].is_number_integer()) return Result::fail(422, say("cfg.canister.channel", {{"name", b->name}, {"max", hw_.channelCount()}}));
    b->channel = j["channel"].get<int>();
  }
  b->pair = jstr(j, "pair", b->pair);
  b->color = jstr(j, "color", b->color);
  if (j.contains("maxMlPerDay")) {
    if (!j["maxMlPerDay"].is_null() && !j["maxMlPerDay"].is_number()) return Result::fail(422, say("api.bad_input"));
    b->maxMlPerDay = j["maxMlPerDay"].is_number() ? j["maxMlPerDay"].get<double>() : kNaN;
  }
  if (j.contains("capacityMl")) {
    if (!j["capacityMl"].is_null() && !j["capacityMl"].is_number()) return Result::fail(422, say("api.bad_input"));
    b->capacityMl = j["capacityMl"].is_number() ? j["capacityMl"].get<double>() : kNaN;
  }
  // A level entered is a measured amount, never below zero (SD-119)
  const bool stockGiven = j.contains("stockMl") && !j["stockMl"].is_null();
  if (stockGiven && (!j["stockMl"].is_number() || !isNum(j["stockMl"].get<double>()) || j["stockMl"].get<double>() < 0))
    return Result::fail(422, say("canister.stock"));
  const bool created = !cfg_.canister(id);
  Result r = saveConfig(next, changed("cfg.what.canisters"));
  if (r.status != 200) return r;
  r.body["id"] = id;
  // A new canister starts with the level entered, else unknown; never with one left behind by
  // an earlier canister of the same ID. An existing one keeps its count (RAT-015).
  if (stockGiven) rt_.stockMl[id] = j["stockMl"].get<double>();
  else if (created) rt_.stockMl.erase(id);
  saveState(true);
  return r;
}

Result Robot::endCatchUp() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!rt_.catchUp.is_object()) return Result::fail(404, say("api.not_found"));
  Ctx c = ctx();
  dropCatchUp(c, say("catchup.by_hand"));
  saveState(true);
  return Result::ok();
}

Result Robot::setStock(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const CanisterCfg* b = cfg_.canister(id);
  if (!b) return Result::fail(404, say("api.not_found"));
  double ml = kNaN;
  if (jbool(j, "full", false)) {
    if (!isNum(b->capacityMl)) return Result::fail(422, say("canister.no_capacity", {{"name", b->name}}));
    ml = b->capacityMl;
  } else if (j.contains("ml") && j["ml"].is_number()) {
    ml = j["ml"].get<double>();
  }
  if (!isNum(ml) || ml < 0) return Result::fail(422, say("canister.stock"));
  rt_.stockMl[id] = ml;
  log_.add(clock_.epoch(), "config", "info", say("ev.stock.set", {{"name", b->name}}), say("ev.stock.set.text", {{"name", b->name}, {"ml", ml}}),
           {{"canister", id}, {"stockMl", ml}});
  saveState(true);
  // A check waiting for a refill tries again now, not at its next retry
  if (cycle_.phase() == Phase::Wait) restartCycle();
  return Result::ok({{"stockMl", ml}});
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
  Result r = saveConfig(next, changed("cfg.what.canisters"));
  if (r.status == 200) {
    rt_.stockMl.erase(id);  // or a new canister of the same ID would take its count
    saveState(true);
  }
  return r;
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
  // A plant or a plant recipe that names it keeps it (SD-128)
  auto uses = [&](const std::vector<PhaseCfg>& phases) {
    return std::any_of(phases.begin(), phases.end(), [&](const PhaseCfg& ph) { return ph.recipe == id; });
  };
  for (const auto& pr : cfg_.plantRecipes)
    if (uses(pr.phases)) return Result::fail(409, say("recipe.in_plant", {{"name", pr.name}}));
  for (const auto& p : cfg_.plants)
    if (uses(p.phases)) return Result::fail(409, say("recipe.in_plant", {{"name", p.name}}));
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
  // While a plant runs, its phase sets the water (SD-128)
  if (const PlantCfg* p = leadingPlant(cfg_)) return Result::fail(409, say("plant.leads", {{"name", p->name}}));
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

Result Robot::importConfig(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.is_object()) return Result::fail(422, say("config.not_object"));
  // A newer schema in words of its own, before anything a newer version may write differently
  if (j.contains("schemaVersion") && j["schemaVersion"].is_number() && j["schemaVersion"].get<double>() > kSchemaVersion)
    return Result::fail(422, say("config.newer"));
  Config next;
  try {
    next = configFromJson(j);
  } catch (const std::exception& e) {
    return Result::fail(422, say("config.import", {{"reason", e.what()}}));
  }
  auto errs = validateConfig(next, hw_.channelCount());
  // A calibration in a file is checked as if it were made now: a broken one is refused, not taken (RAT-025)
  if (!next.phCal.is_null())
    if (auto e = SensorTruth::checkPh(next.phCal)) errs.push_back(say("cfg.calibration.ph", {{"reason", *e}}));
  if (!next.ecCal.is_null())
    if (auto e = SensorTruth::checkEc(next.ecCal)) errs.push_back(say("cfg.calibration.ec", {{"reason", *e}}));
  if (!errs.empty()) return Result::fail(422, errs.front(), {{"errors", errs}});
  if (channelJobActive() || probe_ || doser_.busy() || cycle_.dosing()) return Result::fail(409, say("config.busy"));
  // The lock against a second first setup never comes from a file
  next.system.passwordSet = cfg_.system.passwordSet || auth_.hasPassword();
  const Config before = cfg_;
  Result r = saveConfig(next, say("ev.config.imported"));
  if (r.status != 200) return r;
  // What EC control learned stays for a recipe the robot had and the file keeps, as an edit
  // would leave it (RAT-085); one the file leaves out or brings new starts afresh
  for (const auto& old : before.recipes)
    if (!cfg_.recipe(old.id)) rt_.ecEffects.erase(old.id);
  for (const auto& nr : cfg_.recipes) {
    const RecipeCfg* was = before.recipe(nr.id);
    if (!was) {
      rt_.ecEffects.erase(nr.id);
      continue;
    }
    const EffectEdit e = editEcEffect(*was, nr, learnedEcEffect(rt_, nr.id));
    if (isNum(e.learned)) rt_.ecEffects[nr.id] = e.learned;
    else rt_.ecEffects.erase(nr.id);
  }
  // A level stays only for a canister the robot had and the file keeps: a count is never
  // taken over by another canister (SD-119)
  for (auto it = rt_.stockMl.begin(); it != rt_.stockMl.end();)
    it = before.canister(it->first) && cfg_.canister(it->first) ? std::next(it) : rt_.stockMl.erase(it);
  // The pH effect belongs to the pH− canister: another one starts afresh
  const CanisterCfg* phWas = before.phDown();
  const CanisterCfg* phNow = cfg_.phDown();
  if (!phWas || !phNow || phWas->id != phNow->id || phWas->name != phNow->name) rt_.phEffect = kNaN;
  // Another volume: the next dose checks it again (SD-105)
  const bool sameVolume = (std::isnan(before.tank.volumeL) && std::isnan(cfg_.tank.volumeL)) || before.tank.volumeL == cfg_.tank.volumeL;
  if (!sameVolume) {
    rt_.newTank = true;
    rt_.latches.erase("tank.volume");
  }
  // Everything off, and the check starts again with what the file says
  act_.stopAll();
  restartCycle();
  saveState(true);
  return r;
}

// ---------------------------------------------------------------- channels

Result Robot::startChannelJob(const std::string& type, int channel, double seconds) {
  if (rt_.stopped) return Result::fail(409, say("act.stopped"));
  if (channel < 1 || channel > hw_.channelCount()) return Result::fail(404, say("act.no_channel", {{"channel", channel}}));
  if (channelJobActive()) return Result::fail(409, say("job.busy"));
  if (doser_.busy() || cycle_.dosing()) return Result::fail(409, say("job.cycle_dosing"));
  const Limits lim = cfg_.limits.bounded();
  // A line fill needs no time: it runs until it drips (SD-111)
  if (type != "fill" && (!isNum(seconds) || seconds < 1 || seconds > lim.maxRunS))
    return Result::fail(422, say("job.seconds", {{"max", lim.maxRunS}}));
  const CanisterCfg* b = cfg_.canisterOnChannel(channel);
  ChannelJob job;
  job.id = (type == "calibration" ? "cal-" : type == "fill" ? "fill-" : "prime-") + std::to_string(++idSeq_);
  job.type = type;
  job.channel = channel;
  job.head = hw_.profile().channels[static_cast<size_t>(channel - 1)].head;
  job.ms = static_cast<Ms>(std::llround(seconds * 1000));
  job.at = clock_.epoch();
  DoseOrder o;
  o.id = job.id;
  o.purpose = type;
  o.step.channel = channel;
  o.step.canister = b ? b->id : "";
  o.step.name = b ? b->name : say("channel.n", {{"channel", channel}}).text;
  o.step.runs = {job.ms};
  if (type == "fill") {
    // Runs of at most 60 s until the first drops fall, at most 5 min in all (SD-111)
    const Ms run = static_cast<Ms>(std::min(60.0, lim.maxRunS) * 1000);
    o.step.runs.assign(static_cast<size_t>((kFillMaxMs + run - 1) / run), run);
    job.ms = kFillMaxMs;
  }
  // A line fill is booked like a dose, from the pump's flow; a calibration from the cup (SD-119)
  if (type != "calibration") o.step.flowMlPerMin = cfg_.channelFlow(channel);
  Ctx c = ctx();
  Msg err;
  if (!doser_.start(c, act_, o, err)) return Result::fail(409, err);
  job_ = job;
  return Result::ok({{"id", job.id}});
}

Result Robot::prime(int channel, double seconds) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  // Where the outlet has a drop sensor, the line is filled into the tank until it drips; without
  // one, into a cup for the time given (SD-111)
  return startChannelJob(hw_.drops(channel) ? "fill" : "prime", channel, seconds);
}

Result Robot::startCalibration(int channel, double seconds) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return startChannelJob("calibration", channel, seconds);
}

void Robot::channelJobDone(const DoseProgress& p) {
  job_->actualMs = p.msDone;
  // What a line fill dripped is in the tank: the check mixes it in first and learns nothing (SD-111)
  if (job_->type == "fill" && p.msDone > 0) mixFirstNow();
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
    job_->message = job_->type == "fill" ? say("job.filled", {{"channel", job_->channel}, {"s", job_->actualMs / 1000.0}}) : say("job.primed");
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
  // With the head it measured: if the head changed meanwhile, the next tick voids it (SD-108)
  next.channels[job_->channel] = ChannelCal{std::round(flow * 100) / 100, clock_.epoch(), job_->head};
  Result r = saveConfig(next, std::nullopt);
  if (r.status != 200) return r;
  job_->ml = ml;
  job_->state = "done";
  // What went into the cup came out of the canister (SD-119)
  if (const CanisterCfg* b = cfg_.canisterOnChannel(job_->channel)) {
    Ctx c = ctx();
    takeStock(c, b->id, b->name, ml);
    saveState(true);
  }
  job_->message = say("job.calibrated", {{"channel", job_->channel}, {"flow", flow}});
  log_.add(clock_.epoch(), "calibration", "info", say("ev.channel.calibrated", {{"channel", job_->channel}}),
           say("ev.channel.calibrated.text", {{"flow", flow}, {"ml", ml}, {"s", job_->actualMs / 1000.0}}));
  return Result::ok({{"flowMlPerMin", flow}});
}

Result Robot::cancelChannelJob() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!job_) return Result::fail(404, say("api.not_found"));
  if (job_->state == "running") {
    Ctx c = ctx();
    doser_.abort(c, act_, say("abort.user"));
    if (auto p = doser_.takeFinished()) channelJobDone(*p);
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
    const Signal ch = probe_->kind == "ph" ? Signal::Ph : Signal::Ec;
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
    const Reading& t = truth_.get(Signal::Temp);
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

Result Robot::ackAlarm(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!alarms_.ack(id, clock_.epoch(), rt_)) return Result::fail(404, say("alarm.none"));
  saveState(true);
  return Result::ok({{"alarms", alarms_.list(alarmWatch_, rt_)}});
}

json Robot::notifications() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  json out = json::array();
  for (const auto& n : alarms_.recent()) out.push_back(n);
  return {{"notifications", out}};
}

void Robot::queueDeliveries(std::vector<Delivery> ds) {
  for (auto& d : ds) deliveries_.push_back(std::move(d));
  // Nobody takes them (no platform that sends): the oldest go
  while (deliveries_.size() > kMaxDeliveries) deliveries_.erase(deliveries_.begin());
}

std::vector<Delivery> Robot::takeDeliveries() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  std::vector<Delivery> out;
  out.swap(deliveries_);
  return out;
}

void Robot::deliveryResult(const std::string& channel, bool ok, const std::string& error) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const Epoch epoch = clock_.epoch();
  const bool wasOk = !pushStatus_.contains(channel) || jbool(pushStatus_[channel], "ok", true);
  pushStatus_[channel] = {{"at", epoch}, {"ok", ok}, {"error", ok ? "" : error.substr(0, 200)}};
  // Once when a channel stops getting through, not for every message after
  if (!ok && wasOk)
    log_.add(epoch, "system", "warn", say("ev.push.failed", {{"channel", channel}}), say("ev.push.failed.text", {{"error", error.substr(0, 200)}}));
}

json Robot::pushSettings() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  json j = pushToJson(push_, false);
  j["status"] = pushStatus_;
  return j;
}

Result Robot::putPush(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  PushChannels next = push_;
  const std::vector<Msg> errors = applyPush(next, j);
  if (!errors.empty()) return Result::fail(422, errors.front());
  if (!store_.write(kPushFile, pushToJson(next, true).dump())) return Result::fail(500, say("store.failed"));
  push_ = next;
  log_.add(clock_.epoch(), "config", "info", changed("cfg.what.push"));
  return Result::ok(pushSettings());
}

Result Robot::testPush(const std::string& channel) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Notification n;
  n.kind = "test";
  n.level = "info";
  n.at = clock_.epoch();
  n.title = say("notify.test");
  n.lines = {say("notify.test.text")};
  auto ds = deliveriesFor(n, push_, cfg_.system.language, cfg_.system.name, deliveryId_, channel);
  if (ds.empty()) return Result::fail(409, say("push.none"));
  json sent = json::array();
  for (const auto& d : ds) sent.push_back(d.channel);
  queueDeliveries(std::move(ds));
  return Result::ok({{"channels", sent}});
}

Result Robot::ackLatch(const std::string& id, const json& body) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  auto it = rt_.latches.find(id);
  if (it == rt_.latches.end()) return Result::fail(404, say("latch.none"));
  // A latch of the hardware release stays while the device still reports its cause (SD-110)
  const Release rel = hw_.release();
  if ((id == "hw.leak" && rel.leak) || (id == "hw.high_level" && rel.highLevel))
    return Result::fail(409, say("latch.cause_present"));
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
  if (id == "hw.return") {
    act_.allowReturnPump(true);  // it runs by the level again; on and on once more, it latches again
    returnSince_ = -1;
  }
  log_.add(clock_.epoch(), "unblock", "notice", say("ev.latch.released"), say("latch." + jstr(args, "key", id), args));
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
    if (auto p = doser_.takeFinished()) channelJobDone(*p);
  }
  cycle_.stop(c, act_, doser_);
  if (auto p = doser_.takeFinished()) cycle_.doseFinished(c, *p);
  act_.stopAll();
  saveState(true);
  if (!was) log_.add(clock_.epoch(), "system", "warn", say("ev.stop"), who);
  saveEvents();
  return Result::ok();
}

Epoch Robot::maintenanceUntil() const {
  if (maintenanceUntilMs_ <= 0) return 0;
  return clock_.epoch() + (std::max<Ms>(0, maintenanceUntilMs_ - clock_.nowMs()) + 999) / 1000;
}

Result Robot::maintenance(double minutes) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!isNum(minutes) || minutes < 0 || minutes > kMaxMaintenanceMin)
    return Result::fail(422, say("maint.minutes", {{"max", kMaxMaintenanceMin}}));
  const bool was = maintenanceUntilMs_ > 0;
  maintenanceUntilMs_ = minutes > 0 ? clock_.nowMs() + static_cast<Ms>(std::llround(minutes * 60.0)) * kSecond : 0;
  if (minutes > 0) log_.add(clock_.epoch(), "system", "info", say("ev.maintenance"), say("ev.maintenance.text", {{"min", minutes}}));
  else if (was) log_.add(clock_.epoch(), "system", "info", say("ev.maintenance_end"));
  // The cycle holds at once, or starts anew once the work is done
  if (minutes > 0) {
    Ctx c = ctx();
    cycle_.tick(c, act_, doser_);
  } else if (was) {
    restartCycle();
  }
  saveEvents();
  return Result::ok({{"maintenanceUntil", maintenanceUntil() > 0 ? json(maintenanceUntil()) : json(nullptr)}});
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
