// SPDX-License-Identifier: AGPL-3.0-or-later
// The control cycle as a state machine, run against the simulator's twin
// (SD-105, SD-106).
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "doseph/messages.hpp"
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
std::string phase(sim::Simulation& s) { return st(s)["cycle"]["phase"]; }

// Steps one second at a time until `done` holds or `maxS` seconds have passed
bool runUntil(sim::Simulation& s, const std::function<bool()>& done, double maxS) {
  for (double t = 0; t < maxS; ++t) {
    if (done()) return true;
    s.step(kSecond);
  }
  return done();
}

std::vector<std::string> eventKeys(sim::Simulation& s) {
  std::vector<std::string> keys;
  const json ev = s.robot().events(0, std::numeric_limits<Epoch>::max(), "", 1000);
  for (auto it = ev.rbegin(); it != ev.rend(); ++it) keys.push_back((*it)["title"]["key"]);
  return keys;  // oldest first
}

bool hasEvent(sim::Simulation& s, const std::string& key) {
  for (const auto& k : eventKeys(s))
    if (k == key) return true;
  return false;
}

bool dosing(sim::Simulation& s) { return !st(s)["dosing"].is_null(); }

}  // namespace

TEST_CASE("Cycle: pump on, flow proven, flushed, measured; a test dose first, EC before pH, mixed in with flow, measured again, then it holds (SD-105, RAT-004, RAT-052)") {
  sim::Simulation s(demo());
  // A fresh fill below the recipe: EC 0.45, pH 7.6
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow"; }, 10));
  CHECK(s.world().output);  // the robot switched the circulation pump on
  REQUIRE(runUntil(s, [&] { return phase(s) == "flush"; }, 30));
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 90));
  REQUIRE(runUntil(s, [&] { return phase(s) == "dose"; }, 15 * 60));
  json c = st(s)["cycle"];
  CHECK(c["round"]["kind"] == "ec");
  CHECK(c["round"]["test"] == true);
  CHECK(c["line"]["key"] == "cycle.dose_ec_test");
  // A small test dose: about 0.1 mS/cm in 100 L at the largest effect
  CHECK(c["line"]["args"]["ml"].get<double>() < 0.1 / 0.275 * 100 * 1.01);
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 5 * 60));
  CHECK(st(s)["cycle"]["progress"]["totalS"] == 17 * 60);  // the suggestion: 2 turnovers of 100 L at 12 L/min
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
  json h = st(s);
  CHECK_FALSE(h["newTank"].get<bool>());  // the test dose found the volume fitting
  CHECK(hasEvent(s, "ev.cycle.test_ok"));
  const double ec = h["cycle"]["last"]["ec"], ph = h["cycle"]["last"]["ph"];
  CHECK(ec == doctest::Approx(1.6).epsilon(0.1));
  CHECK(ph == doctest::Approx(6.0).epsilon(0.03));
  // EC first, pH after: the first pH dose comes after the last nutrient round (RAT-004)
  const auto keys = eventKeys(s);
  size_t lastEc = 0, firstPh = keys.size();
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i] == "ev.cycle.ec_round") lastEc = i;
    if (keys[i] == "ev.cycle.ph_dose" && firstPh == keys.size()) firstPh = i;
  }
  CHECK(lastEc > 0);
  CHECK(firstPh < keys.size());
  CHECK(lastEc < firstPh);
  CHECK(hasEvent(s, "ev.cycle.held"));
  // Holding: the circulation pump off (circulation "cycle"), the journal empty again (SD-106)
  CHECK_FALSE(s.world().output);
  CHECK(s.storage().read("journal.json").value_or("") == "[]");
  // After the hold it checks again from the start
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow"; }, 61 * 60));
}

TEST_CASE("Cycle: no flow, no dose; the pump that runs without flow goes off (SD-105)") {
  sim::Simulation s(demo());
  s.world().flowFault = "blocked";
  const double ec = s.world().tank.ec;
  REQUIRE(runUntil(s, [&] { return phase(s) == "wait"; }, 5 * 60));
  json c = st(s)["cycle"];
  CHECK(c["line"]["key"] == "cycle.wait.no_flow");
  CHECK_FALSE(s.world().output);
  CHECK(hasEvent(s, "ev.cycle.no_flow"));
  // It tries again later, never dosing meanwhile
  s.step(30 * kMinute, 1000);
  CHECK(s.world().tank.ec == doctest::Approx(ec).epsilon(0.01));
  CHECK(s.world().tank.pendingEc == doctest::Approx(0));
  // A circulation pump switched outside the robot: it waits for flow and says to switch it on
  s.robot().putTank({{"circulation", "external"}});
  s.world().flowFault.clear();
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow"; }, 6 * 60));
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait_flow_external");
  s.world().external = true;
  REQUIRE(runUntil(s, [&] { return phase(s) == "flush"; }, 60));
}

TEST_CASE("Cycle: a dose that loses its flow stops at once; what went in mixes with flow before it is measured, and nothing is learned (SD-105, RAT-057)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return dosing(s); }, 20 * 60));
  s.step(2 * kSecond);
  s.world().flowFault = "blocked";
  s.step(2 * kSecond);
  CHECK_FALSE(dosing(s));
  CHECK(hasEvent(s, "ev.flow_cut"));
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 60));
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.mix_paused");
  const int done = st(s)["cycle"]["progress"]["doneS"];
  s.step(3 * kMinute);
  CHECK(st(s)["cycle"]["progress"]["doneS"] == done);  // the clock stood without flow
  s.world().flowFault.clear();
  s.step(2 * kMinute);
  CHECK(st(s)["cycle"]["progress"]["doneS"].get<int>() > done);
  REQUIRE(runUntil(s, [&] { return phase(s) == "remeasure"; }, 30 * 60));
  REQUIRE(runUntil(s, [&] { return phase(s) != "remeasure"; }, 20 * 60));
  // The cut test dose taught nothing: the next dose is a test dose again
  CHECK(st(s)["newTank"] == true);
  CHECK(st(s)["effects"]["ecByRecipe"].empty());
}

TEST_CASE("Cycle: the mixing clock runs only with flow; the wait is the user's, else the suggestion (SD-105)") {
  sim::Simulation s(demo());
  CHECK(st(s)["cycle"]["settings"]["suggestEcMin"].is_null());  // no flow measured yet: no suggestion (R5)
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 5 * 60));
  CHECK(st(s)["cycle"]["settings"]["flowLpm"].get<double>() == doctest::Approx(12).epsilon(0.02));
  CHECK(st(s)["cycle"]["settings"]["turnoverMin"].get<double>() == doctest::Approx(8.33).epsilon(0.02));
  CHECK(st(s)["cycle"]["settings"]["suggestEcMin"] == 17);
  CHECK(st(s)["cycle"]["settings"]["suggestPhMin"] == 25);
  s.robot().putTank({{"waitEcMin", 5}});
  CHECK(st(s)["cycle"]["settings"]["waitEcMin"] == 5);
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 30 * 60));
  CHECK(st(s)["cycle"]["progress"]["totalS"] == 5 * 60);
  s.step(kMinute);
  s.world().flowFault = "sensor";  // water flows, but no flow is proven
  s.step(2 * kSecond);
  const int done = st(s)["cycle"]["progress"]["doneS"];
  s.step(2 * kMinute);
  CHECK(st(s)["cycle"]["progress"]["doneS"] == done);
  s.world().flowFault.clear();
  REQUIRE(runUntil(s, [&] { return phase(s) == "remeasure"; }, 6 * 60));
  // Back to the suggestion: null
  s.robot().putTank({{"waitEcMin", nullptr}});
  CHECK(st(s)["cycle"]["settings"]["waitEcMin"] == 17);
}

TEST_CASE("Cycle: a test dose that does not fit the entered volume stops all dosing until a person corrects or confirms it (SD-105)") {
  sim::Simulation s(demo());
  s.robot().putTank({{"volumeL", 40}});  // the twin holds 100 L
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("tank.volume"); }, 3 * 3600));
  json l = st(s)["latches"]["tank.volume"];
  CHECK(l["enteredL"] == 40);
  CHECK(l["impliedL"].get<double>() > 80);  // more than twice the entered volume
  CHECK(phase(s) == "wait");
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.volume");
  CHECK(hasEvent(s, "ev.cycle.volume"));
  CHECK(st(s)["watchdog"]["overall"] == "problem");
  // Nothing more is dosed meanwhile
  const double ec = s.world().tank.ec + s.world().tank.pendingEc;
  s.step(1 * kHour, 1000);
  CHECK(s.world().tank.ec + s.world().tank.pendingEc == doctest::Approx(ec).epsilon(0.02));
  // The person corrects the volume: the next dose is a test dose again, and fits
  s.robot().putTank({{"volumeL", 100}});
  CHECK_FALSE(st(s)["latches"].contains("tank.volume"));
  CHECK(st(s)["newTank"] == true);
  REQUIRE(runUntil(s, [&] { return st(s)["newTank"] == false; }, 3 * 3600));
  CHECK(hasEvent(s, "ev.cycle.test_ok"));
}

TEST_CASE("Cycle: a person may confirm the volume the test dose doubted; then its effect is taken as learned (SD-105)") {
  sim::Simulation s(demo());
  s.robot().putTank({{"volumeL", 40}});
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("tank.volume"); }, 3 * 3600));
  CHECK(s.robot().ackLatch("tank.volume", json::object()).status == 422);  // only with a confirmation
  CHECK(s.robot().ackLatch("tank.volume", {{"confirm", true}}).status == 200);
  CHECK(st(s)["newTank"] == false);
  CHECK(st(s)["effects"]["ecByRecipe"].contains("two_part"));
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow"; }, 60));
}

TEST_CASE("Cycle: a restart while a pump doses is reported; the next check mixes first and learns nothing (SD-106, R6, RAT-007)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return dosing(s); }, 20 * 60));
  s.step(3 * kSecond);
  s.reboot();
  CHECK(hasEvent(s, "ev.boot.cut_dose"));
  CHECK(hasEvent(s, "ev.boot.mix_first"));
  CHECK_FALSE(s.world().output);  // everything off after the start
  // The new check mixes before it measures, for the full mixing wait, with flow
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 5 * 60));
  json p = st(s)["cycle"]["progress"];
  CHECK(p["first"] == true);
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.mix_first");
  CHECK(p["totalS"] == 25 * 60);  // the longer of the two waits
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 30 * 60));
  CHECK(st(s)["newTank"] == true);  // the cut test dose checked nothing
  // A second restart reports the cut dose no more
  s.reboot();
  int cuts = 0;
  for (const auto& k : eventKeys(s)) cuts += k == "ev.boot.cut_dose";
  CHECK(cuts == 1);
}

TEST_CASE("Cycle: a Stop switches everything off at once, holds across a restart, and a resume mixes first what went in (RAT-036)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return dosing(s); }, 20 * 60));
  s.step(2 * kSecond);
  CHECK(s.robot().stop(say("stop.app")).status == 200);
  CHECK_FALSE(s.world().output);
  for (const auto& p : s.world().pumps) CHECK(p.state != 1);
  CHECK(phase(s) == "stopped");
  s.reboot();
  CHECK(st(s)["stopped"] == true);
  s.step(5 * kMinute);
  CHECK(phase(s) == "stopped");
  CHECK_FALSE(s.world().output);
  CHECK(s.robot().resume().status == 200);
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 5 * 60));
  CHECK(st(s)["cycle"]["progress"]["first"] == true);
}

TEST_CASE("Cycle: an empty canister: two rounds without effect lock nutrient dosing until a person releases it (RAT-055, RAT-062)") {
  sim::Simulation s(demo());
  s.world().pumps[1].empty = true;  // Part A
  s.world().pumps[2].empty = true;  // Part B
  s.world().pumps[3].empty = true;  // CalMag
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("ec.no_effect"); }, 4 * 3600));
  CHECK(hasEvent(s, "ev.ec.no_effect"));
  CHECK(st(s)["newTank"] == true);  // no effect: the volume stays unchecked
  // Nutrients wait; pH is still corrected above the EC floor? EC 0.45 is below it: the robot holds and says why
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 2 * 3600));
  bool latchedNote = false, gateNote = false;
  const json notes = st(s)["cycle"]["notes"];
  for (const auto& n : notes) {
    latchedNote |= n["key"] == "cycle.note.ec_latched";
    gateNote |= n["key"] == "cycle.note.ph_gate";
  }
  CHECK(latchedNote);
  CHECK(gateNote);
  for (auto& p : s.world().pumps) p.empty = false;
  CHECK(s.robot().ackLatch("ec.no_effect", json::object()).status == 200);
  REQUIRE(runUntil(s, [&] { return dosing(s); }, 15 * 60));
}

TEST_CASE("Cycle: EC above its target and pH below it are left as they are, with the reason (RAT-012, RAT-037, RAT-041)") {
  sim::Simulation s(demo());
  s.world().fill(100, 2.4, 5.4);
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 15 * 60));
  bool above = false, below = false;
  const json notes = st(s)["cycle"]["notes"];
  for (const auto& n : notes) {
    above |= n["key"] == "cycle.note.ec_above";
    below |= n["key"] == "cycle.note.ph_below";
  }
  CHECK(above);
  CHECK(below);
  CHECK_FALSE(hasEvent(s, "ev.cycle.ec_round"));
}

TEST_CASE("Cycle: a recipe in force for a number of days ends by itself; the robot then mixes nothing (SD-105)") {
  sim::Simulation s(demo());
  CHECK(s.robot().setActive({{"recipe", "two_part"}, {"days", 1}}).status == 200);
  s.step(25 * kHour, 5000);
  CHECK(phase(s) == "off");
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.off.ended");
  CHECK(hasEvent(s, "ev.cycle.recipe_ended"));
  CHECK_FALSE(s.world().output);
  // Choose a recipe again: it mixes again
  CHECK(s.robot().setActive({{"recipe", "two_part"}}).status == 200);
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow"; }, 10));
  CHECK(s.robot().setActive({{"recipe", "two_part"}, {"days", 0}}).status == 422);
  CHECK(s.robot().setActive({{"recipe", "nope"}}).status == 422);
}

TEST_CASE("Cycle: a probe calibration holds the cycle; a dose running is cut and mixed first (RAT-042)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return dosing(s); }, 20 * 60));
  CHECK(s.robot().probe({{"action", "start"}, {"kind", "ph"}}).status == 200);
  s.step(2 * kSecond);
  CHECK_FALSE(dosing(s));
  CHECK(phase(s) == "wait");
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.calibrating");
  s.step(10 * kMinute);
  CHECK(phase(s) == "wait");  // until saved or cancelled
  CHECK(s.robot().probe({{"action", "cancel"}}).status == 200);
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 5 * 60));
  CHECK(st(s)["cycle"]["progress"]["first"] == true);
}

TEST_CASE("Pumps: a calibration runs into the cup and is worked out from the actual run time; a senseless result is refused (RAT-003, RAT-070)") {
  sim::Simulation s(demo());
  s.robot().setActive({{"recipe", ""}});  // nothing in force: the cycle is off
  CHECK(s.robot().startCalibration(9, 30).status == 404);
  CHECK(s.robot().startCalibration(2, 0.5).status == 422);
  REQUIRE(s.robot().startCalibration(2, 30).status == 200);
  CHECK(s.robot().startCalibration(3, 30).status == 409);  // one at a time
  s.step(32 * kSecond, 500);
  json job = st(s)["pumpJob"];
  CHECK(job["state"] == "waiting_result");
  const double cup = s.world().pumps[1].cupMl;
  CHECK(s.robot().calibrationResult(0).status == 422);
  CHECK(s.robot().calibrationResult(1200).status == 422);
  const double before = s.robot().config().pumpFlow(2);
  REQUIRE(s.robot().calibrationResult(cup).status == 200);
  CHECK(s.robot().config().pumpFlow(2) == doctest::Approx(s.world().pumps[1].trueFlow).epsilon(0.02));
  CHECK(before == doctest::Approx(s.robot().config().pumpFlow(2)).epsilon(0.02));
  // The cup is no dose: nothing went into the tank, nothing waits to be mixed
  CHECK(s.storage().read("journal.json").value_or("[]").find("\"purpose\":\"calibration\"") != std::string::npos);
  s.reboot();
  CHECK_FALSE(hasEvent(s, "ev.boot.mix_first"));
}
