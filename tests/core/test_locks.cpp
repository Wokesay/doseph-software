// SPDX-License-Identifier: AGPL-3.0-or-later
// Locks a person releases may be set to report only (SD-133): the alarm comes as
// before, and the robot goes on; the frame of PD-459 never changes.
#include <doctest/doctest.h>

#include <functional>
#include <limits>
#include <string>

#include "doseph/alarms.hpp"
#include "doseph/api.hpp"
#include "doseph/gateway.hpp"
#include "doseph/messages.hpp"
#include "fakes.hpp"
#include "harness.hpp"
#include "simulation.hpp"

using namespace doseph;

namespace {

sim::Options demo() {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  return o;
}

json st(sim::Simulation& s) { return s.robot().state(); }

bool runUntil(sim::Simulation& s, const std::function<bool()>& done, double maxS) {
  for (double t = 0; t < maxS; ++t) {
    if (done()) return true;
    s.step(kSecond);
  }
  return done();
}

// The newest event with this title, or null
json event(sim::Simulation& s, const std::string& key) {
  for (const auto& e : s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 1000))
    if (e["title"]["key"] == key) return e;
  return nullptr;
}

int events(sim::Simulation& s, const std::string& key) {
  int n = 0;
  for (const auto& e : s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 1000)) n += e["title"]["key"] == key;
  return n;
}

const json* item(const json& watchdog, const std::string& id) {
  for (const auto& i : watchdog["items"])
    if (i["id"] == id) return &i;
  return nullptr;
}

Config placed() {
  Config c;
  c.setup["placement"] = {"done", 1};
  return c;
}

}  // namespace

TEST_CASE("Locks: only a lock a person releases may report only, each once; leak, high level and the cable check never; the setting is kept with the configuration (SD-133)") {
  Config c = placed();
  CHECK(validateConfig(c, 4).empty());
  c.locks.reportOnly = reportableLocks();
  CHECK(validateConfig(c, 4).empty());
  for (const char* always : {"hw.leak", "hw.high_level", "pump.cable"}) {
    Config bad = placed();
    bad.locks.reportOnly = {always};
    const auto errs = validateConfig(bad, 4);
    REQUIRE(errs.size() == 1);
    CHECK(errs[0].key == "cfg.locks.always");
    CHECK_FALSE(bad.reportsOnly(always));  // whatever a configuration says
  }
  for (const char* unknown : {"stop", "flow", "channel.2.no_drops"}) {
    Config bad = placed();
    bad.locks.reportOnly = {unknown};
    const auto errs = validateConfig(bad, 4);
    REQUIRE(errs.size() == 1);
    CHECK(errs[0].key == "cfg.locks.unknown");
  }
  Config twice = placed();
  twice.locks.reportOnly = {"flow.dry", "flow.dry"};
  const auto errs = validateConfig(twice, 4);
  REQUIRE(errs.size() == 1);
  CHECK(errs[0].key == "cfg.locks.twice");
  // Kept with the configuration, so a backup carries it
  c.locks.reportOnly = {"ec.no_effect", "hw.return"};
  const Config back = configFromJson(json(c));
  CHECK(back.locks.reportOnly == c.locks.reportOnly);
  CHECK(back.reportsOnly("hw.return"));
  CHECK_FALSE(back.reportsOnly("flow.dry"));
  CHECK(configFromJson(json::object()).locks.reportOnly.empty());  // the default: every lock locks
}

TEST_CASE("Locks: only a session or an app token may set them, never an account token, MQTT or the coupled device; each change is an event (SD-133)") {
  CHECK(permitted(Role::Owner, "PUT", {"locks"}));
  CHECK(permitted(Role::App, "PUT", {"locks"}));
  CHECK_FALSE(permitted(Role::Account, "PUT", {"locks"}));  // MQTT commands are account calls (SD-130)
  CHECK_FALSE(permitted(Role::Peer, "PUT", {"locks"}));

  sim::Simulation s(demo());
  CHECK(s.robot().putLocks({{"reportOnly", {"stop"}}}).status == 422);
  CHECK(s.robot().putLocks({{"reportOnly", "flow.dry"}}).status == 422);
  CHECK(s.robot().putLocks({{"reportOnly", {"hw.leak"}}}).status == 422);
  REQUIRE(s.robot().putLocks({{"reportOnly", {"flow.dry", "channel.no_drops"}}}).status == 200);
  const json set = event(s, "ev.locks.report_only");
  REQUIRE(set.is_object());
  CHECK(set["severity"] == "warn");
  s.step(6 * kSecond);  // the watchdog looks every 5 s
  // A note, not a problem: no alarm, and the problem flag stays free for real faults
  const json wd = st(s)["watchdog"];
  const json* note = item(wd, "locks.report_only");
  REQUIRE(note);
  CHECK((*note)["status"] == "neutral");
  CHECK(st(s)["alarms"].dump().find("locks.report_only") == std::string::npos);
  REQUIRE(s.robot().putLocks({{"reportOnly", json::array()}}).status == 200);
  CHECK(event(s, "ev.locks.lock").is_object());
  s.step(6 * kSecond);
  const json after = st(s)["watchdog"];
  CHECK(item(after, "locks.report_only") == nullptr);
}

TEST_CASE("Locks: a feed pump without flow set to report only alarms as before and tries again; clearing the report leaves the check alone (SD-133, SD-127)") {
  sim::Simulation s(demo());
  REQUIRE(s.robot().putLocks({{"reportOnly", {"flow.dry"}}}).status == 200);
  s.world().flowFault = "hose";
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("flow.dry"); }, 7 * 60));
  const json l = st(s)["latches"]["flow.dry"];
  CHECK(l["reportOnly"] == true);
  CHECK(l["tries"] == 2);
  const json ev = event(s, "ev.lock.reported");
  REQUIRE(ev.is_object());
  CHECK(ev["severity"] == "alarm");
  CHECK(ev["title"]["args"]["lock"]["key"] == "lock.flow.dry");
  CHECK_FALSE(event(s, "ev.flow.dry").is_object());
  // Shown as reported, with the level the lock has (SD-123); and a warning that a lock only reports
  s.step(6 * kSecond);
  const json w = st(s)["watchdog"];
  const json* rep = item(w, "latch.flow.dry");
  REQUIRE(rep);
  CHECK((*rep)["status"] == "problem");
  CHECK((*rep)["label"]["key"] == "watch.reported");
  CHECK(levelOf("latch.flow.dry") == Level::Critical);
  const json* note = item(w, "locks.report_only");
  REQUIRE(note);
  CHECK((*note)["status"] == "neutral");
  // It goes on: no wait for a person; the next tries come as before, and while the report stands
  // they are not reported again
  CHECK(st(s)["cycle"]["line"]["key"] != "cycle.wait.dry");
  const Epoch first = st(s)["latches"]["flow.dry"]["at"];
  REQUIRE(runUntil(s, [&] { return st(s)["latches"]["flow.dry"]["at"] != first; }, 12 * 60));
  CHECK(events(s, "ev.lock.reported") == 1);
  s.world().flowFault.clear();
  REQUIRE(runUntil(s, [&] { return st(s)["cycle"]["phase"] == "flush"; }, 6 * 60));
  // Seen: cleared without a restart of the check
  REQUIRE(s.robot().ackLatch("flow.dry", json::object()).status == 200);
  CHECK(st(s)["cycle"]["phase"] == "flush");
  CHECK(event(s, "ev.lock.cleared").is_object());
  CHECK_FALSE(event(s, "ev.latch.released").is_object());
}

TEST_CASE("Locks: an outlet without drops set to report only lets the run go to its end, and the channel doses on; drops without a pump likewise (SD-133, SD-111)") {
  MemoryStorage store;
  fake::Hardware hw;
  hw.dropsSeen[2] = {0, -1};
  Config cfg = placed();
  cfg.locks.reportOnly = {"channel.no_drops", "channel.siphon"};
  cfg.channels[2] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Msg err;
  REQUIRE(act.startRun(c, 2, 20000, "ec", "j1", err));
  c.now = 8100;
  act.enforce(c);
  CHECK(hw.runs[2].state == RunStatus::State::Running);  // not cut
  REQUIRE(rt.latches.count("channel.2.no_drops"));
  CHECK(rt.latches["channel.2.no_drops"]["reportOnly"] == true);
  CHECK_FALSE(holds(rt, "channel.2.no_drops"));
  CHECK_FALSE(channelLocked(rt, 2));
  const auto alarms = log.query(0, std::numeric_limits<Epoch>::max(), "alarm", 5);
  REQUIRE_FALSE(alarms.empty());
  CHECK(alarms.at(0).title.key == "ev.lock.reported_channel");
  hw.runs[2].state = RunStatus::State::Done;
  act.clearRun(2);
  CHECK(act.startRun(c, 2, 6000, "ec", "j2", err));  // it doses on
  act.clearRun(2);
  // Drops while the pump stands: reported, not locked
  c.now = 60000;
  hw.dropsSeen[2].count = 9;
  act.enforce(c);
  c.now = 61000;
  hw.dropsSeen[2].count = 12;
  act.enforce(c);
  REQUIRE(rt.latches.count("channel.2.siphon"));
  CHECK_FALSE(holds(rt, "channel.2.siphon"));
  CHECK_FALSE(channelLocked(rt, 2));
  // Set back to lock while the report stands: the next siphoning locks the channel
  cfg.locks.reportOnly.clear();
  c.now = 75000;
  hw.dropsSeen[2].count = 15;
  act.enforce(c);
  CHECK(holds(rt, "channel.2.siphon"));
  CHECK(channelLocked(rt, 2));
}

TEST_CASE("Locks: with every lock set to report only the frame holds: no dose without flow, one channel at a time, the run time and the daily amount (SD-133, PD-459)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg = placed();
  cfg.locks.reportOnly = reportableLocks();
  cfg.channels[1] = {50, 1, "S"};
  cfg.channels[2] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  Actuators act(hw);
  Msg err;
  CHECK_FALSE(act.startRun(c, 1, 5000, "ec", "j1", err));  // no flow proven
  CHECK(err.key == "act.no_flow");
  c.flowOk = true;
  CHECK_FALSE(act.startRun(c, 1, static_cast<Ms>(kHardMaxRunS * 1000) + 1000, "ec", "j2", err));  // longer than a dose may run
  REQUIRE(act.startRun(c, 1, 5000, "ec", "j3", err));
  CHECK_FALSE(act.startRun(c, 2, 5000, "ec", "j4", err));  // one channel at a time
  hw.runs[1].state = RunStatus::State::Done;
  act.clearRun(1);
  // The daily amount: a channel that ran its cap of the last 24 h doses no more
  rt.dayRuns[2] = {{c.epoch, static_cast<Ms>(kDailyMaxMin * 60 * 1000)}};
  CHECK_FALSE(act.startRun(c, 2, 5000, "ec", "j5", err));
}

TEST_CASE("Locks: a tank volume set to report only counts as confirmed: what the test dose showed is learned, and the check goes on; seen, it clears without a confirmation (SD-133, SD-105)") {
  sim::Simulation s(demo());
  REQUIRE(s.robot().putLocks({{"reportOnly", {"tank.volume"}}}).status == 200);
  s.robot().putTank({{"volumeL", 40}});  // the twin holds 100 L
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("tank.volume"); }, 3 * 3600));
  const json l = st(s)["latches"]["tank.volume"];
  CHECK(l["reportOnly"] == true);
  CHECK(l["enteredL"] == 40);
  CHECK(st(s)["newTank"] == false);
  CHECK(st(s)["effects"]["ecByRecipe"].contains("two_part"));
  CHECK(st(s)["cycle"]["line"]["key"] != "cycle.wait.volume");
  CHECK(event(s, "ev.lock.reported").is_object());
  CHECK_FALSE(event(s, "ev.cycle.volume").is_object());
  CHECK_FALSE(event(s, "ev.cycle.test_ok").is_object());
  // It doses on
  const double ec = s.world().tank.ec + s.world().tank.pendingEc;
  REQUIRE(runUntil(s, [&] { return s.world().tank.ec + s.world().tank.pendingEc > ec + 0.05; }, 2 * 3600));
  CHECK(s.robot().ackLatch("tank.volume", json::object()).status == 200);  // seen: no confirmation needed
  CHECK_FALSE(st(s)["latches"].contains("tank.volume"));
}

TEST_CASE("Locks: a return pump set to report only is not held off and holds nothing; set back to lock while its report stands, the next minute without pause holds it (SD-133, SD-109)") {
  sim::Simulation s(harness::device("pro"));
  REQUIRE(s.robot().putLocks({{"reportOnly", {"hw.return"}}}).status == 200);
  REQUIRE(runUntil(s, [&] { return s.world().flowing(); }, 60));
  s.world().returnFault = "failed";  // it runs and moves nothing
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("hw.return"); }, 3 * 60));
  CHECK(st(s)["latches"]["hw.return"]["reportOnly"] == true);
  CHECK(event(s, "ev.lock.reported").is_object());
  CHECK_FALSE(event(s, "ev.hw.return").is_object());
  s.step(2 * kSecond);
  CHECK_FALSE(s.world().returnLocked);
  CHECK_FALSE(holds(s.robot().runtime(), "hw.return"));  // what the hold of every pump asks
  REQUIRE(s.robot().putLocks({{"reportOnly", json::array()}}).status == 200);
  REQUIRE(runUntil(s, [&] { return holds(s.robot().runtime(), "hw.return"); }, 2 * 60));
  CHECK(event(s, "ev.hw.return").is_object());
  s.step(2 * kSecond);
  CHECK(s.world().returnLocked);
}

TEST_CASE("Locks: a lock set to report only after it latched still holds until a person releases it; set back to lock, the next one holds (SD-133)") {
  Config cfg = placed();
  RuntimeState rt;
  CHECK(setLatch(cfg, rt, "ec.no_effect", {{"at", 1}}));
  cfg.locks.reportOnly = {"ec.no_effect"};
  CHECK(holds(rt, "ec.no_effect"));  // its reason was frozen when it was set (RAT-062)
  rt.latches.clear();
  CHECK_FALSE(setLatch(cfg, rt, "ec.no_effect", {{"at", 2}}));
  CHECK_FALSE(holds(rt, "ec.no_effect"));
  cfg.locks.reportOnly.clear();
  CHECK(setLatch(cfg, rt, "ec.no_effect", {{"at", 3}}));
  CHECK(holds(rt, "ec.no_effect"));
  // Leak and high level never report only, even where a configuration said so
  cfg.locks.reportOnly = {"hw.leak", "hw.high_level"};
  CHECK(setLatch(cfg, rt, "hw.leak", {{"at", 4}}));
  CHECK(setLatch(cfg, rt, "hw.high_level", {{"at", 4}}));
  CHECK(holds(rt, "hw.leak"));
  CHECK(holds(rt, "hw.high_level"));
}
