// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/robot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "doseph/embedded.hpp"
#include "doseph/localtime.hpp"
#include "doseph/messages.hpp"
#include "doseph/plants.hpp"
#include "doseph/sha256.hpp"

namespace doseph {

namespace {

constexpr const char* kConfigFile = "config.json";
constexpr const char* kStateFile = "state.json";
constexpr const char* kAuthFile = "auth.json";
constexpr const char* kDeviceFile = "device.json";  // the device's identity, never in a backup (SD-129)
constexpr const char* kPeerFile = "peer.json";  // the coupled device with its token, never in a backup (SD-131)
constexpr const char* kMqttFile = "mqtt.json";  // MQTT with its password, apart from the configuration (SD-130)
constexpr const char* kDeviceSecretsFile = "devices.json";  // network devices' passwords, never in a backup (SD-132)
constexpr const char* kPushFile = "push.json";  // the push channels with their secrets, apart from the configuration (SD-124)
constexpr const char* kEventsFile = "events.json";
constexpr const char* kEventsLog = "events.log";

// The local day, YYYY-MM-DD
std::string localDate(Epoch epoch, const std::string& zone) {
  const LocalTime t = localTime(epoch, zone);
  char date[16];
  std::snprintf(date, sizeof date, "%04d-%02d-%02d", t.year, t.month, t.day);
  return date;
}
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
  c.refilling = refill_.filling();
  // The hardware release (SD-110): a leak and high level hold until a person releases their latch
  const Release r = hw_.release();
  c.pumpCut = r.noFlow;
  c.hold = rt_.latches.count("hw.leak") || r.leak             ? "leak"
           : rt_.latches.count("hw.high_level") || r.highLevel ? "high_level"
           : holds(rt_, "hw.return")                           ? "return"
           : r.stopButton                                      ? "stop_button"
           : !r.on                                             ? "release"
                                                               : "";
  c.follower = peer_ && peer_->role == "follower";
  c.linkOk = c.follower && peerLinkOk(c.now);
  c.leaderPump = c.linkOk && leaderPump_;
  if (peer_ && peer_->role == "leader") c.remote = this;
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

json Robot::setup() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  // What the robot sees of each step it judges itself
  auto calibrated = [&](int ch) {
    if (ch < 1 || ch > hw_.channelCount()) return false;
    const auto it = cfg_.channels.find(ch);
    const std::string& head = hw_.profile().channels[static_cast<size_t>(ch - 1)].head;
    return it != cfg_.channels.end() && isNum(it->second.flowMlPerMin) && it->second.flowMlPerMin > 0 && !head.empty() && it->second.head == head;
  };
  bool phDown = false, channels = true, lines = true;
  for (const auto& b : cfg_.canisters) {
    if (b.channel < 1) continue;
    phDown |= b.role == "ph_down";
    channels &= calibrated(b.channel);
    lines &= rt_.filled.count(b.channel) > 0;
  }
  const std::map<std::string, bool> seen = {
      {"password", auth_.hasPassword()},
      {"flow", rt_.flowAt > 0},
      {"probes", !cfg_.phCal.is_null() && !cfg_.ecCal.is_null()},
      {"channels", phDown && channels},
      {"lines", phDown && lines},
      {"tank", isNum(cfg_.tank.volumeL) && waterPlan(cfg_, clock_.epoch()).recipe != nullptr},
      {"push", push_.ntfy.enabled || push_.telegram.enabled || push_.email.enabled}};
  json steps = json::array();
  json next = nullptr;
  bool complete = true;
  for (const auto& st : kSetupSteps) {
    const std::string how = st.how;
    const auto mark = cfg_.setup.find(st.id);
    std::string state = "open";
    Epoch at = 0;
    if (how == "later") state = "later";
    else if (how == "device" && seen.at(st.id)) state = "done";
    else if (mark != cfg_.setup.end()) {
      state = mark->second.state;
      at = mark->second.at;
    }
    if (state == "open") {
      complete = false;
      if (next.is_null()) next = st.id;
    }
    steps.push_back({{"id", st.id}, {"how", how}, {"optional", st.optional}, {"state", state}, {"at", at}});
  }
  return {{"steps", steps}, {"next", next}, {"complete", complete}};
}

Result Robot::setSetup(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const SetupStep* st = setupStep(id);
  if (!st) return Result::fail(404, say("setup.unknown"));
  const std::string state = jstr(j, "state"), how = st->how;
  Config next = cfg_;
  if (state == "open") {
    next.setup.erase(id);
  } else if (state == "done") {
    // A step the robot sees is done only when it sees it
    if (how != "confirm") return Result::fail(409, say("setup.seen"));
    next.setup[id] = SetupMark{state, clock_.epoch()};
    if (id == "placement") {  // a statement of the owner, kept as an event (SD-118)
      Result r = saveConfig(next, say("ev.setup.placement"));
      if (r.status != 200) return r;
      restartCycle();
      return Result::ok(setup());
    }
  } else if (state == "skipped") {
    if (!st->optional) return Result::fail(409, say("setup.needed"));
    next.setup[id] = SetupMark{state, clock_.epoch()};
  } else {
    return Result::fail(422, say("setup.state"));
  }
  Result r = saveConfig(next, changed("cfg.what.setup"));
  if (r.status != 200) return r;
  if (id == "placement") restartCycle();  // opened again: the cycle goes off
  return Result::ok(setup());
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
  bool configRead = false;
  if (auto s = store_.read(kConfigFile)) {
    try {
      Config c = configFromJson(json::parse(*s));
      if (validateConfig(c, hw_.channelCount()).empty()) {
        cfg_ = c;
        configRead = true;
      } else {
        stateUnknown = true;
      }
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
  // A peer that cannot be read is none: the device stands alone and says so (SD-131)
  if (auto s = store_.read(kPeerFile)) {
    const json j = json::parse(*s, nullptr, false);
    peer_ = peerFromJson(j);
    if (!peer_ && j.is_object() && !j.empty()) log_.add(clock_.epoch(), "coupling", "warn", say("ev.coupling.unreadable"));
  }
  // MQTT settings that cannot be read are off: nothing goes to a broker no one checked (SD-130)
  if (auto s = store_.read(kMqttFile)) {
    const json j = json::parse(*s, nullptr, false);
    mqtt_ = mqttFromJson(j);
  }
  // Network devices' passwords that cannot be read are none: such a device refuses until set again
  if (auto s = store_.read(kDeviceSecretsFile)) devices_.secretsFromJson(json::parse(*s, nullptr, false));
  if (auto s = store_.read(kStateFile)) {
    try {
      rt_ = runtimeFromJson(json::parse(*s));
    } catch (...) {
      // Whether a latch stood is unknown: the robot starts stopped, a person looks (RAT-063)
      rt_ = RuntimeState{};
      stateUnknown = true;
    }
  }
  // The tank's count for the plants' records: a new one where none could be read, so a plant that
  // kept the old one knows its amounts are lost (RAT-064)
  if (rt_.count.since == 0) rt_.count.since = std::max<Epoch>(clock_.epoch(), 1);
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
  climate_.start(bootMs_);  // the dehumidifier rests, and CO2 waits for a value read since (SD-136)
  lastSnapshot_ = bootEpoch_;
  doser_.setBootTag(std::to_string(rt_.bootCount));
  // After a start everything is off; interrupted runs are reported, not resumed (R6, RAT-007). Light
  // follows its plan, and a room's fans their rule (SD-135, SD-136).
  act_.stopAll();
  outputs_.allOff();  // the network devices' outputs too, at the first tick, whatever they say (SD-132)
  if (configRead) {
    // What is kept of a device the configuration no longer has, or has at another address, goes: a
    // later one with its ID starts afresh, and no password reaches another host. Without a
    // configuration read from the file nothing goes: it may come back.
    json kept;
    if (auto s = store_.read(kDeviceSecretsFile)) kept = json::parse(*s, nullptr, false);
    bool dropped = false;
    if (kept.is_object())
      for (const auto& [id, v] : kept.items()) {
        const NetDeviceCfg* d = cfg_.device(id);
        const std::string at = jstr(v, "address");
        if (!d || (!at.empty() && at != d->address)) {
          devices_.forget(id);
          dropped = true;
        }
      }
    if (dropped) saveDeviceSecrets();
  }
  if (holds(rt_, "hw.return")) act_.allowReturnPump(false);  // the device forgot it (SD-109)
  if (stateUnknown) {
    rt_.stopped = true;
    log_.add(bootEpoch_, "alarm", "alarm", say("ev.boot.state_unknown"), say("ev.boot.state_unknown.text"));
  }
  {
    // A fill cut by the restart: the valve is off with every output, the fill is reported and
    // never resumed, and its water is mixed in first (SD-132, R6)
    Ctx c = ctx();
    if (RefillValve::reportCut(c)) afterFill();
    Watering::reportCut(c, localDate(c.epoch, cfg_.system.timezone));  // a shot cut by the restart is reported, not repeated (SD-134)
    // Light is switched by its plan after a start; a hand switch is not resumed (SD-135, R6)
    rt_.lighting = json::object();
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
  // Network devices (SD-132): polled, and their outputs switched only by the gateway (R1)
  outputs_.want("circulation_pump", circulationWanted(c));
  for (const auto& a : devices_.tick(cfg_, now)) outputs_.answer(cfg_, devices_, a, now);
  // The refill valve (SD-132): a fill "top up water" asked for starts, one under way ends when it is
  // done or has to; the gateway switches the valve
  const bool wasFilling = refill_.filling();
  const bool opened = refill_.tick(c, refillIn(now), outputs_);
  if (wasFilling && !refill_.filling()) fillEnded(opened);
  c.refilling = refill_.filling();
  if (c.refilling != wasFilling) saveState(true);
  // Watering (SD-134): the water is judged by a check after the last dose or fill; a tank found
  // full starts the count of litres given anew
  {
    const Phase ph = cycle_.phase();
    if (ph == Phase::Dose || c.refilling) waterChangedAt_ = c.epoch;
    const RoleValue full = devices_.value(cfg_, "tank_full", now);
    const bool counted = rt_.watering.is_object() && rt_.watering.contains("givenL") && rt_.watering["givenL"] == 0;
    if (full.state == "ok" && full.value->is_boolean() && full.value->get<bool>() && (!counted || rt_.waterLow)) {
      rt_.watering["givenL"] = 0;
      rt_.waterLow = false;
    }
    const bool wasWatering = water_.running();
    water_.tick(c, waterIn(c), outputs_);
    if (water_.running() != wasWatering) saveState(true);
  }
  outputs_.want("circulation_pump", circulationWanted(c));
  // Light by its plan or a person's hand, through Stop, holds and maintenance (SD-135)
  // Climate by its rules; the fans through a Stop, the rest off at one; then light within what its
  // heat protection leaves of it (SD-136)
  climate_.tick(c, secondOfDay(c.epoch), devices_, outputs_);
  light_.tick(c, secondOfDay(c.epoch), outputs_, climate_.lightCaps());
  outputs_.tick(cfg_, devices_, rt_.stopped, !c.hold.empty(), now);
  for (const auto& name : outputs_.takeFailedReleases())
    log_.add(epoch, "alarm", "alarm", say("ev.device.release_failed", {{"name", name}}), say("ev.device.release_failed.text"));
  // Drops without a running pump went into the tank: the check mixes first and learns nothing,
  // as after a cut dose (SD-111)
  if (act_.takeUnplannedDrops()) mixFirstNow();
  doser_.tick(c, act_);
  if (auto p = doser_.takeFinished()) {
    // A run that went through its line has filled it: a step of commissioning the robot sees (SD-129)
    if (p->state == DoseProgress::State::Done && p->msDone > 0 && p->channel > 0 && rt_.filled.insert(p->channel).second) saveState(true);
    if (job_ && p->orderId == job_->id) channelJobDone(*p);
    else if (p->orderId.rfind("peer-", 0) == 0) {
      // A run the leader asked for: its result goes back with the next call (SD-131)
      static const char* kStates[] = {"idle", "running", "done", "failed", "aborted"};
      peerRun_ = {{"job", p->orderId.substr(5)}, {"state", kStates[static_cast<int>(p->state)]}, {"channel", p->channel},
                  {"ms", p->msDone}, {"ml", p->mlDone}, {"error", p->error.key.empty() ? json(nullptr) : json(p->error)}};
    } else {
      cycle_.doseFinished(c, *p);
    }
  }
  watchPeer(c);
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
    if (peer_) in.peer = {true, peer_->role == "leader", peerLinkOk(c.now), deviation_, peer_->name};
    in.roles = roleStates(now);
    for (const auto& a : climate_.alerts(cfg_)) in.rooms.push_back({a.id, a.name, a.co2High, a.heat, a.limitC});
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
    if (latched(cfg_, rt_, id, id)) return;
    // Leak and high level always hold; the return pump may report only (SD-133)
    if (setLatch(cfg_, rt_, id, {{"at", epoch}})) {
      act_.stopAll();
      log_.add(epoch, "alarm", "alarm", say(title), say(std::string(title) + ".text"));
    } else {
      log_.add(epoch, "alarm", "alarm", reportedEvent(id, nullptr), say("ev.lock.reported.text"));
    }
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
  if (returnSince_ >= 0 && now - returnSince_ >= kReturnMaxMs && !latched(cfg_, rt_, "hw.return", "hw.return")) {
    latch("hw.return", "ev.hw.return");
    if (holds(rt_, "hw.return")) act_.allowReturnPump(false);
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
  // The package leaves out addresses: the network devices' are in the home network (SD-132)
  json diagConfig = cfg_;
  for (auto& d : diagConfig["devices"]) d["address"] = nullptr;
  json evs = json::array();
  for (const auto& e : log_.query(clock_.epoch() - 72 * 3600, clock_.epoch(), "", 300)) evs.push_back(e);
  return {{"reportId", "DP-" + toHex(b, sizeof b)},
          {"createdAt", clock_.epoch()},
          {"info", info()},
          {"config", diagConfig},
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
          // Commissioning (SD-129): whether every step is done or skipped, and the next open one
          {"setup", [&] {
             const json su = setup();
             return json{{"complete", su["complete"]}, {"next", su["next"]}};
           }()},
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
          {"refill", [&] {
             json r = refill_.status(c, refillIn(c.now));
             r["maxFillMin"] = cfg_.refill.maxFillMin;
             return r;
           }()},
          {"circulation", {{"mode", cfg_.circulation.mode}, {"wanted", circulationWanted(c)}}},
          {"watering", water_.status(c, waterIn(c))},
          {"lighting", Lighting::status(c, secondOfDay(c.epoch), outputs_.status(cfg_, devices_, c.now), climate_.lightCaps())},
          {"climate", climate_.status(c, secondOfDay(c.epoch), devices_, outputs_.status(cfg_, devices_, c.now))},
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
  p.began = {p.start};
  // Its record counts from now: what went in before the device knew of it is not known (RAT-064)
  p.counted = countedNow(rt_.count, now);
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
  // The yield, by hand: while it runs or after it is completed, and changed later (RAT-064)
  if (j.contains("yieldG")) {
    const json& y = j["yieldG"];
    if (!y.is_null() && (!y.is_number() || !isNum(y.get<double>()) || y.get<double>() < 0 || y.get<double>() > kYieldMaxG))
      return Result::fail(422, say("plant.yield"));
    p->yieldG = y.is_null() ? kNaN : y.get<double>();
  }
  if (j.contains("phases")) {
    // A running plant's own phases (SD-137): the current one and those to come change, those past
    // stay as they were; a completed plant's are its record
    if (!p->running()) return Result::fail(409, say("plant.not_running"));
    std::vector<PhaseCfg> phases;
    try {
      phases = phasesFromJson(j);
    } catch (const std::exception&) {
      return Result::fail(422, say("api.bad_input"));
    }
    const auto past = static_cast<size_t>(std::max(p->phase, 0));
    if (phases.size() <= past) return Result::fail(409, say("plant.current_phase"));
    if (phasesJson({p->phases.begin(), p->phases.begin() + static_cast<std::ptrdiff_t>(std::min(past, p->phases.size()))}) !=
        phasesJson({phases.begin(), phases.begin() + static_cast<std::ptrdiff_t>(past)}))
      return Result::fail(409, say("plant.past_phases"));
    p->phases = std::move(phases);
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
  p->began.resize(static_cast<size_t>(p->phase) + 1, 0);
  p->began.back() = p->phaseStart;
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
  // Its record is frozen now: what follows does not change it (RAT-064)
  p->record = plantRecord(next, *p, rt_.count, cfg_.system.timezone);
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
  // A circuit that followed it keeps its own shots (SD-134)
  for (auto& w : next.watering.circuits)
    if (w.plant == id) w.plant.clear();
  for (auto& lc : next.lighting.circuits)
    if (lc.plant == id) lc.plant.clear();
  for (auto& room : next.climate.rooms)
    if (room.plant == id) room.plant.clear();
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

Result Robot::putLocks(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.contains("reportOnly") || !j["reportOnly"].is_array()) return Result::fail(422, say("api.bad_input"));
  Config next = cfg_;
  next.locks.reportOnly.clear();
  for (const auto& x : j["reportOnly"]) {
    if (!x.is_string()) return Result::fail(422, say("api.bad_input"));
    next.locks.reportOnly.push_back(x.get<std::string>());
  }
  const Config was = cfg_;
  Result r = saveConfig(next, std::nullopt);
  if (r.status != 200) return r;
  logLockChanges(was);
  return r;
}

// Each change of a lock is an event, also one a backup brings; one to report only is a warning,
// since the robot then goes on (SD-133)
void Robot::logLockChanges(const Config& was) {
  for (const auto& kind : reportableLocks()) {
    const bool before = was.reportsOnly(kind), now = cfg_.reportsOnly(kind);
    if (now && !before) log_.add(clock_.epoch(), "config", "warn", say("ev.locks.report_only", {{"lock", say("lock." + kind)}}));
    if (before && !now) log_.add(clock_.epoch(), "config", "info", say("ev.locks.lock", {{"lock", say("lock." + kind)}}));
  }
}

Result Robot::newTank() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  // A person announces a new fill: its jumps are explained, and the next dose is a test dose (SD-105, RAT-042)
  rt_.newTank = true;
  rt_.ecInBand = nullptr;  // a new fill (SD-115)
  rt_.topUp = false;
  rt_.watering["givenL"] = 0;  // and the count of litres watered starts anew (SD-134)
  rt_.waterLow = false;
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
  // On the coupled device's channel, named by its ID; "" back on this device (SD-131)
  if (j.contains("device")) {
    if (!j["device"].is_string()) return Result::fail(422, say("api.bad_input"));
    b->device = j["device"].get<std::string>();
  }
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
  if (channelJobActive() || probe_ || doser_.busy() || cycle_.dosing() || refill_.filling() || water_.running())
    return Result::fail(409, say("config.busy"));
  // The lock against a second first setup never comes from a file
  next.system.passwordSet = cfg_.system.passwordSet || auth_.hasPassword();
  // Nor what the owner confirmed of commissioning: placement is physical, so a restored or new
  // device asks again (SD-118, SD-129)
  next.setup = cfg_.setup;
  const Config before = cfg_;
  Result r = saveConfig(next, say("ev.config.imported"));
  if (r.status != 200) return r;
  logLockChanges(before);
  // The network devices' outputs off, and what moved or went is let go (SD-132)
  outputs_.allOff();
  reconcileDevices(before);
  // Watering starts from the new schedules; a circuit that went takes its records with it (SD-134)
  water_.reset();
  for (const auto& w : before.watering.circuits)
    if (!cfg_.watering.circuit(w.id)) Watering::forgetRuntime(rt_, w.id);
  for (const auto& lc : before.lighting.circuits)
    if (!cfg_.lighting.circuit(lc.id)) Lighting::forget(rt_, lc.id);
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
  // Coupled as the follower: never a run here while the leader may dose; in its maintenance, yes (SD-131)
  if (peer_ && peer_->role == "follower" && peerLinkOk(clock_.nowMs()) && !leaderMaintenance_)
    return Result::fail(409, say("coupling.channel_job", {{"name", peer_->name}}));
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

// ---------------------------------------------------------------- coupling (SD-131)

bool Robot::savePeer() { return store_.write(kPeerFile, peer_ ? peerToJson(*peer_, true).dump() : std::string("{}")); }

json Robot::coupling() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const bool armed = pairing_ && clock_.nowMs() < pairing_->until;
  return {{"deviceId", deviceId_},
          {"peer", peer_ ? peerToJson(*peer_, false) : json(nullptr)},
          {"code", armed ? json{{"until", clock_.epoch() + (pairing_->until - clock_.nowMs()) / 1000}} : json(nullptr)},
          {"pairing", pairStatus_},
          {"link", peer_ ? json{{"ok", peerLinkOk(clock_.nowMs())},
                                {"heard", peerHeard_ >= 0 ? json(clock_.epoch() - (clock_.nowMs() - peerHeard_) / 1000) : json(nullptr)},
                                {"peer", peerState_},
                                {"run", peerRun_}}
                         : json(nullptr)}};
}

Result Robot::armPairingCode() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (peer_) return Result::fail(409, say("coupling.already", {{"name", peer_->name}}));
  static constexpr char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  std::uint8_t r[8];
  rng_(r, sizeof r);
  PairingCode c;
  for (std::uint8_t b : r) c.code += kAlphabet[b & 31];  // 256 is a multiple of 32: no bias
  c.until = clock_.nowMs() + PairingCode::kValid;
  pairing_ = c;
  return Result::ok({{"code", c.code.substr(0, 4) + "-" + c.code.substr(4)}, {"until", clock_.epoch() + PairingCode::kValid / 1000}});
}

Result Robot::pair(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (peer_) return Result::fail(409, say("coupling.already", {{"name", peer_->name}}));
  if (std::any_of(peerPending_.begin(), peerPending_.end(), [](const auto& x) { return x.second.kind == "join"; }))
    return Result::fail(409, say("coupling.pending"));
  const std::string address = jstr(j, "address"), code = jstr(j, "code");
  if (!peerAddress(address)) return Result::fail(422, say("coupling.address"));
  if (code.empty() || code.size() > 16) return Result::fail(422, say("coupling.code"));
  // The token the follower will use to call this device; revoked again if the pairing fails
  const Auth before = auth_;
  Msg err;
  const auto token = auth_.createToken(say("coupling.token_name").text, Role::Peer, clock_.epoch(), err);
  if (!token) return Result::fail(422, err);
  if (!saveAuth()) {
    auth_ = before;
    return Result::fail(500, say("store.failed"));
  }
  PeerRequest req;
  req.id = ++peerSeq_;
  req.address = address;
  req.method = "POST";
  req.path = "/api/v1/coupling/join";
  req.body = {{"code", code}, {"id", deviceId_}, {"name", cfg_.system.name}, {"token", *token}};
  peerOut_.push_back(req);
  peerPending_[req.id] = {"join", address, auth_.tokens().back().id, ""};
  pairStatus_ = {{"state", "pairing"}, {"address", address}, {"error", nullptr}, {"at", clock_.epoch()}};
  return Result{202, {{"pending", true}}};
}

Result Robot::join(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (peer_) return Result::fail(409, say("coupling.already", {{"name", peer_->name}}));
  const Ms now = clock_.nowMs();
  if (!pairing_ || now >= pairing_->until) return Result::fail(403, say("coupling.code"));
  std::string typed;  // as the owner typed it: any case, a dash or spaces between the groups
  for (char ch : jstr(j, "code"))
    if (ch != '-' && !std::isspace(static_cast<unsigned char>(ch))) typed += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  unsigned diff = typed.size() == pairing_->code.size() ? 0 : 1;
  for (size_t i = 0; i < typed.size() && i < pairing_->code.size(); ++i) diff |= static_cast<unsigned>(typed[i] ^ pairing_->code[i]);
  if (diff != 0) {
    // The fifth wrong try voids the code: 40 bits leave nothing to guess in five
    if (++pairing_->wrong >= PairingCode::kMaxWrong) pairing_.reset();
    log_.add(clock_.epoch(), "coupling", "warn", say("ev.coupling.code_wrong"));
    return Result::fail(403, say("coupling.code"));
  }
  Peer p;
  p.role = "follower";
  p.id = jstr(j, "id");
  p.name = jstr(j, "name");
  p.token = jstr(j, "token");
  p.since = clock_.epoch();
  if (p.id == deviceId_) return Result::fail(422, say("coupling.self"));
  const Auth before = auth_;
  Msg err;
  const auto token = auth_.createToken(say("coupling.token_name").text, Role::Peer, clock_.epoch(), err);
  if (!token) return Result::fail(422, err);
  p.tokenId = auth_.tokens().back().id;
  if (!peerFromJson(peerToJson(p, true))) {
    auth_ = before;
    return Result::fail(422, say("api.bad_input"));
  }
  peer_ = p;
  if (!saveAuth() || !savePeer()) {
    auth_ = before;
    peer_.reset();
    saveAuth();
    savePeer();
    return Result::fail(500, say("store.failed"));
  }
  pairing_.reset();
  log_.add(clock_.epoch(), "coupling", "notice", say("ev.coupling.joined", {{"name", p.name}}));
  return Result::ok({{"id", deviceId_}, {"name", cfg_.system.name}, {"token", *token}});
}

void Robot::peerResponse(long long id, int status, const json& body) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const auto it = peerPending_.find(id);
  if (it == peerPending_.end()) return;
  const PeerPending pending = it->second;
  peerPending_.erase(it);
  auto msgOf = [](const json& e, const char* fallback) {
    return e.is_object() && e.contains("key") ? Msg{jstr(e, "key"), jstr(e, "text"), e.value("args", json::object())} : say(fallback);
  };
  if (pending.kind == "run") {
    // Refused there, or not delivered: the run ended before it began
    const auto r = remoteRuns_.find(pending.job);
    if (status != 200 && r != remoteRuns_.end() && r->second.state == "running")
      r->second = RemoteResult{"failed", 0, 0, msgOf(body.is_object() ? body.value("error", json()) : json(), status == 0 ? "coupling.unreachable" : "coupling.refused")};
    return;
  }
  if (pending.kind == "beat") {
    if (status != 200 || !body.is_object() || !peer_) return;
    if (linkLost_) log_.add(clock_.epoch(), "coupling", "notice", say("ev.coupling.link_back", {{"name", peer_->name}}));
    linkLost_ = false;
    peerHeard_ = clock_.nowMs();
    peerState_ = body;
    // What the follower's channels move, for the plan (SD-131)
    cfg_.peerId = peer_->id;
    cfg_.peerFlows.clear();
    for (const auto& ch : body.value("channels", json::array()))
      if (ch.is_object() && ch.value("flow", json()).is_number()) cfg_.peerFlows[ch.value("channel", 0)] = ch["flow"].get<double>();
    // The run asked for, as it stands there
    const json run = body.value("run", json());
    if (run.is_object())
      if (auto r = remoteRuns_.find(jstr(run, "job")); r != remoteRuns_.end() && r->second.state == "running") {
        const std::string st = jstr(run, "state");
        if (st == "done" || st == "failed" || st == "aborted")
          r->second = RemoteResult{st, static_cast<Ms>(jnum(run, "ms", 0)), jnum(run, "ml"), msgOf(run.value("error", json()), "dose.run_cancelled")};
      }
    return;
  }
  if (pending.kind != "join") return;
  Peer p;
  p.role = "leader";
  p.id = jstr(body, "id");
  p.name = jstr(body, "name");
  p.address = pending.address;
  p.token = jstr(body, "token");
  p.tokenId = pending.tokenId;
  p.since = clock_.epoch();
  const bool ok = status == 200 && body.is_object() && peerFromJson(peerToJson(p, true)) && p.id != deviceId_;
  if (ok) {
    peer_ = p;
    if (savePeer()) {
      pairStatus_ = nullptr;
      log_.add(clock_.epoch(), "coupling", "notice", say("ev.coupling.paired", {{"name", p.name}}));
      return;
    }
    peer_.reset();
  }
  // Failed: the token made for the other device goes again, and the reason stays to be shown
  auth_.revokeToken(pending.tokenId);
  saveAuth();
  json error = body.is_object() && body.contains("error") ? body["error"] : json(say(status == 0 ? "coupling.unreachable" : "coupling.refused"));
  if (ok) error = say("store.failed");
  pairStatus_ = {{"state", "failed"}, {"address", pending.address}, {"error", error}, {"at", clock_.epoch()}};
}

Result Robot::unpair() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!peer_) return Result::fail(404, say("coupling.none"));
  const Peer was = *peer_;
  // The leader knows where the follower is and tells it; a follower's leader learns it from its
  // next call, which the revoked token no longer opens
  if (!was.address.empty()) {
    PeerRequest req;
    req.id = ++peerSeq_;
    req.address = was.address;
    req.method = "POST";
    req.path = "/api/v1/coupling/leave";
    req.token = was.token;
    peerOut_.push_back(req);
    peerPending_[req.id] = {"leave", was.address, "", ""};
  }
  auth_.revokeToken(was.tokenId);
  peer_.reset();
  peerHeard_ = beatSent_ = -1;
  leaderPump_ = linkLost_ = false;
  peerState_ = peerRun_ = nullptr;
  if (!saveAuth() || !savePeer()) return Result::fail(500, say("store.failed"));
  log_.add(clock_.epoch(), "coupling", "notice", say("ev.coupling.ended", {{"name", was.name}}));
  return Result::ok(coupling());
}

Result Robot::left(const std::string& token) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!peer_ || !isPeerToken(token)) return Result::fail(403, say("coupling.not_peer"));
  const Peer was = *peer_;
  auth_.revokeToken(was.tokenId);
  peer_.reset();
  peerHeard_ = beatSent_ = -1;
  leaderPump_ = linkLost_ = false;
  peerState_ = peerRun_ = nullptr;
  if (!saveAuth() || !savePeer()) return Result::fail(500, say("store.failed"));
  log_.add(clock_.epoch(), "coupling", "notice", say("ev.coupling.left", {{"name", was.name}}));
  return Result::ok();
}

// The follower measures as a check (SD-131): compared only while both pumps move water
void Robot::watchDeviation(const Ctx& c) {
  bool compared = false, differ = false;
  if (peerLinkOk(c.now) && c.flowOk && peerState_.is_object() && peerState_.value("flowOk", false)) {
    const json theirs = peerState_.value("readings", json::object());
    for (const auto& [sig, tol] : {std::pair<Signal, double>{Signal::Ph, kPeerDevPh}, {Signal::Ec, kPeerDevEc}}) {
      const Reading& mine = truth_.get(sig);
      const json v = theirs.value(signalName(sig), json());
      if (!mine.usable() || !v.is_number()) continue;
      compared = true;
      differ |= std::fabs(*mine.value - v.get<double>()) > tol;
    }
  }
  if (!compared || !differ) deviatesSince_ = -1;
  if (compared && !differ && deviation_) {
    deviation_ = false;
    log_.add(c.epoch, "coupling", "notice", say("ev.coupling.agree", {{"name", peer_->name}}));
  }
  if (!compared || !differ) return;
  if (deviatesSince_ < 0) deviatesSince_ = c.now;
  if (!deviation_ && c.now - deviatesSince_ >= kPeerDevMs) {
    deviation_ = true;
    log_.add(c.epoch, "coupling", "warn", say("ev.coupling.deviation", {{"name", peer_->name}}),
             say("watch.coupling.deviation", {{"name", peer_->name}, {"ph", kPeerDevPh}, {"ec", kPeerDevEc}}));
  }
}

void Robot::watchPeer(Ctx& c) {
  if (!peer_) return;
  if (peer_->role == "follower") {
    // 10 s without the leader: a run it asked for stops and is booked, and none is taken until it
    // calls again (SD-131)
    const bool ok = peerLinkOk(c.now);
    if (!ok && peerHeard_ >= 0 && !linkLost_) {
      linkLost_ = true;
      log_.add(c.epoch, "coupling", "warn", say("ev.coupling.link_lost", {{"name", peer_->name}}), say("ev.coupling.link_lost.text"));
    }
    if (!ok && doser_.busy() && doser_.progress().orderId.rfind("peer-", 0) == 0) {
      doser_.abort(c, act_, say("abort.link"));
      if (auto p = doser_.takeFinished())
        peerRun_ = {{"job", p->orderId.substr(5)}, {"state", "aborted"}, {"channel", p->channel}, {"ms", p->msDone}, {"ml", p->mlDone}, {"error", say("abort.link")}};
    }
    return;
  }
  if (!peerLinkOk(c.now)) {
    // Without the follower its channels are not planned, and a run sent there counts as ended:
    // it stops itself after 10 s without a call
    cfg_.peerId.clear();
    cfg_.peerFlows.clear();
    for (auto& [job, r] : remoteRuns_)
      if (r.state == "running") r = RemoteResult{"aborted", 0, kNaN, say("coupling.no_link")};
  }
  watchDeviation(c);
  // The leader calls every 2 s, whether or not the last call came back
  if (beatSent_ >= 0 && c.now - beatSent_ < kPeerBeatMs) return;
  beatSent_ = c.now;
  for (auto it = peerPending_.begin(); it != peerPending_.end();)
    it = it->second.kind == "beat" ? peerPending_.erase(it) : std::next(it);  // an answer that never came is not waited for
  PeerRequest req;
  req.id = ++peerSeq_;
  req.address = peer_->address;
  req.method = "POST";
  req.path = "/api/v1/peer/beat";
  req.token = peer_->token;
  req.body = {{"pump", act_.feedPump().value_or(false)}, {"maintenance", c.maintenanceUntil > 0}, {"at", c.epoch}};
  peerOut_.push_back(req);
  peerPending_[req.id] = {"beat", peer_->address, "", ""};
  const bool ok = peerLinkOk(c.now);
  if (!ok && peerHeard_ >= 0 && !linkLost_) {
    linkLost_ = true;
    log_.add(c.epoch, "coupling", "warn", say("ev.coupling.link_lost", {{"name", peer_->name}}));
  }
}

Result Robot::peerBeat(const std::string& token, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!isPeerToken(token)) return Result::fail(403, say("coupling.not_peer"));
  if (peer_->role != "follower") return Result::fail(409, say("coupling.not_follower"));
  const Ms now = clock_.nowMs();
  if (linkLost_) log_.add(clock_.epoch(), "coupling", "notice", say("ev.coupling.link_back", {{"name", peer_->name}}));
  linkLost_ = false;
  peerHeard_ = now;
  leaderPump_ = jbool(j, "pump", false);
  leaderMaintenance_ = jbool(j, "maintenance", false);
  // Each channel's calibrated flow, for the leader's plan; none where it is void (SD-108)
  json channels = json::array();
  for (int ch = 1; ch <= hw_.channelCount(); ++ch) {
    const auto it = cfg_.channels.find(ch);
    const std::string& head = hw_.profile().channels[static_cast<size_t>(ch - 1)].head;
    const bool ok = it != cfg_.channels.end() && isNum(it->second.flowMlPerMin) && it->second.flowMlPerMin > 0 && !head.empty() && it->second.head == head;
    channels.push_back({{"channel", ch}, {"flow", ok ? json(it->second.flowMlPerMin) : json(nullptr)}});
  }
  json readings = json::object();
  for (Signal ch : {Signal::Ph, Signal::Ec, Signal::Temp}) {
    const Reading& r = truth_.get(ch);
    readings[signalName(ch)] = r.usable() ? numOrNull(r.value) : json(nullptr);
  }
  return Result::ok({{"readings", readings},
                     {"flowOk", flowOk()},
                     {"feedPump", act_.feedPump().value_or(false)},
                     {"stopped", rt_.stopped},
                     {"channels", channels},
                     {"run", peerRun_}});
}

Result Robot::peerRun(const std::string& token, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!isPeerToken(token)) return Result::fail(403, say("coupling.not_peer"));
  if (peer_->role != "follower") return Result::fail(409, say("coupling.not_follower"));
  Ctx c = ctx();
  if (!c.linkOk) return Result::fail(409, say("coupling.no_link"));
  if (doser_.busy() || channelJobActive()) return Result::fail(409, say("coupling.busy"));
  const std::string job = jstr(j, "job"), purpose = jstr(j, "purpose");
  const double ms = jnum(j, "ms");
  const int channel = j.contains("channel") && j["channel"].is_number_integer() ? j["channel"].get<int>() : 0;
  if (job.empty() || job.size() > 40 || (purpose != "ec" && purpose != "ph") || !isNum(ms) || ms <= 0 || channel < 1)
    return Result::fail(422, say("coupling.run"));
  const CanisterCfg* b = cfg_.canisterOnChannel(channel);
  DoseOrder o;
  o.id = "peer-" + job;
  o.purpose = purpose;
  o.step.channel = channel;
  o.step.canister = b ? b->id : "";
  o.step.name = b ? b->name : say("channel.n", {{"channel", channel}}).text;
  o.step.flowMlPerMin = cfg_.channelFlow(channel);
  o.step.runs = {static_cast<Ms>(std::llround(ms))};
  o.step.ml = isNum(o.step.flowMlPerMin) ? ms / 60000.0 * o.step.flowMlPerMin : kNaN;
  // The gateway of this device decides, as for its own doses: flow, placement, calibration, locks,
  // daily amount, drops (SD-105, SD-111, SD-113, SD-118)
  Msg err;
  if (!doser_.start(c, act_, o, err)) return Result::fail(409, err);
  peerRun_ = {{"job", job}, {"state", "running"}, {"channel", channel}, {"ms", 0}, {"ml", 0}, {"error", nullptr}};
  return Result::ok({{"job", job}});
}

bool Robot::linked() const { return peer_ && peer_->role == "leader" && peerLinkOk(clock_.nowMs()); }

void Robot::send(const std::string& job, int channel, Ms ms, const std::string& purpose) {
  if (!peer_) return;
  PeerRequest req;
  req.id = ++peerSeq_;
  req.address = peer_->address;
  req.method = "POST";
  req.path = "/api/v1/peer/run";
  req.token = peer_->token;
  req.body = {{"job", job}, {"channel", channel}, {"ms", ms}, {"purpose", purpose}};
  peerOut_.push_back(req);
  peerPending_[req.id] = {"run", peer_->address, "", job};
  remoteRuns_[job] = RemoteResult{"running", 0, kNaN, {}};
}

std::optional<RemoteResult> Robot::result(const std::string& job) {
  const auto it = remoteRuns_.find(job);
  if (it == remoteRuns_.end() || it->second.state == "running") return std::nullopt;
  RemoteResult r = it->second;
  remoteRuns_.erase(it);
  return r;
}

void Robot::abort(const std::string& job) {
  remoteRuns_.erase(job);
  if (!peer_) return;
  PeerRequest req;
  req.id = ++peerSeq_;
  req.address = peer_->address;
  req.method = "POST";
  req.path = "/api/v1/peer/abort";
  req.token = peer_->token;
  peerOut_.push_back(req);
  peerPending_[req.id] = {"abort", peer_->address, "", job};
}

Result Robot::peerAbort(const std::string& token) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!isPeerToken(token)) return Result::fail(403, say("coupling.not_peer"));
  if (doser_.busy() && doser_.progress().orderId.rfind("peer-", 0) == 0) {
    Ctx c = ctx();
    doser_.abort(c, act_, say("abort.peer"));
  }
  return Result::ok();
}

bool Robot::isPeerToken(const std::string& token) const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const auto id = auth_.tokenIdOf(token);
  return peer_ && id && *id == peer_->tokenId;
}

std::vector<PeerRequest> Robot::takePeerRequests() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  std::vector<PeerRequest> out;
  out.swap(peerOut_);
  return out;
}

json Robot::mqttSettings() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  json j = mqttToJson(mqtt_, false);
  j["commandsOn"] = mqttCommands(mqtt_);
  j["status"] = mqttStatus_;
  j["topic"] = mqttBase(deviceId_);
  return j;
}

Result Robot::putMqtt(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  MqttSettings next = mqtt_;
  const std::vector<Msg> errors = applyMqtt(next, j);
  if (!errors.empty()) return Result::fail(422, errors.front(), {{"errors", errors}});
  if (!store_.write(kMqttFile, mqttToJson(next, true).dump())) return Result::fail(500, say("store.failed"));
  mqtt_ = next;
  mqttRevision_++;
  mqttStatus_ = {{"connected", false}, {"error", nullptr}, {"at", 0}};
  devices_.mqttConnected(false);  // the platform connects anew; what came before counts no more
  log_.add(clock_.epoch(), "config", "info", changed("cfg.what.mqtt"));
  return Result::ok(mqttSettings());
}

void Robot::setMqttStatus(bool connected, const Msg& error) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  mqttStatus_ = {{"connected", connected}, {"error", error.key.empty() ? json(nullptr) : json(error)}, {"at", clock_.epoch()}};
  devices_.mqttConnected(connected);
}

std::vector<std::string> Robot::mqttValueTopics() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return devices_.mqttTopics(cfg_);
}

void Robot::mqttValue(const std::string& topic, const std::string& payload) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  devices_.mqttMessage(cfg_, topic, payload, clock_.nowMs());
}

// ---------------------------------------------------------------- Network devices (SD-132)

std::vector<WatchInput::Role> Robot::roleStates(Ms now) const {
  std::vector<WatchInput::Role> out;
  const json outs = outputs_.status(cfg_, devices_, now);
  for (const auto& def : roleDefs(cfg_)) {
    if (!cfg_.roles.count(def.id)) continue;
    WatchInput::Role r{def.id, false, {}, roleLabel(cfg_, def.id)};
    if (def.kind == "output") {
      const json& o = outs[def.id];
      const std::string st = jstr(o, "state");
      r.ok = st == "on" || st == "off";
      const Msg why = outputs_.problem(def.id);
      r.text = r.ok ? say(st == "on" ? "watch.role.on" : "watch.role.off") : !why.key.empty() ? why : say("watch.role." + st);
    } else {
      const RoleValue v = devices_.value(cfg_, def.id, now);
      r.ok = v.state == "ok";
      r.text = r.ok ? say(v.value->is_boolean() ? (v.value->get<bool>() ? "watch.role.true" : "watch.role.false") : "watch.role.value",
                          {{"value", *v.value}})
                    : say("watch.role." + v.state);
    }
    out.push_back(std::move(r));
  }
  return out;
}

bool Robot::saveDeviceSecrets() {
  // Each entry with the address it belongs to: a configuration that moved the device without this
  // file being written after it (a power loss between the two) drops it at the next start
  json j = devices_.secretsJson();
  for (auto& [id, v] : j.items())
    if (const NetDeviceCfg* d = cfg_.device(id)) v["address"] = d->address;
  return store_.write(kDeviceSecretsFile, j.dump());
}

// After the configuration changed (a device added or removed, a role bound or released, an import):
// an output whose binding moved or went is let go and switched off, and a device that went or moved
// to another address or kind is forgotten with its password and the ID it had (SD-132)
void Robot::reconcileDevices(const Config& before) {
  const Ms now = clock_.nowMs();
  // A fill whose valve or float changed ends: the new binding starts with nothing wanted
  auto bound = [](const Config& c, const char* role) {
    if (!c.roles.count(role)) return json(nullptr);
    json j = bindingToJson(c.roles.at(role));
    if (const NetDeviceCfg* d = c.device(c.roles.at(role).device)) j["at"] = deviceToJson(*d);
    return j;
  };
  if (refill_.filling() && (bound(before, "refill_valve") != bound(cfg_, "refill_valve") || bound(before, "tank_full") != bound(cfg_, "tank_full"))) {
    Ctx c = ctx();
    if (const auto opened = refill_.stop(c, outputs_, say("refill.end.changed"))) fillEnded(*opened);
  }
  // So does a shot whose circuit's output changed (SD-134)
  for (const auto& w : before.watering.circuits)
    if (bound(before, waterRole(w.id).c_str()) != bound(cfg_, waterRole(w.id).c_str())) {
      Ctx c = ctx();
      water_.stop(c, outputs_, w.id, say("water.end.changed"));
    }
  for (const auto& def : roleDefs(before)) {
    if (def.kind != "output") continue;
    const auto was = before.roles.find(def.id);
    if (was == before.roles.end() || was->second.source != "shelly") continue;
    const NetDeviceCfg* d0 = before.device(was->second.device);
    const auto is = cfg_.roles.find(def.id);
    const NetDeviceCfg* d1 = is == cfg_.roles.end() ? nullptr : cfg_.device(is->second.device);
    // A light changed between switch and dimmer is another Shelly component: let go as the old one
    const auto def1 = roleDef(cfg_, def.id);
    const bool same = d0 && d1 && is->second.source == "shelly" && is->second.channel == was->second.channel && d0->id == d1->id &&
                      d0->address == d1->address && d0->kind == d1->kind && def1 && def1->dimmer == def.dimmer;
    if (!same) outputs_.release(before, devices_, def.id, now);
  }
  bool forgot = false;
  for (const auto& d0 : before.devices) {
    const NetDeviceCfg* d1 = cfg_.device(d0.id);
    if (!d1 || d1->address != d0.address || d1->kind != d0.kind) {
      devices_.forget(d0.id);
      forgot = true;
    }
  }
  devices_.pruneMqtt(cfg_);
  if (forgot) saveDeviceSecrets();
}

json Robot::devices() const {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const Ms now = clock_.nowMs();
  json roles = json::object();
  const json outs = outputs_.status(cfg_, devices_, now);
  for (const auto& def : roleDefs(cfg_)) {
    json r = {{"kind", def.kind}, {"pulse", def.pulse}, {"autoOffS", autoOffS(def)}, {"binding", nullptr}, {"label", roleLabel(cfg_, def.id)}};
    if (const auto it = cfg_.roles.find(def.id); it != cfg_.roles.end()) r["binding"] = bindingToJson(it->second);
    if (def.kind == "output") {
      r["output"] = outs[def.id];
    } else {
      const RoleValue v = devices_.value(cfg_, def.id, now);
      r["value"] = v.value ? *v.value : json(nullptr);
      r["state"] = v.state;
    }
    roles[def.id] = r;
  }
  return {{"devices", devices_.status(cfg_, now)}, {"roles", roles}, {"mqttTopics", devices_.mqttTopics(cfg_)}};
}

Result Robot::addDevice(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Config next = cfg_;
  NetDeviceCfg d;
  for (int n = 1; n < 1000 && d.id.empty(); ++n)
    if (!cfg_.device("d" + std::to_string(n))) d.id = "d" + std::to_string(n);
  d.kind = jstr(j, "kind");
  d.address = jstr(j, "address");
  d.name = utf8Prefix(jstr(j, "name"), 40);
  next.devices.push_back(d);
  const Config before = cfg_;
  Result r = saveConfig(next, say("ev.device.added", {{"name", d.name}}));
  if (r.status != 200) return r;
  reconcileDevices(before);
  devices_.forget(d.id);  // an ID used before starts afresh: nothing it said, no password
  saveDeviceSecrets();
  return Result::ok({{"id", d.id}});
}

Result Robot::removeDevice(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const NetDeviceCfg* d = cfg_.device(id);
  if (!d) return Result::fail(404, say("device.none"));
  const std::string name = d->name;
  Config next = cfg_;
  // Its roles go with it; an output is switched off (R1: through the gateway)
  for (auto it = next.roles.begin(); it != next.roles.end();)
    it = (it->second.source == "shelly" || it->second.source == "esphome") && it->second.device == id ? next.roles.erase(it) : std::next(it);
  next.devices.erase(std::remove_if(next.devices.begin(), next.devices.end(), [&](const NetDeviceCfg& x) { return x.id == id; }), next.devices.end());
  const Config before = cfg_;
  Result r = saveConfig(next, say("ev.device.removed", {{"name", name}}));
  if (r.status != 200) return r;
  reconcileDevices(before);
  // Removed either way; a password file not written is dropped at the next start, as its device is gone
  if (!saveDeviceSecrets()) log_.add(clock_.epoch(), "config", "warn", say("store.failed"));
  return r;
}

Result Robot::putDeviceCredentials(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const NetDeviceCfg* d = cfg_.device(id);
  if (!d) return Result::fail(404, say("device.none"));
  const std::string user = jstr(j, "user"), password = jstr(j, "password");
  if (user.size() > 64 || password.size() > 128) return Result::fail(422, say("api.bad_input"));
  devices_.setCredentials(id, d->kind == "shelly" ? "admin" : user, password);
  if (!saveDeviceSecrets()) return Result::fail(500, say("store.failed"));
  return Result::ok();
}

Result Robot::putRole(const std::string& role, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!roleDef(cfg_, role)) return Result::fail(404, say("role.none"));
  RoleBinding b;
  try {
    b = bindingFromJson(j);
  } catch (...) {
    return Result::fail(422, say("api.bad_input"));
  }
  Config next = cfg_;
  next.roles[role] = b;
  const Config before = cfg_;
  Result r = saveConfig(next, say("ev.role.bound", {{"role", roleLabel(cfg_, role)}}));
  if (r.status != 200) return r;  // refused: the output it had stays as it is
  reconcileDevices(before);
  return r;
}

Result Robot::deleteRole(const std::string& role) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!cfg_.roles.count(role)) return Result::fail(404, say("role.none"));
  Config next = cfg_;
  next.roles.erase(role);
  const Config before = cfg_;
  Result r = saveConfig(next, say("ev.role.unbound", {{"role", roleLabel(cfg_, role)}}));
  if (r.status != 200) return r;
  reconcileDevices(before);
  return r;
}

bool Robot::wantOutput(const std::string& role, bool on) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return outputs_.want(role, on);
}

RefillIn Robot::refillIn(Ms now) const {
  RefillIn in;
  const RoleValue v = devices_.value(cfg_, "tank_full", now);
  in.floatAssigned = v.state != "not_assigned";
  if (v.state == "ok" && v.value->is_boolean()) in.full = v.value->get<bool>();
  const json valve = outputs_.status(cfg_, devices_, now)["refill_valve"];
  in.valve = jstr(valve, "state");
  in.switchedOn = jbool(valve, "switchedOn", false);
  in.busy = doser_.busy() || cycle_.dosing() || channelJobActive() || probe_.has_value();
  return in;
}

void Robot::afterFill() {
  // Fresh water moves pH, EC and the temperature: the jumps are explained (RAT-042), and the check
  // mixes the water in before it measures (SD-112)
  const Ms now = clock_.nowMs();
  for (Signal ch : {Signal::Ph, Signal::Ec, Signal::Temp}) truth_.expectChange(ch, now + 10 * kMinute);
  mixFirstNow();
}

// The circulation pump (SD-132, PD-421): never during Stop, maintenance or a hold, nor on the
// follower of a coupled pair, whose leader runs the check; set to always, all the time; otherwise
// during a dose and its mixing wait, and during a fill. It is off while the check measures: every
// reading is taken alike, and a pump that disturbs the probes cannot bias one (RAT-044). The mixing
// wait stays as long as without it (#1).
bool Robot::circulationWanted(const Ctx& c) const {
  if (c.stopped || c.maintenanceUntil > 0 || !c.hold.empty() || c.follower) return false;
  if (cfg_.circulation.mode == "always") return true;
  const Phase ph = cycle_.phase();
  return c.refilling || ph == Phase::Dose || ph == Phase::Mix;
}

Result Robot::putCirculation(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.contains("mode") || !j["mode"].is_string()) return Result::fail(422, say("api.bad_input"));
  Config next = cfg_;
  next.circulation.mode = j["mode"].get<std::string>();
  return saveConfig(next, changed("cfg.what.circulation"));
}

WaterIn Robot::waterIn(const Ctx& c) const {
  WaterIn in;
  in.date = localDate(c.epoch, cfg_.system.timezone);
  in.minuteOfDay = localTime(c.epoch, cfg_.system.timezone).minuteOfDay;
  in.clockSecured = clock_.secured();
  const Phase ph = cycle_.phase();
  const Measured& last = cycle_.last();
  // Mixed in: no dose, fill or mixing wait under way, and the check measured after the last of them
  in.dosing = ph == Phase::Dose;
  in.mixing = c.refilling || in.dosing || ph == Phase::Mix || ph == Phase::Remeasure || cycle_.mixPending() || last.at <= waterChangedAt_;
  in.ph = last.ph;
  in.ec = last.ec;
  const CycleSettings s = cycleSettings(c);
  in.stale = last.at <= 0 || c.epoch - last.at > 3 * s.holdMs / 1000;  // three holds: the check has stopped
  // The EC target: the higher of this phase's and the last phase's, so that a phase that asks for
  // less does not stop watering on the water still in the tank (RAT-086)
  in.ecTarget = s.ecTarget;
  const WaterPlan plan = waterPlan(cfg_, c.epoch);
  if (plan.source == "plant" && plan.plant && plan.plant->phase > 0 && plan.plant->phase <= static_cast<int>(plan.plant->phases.size())) {
    const double before = phaseEcTarget(cfg_, plan.plant->phases[static_cast<size_t>(plan.plant->phase - 1)]);
    if (isNum(before) && (!isNum(in.ecTarget) || before > in.ecTarget)) in.ecTarget = before;
  }
  in.phTarget = s.phTarget;
  in.phTol = s.phTol;
  in.calibrating = c.probeCalibrating;
  in.flowDry = holds(rt_, "flow.dry");
  const RoleValue empty = devices_.value(cfg_, "tank_empty", c.now);
  in.emptyAssigned = empty.state != "not_assigned";
  if (empty.state == "ok" && empty.value->is_boolean()) in.empty = empty.value->get<bool>();
  const RoleValue level = devices_.value(cfg_, "tank_level", c.now);
  in.levelAssigned = level.state != "not_assigned";
  if (level.state == "ok" && level.value->is_number()) in.levelL = level.value->get<double>();
  in.volumeL = cfg_.tank.volumeL;
  const json outs = outputs_.status(cfg_, devices_, c.now);
  for (const auto& w : cfg_.watering.circuits)
    if (outs.contains(waterRole(w.id))) in.outputs[w.id] = outs[waterRole(w.id)];
  return in;
}

Result Robot::putWatering(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.contains("reservePct") || !j["reservePct"].is_number()) return Result::fail(422, say("api.bad_input"));
  Config next = cfg_;
  next.watering.reservePct = j["reservePct"].get<double>();
  return saveConfig(next, changed("cfg.what.watering"));
}

Result Robot::putCircuit(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.is_object()) return Result::fail(422, say("api.bad_input"));
  json body = j;
  std::string id = jstr(body, "id");
  if (id.empty()) {
    // A new circuit: the next free number
    for (int n = 1; id.empty(); ++n)
      if (!cfg_.watering.circuit("w" + std::to_string(n))) id = "w" + std::to_string(n);
    body["id"] = id;
  } else if (const WaterCircuitCfg* was = cfg_.watering.circuit(id)) {
    // A schedule paused stays so, and a plant followed, unless the change says otherwise
    if (!body.contains("paused")) body["paused"] = was->paused;
    if (!body.contains("plant")) body["plant"] = was->plant;
  } else {
    return Result::fail(404, say("water.none"));
  }
  WaterCircuitCfg w;
  try {
    w = waterCircuitFromJson(body);
  } catch (...) {
    return Result::fail(422, say("api.bad_input"));
  }
  Config next = cfg_;
  bool found = false;
  for (auto& x : next.watering.circuits)
    if (x.id == id) {
      x = w;
      found = true;
    }
  if (!found) next.watering.circuits.push_back(w);
  Result r = saveConfig(next, changed("cfg.what.watering"));
  if (r.status != 200) return r;
  // A shot of this circuit ends, and what waited for it goes: what it runs by has changed
  Ctx c = ctx();
  if (water_.stop(c, outputs_, id, say("water.end.changed"))) saveState(true);
  water_.forget(id);
  r.body["id"] = id;
  return r;
}

Result Robot::deleteCircuit(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!cfg_.watering.circuit(id)) return Result::fail(404, say("water.none"));
  Config next = cfg_;
  next.watering.circuits.erase(std::remove_if(next.watering.circuits.begin(), next.watering.circuits.end(),
                                              [&](const WaterCircuitCfg& w) { return w.id == id; }),
                               next.watering.circuits.end());
  next.roles.erase(waterRole(id));
  const Config before = cfg_;
  Result r = saveConfig(next, changed("cfg.what.watering"));
  if (r.status != 200) return r;
  // Its shot ends, booked as the circuit was when it began
  Ctx c = ctx();
  water_.stop(c, outputs_, id, say("water.end.removed"));
  water_.forget(id);
  Watering::forgetRuntime(rt_, id);
  saveState(true);
  reconcileDevices(before);  // its output is let go and switched off
  return r;
}

Result Robot::waterNow(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.contains("seconds") || !j["seconds"].is_number()) return Result::fail(422, say("api.bad_input"));
  if (j.contains("override") && !j["override"].is_boolean()) return Result::fail(422, say("api.bad_input"));
  Ctx c = ctx();
  if (auto why = water_.waterNow(c, waterIn(c), id, j["seconds"].get<double>(), jbool(j, "override", false), outputs_))
    return Result::fail(why->key == "water.none" ? 404 : why->key == "water.no.length" ? 422 : 409, *why);
  saveState(true);
  saveEvents();
  return Result::ok();
}

Result Robot::stopWater(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Ctx c = ctx();
  if (!water_.stop(c, outputs_, id, say("water.end.person"))) return Result::fail(409, say("water.none_running"));
  saveState(true);
  saveEvents();
  return Result::ok();
}

Result Robot::pauseCircuit(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.contains("paused") || !j["paused"].is_boolean()) return Result::fail(422, say("api.bad_input"));
  if (!cfg_.watering.circuit(id)) return Result::fail(404, say("water.none"));
  Config next = cfg_;
  for (auto& w : next.watering.circuits)
    if (w.id == id) w.paused = j["paused"].get<bool>();
  Result r = saveConfig(next, changed(j["paused"].get<bool>() ? "cfg.what.water_paused" : "cfg.what.water_resumed"));
  if (r.status == 200) water_.forget(id);  // resumed, it starts with its next shot
  return r;
}

int Robot::secondOfDay(Epoch epoch) const {
  // Zones are whole minutes off UTC: the seconds are UTC's
  return localTime(epoch, cfg_.system.timezone).minuteOfDay * 60 + static_cast<int>(((epoch % 60) + 60) % 60);
}

Result Robot::putLight(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.is_object()) return Result::fail(422, say("api.bad_input"));
  json body = j;
  std::string id = jstr(body, "id");
  if (id.empty()) {
    // A new circuit: the next free number
    for (int n = 1; id.empty(); ++n)
      if (!cfg_.lighting.circuit("l" + std::to_string(n))) id = "l" + std::to_string(n);
    body["id"] = id;
  } else if (const LightCircuitCfg* was = cfg_.lighting.circuit(id)) {
    // A plant followed stays so, unless the change says otherwise
    if (!body.contains("plant")) body["plant"] = was->plant;
  } else {
    return Result::fail(404, say("light.none"));
  }
  LightCircuitCfg lc;
  try {
    lc = lightCircuitFromJson(body);
  } catch (...) {
    return Result::fail(422, say("api.bad_input"));
  }
  Config next = cfg_;
  bool found = false;
  for (auto& x : next.lighting.circuits)
    if (x.id == id) {
      x = lc;
      found = true;
    }
  if (!found) next.lighting.circuits.push_back(lc);
  const Config before = cfg_;
  Result r = saveConfig(next, changed("cfg.what.lighting"));
  if (r.status != 200) return r;
  reconcileDevices(before);  // a switch made a dimmer, or back: its output is let go as it was
  r.body["id"] = id;
  return r;
}

Result Robot::deleteLight(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!cfg_.lighting.circuit(id)) return Result::fail(404, say("light.none"));
  Config next = cfg_;
  next.lighting.circuits.erase(std::remove_if(next.lighting.circuits.begin(), next.lighting.circuits.end(),
                                              [&](const LightCircuitCfg& lc) { return lc.id == id; }),
                               next.lighting.circuits.end());
  next.roles.erase(lightRole(id));
  // A room it lay in goes on without it (SD-136)
  for (auto& room : next.climate.rooms) room.lights.erase(std::remove(room.lights.begin(), room.lights.end(), id), room.lights.end());
  const Config before = cfg_;
  Result r = saveConfig(next, changed("cfg.what.lighting"));
  if (r.status != 200) return r;
  Lighting::forget(rt_, id);
  saveState(true);
  reconcileDevices(before);  // its output is let go and switched off
  return r;
}

Result Robot::putRoom(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.is_object()) return Result::fail(422, say("api.bad_input"));
  json body = j;
  std::string id = jstr(body, "id");
  if (id.empty()) {
    // A new room: the next free number
    for (int n = 1; id.empty(); ++n)
      if (!cfg_.climate.room("r" + std::to_string(n))) id = "r" + std::to_string(n);
    body["id"] = id;
  } else if (const ClimateRoomCfg* was = cfg_.climate.room(id)) {
    // A plant followed stays so, unless the change says otherwise
    if (!body.contains("plant")) body["plant"] = was->plant;
  } else {
    return Result::fail(404, say("climate.none"));
  }
  ClimateRoomCfg room;
  try {
    room = climateRoomFromJson(body);
  } catch (...) {
    return Result::fail(422, say("api.bad_input"));
  }
  Config next = cfg_;
  bool found = false;
  for (auto& x : next.climate.rooms)
    if (x.id == id) {
      x = room;
      found = true;
    }
  if (!found) next.climate.rooms.push_back(room);
  Result r = saveConfig(next, changed("cfg.what.climate"));
  if (r.status == 200) r.body["id"] = id;
  return r;
}

Result Robot::deleteRoom(const std::string& id) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!cfg_.climate.room(id)) return Result::fail(404, say("climate.none"));
  Config next = cfg_;
  next.climate.rooms.erase(std::remove_if(next.climate.rooms.begin(), next.climate.rooms.end(), [&](const ClimateRoomCfg& r) { return r.id == id; }),
                           next.climate.rooms.end());
  for (const auto& kind : climateOutputs()) next.roles.erase(climateRole(id, kind));
  for (const auto& kind : climateSensors()) next.roles.erase(climateRole(id, kind));
  const Config before = cfg_;
  Result r = saveConfig(next, changed("cfg.what.climate"));
  if (r.status != 200) return r;
  climate_.forget(id);
  rt_.latches.erase("climate." + id + ".co2");  // its valve's lock goes with it
  saveState(true);
  reconcileDevices(before);  // its outputs are let go and switched off
  return r;
}

Result Robot::lightHand(const std::string& id, const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.is_object() || !j.contains("on") || !(j["on"].is_boolean() || j["on"].is_null())) return Result::fail(422, say("api.bad_input"));
  Ctx c = ctx();
  const std::optional<bool> on = j["on"].is_null() ? std::nullopt : std::optional<bool>(j["on"].get<bool>());
  if (!Lighting::hand(c, id, on, secondOfDay(c.epoch))) return Result::fail(404, say("light.none"));
  light_.tick(c, secondOfDay(c.epoch), outputs_, climate_.lightCaps());
  saveState(true);
  saveEvents();
  return Result::ok();
}

void Robot::fillEnded(bool opened) {
  // The check held while it filled: it starts anew, and mixes first where water may have gone in
  if (opened) afterFill();
  else restartCycle();
}

Result Robot::startRefill() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Ctx c = ctx();
  if (auto why = refill_.start(c, refillIn(c.now), outputs_)) return Result::fail(409, *why);
  saveState(true);
  saveEvents();
  return Result::ok();
}

Result Robot::stopRefill() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  Ctx c = ctx();
  const auto opened = refill_.stop(c, outputs_, say("refill.end.person"));
  if (!opened) return Result::fail(409, say("refill.none"));
  fillEnded(*opened);
  saveState(true);
  saveEvents();
  return Result::ok();
}

Result Robot::putRefill(const json& j) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  if (!j.contains("maxFillMin") || !j["maxFillMin"].is_number()) return Result::fail(422, say("api.bad_input"));
  if (refill_.filling()) return Result::fail(409, say("refill.no.filling"));  // its limit stays as it began
  Config next = cfg_;
  next.refill.maxFillMin = j["maxFillMin"].get<double>();
  return saveConfig(next, changed("cfg.what.refill"));
}

std::vector<NetRequest> Robot::takeDeviceRequests() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return devices_.take();
}

void Robot::deviceResponse(long long id, int status, const json& body) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const Ms now = clock_.nowMs();
  if (const auto a = devices_.response(cfg_, id, status, body, now)) outputs_.answer(cfg_, devices_, *a, now);
  if (devices_.takeLearned()) saveDeviceSecrets();  // a Shelly's own ID, from now on how it is known
}

std::vector<MqttMessage> Robot::mqttDiscoveryMessages() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return mqttDiscovery(deviceId_, info(), cfg_, mqtt_);
}

MqttMessage Robot::mqttStateMessage() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return mqttState(deviceId_, state(), cfg_);
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
  // Only a person who confirms the entered volume releases it; a new volume releases it too
  // (putTank). One that reported only was taken as confirmed when it was set (SD-133).
  if (id == "tank.volume" && holds(rt_, id)) {
    if (!jbool(body, "confirm", false)) return Result::fail(422, say("latch.volume_confirm"));
    confirmVolume(cfg_, rt_, it->second);
  }
  const json args = it->second.is_object() ? it->second : json::object();
  const bool held = holds(rt_, id);
  rt_.latches.erase(it);
  if (id == "hw.return") {
    if (held) act_.allowReturnPump(true);  // it runs by the level again; on and on once more, it latches again
    returnSince_ = -1;
  }
  if (!held) {
    // A report seen: it held nothing, so the check goes on undisturbed (SD-133)
    log_.add(clock_.epoch(), "unblock", "notice", say("ev.lock.cleared"), reportedEvent(jstr(args, "key", id), args));
    saveState(true);
    return Result::ok();
  }
  log_.add(clock_.epoch(), "unblock", "notice", say("ev.latch.released"), say("latch." + jstr(args, "key", id), args));
  restartCycle();
  saveState(true);
  return Result::ok();
}

Result Robot::stop(const Msg& who) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  // A Stop switches everything off actively, books what has run and holds until Resume (RAT-036);
  // light follows its plan, and a room's fans their rule (SD-135, SD-136)
  act_.stopAll();
  outputs_.allOff();
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
  if (minutes > 0) outputs_.want("circulation_pump", false);  // at once, not only at the next tick (SD-132)
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
