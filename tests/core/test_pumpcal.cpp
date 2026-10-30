// SPDX-License-Identifier: AGPL-3.0-or-later
// A pump calibration by the user (SD-143): water from a cylinder or a glass on a scale on the
// intake side, the line emptied into its canister first and refilled from it at the end.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "doseph/journal.hpp"
#include "doseph/pumpcal.hpp"
#include "fakes.hpp"
#include "harness.hpp"

using namespace doseph;

namespace {

json cal(harness::Run& r) { return r.state()["pumpCalibration"]; }
std::string step(harness::Run& r) { return cal(r)["step"]; }

void reach(harness::Run& r, const std::string& want, double maxS = 300) {
  r.waitUntil("step " + want, [&] { return step(r) == want; }, maxS, 250);
}

// What the person reads: the cylinder's ml, or the scale's grams
double reading(harness::Run& r, const std::string& method) {
  return r.twin()["calWater"][method == "cylinder" ? "cylinderMl" : "scaleG"].get<double>();
}

double tankSalt(harness::Run& r) {
  const json t = r.twin()["tank"];
  return t["ec"].get<double>() + t["pendingEc"].get<double>();
}

void idle(harness::Run& r) {
  r.waitUntil("the check holds", [&] { return r.phase() == "hold"; }, 4 * 3600, 1000);
}

}  // namespace

TEST_CASE("Pump calibration: the line goes back into its canister, water from the cylinder primes it, two measuring runs give the rate corrected for the cylinder from the run time the driver reported, and the line is refilled from the canister; the water takes nothing from the canister and nothing else doses meanwhile (SD-143, RAT-070)") {
  for (const std::string method : {"cylinder", "scale"}) {
    CAPTURE(method);
    harness::Run r(harness::device());
    idle(r);
    const double trueFlow = r.twin()["channels"][1]["trueFlow"];
    const double salt = tankSalt(r);
    const json stockBefore = r.state()["stock"];
    r.ok("POST", "channels/2/pump-calibration", {{"method", method}});
    CHECK(r.twin()["channels"][1]["light"] == true);
    CHECK(step(r) == "reverse");
    reach(r, "to_water");
    CHECK(r.twin()["channels"][1]["lineHolds"] == "air");  // the concentrate went back
    CHECK(cal(r)["waiting"] == true);
    const std::string ph = r.phase();
    CHECK((ph == "pump_on" || ph == "prove_flow" || ph == "flush"));  // it circulates, and measures nothing
    CHECK(r.twin()["output"] == true);
    r.act("intake", {{"channel", 2}, {"where", "water"}});
    r.act("calWater", {{"ml", 100}});
    r.ok("POST", "pump-calibration/next");
    reach(r, "reading");
    CHECK(r.twin()["channels"][1]["lineHolds"] == "water");
    r.ok("POST", "pump-calibration/next", {{"reading", reading(r, method)}});
    for (int run = 1; run <= 3 && step(r) != "empty"; ++run) {
      reach(r, "reading");
      r.ok("POST", "pump-calibration/next", {{"reading", reading(r, method)}});
    }
    const json c = cal(r);
    CHECK(c["runs"].size() == 2);  // two that agree
    const double flow = r.s.robot().config().channels.at(2).flowMlPerMin;
    CHECK(flow == doctest::Approx(trueFlow).epsilon(0.02));
    CHECK(c["flowMlPerMin"].get<double>() == doctest::Approx(flow));
    reach(r, "to_canister");
    CHECK(r.twin()["channels"][1]["lineHolds"] == "air");  // the water went back into the glass
    CHECK(std::fabs(tankSalt(r) - salt) < 0.005);  // only water reached the tank
    // The water took nothing from a canister; the line's concentrate went back into its own
    const json stock = r.state()["stock"];
    for (const auto& [id, s] : stockBefore.items())
      CHECK(stock[id]["ml"].get<double>() == doctest::Approx(s["ml"].get<double>() + (id == "part_a" ? 6.0 : 0.0)));
    r.act("intake", {{"channel", 2}, {"where", "canister"}});
    r.ok("POST", "pump-calibration/next");
    reach(r, "done", 400);
    CHECK(r.twin()["channels"][1]["lineHolds"] == "concentrate");
    CHECK(r.twin()["channels"][1]["light"] == false);
    CHECK(r.events("dose")[0]["title"]["key"] == "ev.fill");  // the first drop on is the first dose
    // … and the fill took the line back out of the canister: its level is where it was, give or take a drop
    CHECK(r.state()["stock"]["part_a"]["ml"].get<double>() == doctest::Approx(stockBefore["part_a"]["ml"].get<double>()).epsilon(0.002));
    const json ev = r.events("calibration");
    CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.channel.calibrated"; }));
    // The check goes on afterwards
    r.waitUntil("the check measures again", [&] { const std::string ph = r.phase(); return ph == "measure" || ph == "mix" || ph == "hold"; }, 600);
  }
}

TEST_CASE("Pump calibration: a reading far from the head's rate is refused with its likely cause, as is one that rises; two runs that disagree ask for a third, and the two that agree give the rate (SD-143, RAT-003)") {
  harness::Run r(harness::device());
  idle(r);
  r.ok("POST", "channels/1/pump-calibration", {{"method", "scale"}});
  reach(r, "to_water");
  r.act("intake", {{"channel", 1}, {"where", "water"}});
  r.ok("POST", "pump-calibration/next");
  reach(r, "reading");
  const double start = reading(r, "scale");
  CHECK(r.call("POST", "pump-calibration/next", {{"reading", -20000}}).status == 422);
  r.ok("POST", "pump-calibration/next", {{"reading", start}});
  reach(r, "reading");
  const double after1 = reading(r, "scale");
  auto refused = [&](double v) { return json::parse(r.call("POST", "pump-calibration/next", {{"reading", v}}).body)["error"]["key"]; };
  CHECK(refused(start + 5) == "pumpcal.reading_up");
  CHECK(refused(start - (start - after1) * 3) == "pumpcal.too_high");  // a typo: three times as much
  CHECK(refused(start - (start - after1) * 0.3) == "pumpcal.too_low");  // air or a kink
  CHECK(step(r) == "reading");
  r.ok("POST", "pump-calibration/next", {{"reading", after1}});
  reach(r, "reading");
  // The second reading 6 % off: a third run
  const double after2 = reading(r, "scale");
  r.ok("POST", "pump-calibration/next", {{"reading", after1 - (after1 - after2) * 1.06}});
  CHECK(step(r) == "run");
  reach(r, "reading");
  r.ok("POST", "pump-calibration/next", {{"reading", reading(r, "scale") - (after1 - after2) * 0.06}});
  CHECK(cal(r)["runs"].size() == 3);
  const double trueFlow = r.twin()["channels"][0]["trueFlow"];
  CHECK(r.s.robot().config().channels.at(1).flowMlPerMin == doctest::Approx(trueFlow).epsilon(0.02));
}

TEST_CASE("Pump calibration: cancelled or cut by a restart with the intake perhaps in the water, the channel doses nothing until a person releases it; before that it is free (SD-143, R6)") {
  harness::Run r(harness::device());
  idle(r);
  // Cancelled while the concentrate goes back: the intake never left its canister
  r.ok("POST", "channels/3/pump-calibration", {{"method", "cylinder"}});
  r.ok("POST", "pump-calibration/cancel");
  CHECK(step(r) == "cancelled");
  CHECK_FALSE(r.state()["latches"].contains("channel.3.cal_cut"));
  CHECK(r.twin()["channels"][2]["light"] == false);
  // Cancelled once the person was asked to move the intake
  r.ok("POST", "channels/3/pump-calibration", {{"method", "cylinder"}});
  reach(r, "to_water");
  r.ok("POST", "pump-calibration/cancel");
  REQUIRE(r.state()["latches"].contains("channel.3.cal_cut"));
  CHECK(r.state()["latches"]["channel.3.cal_cut"]["channel"] == 3);
  r.ok("POST", "latches/channel.3.cal_cut/ack");
  // Cut by a restart in the middle
  r.ok("POST", "channels/3/pump-calibration", {{"method", "cylinder"}});
  reach(r, "to_water");
  r.ok("POST", "pump-calibration/next");
  r.step(2 * kSecond);
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  r.step(kSecond);
  CHECK(r.state()["latches"].contains("channel.3.cal_cut"));
  CHECK(r.state()["pumpCalibration"].is_null());
  const json ev = r.events("calibration");
  CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.pumpcal.cut"; }));
}

TEST_CASE("Pump calibration: refused while stopped, while another job runs, and for an account; while it runs, no other job starts; a Stop ends it, and so do 30 min without a step (SD-143, RAT-003)") {
  harness::Run r(harness::device());
  idle(r);
  CHECK(r.call("POST", "channels/9/pump-calibration", {{"method", "cylinder"}}).status != 200);
  CHECK(r.call("POST", "channels/1/pump-calibration", {{"method", "jug"}}).status == 409);
  r.ok("POST", "stop");
  CHECK(r.call("POST", "channels/1/pump-calibration", {{"method", "cylinder"}}).status == 409);
  r.ok("POST", "resume");
  idle(r);
  r.ok("POST", "channels/1/pump-calibration", {{"method", "cylinder"}});
  CHECK(r.call("POST", "channels/2/pump-calibration", {{"method", "cylinder"}}).status == 409);
  CHECK(r.call("POST", "channels/2/prime", json::object()).status == 409);
  CHECK(r.call("POST", "pump-calibration/next", {{"reading", 5}}).status == 409);  // not the person's turn
  // Nothing else that would stop its circulation, or its pump, starts meanwhile
  CHECK(r.call("POST", "probe", {{"action", "start"}, {"kind", "ph"}}).status == 409);
  CHECK(r.call("POST", "maintenance", {{"minutes", 10}}).status == 409);
  // A Stop ends it
  r.ok("POST", "stop");
  CHECK(step(r) == "cancelled");
  r.ok("POST", "resume");
  // Left idle
  idle(r);
  r.ok("POST", "channels/4/pump-calibration", {{"method", "cylinder"}});
  reach(r, "to_water");
  r.step(31 * kMinute, 1000);
  CHECK(step(r) == "cancelled");
  CHECK(cal(r)["message"]["key"] == "pumpcal.idle");
  CHECK(r.state()["latches"].contains("channel.4.cal_cut"));
  // An account may not
  auto made = r.ok("POST", "tokens", {{"name", "Cloud"}, {"role", "account"}});
  harness::Run* rr = &r;
  const std::string owner = rr->token;
  r.token = made["token"];
  CHECK(r.call("POST", "channels/1/pump-calibration", {{"method", "cylinder"}}).status == 403);
  r.token = owner;
}

TEST_CASE("Pump calibration: a prime that never drips steadily, or three runs that all disagree, fail and lock the channel; a locked channel doses nothing into the tank (SD-143)") {
  harness::Run r(harness::device());
  idle(r);
  r.ok("POST", "channels/2/pump-calibration", {{"method", "scale"}});
  reach(r, "to_water");
  r.act("intake", {{"channel", 2}, {"where", "water"}});
  r.act("channelFault", {{"channel", 2}, {"outletBlocked", true}});
  r.ok("POST", "pump-calibration/next");
  reach(r, "failed", 400);
  CHECK(cal(r)["message"]["key"] == "pumpcal.run_failed");
  CHECK(channelLocked(r.s.robot().runtime(), 2));
  r.act("channelFault", {{"channel", 2}, {"outletBlocked", false}});
  r.ok("POST", "latches/channel.2.cal_cut/ack");
  // Three runs, no two within 3 %
  r.ok("POST", "channels/2/pump-calibration", {{"method", "scale"}});
  reach(r, "to_water");
  r.ok("POST", "pump-calibration/next");
  reach(r, "reading");
  double truth = reading(r, "scale"), entered = truth;
  r.ok("POST", "pump-calibration/next", {{"reading", entered}});
  for (const double share : {1.0, 1.08, 1.16}) {  // what went, as read: no two within 3 %
    reach(r, "reading");
    const double now = reading(r, "scale");
    entered -= (truth - now) * share;
    truth = now;
    r.ok("POST", "pump-calibration/next", {{"reading", entered}});
  }
  CHECK(step(r) == "failed");
  CHECK(cal(r)["message"]["key"] == "pumpcal.disagree");
  CHECK(channelLocked(r.s.robot().runtime(), 2));
}

TEST_CASE("Pump calibration: refused for a pump without its head's nominal rate, one that cannot run backwards, or one without a drop sensor; the gateway doses nothing into the tank while one runs, whoever asks (SD-143)") {
  MemoryStorage store;
  fake::Hardware hw;
  Config cfg;
  cfg.setup["placement"] = {"done", 1};
  cfg.channels[2] = {50, 1, "S"};
  RuntimeState rt;
  SensorTruth truth;
  EventLog log;
  DoseJournal journal(store);
  Ctx c{cfg, rt, truth, log, journal};
  c.flowOk = true;
  Actuators act(hw);
  PumpCal pc;
  Msg err;
  CHECK_FALSE(pc.start(c, act, 2, "cylinder", err));
  CHECK(err.key == "pumpcal.no_nominal");
  hw.prof.headMlPerMin = {{"S", 50.0}};
  hw.prof.channels[1].reverse = false;
  CHECK_FALSE(pc.start(c, act, 2, "cylinder", err));
  CHECK(err.key == "pumpcal.no_reverse");
  hw.prof.channels[1].reverse = true;
  CHECK_FALSE(pc.start(c, act, 2, "cylinder", err));
  CHECK(err.key == "pumpcal.no_drop_sensor");
  hw.dropsSeen[2] = {0, -1};
  CHECK(pc.start(c, act, 2, "cylinder", err));
  CHECK(pc.active());
  CHECK(hw.lights[2]);
  // While it runs, no dose into the tank
  c.pumpCalibration = true;
  CHECK_FALSE(act.startRun(c, 2, 6000, "ec", "j1", err));
  CHECK(err.key == "pumpcal.busy");
}
