// SPDX-License-Identifier: AGPL-3.0-or-later
// Checks of a pump's rate in use (SD-145): the consumption check from levels read, and the drops
// per ml its doses give after a calibration.
#include <doctest/doctest.h>

#include <limits>

#include "doseph/gateway.hpp"
#include "doseph/ratecheck.hpp"
#include "doseph/service.hpp"
#include "fakes.hpp"
#include "harness.hpp"

using namespace doseph;

namespace {

Config oneCanister() {
  Config c;
  c.setup["placement"] = {"done", 1};
  CanisterCfg b;
  b.id = "a";
  b.name = "Part A";
  b.channel = 2;
  c.canisters.push_back(b);
  c.channels[2] = {10.0, 1000, "S"};
  return c;
}

}  // namespace

TEST_CASE("Consumption check: a level read is compared with what the doses counted since the last reading; off by more than 5 % a rate is proposed, by more than 30 % the pump is to be calibrated (SD-145)") {
  const Config cfg = oneCanister();
  const CanisterCfg& b = cfg.canisters[0];
  RuntimeState rt;
  // The first reading only starts the comparison
  CHECK(checkUse(cfg, rt, b, 1000, 2000).is_null());
  REQUIRE(rt.stockRead.contains("a"));
  CHECK(jnum(rt.stockRead["a"], "calAt") == 1000);
  // The doses counted 300 ml; 330 ml went: 10 % more, so the pump delivers more than its rate
  rt.stockMl["a"] = 700;
  json c = checkUse(cfg, rt, b, 670, 3000);
  REQUIRE(c.is_object());
  CHECK(jnum(c, "countedMl") == doctest::Approx(300));
  CHECK(jnum(c, "usedMl") == doctest::Approx(330));
  CHECK(jnum(c, "share") == doctest::Approx(0.10));
  REQUIRE(c["proposal"].is_object());
  CHECK(jnum(c["proposal"], "to") == doctest::Approx(11.0));
  CHECK(jnum(c["proposal"], "from") == 10.0);
  CHECK(rt.rateProposal.contains("2"));
  CHECK(jnum(rt.stockRead["a"], "ml") == 670);  // the next comparison starts here
  // Within 5 %: no proposal, and an older one goes
  rt.stockMl["a"] = 370;
  c = checkUse(cfg, rt, b, 360, 4000);
  REQUIRE(c.is_object());
  CHECK(c["proposal"].is_null());
  CHECK_FALSE(rt.rateProposal.contains("2"));
  // Less than 250 ml counted: no comparison yet, but the reading starts the next one
  rt.stockMl["a"] = 200;
  CHECK(checkUse(cfg, rt, b, 150, 5000).is_null());
  // Off by more than 30 %: too far for a correction
  rt.stockMl["a"] = 1000;
  CHECK(checkUse(cfg, rt, b, 1000, 7000).is_null());  // refilled and read: nothing counted
  rt.stockMl["a"] = 700;
  c = checkUse(cfg, rt, b, 550, 8000);  // 300 counted, 450 went
  REQUIRE(c.is_object());
  CHECK(c["calibrate"] == true);
  CHECK(c["proposal"].is_null());
  CHECK_FALSE(rt.rateProposal.contains("2"));
}

TEST_CASE("Consumption check: only along one calibration of the same channel; a new calibration or another channel drops the reading and the proposal (SD-145)") {
  Config cfg = oneCanister();
  RuntimeState rt;
  checkUse(cfg, rt, cfg.canisters[0], 1000, 2000);
  rt.stockMl["a"] = 700;
  REQUIRE(checkUse(cfg, rt, cfg.canisters[0], 640, 3000)["proposal"].is_object());
  CHECK_FALSE(pruneRateChecks(cfg, rt));  // all still fits
  // Calibrated again: the proposal and the reading belong to the old rate
  cfg.channels[2].at = 3500;
  CHECK(pruneRateChecks(cfg, rt));
  CHECK(rt.rateProposal.empty());
  CHECK(rt.stockRead.empty());
  // Without a reading of this calibration, a level read starts anew
  rt.stockMl["a"] = 400;
  CHECK(checkUse(cfg, rt, cfg.canisters[0], 300, 4000).is_null());
  // Moved to another channel: what was read on the old one does not compare
  cfg.canisters[0].channel = 3;
  cfg.channels[3] = {12.0, 1000, "S"};
  CHECK(pruneRateChecks(cfg, rt));
  CHECK(rt.stockRead.empty());
  // A canister on a coupled device's channel: its rate is that device's
  cfg.canisters[0].device = "other";
  startUse(cfg, rt, cfg.canisters[0], 500, 5000);
  CHECK(rt.stockRead.empty());
  // Stored and read back, as numbers or not at all
  RuntimeState s;
  s.stockRead["a"] = {{"ml", 500}, {"at", 1}, {"channel", 2}, {"calAt", 1000}};
  s.rateProposal["2"] = {{"canister", "a"}, {"to", 11}};
  s.dropRate["2"] = {{"calAt", 1000}, {"perMl", 20}};
  const RuntimeState back = runtimeFromJson(json(s));
  CHECK(back.stockRead == s.stockRead);
  CHECK(back.rateProposal == s.rateProposal);
  CHECK(back.dropRate == s.dropRate);
  CHECK(runtimeFromJson(json{{"dropRate", {{"2", 5}, {"3", {{"calAt", 1}}}}}}).dropRate.size() == 1);
}

TEST_CASE("Drops per ml: learned from the first 10 ml of doses after a calibration, then watched over 10 ml at a time; a change beyond 20 % is said once (SD-145)") {
  RuntimeState rt;
  // Learned: 20 drops per ml
  CHECK_FALSE(watchDrops(rt, 2, 1000, 6, 120, 1));
  CHECK_FALSE(watchDrops(rt, 2, 1000, 4, 80, 2));
  CHECK(jnum(rt.dropRate["2"], "perMl") == doctest::Approx(20));
  // 15 % fewer: within
  CHECK_FALSE(watchDrops(rt, 2, 1000, 10, 170, 3));
  // 25 % fewer: the pump delivers less than its rate says
  const auto share = watchDrops(rt, 2, 1000, 10, 150, 4);
  REQUIRE(share);
  CHECK(*share == doctest::Approx(-0.25));
  CHECK(jnum(rt.dropRate["2"]["drift"], "at") == 4);
  CHECK_FALSE(watchDrops(rt, 2, 1000, 10, 100, 5));  // once
  // A new calibration learns afresh
  CHECK_FALSE(watchDrops(rt, 2, 2000, 10, 300, 6));
  CHECK(jnum(rt.dropRate["2"], "perMl") == doctest::Approx(30));
  CHECK_FALSE(rt.dropRate["2"].contains("drift"));
  CHECK(watchDrops(rt, 2, 2000, 10, 400, 7));  // 33 % more
  // A run of no ml says nothing
  CHECK_FALSE(watchDrops(rt, 3, 2000, 0, 5, 8));
  CHECK_FALSE(rt.dropRate.contains("3"));
}

TEST_CASE("Drops per ml: counted only for a run of a dose that began with a full line, after its refill or as a later run of the order (SD-145)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg = oneCanister();
  cfg.channels[2] = {60, 1000, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  hw.dropsSeen[2] = {};
  Actuators act(hw);
  Doser doser;
  Msg err;
  // Each run ends with its drops: 20 per ml
  auto done = [&](bool dose) {
    if (dose) hw.dropsSeen[2].count += static_cast<std::uint64_t>(hw.runs[2].requestedMs / 1000.0 * 20);
    hw.runs[2].state = RunStatus::State::Done;
    hw.runs[2].actualMs = hw.runs[2].requestedMs;
    c.now += hw.runs[2].requestedMs + 100;
    doser.tick(c, act);
    c.now += 3 * kSecond;
    doser.tick(c, act);
  };
  DoseOrder o;
  o.purpose = "ec";
  o.step.canister = "a";
  o.step.name = "Part A";
  o.step.channel = 2;
  o.step.flowMlPerMin = 60;
  o.step.runs = {3000, 3000};
  // The first order: the line's state is not known, so its first run says nothing; its second does
  REQUIRE(doser.start(c, act, o, err));
  done(true);
  CHECK_FALSE(rt.dropRate.contains("2"));
  done(true);
  REQUIRE(rt.dropRate.contains("2"));
  CHECK(jnum(rt.dropRate["2"], "ml") == doctest::Approx(3.0));
  CHECK(jnum(rt.dropRate["2"], "drops") == doctest::Approx(60));
  done(false);  // the pull back
  REQUIRE(doser.takeFinished());
  // The next order fills the line first: its runs count
  o.step.runs = {3000};
  REQUIRE(doser.start(c, act, o, err));
  done(false);  // the refill
  done(true);
  CHECK(jnum(rt.dropRate["2"], "ml") == doctest::Approx(6.0));
  done(false);
  REQUIRE(doser.takeFinished());
  // A run of no dose (a line fill) says nothing
  o.purpose = "fill";
  REQUIRE(doser.start(c, act, o, err));
  done(false);
  done(true);
  CHECK(jnum(rt.dropRate["2"], "ml") == doctest::Approx(6.0));
}

TEST_CASE("Scenario pump wearing: its drops per ml fall, the event says to calibrate it and its calibration falls due at once; a calibration learns afresh (SD-145)") {
  harness::Run r(harness::device());
  sim::World& w = r.s.world();
  // Doses from the demo's start: the robot learns channel 1's drops per ml
  r.act("topup", {{"litres", 20}});
  r.waitUntil("channel 2 learned", [&] { return r.s.robot().runtime().dropRate.contains("2") && r.s.robot().runtime().dropRate.at("2").contains("perMl"); }, 12 * 3600);
  CHECK(r.state()["channels"][1]["drift"].is_null());
  // The tube wears: a third less per run than its rate says
  const double fresh = w.pumps[1].trueFlow;
  r.act("pumpWear", {{"channel", 2}, {"factor", 0.65}});
  CHECK(w.pumps[1].trueFlow == doctest::Approx(fresh * 0.65));
  auto drifted = [&] { return r.state()["channels"][1]["drift"].is_object(); };
  for (int i = 0; i < 12 && !drifted(); ++i) {
    r.act("topup", {{"litres", 20}});
    r.waitUntil("a nutrient dose", [&] { return drifted() || r.phase() == "hold"; }, 6 * 3600);
    r.step(10 * kMinute);
  }
  REQUIRE(drifted());
  CHECK(r.state()["channels"][1]["drift"]["share"].get<double>() < -0.2);
  const json ev = r.events("calibration");
  CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.channel.drift" && e["text"]["key"] == "ev.channel.drift.fewer"; }));
  // Its calibration is due now
  const json plan = r.ok("GET", "service");
  bool due = false;
  for (const auto& i : plan["items"]) due |= i["id"] == "channel.2.cal" && i["due"].get<Epoch>() <= r.state()["now"].get<Epoch>();
  CHECK(due);
  // A new tube, calibrated again: the change is gone, and it learns afresh
  w.pumps[1].trueFlow = fresh;
  r.calibrateByHand(2);
  r.step(2 * kSecond);
  CHECK(r.state()["channels"][1]["drift"].is_null());
  CHECK_FALSE(r.s.robot().runtime().dropRate.count("2"));
}

TEST_CASE("Scenario consumption check: a level read after the doses proposes the rate that fits; taken, it is the channel's rate; dismissed, it goes (SD-145)") {
  harness::Run r(harness::device());
  // Read and entered: the start of the comparison
  r.ok("POST", "canisters/part_a/stock", {{"ml", 900}});
  // The doses counted 300 ml since (written into the stored state, as after a long run)
  json st = json::parse(*r.s.storage().read("state.json"));
  st["stockMl"]["part_a"] = 600;
  REQUIRE(r.s.storage().write("state.json", st.dump()));
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  const double rate = r.s.robot().config().channelFlow(2);
  // 330 ml went: 10 % more than counted
  const json out = r.ok("POST", "canisters/part_a/stock", {{"ml", 570}});
  REQUIRE(out["check"].is_object());
  CHECK(out["check"]["share"].get<double>() == doctest::Approx(0.1));
  const json p = r.state()["channels"][1]["rateProposal"];
  REQUIRE(p.is_object());
  CHECK(p["to"].get<double>() == doctest::Approx(rate * 1.1).epsilon(0.002));
  const json ev = r.events("calibration");
  CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.rate.checked" && e["text"]["key"] == "ev.rate.proposed.text"; }));
  // Without the answer, refused; on a channel with none, not found
  CHECK(r.call("POST", "channels/2/rate-proposal", json::object()).status == 422);
  CHECK(r.call("POST", "channels/4/rate-proposal", {{"take", true}}).status == 404);
  // Taken: the channel's rate from now on, with its head
  const std::string head = r.s.robot().config().channels.at(2).head;
  r.ok("POST", "channels/2/rate-proposal", {{"take", true}});
  CHECK(r.s.robot().config().channelFlow(2) == doctest::Approx(p["to"].get<double>()));
  CHECK(r.s.robot().config().channels.at(2).head == head);
  CHECK(r.state()["channels"][1]["rateProposal"].is_null());
  CHECK(r.s.robot().runtime().stockRead.at("part_a")["calAt"] == r.s.robot().config().channels.at(2).at);
  // A refill to its size was not read: the next reading only starts a comparison
  r.ok("POST", "canisters/part_a/stock", {{"full", true}});
  CHECK_FALSE(r.s.robot().runtime().stockRead.count("part_a"));

  // Dismissed: it goes, the rate stays
  r.ok("POST", "canisters/part_b/stock", {{"ml", 900}});
  st = json::parse(*r.s.storage().read("state.json"));
  st["stockMl"]["part_b"] = 600;
  REQUIRE(r.s.storage().write("state.json", st.dump()));
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  const double before = r.s.robot().config().channelFlow(3);
  r.ok("POST", "canisters/part_b/stock", {{"ml", 630}});  // 270 went: 10 % less
  REQUIRE(r.state()["channels"][2]["rateProposal"].is_object());
  r.ok("POST", "channels/3/rate-proposal", {{"take", false}});
  CHECK(r.s.robot().config().channelFlow(3) == before);
  CHECK(r.state()["channels"][2]["rateProposal"].is_null());
}

TEST_CASE("Scenario pumps true to their rates: days of doses say no change of drops per ml (SD-145)") {
  harness::Run r(harness::device());
  for (int day = 0; day < 3; ++day) {
    r.act("topup", {{"litres", 15}});
    r.step(24 * 3600 * kSecond, 5000);
  }
  const auto& rates = r.s.robot().runtime().dropRate;
  CHECK(rates.size() >= 2);  // learned on the channels that dosed
  for (const auto& [ch, d] : rates.items()) {
    INFO("channel " << ch << ": " << d.dump());
    CHECK_FALSE(d.contains("drift"));
  }
  const json ev = r.s.robot().events(0, std::numeric_limits<Epoch>::max(), "calibration", 1000);  // the session has expired by now
  CHECK(std::none_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.channel.drift"; }));
}
