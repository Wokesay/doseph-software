// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include "doseph/gateway.hpp"
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
  Config cfg;
  cfg.channels[2] = {50, 1};
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
  Config cfg;
  cfg.channels[2] = {50, 1};
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
