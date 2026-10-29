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

TEST_CASE("Locks: only a lock a person releases may report only, each once; leak and high level never; the setting is kept with the configuration (SD-133)") {
  Config c = placed();
  CHECK(validateConfig(c, 4).empty());
  c.locks.reportOnly = reportableLocks();
  CHECK(validateConfig(c, 4).empty());
  for (const char* never : {"hw.leak", "hw.high_level", "stop", "flow", "channel.2.no_drops"}) {
    Config bad = placed();
    bad.locks.reportOnly = {never};
    const auto errs = validateConfig(bad, 4);
    REQUIRE(errs.size() == 1);
    CHECK(errs[0].key == "cfg.locks.unknown");
  }
  Config twice = placed();
  twice.locks.reportOnly = {"flow.dry", "flow.dry"};
  CHECK(validateConfig(twice, 4).size() == 1);
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
  CHECK(item(st(s)["watchdog"], "locks.report_only") != nullptr);
  REQUIRE(s.robot().putLocks({{"reportOnly", json::array()}}).status == 200);
  CHECK(event(s, "ev.locks.lock").is_object());
  s.step(6 * kSecond);
  CHECK(item(st(s)["watchdog"], "locks.report_only") == nullptr);
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
  const json* warn = item(w, "locks.report_only");
  REQUIRE(warn);
  CHECK((*warn)["status"] == "problem");
  CHECK(levelOf("locks.report_only") == Level::Warning);
  // It goes on: no wait for a person; the next try comes as before
  CHECK(st(s)["cycle"]["line"]["key"] != "cycle.wait.dry");
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
