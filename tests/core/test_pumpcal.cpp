// SPDX-License-Identifier: AGPL-3.0-or-later
// A pump calibration by the user (SD-143): water from a cylinder or a glass on a scale on the
// intake side, the line emptied into its canister first and refilled from it at the end.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

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

TEST_CASE("Pump calibration: the line goes back into its canister, water from the cylinder primes it, two measuring runs give the rate corrected for the cylinder, and the line is refilled from the canister; the water takes nothing from the canister and nothing else doses meanwhile (SD-143)") {
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
    CHECK(r.state()["stock"] == stockBefore);  // the water took nothing from a canister
    r.act("intake", {{"channel", 2}, {"where", "canister"}});
    r.ok("POST", "pump-calibration/next");
    reach(r, "done", 400);
    CHECK(r.twin()["channels"][1]["lineHolds"] == "concentrate");
    CHECK(r.twin()["channels"][1]["light"] == false);
    CHECK(r.events("dose")[0]["title"]["key"] == "ev.fill");  // the first drop on is the first dose
    const json ev = r.events("calibration");
    CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.channel.calibrated"; }));
    // The check goes on afterwards
    r.waitUntil("the check measures again", [&] { const std::string ph = r.phase(); return ph == "measure" || ph == "mix" || ph == "hold"; }, 600);
  }
}

TEST_CASE("Pump calibration: a reading far from the head's rate is refused with its likely cause, as is one that rises; two runs that disagree ask for a third, and the two that agree give the rate (SD-143)") {
  harness::Run r(harness::device());
  idle(r);
  r.ok("POST", "channels/1/pump-calibration", {{"method", "scale"}});
  reach(r, "to_water");
  r.act("intake", {{"channel", 1}, {"where", "water"}});
  r.ok("POST", "pump-calibration/next");
  reach(r, "reading");
  const double start = reading(r, "scale");
  CHECK(r.call("POST", "pump-calibration/next", {{"reading", -1}}).status == 422);
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

TEST_CASE("Pump calibration: refused while stopped, while another job runs, without the placement confirmed, for a pump without a head's rate, and for an account; left idle it ends after 30 min (SD-143)") {
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
