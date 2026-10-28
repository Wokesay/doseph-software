// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include "doseph/gateway.hpp"
#include "doseph/messages.hpp"
#include "fakes.hpp"

using namespace doseph;

namespace {

JournalRun run(const std::string& order, int n, const std::string& purpose = "ec") {
  JournalRun r;
  r.order = order;
  r.run = n;
  r.purpose = purpose;
  r.canister = "a";
  r.name = "Part A";
  r.channel = 2;
  r.ml = 12.5;
  r.ms = 15000;
  return r;
}

// A configuration whose owner confirmed where pump and canisters stand (SD-118)
Config placed() {
  Config c;
  c.setup["placement"] = {"done", 1};
  return c;
}

}  // namespace

TEST_CASE("Dosing journal: a run begun is on record before it ends; after a restart it is reported once as cut (SD-106, R6)") {
  MemoryStorage store;
  DoseJournal j(store);
  REQUIRE(j.begin(run("ec-1", 1)));
  j.end("ec-1", 1, 15000, 12.5);
  REQUIRE(j.begin(run("ec-1", 2)));
  // The restart: read anew
  DoseJournal after(store);
  after.load();
  REQUIRE(after.runs().size() == 2);
  CHECK(after.runs()[0].ended);
  CHECK_FALSE(after.runs()[1].ended);
  auto cut = after.closeOpen();
  REQUIRE(cut.size() == 1);
  CHECK(cut[0].run == 2);
  CHECK(cut[0].ml == doctest::Approx(12.5));
  CHECK(after.anyIntoTank());
  // A second restart reports nothing again, and still knows a dose is not mixed in
  DoseJournal again(store);
  again.load();
  CHECK(again.closeOpen().empty());
  CHECK(again.anyIntoTank());
  CHECK(again.runs()[1].cut);
  CHECK(std::isnan(again.runs()[1].mlDone));  // how much ran is not known (R5)
  again.clear();
  DoseJournal empty(store);
  empty.load();
  CHECK(empty.runs().empty());
  CHECK_FALSE(empty.anyIntoTank());
}

TEST_CASE("Dosing journal: a calibration or priming run goes into a cup, not into the tank (RAT-070)") {
  MemoryStorage store;
  DoseJournal j(store);
  j.begin(run("cal-1", 1, "calibration"));
  j.begin(run("prime-2", 1, "prime"));
  CHECK_FALSE(j.anyIntoTank());
}

TEST_CASE("Dosing journal: a damaged file counts as a dose that may be in, never as nothing (R5)") {
  MemoryStorage store;
  store.write("journal.json", "[{\"order\":\"ec-1\",\"run\":1,\"pur");
  DoseJournal j(store);
  j.load();
  CHECK(j.damaged());
  CHECK(j.anyIntoTank());
  store.write("journal.json", "[{\"order\":\"x\",\"purpose\":\"mystery\"}]");
  DoseJournal k(store);
  k.load();
  REQUIRE(k.runs().size() == 1);
  CHECK(k.runs()[0].intoTank());
}

TEST_CASE("Doser: a run whose journal record cannot be written does not start (SD-106)") {
  fake::FailingStorage store;
  fake::Hardware hw;
  Config cfg = placed();
  cfg.channels[2] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Doser doser;
  DoseOrder o;
  o.purpose = "ec";
  o.step.canister = "a";
  o.step.name = "Part A";
  o.step.channel = 2;
  o.step.flowMlPerMin = 50;
  o.step.runs = {6000};
  store.fail = true;
  Msg err;
  CHECK_FALSE(doser.start(c, act, o, err));
  CHECK(err.key == "store.failed");
  CHECK(hw.started == 0);
  store.fail = false;
  CHECK(doser.start(c, act, o, err));
  CHECK(hw.started == 1);
  REQUIRE(journal.runs().size() == 1);
  CHECK_FALSE(journal.runs()[0].ended);
}

TEST_CASE("Gateway: no dose without flow; one that loses its flow stops at once; one run at a time (SD-105, RAT-018)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg = placed();
  cfg.channels[2] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  Actuators act(hw);
  Msg err;
  c.flowOk = false;
  CHECK_FALSE(act.startRun(c, 2, 6000, "ec", "j1", err));
  CHECK(err.key == "act.no_flow");
  // A calibration into a cup needs no flow, an uncalibrated pump doses nothing (RAT-015)
  CHECK(act.startRun(c, 3, 6000, "calibration", "j2", err));
  act.clearRun(3);
  c.flowOk = true;
  CHECK_FALSE(act.startRun(c, 3, 6000, "ec", "j3", err));
  CHECK(err.key == "act.uncalibrated");
  CHECK_FALSE(act.startRun(c, 2, 500, "ec", "j4", err));  // under 1 s (RAT-050)
  CHECK(err.key == "act.too_short");
  CHECK_FALSE(act.startRun(c, 2, 61000, "ec", "j5", err));  // over 60 s (RAT-007)
  CHECK(err.key == "act.too_long");
  REQUIRE(act.startRun(c, 2, 6000, "ec", "j6", err));
  CHECK_FALSE(act.startRun(c, 2, 6000, "ec", "j7", err));  // one run at a time (RAT-018)
  CHECK(err.key == "act.busy");
  c.flowOk = false;
  act.enforce(c);
  CHECK(hw.runs[2].state == RunStatus::State::Failed);
  CHECK(log.query(0, std::numeric_limits<Epoch>::max(), "", 5).at(0).title.key == "ev.flow_cut");
  // A Stop refuses every run, also into a cup (RAT-036)
  act.clearRun(2);
  c.stopped = true;
  CHECK_FALSE(act.startRun(c, 3, 6000, "calibration", "j8", err));
  CHECK(err.key == "act.stopped");
}

TEST_CASE("Gateway: a channel without a pump head runs nothing; a calibration of another head doses nothing (SD-108)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg = placed();
  cfg.channels[2] = {50, 1, "M"};  // the head fitted is "S"
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Msg err;
  CHECK_FALSE(act.startRun(c, 2, 6000, "ec", "j1", err));
  CHECK(err.key == "act.head_changed");
  // Calibrating it again is the way back
  CHECK(act.startRun(c, 2, 6000, "calibration", "j2", err));
  act.clearRun(2);
  hw.prof.channels[1].head = "";
  for (const char* purpose : {"calibration", "prime"}) {
    CHECK_FALSE(act.startRun(c, 2, 6000, purpose, std::string("j3") + purpose, err));
    CHECK(err.key == "act.no_head");
  }
  CHECK(hw.started == 1);
}

TEST_CASE("Gateway: a run into the tank needs drops; none within the wait cuts it and locks the channel (SD-111)") {
  MemoryStorage store;
  fake::Hardware hw;
  hw.dropsSeen[2] = {0, -1};
  Config cfg = placed();
  cfg.channels[2] = {50, 1, "S"};  // a drop every 0.06 s: the wait is its shortest, 5 s
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Msg err;
  REQUIRE(act.startRun(c, 2, 20000, "ec", "j1", err));
  // Drops keep it going; a gap as long as the wait cuts it
  c.now = 3000;
  hw.dropsSeen[2].count = 4;
  act.enforce(c);
  c.now = 8000;
  act.enforce(c);
  CHECK(hw.runs[2].state == RunStatus::State::Running);
  c.now = 8100;
  act.enforce(c);
  CHECK(hw.runs[2].state == RunStatus::State::Failed);
  CHECK(rt.latches.count("channel.2.no_drops"));
  CHECK(log.query(0, std::numeric_limits<Epoch>::max(), "alarm", 5).at(0).title.key == "ev.channel.no_drops");
  act.clearRun(2);
  // Locked: no dose, but a calibration into a cup still runs
  CHECK_FALSE(act.startRun(c, 2, 6000, "ec", "j2", err));
  CHECK(err.key == "act.channel_locked");
  CHECK(act.startRun(c, 2, 6000, "calibration", "j3", err));
  act.clearRun(2);
  rt.latches.clear();
  // A run shorter than the wait is judged at its end plus the wait
  REQUIRE(act.startRun(c, 2, 2000, "ec", "j4", err));
  hw.runs[2].state = RunStatus::State::Done;
  act.clearRun(2);
  c.now = 10100;
  act.enforce(c);
  c.now = 15000;
  act.enforce(c);
  CHECK(rt.latches.empty());
  c.now = 15200;
  act.enforce(c);
  CHECK(rt.latches.count("channel.2.no_drops"));
  // A Stop: what has not dripped by then is not judged
  rt.latches.clear();
  REQUIRE(act.startRun(c, 2, 2000, "ec", "j5", err));
  act.stopAll();
  act.clearRun(2);
  c.now = 30000;
  act.enforce(c);
  CHECK(rt.latches.empty());
}

TEST_CASE("Gateway: drops while the pump stands, past its own run, lock the channel; the check mixes first (SD-111, PD-455)") {
  MemoryStorage store;
  fake::Hardware hw;
  hw.dropsSeen[3] = {0, -1};
  Config cfg = placed();
  cfg.channels[3] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Msg err;
  act.enforce(c);
  REQUIRE(act.startRun(c, 3, 3000, "ec", "j1", err));
  c.now = 1000;
  hw.dropsSeen[3].count = 20;
  act.enforce(c);
  c.now = 3000;
  act.enforce(c);
  hw.runs[3].state = RunStatus::State::Done;
  act.clearRun(3);
  c.now = 3500;
  act.enforce(c);  // it dripped: its watch is done
  // What still drips in the 10 s after the run is its own
  c.now = 9000;
  hw.dropsSeen[3].count = 21;
  act.enforce(c);
  CHECK(rt.latches.empty());
  CHECK_FALSE(act.takeUnplannedDrops());
  c.now = 20000;
  hw.dropsSeen[3].count = 22;
  act.enforce(c);
  CHECK(rt.latches.count("channel.3.siphon"));
  CHECK(log.query(0, std::numeric_limits<Epoch>::max(), "alarm", 5).at(0).title.key == "ev.channel.siphon");
  CHECK(act.takeUnplannedDrops());
  CHECK_FALSE(act.takeUnplannedDrops());  // once
  // Locked, it raises no second alarm while it goes on dripping
  c.now = 30000;
  hw.dropsSeen[3].count = 25;
  act.enforce(c);
  CHECK(log.query(0, std::numeric_limits<Epoch>::max(), "alarm", 5).size() == 1);
  CHECK_FALSE(act.startRun(c, 3, 3000, "ph", "j2", err));
  CHECK(err.key == "act.channel_locked");
}

TEST_CASE("Doser: after a dose the line is pulled back; the channel's next run fills it first, unbooked (SD-111)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg = placed();
  cfg.channels[2] = {60, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Doser doser;
  Msg err;
  auto done = [&] {
    hw.runs[2].state = RunStatus::State::Done;
    hw.runs[2].actualMs = hw.runs[2].requestedMs;
    c.now += hw.runs[2].requestedMs + 100;
    doser.tick(c, act);
  };
  DoseOrder o;
  o.purpose = "ec";
  o.step.canister = "a";
  o.step.name = "Part A";
  o.step.channel = 2;
  o.step.flowMlPerMin = 60;
  o.step.runs = {3000};
  REQUIRE(doser.start(c, act, o, err));
  done();
  // 0.2 ml backwards at 60 ml/min: 200 ms; the order ends after it
  CHECK(hw.backwards);
  CHECK(hw.runs[2].requestedMs == 200);
  CHECK_FALSE(doser.takeFinished());
  done();
  auto f = doser.takeFinished();
  REQUIRE(f);
  CHECK(f->state == DoseProgress::State::Done);
  CHECK(f->mlDone == doctest::Approx(3.0));
  CHECK(rt.pulledBackMs.at(2) == 200);
  // The next dose: first the line, 200 ms forwards, not booked and not in the journal
  REQUIRE(doser.start(c, act, o, err));
  CHECK_FALSE(hw.backwards);
  CHECK(hw.runs[2].requestedMs == 200);
  const size_t journaled = journal.runs().size();
  done();
  CHECK(rt.pulledBackMs.count(2) == 0);
  CHECK(hw.runs[2].requestedMs == 3000);  // the dose follows at once
  CHECK(doser.progress().mlDone == 0);
  CHECK(journal.runs().size() == journaled + 1);
  done();
  done();
  f = doser.takeFinished();
  REQUIRE(f);
  CHECK(f->mlDone == doctest::Approx(3.0));
  // A dose that would be refused is refused before any refill: the line stays pulled back
  c.flowOk = false;
  CHECK_FALSE(doser.start(c, act, o, err));
  CHECK(err.key == "act.no_flow");
  CHECK(rt.pulledBackMs.at(2) == 200);
  // A pump that cannot run backwards pulls nothing back
  c.flowOk = true;
  rt.pulledBackMs.clear();
  hw.prof.channels[1].reverse = false;
  REQUIRE(doser.start(c, act, o, err));
  done();
  CHECK(doser.takeFinished());
  CHECK(rt.pulledBackMs.empty());
}

TEST_CASE("Doser: a Stop during the pull back keeps what it pulled back and the dose complete (SD-111)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg = placed();
  cfg.channels[2] = {60, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Doser doser;
  Msg err;
  DoseOrder o;
  o.purpose = "ph";
  o.step.canister = "down";
  o.step.name = "pH−";
  o.step.channel = 2;
  o.step.flowMlPerMin = 60;
  o.step.runs = {2000};
  REQUIRE(doser.start(c, act, o, err));
  hw.runs[2].state = RunStatus::State::Done;
  hw.runs[2].actualMs = 2000;
  c.now = 2100;
  doser.tick(c, act);
  REQUIRE(hw.backwards);
  hw.runs[2].actualMs = 80;
  doser.abort(c, act, say("abort.interrupted"));
  auto f = doser.takeFinished();
  REQUIRE(f);
  CHECK(f->state == DoseProgress::State::Done);
  CHECK(rt.pulledBackMs.at(2) == 80);
}

TEST_CASE("Gateway: a channel's run time into the tank per rolling 24 h stays within its head's value, never over 60 min; the user's ml per day below it (SD-113)") {
  MemoryStorage store;
  fake::Hardware hw;
  hw.prof.channels[1].dailyMaxMin = 2;
  Config cfg = placed();
  cfg.channels[2] = {60, 1, "S"};
  cfg.canisters = {{"a", "Part A", "nutrient", 2, "", "#00c"}};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  c.epoch = 100000;
  Actuators act(hw);
  Msg err;
  auto run = [&](Ms ms, const char* purpose, const std::string& id) {
    const bool ok = act.startRun(c, 2, ms, purpose, id, err);
    if (ok) {
      hw.runs[2].state = RunStatus::State::Done;
      act.clearRun(2);
    }
    return ok;
  };
  CHECK(run(60000, "ec", "j1"));
  CHECK(run(60000, "ph", "j2"));
  CHECK_FALSE(run(1000, "ec", "j3"));
  CHECK(err.key == "act.daily_cap");
  CHECK(run(30000, "calibration", "j4"));  // into a cup: not counted
  // A line fill drips into the tank: counted (SD-111)
  CHECK_FALSE(run(1000, "fill", "j5"));
  // A day later the runs have rolled out
  c.epoch += 24 * 3600 + 1;
  CHECK(run(60000, "ec", "j6"));
  CHECK(rt.dayRuns.at(2).size() == 1);  // the old ones are dropped
  // A head's value above the code's ceiling, or none: 60 min
  hw.prof.channels[1].dailyMaxMin = 600;
  CHECK(act.dailyCapMin(2) == 60);
  hw.prof.channels[1].dailyMaxMin = kNaN;
  CHECK(act.dailyCapMin(2) == 60);
  // The user's 70 ml per day at 60 ml/min: 60 ml used, 10 s more
  cfg.canisters[0].maxMlPerDay = 70;
  CHECK(run(10000, "ec", "j7"));
  CHECK_FALSE(run(1000, "ec", "j8"));
  CHECK(err.key == "act.daily_ml");
}

TEST_CASE("Dosing journal: a run recorded before the terms changed still names its canister (SD-107)") {
  MemoryStorage store;
  json old = json::array({{{"order", "ec-1"}, {"run", 1}, {"purpose", "ec"}, {"bottle", "a"}, {"name", "Part A"}, {"pump", 2}, {"ml", 12.5}, {"ms", 15000}, {"at", 0}, {"ended", false}}});
  store.write("journal.json", old.dump());
  DoseJournal j(store);
  j.load();
  REQUIRE(j.runs().size() == 1);
  CHECK(j.runs()[0].canister == "a");
  CHECK(j.runs()[0].channel == 2);  // "pump" before SD-107
  CHECK(j.closeOpen().size() == 1);
}

TEST_CASE("Gateway: nothing runs towards the tank before the owner confirmed the placement; into a cup it may (SD-118)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg;
  cfg.channels[2] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  Msg err;
  for (const char* purpose : {"ec", "ph", "fill", "refill"}) {
    CAPTURE(purpose);
    CHECK_FALSE(act.startRun(c, 2, 6000, purpose, "j1", err));
    CHECK(err.key == "act.placement");
  }
  CHECK(act.startRun(c, 2, 6000, "calibration", "j2", err));
  act.clearRun(2);
  CHECK(act.startRun(c, 2, 6000, "prime", "j3", err));
  act.clearRun(2);
  cfg.setup["placement"] = {"done", 1};
  CHECK(act.startRun(c, 2, 6000, "ec", "j4", err));
}
