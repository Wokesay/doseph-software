// SPDX-License-Identifier: AGPL-3.0-or-later
// The control cycle as a state machine, run against the simulator's twin
// (SD-105, SD-106).
#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "doseph/alarms.hpp"
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

TEST_CASE("Cycle: feed pump on, flow proven, flushed, measured; a test dose first, EC before pH, mixed in with flow, measured again, then it holds (SD-105, RAT-004, RAT-052)") {
  sim::Simulation s(demo());
  // A fresh fill below the recipe: EC 0.45, pH 7.6
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow"; }, 10));
  CHECK(s.world().output);  // the robot switched the feed pump on
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
  CHECK(st(s)["cycle"]["progress"]["totalS"] == 10 * 60);  // one turnover of 100 L at the estimated 10 L/min (SD-112)
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
  json h = st(s);
  CHECK_FALSE(h["newTank"].get<bool>());  // the test dose found the volume fitting
  CHECK(hasEvent(s, "ev.cycle.test_ok"));
  const double ec = h["cycle"]["last"]["ec"], ph = h["cycle"]["last"]["ph"];
  CHECK(ec == doctest::Approx(1.6).epsilon(0.1));
  CHECK(ph == doctest::Approx(6.0).epsilon(0.03));
  // EC first, pH after: the first pH dose comes after a nutrient round (RAT-004); a round that tops
  // EC up after pH (the reserve, SD-121) is measured again before the robot holds
  const auto keys = eventKeys(s);
  size_t firstEc = keys.size(), lastEc = 0, firstPh = keys.size(), held = keys.size();
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i] == "ev.cycle.ec_round" && firstEc == keys.size()) firstEc = i;
    if (keys[i] == "ev.cycle.ec_round") lastEc = i;
    if (keys[i] == "ev.cycle.ph_dose" && firstPh == keys.size()) firstPh = i;
    if (keys[i] == "ev.cycle.held" && held == keys.size()) held = i;
  }
  CHECK(firstEc < keys.size());
  CHECK(firstPh < keys.size());
  CHECK(firstEc < firstPh);
  CHECK(lastEc < held);
  CHECK(firstPh < held);
  CHECK(hasEvent(s, "ev.cycle.held"));
  // Holding: the feed pump off (SD-109), the journal empty again (SD-106)
  CHECK_FALSE(s.world().output);
  CHECK(s.storage().read("journal.json").value_or("") == "[]");
  // After the hold it checks again from the start
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow"; }, 61 * 60));
}

TEST_CASE("Cycle: no flow, no dose; the feed pump in the tank that finds no flow goes off after 6 s, before the hardware's own cut, and after two tries in a row it is locked until a person releases it (SD-105, SD-127, PD-477)") {
  sim::Simulation s(demo());
  s.world().flowFault = "hose";  // the hose came off: each try could spill water
  const double ec = s.world().tank.ec;
  REQUIRE(runUntil(s, [&] { return s.world().output; }, 60));
  const Ms on = s.world().now();
  REQUIRE(runUntil(s, [&] { return phase(s) == "wait"; }, 60));
  CHECK(s.world().now() - on <= 8 * kSecond);
  CHECK_FALSE(s.world().noFlowCut);  // the robot was first
  json c = st(s)["cycle"];
  CHECK(c["line"]["key"] == "cycle.wait.no_flow");
  CHECK_FALSE(s.world().output);
  CHECK(hasEvent(s, "ev.cycle.no_flow"));
  CHECK_FALSE(st(s)["latches"].contains("flow.dry"));
  // It tries once more after 5 min, never dosing meanwhile; the second try without flow locks it
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("flow.dry"); }, 6 * 60));
  CHECK(st(s)["latches"]["flow.dry"]["tries"] == 2);
  CHECK(s.world().tank.ec == doctest::Approx(ec).epsilon(0.01));
  CHECK(s.world().tank.pendingEc == doctest::Approx(0));
  CHECK(hasEvent(s, "ev.flow.dry"));
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.dry");
  s.step(30 * kMinute, 1000);
  CHECK_FALSE(s.world().output);
  CHECK_FALSE(s.world().noFlowCut);
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.dry");
  s.world().flowFault.clear();
  REQUIRE(s.robot().ackLatch("flow.dry", json::object()).status == 200);
  REQUIRE(runUntil(s, [&] { return phase(s) == "flush"; }, 60));
}

TEST_CASE("Cycle: a feed pump in the device primes for up to 2 min, and three tries in a row without flow lock it (SD-109, SD-127)") {
  sim::Options o = demo();
  o.pumpIn = "device";
  sim::Simulation s(o);
  CHECK_FALSE(s.world().pumpInTank);
  // It primes for 8 s before water flows, well inside its window
  REQUIRE(runUntil(s, [&] { return phase(s) == "flush"; }, 60));
  CHECK(s.world().primed());
  // The suction line out of the water: 2 min without flow before it goes off
  s.world().flowFault = "dry";
  const Ms lost = s.world().now();
  REQUIRE(runUntil(s, [&] { return phase(s) == "wait"; }, 3 * 60));
  CHECK(s.world().now() - lost >= 2 * kMinute);
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.no_flow");
  CHECK_FALSE(s.world().noFlowCut);  // the hardware's cut is the tank pump's
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("flow.dry"); }, 20 * 60));
  CHECK(st(s)["latches"]["flow.dry"]["tries"] == 3);
}

TEST_CASE("Cycle: when the hardware cuts the feed pump for no flow, the check stops, says so and counts a try; switching the pump off re-arms the hardware (SD-127, PD-477)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 5 * 60));
  // The hardware switches the pump off below the robot, as after 9 s without flow
  s.world().noFlowCut = true;
  s.world().output = false;
  CHECK(st(s)["hardware"]["release"]["noFlow"] == true);
  s.step(kSecond);
  CHECK(phase(s) == "wait");
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.pump_cut");
  CHECK_FALSE(s.world().noFlowCut);  // re-armed
  CHECK(hasEvent(s, "ev.cycle.no_flow"));
  CHECK_FALSE(st(s)["latches"].contains("flow.dry"));  // one try of two
  // The next try finds flow: the count starts anew
  REQUIRE(runUntil(s, [&] { return phase(s) == "flush"; }, 6 * 60));
  s.world().noFlowCut = true;
  s.world().output = false;
  s.step(kSecond);
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.pump_cut");
  CHECK_FALSE(st(s)["latches"].contains("flow.dry"));
}

TEST_CASE("Cycle: the feed pump's current is learned while it moves water; once it has, far less for 2 s is a dry run that switches it off at once as a try without flow (SD-127, RAT-049)") {
  sim::Simulation s(demo());
  // Out of the water at its start, before anything is learned: the window protects it
  s.world().flowFault = "dry";
  REQUIRE(runUntil(s, [&] { return phase(s) == "wait"; }, 60));
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.no_flow");
  s.world().flowFault.clear();
  // The next try moves water: the current is learned over a minute of flow
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 7 * 60));
  CHECK(s.robot().state()["pump"]["normalA"].get<double>() == doctest::Approx(0.80).epsilon(0.01));
  // Lifted out of the water while it runs: off within 2 s, long before the window
  s.world().flowFault = "dry";
  const Ms lifted = s.world().now();
  REQUIRE(runUntil(s, [&] { return phase(s) == "wait"; }, 10));
  CHECK(s.world().now() - lifted <= 3 * kSecond);
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.dry_run");
  CHECK_FALSE(s.world().output);
  // The second try in a row: the pump starts out of the water, the window ends it, and it is locked
  REQUIRE(runUntil(s, [&] { return st(s)["latches"].contains("flow.dry"); }, 6 * 60));
  // The learned current survives a restart
  s.reboot();
  CHECK(s.robot().state()["pump"]["normalA"].get<double>() == doctest::Approx(0.80).epsilon(0.01));
}

TEST_CASE("Cycle: at the end of each check pH and EC with the feed pump running are compared with them standing; a shift is checked once more at once, and confirmed it locks the pump and dosing as a critical alarm until a person releases it (SD-127)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
  // A sound pump: the comparison passes, the pump stands
  s.step(30 * kSecond);
  CHECK_FALSE(s.world().output);
  CHECK_FALSE(st(s)["latches"].contains("pump.cable"));
  // The cable leaks: pH and EC read shifted while the pump runs, from the next check on
  s.world().cableFault = true;
  REQUIRE(runUntil(s, [&] { return phase(s) != "hold"; }, 61 * 60));
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 2 * 3600));
  // Standing 10–20 s after the stop, then once more running and standing: about a minute
  bool ranAgain = false;
  REQUIRE(runUntil(s, [&] {
    ranAgain = ranAgain || s.world().output;
    return st(s)["latches"].contains("pump.cable");
  }, 90));
  CHECK(ranAgain);
  CHECK_FALSE(s.world().output);
  const json l = st(s)["latches"]["pump.cable"];
  CHECK(l["phOn"].get<double>() - l["phOff"].get<double>() == doctest::Approx(0.3).epsilon(0.2));
  CHECK(hasEvent(s, "ev.pump.cable"));
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.cable");
  CHECK(levelOf("latch.pump.cable") == Level::Critical);
  // It stays off and doses nothing until a person releases it
  const double ec = s.world().tank.ec;
  s.step(30 * kMinute, 1000);
  CHECK_FALSE(s.world().output);
  CHECK(s.world().tank.ec == doctest::Approx(ec).epsilon(0.001));
  s.world().cableFault = false;
  REQUIRE(s.robot().ackLatch("pump.cable", json::object()).status == 200);
  REQUIRE(runUntil(s, [&] { return phase(s) == "flush"; }, 60));
}

TEST_CASE("Cycle: a shift seen once that is gone when checked again locks nothing (SD-127)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
  s.step(30 * kSecond);
  s.world().cableFault = true;
  REQUIRE(runUntil(s, [&] { return phase(s) != "hold"; }, 61 * 60));
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 2 * 3600));
  // The first comparison sees the shift; before the repeat the fault is gone
  REQUIRE(runUntil(s, [&] { return s.world().output; }, 40));
  s.world().cableFault = false;
  s.step(60 * kSecond);
  CHECK_FALSE(st(s)["latches"].contains("pump.cable"));
  CHECK(phase(s) == "hold");
  CHECK_FALSE(s.world().output);
}

TEST_CASE("Cycle: a dose that loses its flow stops at once; without flow for 6 s the pump goes off, and what went in mixes with flow before it is measured, learning nothing (SD-105, SD-127, RAT-057)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return dosing(s); }, 20 * 60));
  s.step(2 * kSecond);
  s.world().flowFault = "hose";  // the pump draws its normal current: only the window sees it
  s.step(2 * kSecond);
  CHECK_FALSE(dosing(s));
  CHECK(hasEvent(s, "ev.flow_cut"));
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 60));
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.mix_paused");
  REQUIRE(runUntil(s, [&] { return phase(s) == "wait"; }, 10));
  CHECK(st(s)["cycle"]["line"]["key"] == "cycle.wait.no_flow");
  CHECK_FALSE(s.world().output);
  CHECK_FALSE(s.world().noFlowCut);
  s.world().flowFault.clear();
  // The next try mixes first, with flow, then measures
  REQUIRE(runUntil(s, [&] { return st(s)["cycle"]["line"]["key"] == "cycle.mix_first"; }, 7 * 60));
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 30 * 60));
  // The cut test dose taught nothing: the next dose is a test dose again
  CHECK(st(s)["newTank"] == true);
  CHECK(st(s)["effects"]["ecByRecipe"].empty());
}

TEST_CASE("Cycle: the mixing clock runs only with flow; the wait is one turnover, at least 2 min, which the user may only lengthen; then stable over a fifth of a turnover (SD-105, SD-112, RAT-052, RAT-058)") {
  sim::Simulation s(demo());
  CHECK(st(s)["cycle"]["settings"]["mixOwnMin"].is_null());  // no flow measured yet: no turnover (R5)
  CHECK(st(s)["cycle"]["settings"]["mixWhy"] == "minimum");
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 5 * 60));
  // Without a flow meter the flow is the low end of the feed pump's delivery, so a turnover errs long (SD-109)
  json set = st(s)["cycle"]["settings"];
  CHECK(set["flowLpm"].get<double>() == doctest::Approx(10).epsilon(0.001));
  CHECK(set["turnoverMin"].get<double>() == doctest::Approx(10).epsilon(0.001));
  CHECK(set["mixOwnMin"] == 10);
  CHECK(set["mixMin"] == 10);
  CHECK(set["mixWhy"] == "turnover");
  CHECK(set["stableS"] == 120);  // a fifth of 10 min
  // A shorter wait of the user's does not apply; a longer one does
  s.robot().putTank({{"mixMinMin", 5}});
  CHECK(st(s)["cycle"]["settings"]["mixMin"] == 10);
  s.robot().putTank({{"mixMinMin", 15}});
  set = st(s)["cycle"]["settings"];
  CHECK(set["mixMin"] == 15);
  CHECK(set["mixWhy"] == "user");
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 30 * 60));
  CHECK(st(s)["cycle"]["progress"]["totalS"] == 15 * 60);
  s.step(kMinute);
  s.world().flowFault = "sensor";  // water flows, but no flow is proven
  s.step(kSecond);
  const int done = st(s)["cycle"]["progress"]["doneS"];
  s.step(4 * kSecond);  // inside the pump's window (SD-127)
  CHECK(phase(s) == "mix");
  CHECK(st(s)["cycle"]["progress"]["doneS"] == done);
  s.world().flowFault.clear();
  REQUIRE(runUntil(s, [&] { return phase(s) == "remeasure"; }, 16 * 60));
  s.robot().putTank({{"mixMinMin", nullptr}});
  CHECK(st(s)["cycle"]["settings"]["mixMin"] == 10);
}

TEST_CASE("Cycle: with a flow meter the measured flow gives the turnover (SD-109, SD-112)") {
  sim::Options o = demo();
  o.profile = "pro";
  sim::Simulation s(o);
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 5 * 60));
  const json set = st(s)["cycle"]["settings"];
  CHECK(set["flowLpm"].get<double>() == doctest::Approx(12).epsilon(0.02));
  CHECK(set["turnoverMin"].get<double>() == doctest::Approx(8.33).epsilon(0.02));
  CHECK(set["mixOwnMin"] == 9);
  CHECK(set["stableS"] == 100);
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

TEST_CASE("Cycle: a restart while a channel doses is reported; the next check mixes first and learns nothing (SD-106, R6, RAT-007)") {
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
  CHECK(p["totalS"] == 10 * 60);  // the mixing wait: one turnover (SD-112)
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

TEST_CASE("Cycle: canisters without effect: two rounds without effect lock nutrient dosing until a person releases it (RAT-055, RAT-062)") {
  sim::Simulation s(demo());
  // Water in the nutrient canisters: it drips, so the drop sensors see nothing wrong (an empty
  // canister drips nothing and locks its channel at once, SD-111), but EC does not rise
  const auto liquids = s.world().liquids;
  for (const char* l : {"grow_a", "grow_b", "calmag"}) s.world().liquids[l].ecPerMlL = 0;
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
  s.world().liquids = liquids;
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

TEST_CASE("Cycle: the checklist says why the robot doses or not, and follows the state (RAT-037)") {
  sim::Simulation s(demo());
  auto group = [&](const char* g) { return st(s)["cycle"]["checks"][g]; };
  auto item = [&](const char* g, const std::string& key) {
    for (const auto& i : group(g))
      if (i["key"] == key) return i;
    return json(nullptr);
  };
  auto allOk = [&](const char* g) {
    for (const auto& i : group(g))
      if (!i["ok"].get<bool>()) return false;
    return true;
  };
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 3 * 60));
  CHECK(allOk("all"));
  CHECK(item("all", "check.recipe")["args"]["name"].is_string());
  CHECK(allOk("ec"));
  CHECK(item("ph", "check.ph_down")["ok"] == true);
  // An EC round: pH comes after it (RAT-004)
  REQUIRE(runUntil(s, [&] { return phase(s) == "dose"; }, 15 * 60));
  CHECK(item("ph", "check.ec_first")["ok"] == false);
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 5 * 60));
  CHECK(item("ph", "check.rest_left")["ok"] == false);  // pH rests after a nutrient dose (RAT-046)
  // A part on no pump: the round cannot be planned, and the list names it (RAT-015)
  json b = s.robot().configJson()["canisters"][2];
  b["channel"] = 0;
  REQUIRE(s.robot().putCanister(b).status == 200);
  CHECK(item("ec", "check.part_not_ready")["args"]["name"] == b["name"]);
  // A channel's line filled by hand, and the Stop
  b["channel"] = 3;
  REQUIRE(s.robot().putCanister(b).status == 200);
  s.world().emptyLine(4);
  REQUIRE(s.robot().prime(4, 5).status == 200);
  CHECK(item("all", "check.channel_job")["ok"] == false);
  REQUIRE(s.robot().stop(say("stop.app")).status == 200);
  CHECK(item("all", "check.stopped")["ok"] == false);
  REQUIRE(s.robot().resume().status == 200);
  CHECK(item("all", "check.not_stopped")["ok"] == true);
}

TEST_CASE("Maintenance: the cycle holds and the feed pump is off, probe jumps count as the work, the targets are not judged; it ends by hand, by time or with a restart (RAT-042)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure"; }, 3 * 60));
  CHECK(s.robot().maintenance(-1).status == 422);
  CHECK(s.robot().maintenance(Robot::kMaxMaintenanceMin + 1).status == 422);
  REQUIRE(s.robot().maintenance(30).status == 200);
  json j = st(s);
  CHECK(j["maintenanceUntil"].is_number_integer());
  CHECK(j["cycle"]["phase"] == "wait");
  CHECK(j["cycle"]["line"]["key"] == "cycle.wait.maintenance");
  CHECK_FALSE(s.world().output);  // the pump is off at once
  CHECK(hasEvent(s, "ev.maintenance"));
  // Someone dips the pH probe into a buffer: the jump is the work, no jump lock (RAT-042)
  s.control("buffer", {{"kind", "ph"}, {"value", 4.0}});
  s.step(30 * kSecond);
  s.control("buffer", {{"kind", "ph"}});
  s.step(10 * kSecond);
  j = st(s);
  CHECK(j["jumpLocks"].empty());
  CHECK_FALSE(s.world().output);
  for (const auto& i : j["watchdog"]["items"])
    if (i["id"] == "target.ph" || i["id"] == "target.ec") CHECK(i["text"]["key"] == "watch.maintenance");
  // Ended by hand: a new check starts
  REQUIRE(s.robot().maintenance(0).status == 200);
  CHECK(st(s)["maintenanceUntil"].is_null());
  CHECK(hasEvent(s, "ev.maintenance_end"));
  REQUIRE(runUntil(s, [&] { return phase(s) == "prove_flow" || phase(s) == "flush"; }, 30));
  // Over by time
  REQUIRE(s.robot().maintenance(1).status == 200);
  REQUIRE(runUntil(s, [&] { return st(s)["maintenanceUntil"].is_null(); }, 70));
  CHECK(hasEvent(s, "ev.maintenance_over"));
  CHECK(st(s)["cycle"]["line"]["key"] != "cycle.wait.maintenance");
  // In memory only: a restart ends it (R6)
  REQUIRE(s.robot().maintenance(30).status == 200);
  s.reboot();
  CHECK(st(s)["maintenanceUntil"].is_null());
  // Outside maintenance the same jump locks dosing (RAT-039)
  sim::Simulation t(demo());
  REQUIRE(t.robot().stop(say("stop.app")).status == 200);  // nothing the robot does announces a change
  t.step(2 * kMinute);
  t.control("buffer", {{"kind", "ph"}, {"value", 4.0}});
  t.step(30 * kSecond);
  CHECK_FALSE(st(t)["jumpLocks"].empty());
}

TEST_CASE("Canisters: the level is counted run by run; low and used up are problems; a dose the count cannot cover waits for a refill (SD-119, RAT-071)") {
  sim::Simulation s(demo());
  Robot& r = s.robot();
  // The demo's canisters are full: 1 L of each nutrient, 0.5 L of pH−
  CHECK(st(s)["stock"]["part_a"]["ml"] == 1000);
  CHECK(st(s)["stock"]["ph_down"]["ml"] == 500);
  CHECK(st(s)["stock"]["part_a"]["status"] == "ok");

  // A dose the count cannot cover is not started: the check waits and names the canister
  REQUIRE(r.setStock("part_a", {{"ml", 0.2}}).status == 200);
  REQUIRE(runUntil(s, [&] { return st(s)["cycle"]["line"]["key"] == "cycle.wait.stock"; }, 15 * 60));
  CHECK_FALSE(dosing(s));
  CHECK(st(s)["cycle"]["line"]["args"]["name"] == "Part A");
  // Marked refilled: its size again, and the check starts anew at once
  REQUIRE(r.setStock("part_a", {{"full", true}}).status == 200);
  CHECK(hasEvent(s, "ev.stock.set"));
  CHECK(phase(s) != "wait");

  // To the first hold: every dose taken from its canister, as the dose events book it
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
  std::map<std::string, double> dosed;
  for (const auto& e : r.events(0, std::numeric_limits<Epoch>::max(), "dose", 1000))
    if (e["title"]["key"] == "ev.dose") dosed[e["data"]["canister"]] += e["data"]["ml"].get<double>();
  REQUIRE(dosed["part_a"] > 0);
  REQUIRE(dosed["ph_down"] > 0);
  CHECK(st(s)["stock"]["part_a"]["ml"].get<double>() == doctest::Approx(1000 - dosed["part_a"]).epsilon(1e-6));
  CHECK(st(s)["stock"]["part_b"]["ml"].get<double>() == doctest::Approx(1000 - dosed["part_b"]).epsilon(1e-6));
  CHECK(st(s)["stock"]["ph_down"]["ml"].get<double>() == doctest::Approx(500 - dosed["ph_down"]).epsilon(1e-6));
  // An edit keeps the count; a level is never entered below zero, nor "full" without a size
  REQUIRE(r.putCanister({{"id", "calmag"}, {"name", "CalMag 2"}}).status == 200);
  CHECK(st(s)["stock"]["calmag"]["ml"].get<double>() == doctest::Approx(1000 - dosed["calmag"]));
  CHECK(r.setStock("part_b", {{"ml", -5}}).status == 422);
  CHECK(r.setStock("nope", {{"ml", 5}}).status == 404);
  REQUIRE(r.putCanister({{"id", "spare"}, {"name", "Spare"}}).status == 200);
  CHECK(st(s)["stock"]["spare"]["status"] == "unknown");  // never assumed full (RAT-015)
  CHECK(r.setStock("spare", {{"full", true}}).status == 422);

  // Low below 150 ml of a nutrient, 20 ml of pH− (RAT-071); used up below zero, reported once
  REQUIRE(r.setStock("part_b", {{"ml", 120}}).status == 200);
  REQUIRE(r.setStock("ph_down", {{"ml", 25}}).status == 200);
  s.step(6 * kSecond);
  auto item = [&](const std::string& id) {
    const json w = st(s)["watchdog"];
    for (const auto& a : w["items"])
      if (a["id"] == id) return a;
    return json(nullptr);
  };
  CHECK(item("stock.part_b")["status"] == "problem");
  CHECK(item("stock.part_b")["text"]["key"] == "watch.stock_low");
  CHECK(item("stock.ph_down")["status"] == "ok");
  CHECK(st(s)["stock"]["part_b"]["status"] == "low");
  // A line fill takes from the canister too; below zero by the count is reported once (SD-119)
  // (a line of air fills until it drips, SD-111)
  REQUIRE(r.setStock("part_b", {{"ml", 2}}).status == 200);
  s.control("lineEmpty", {{"channel", 3}});
  REQUIRE(r.prime(3, 0).status == 200);
  s.step(15 * kSecond, 500);
  REQUIRE(st(s)["channelJob"]["state"] == "done");
  const double fill = r.config().channelFlow(3) * st(s)["channelJob"]["actualS"].get<double>() / 60;
  CHECK(fill > 2);
  CHECK(r.runtime().stockMl.at("part_b") == doctest::Approx(2 - fill).epsilon(0.05));
  REQUIRE(r.cancelChannelJob().status == 200);
  s.control("lineEmpty", {{"channel", 3}});
  REQUIRE(r.prime(3, 0).status == 200);
  s.step(15 * kSecond, 500);
  REQUIRE(r.cancelChannelJob().status == 200);
  int below = 0;
  for (const auto& k : eventKeys(s)) below += k == "ev.stock.below_zero";
  CHECK(below == 1);
  s.step(6 * kSecond);
  CHECK(item("stock.part_b")["text"]["key"] == "watch.stock_empty");
  CHECK(st(s)["stock"]["part_b"]["status"] == "empty");
  bool listed = false;
  const json checks = st(s)["cycle"]["checks"];
  for (const auto& x : checks["ec"]) listed |= x["key"] == "check.stock_empty";
  CHECK(listed);

  // A restart keeps the count; deleting a canister drops it
  const double left = r.runtime().stockMl.at("part_b");
  CHECK(left < 0);
  s.reboot();
  CHECK(s.robot().runtime().stockMl.at("part_b") == doctest::Approx(left));
  Robot& again = s.robot();
  REQUIRE(again.setStock("spare", {{"ml", 50}}).status == 200);
  REQUIRE(again.deleteCanister("spare").status == 200);
  CHECK(again.runtime().stockMl.count("spare") == 0);
  // A backup keeps the count of a canister the robot had and the file keeps, and only that
  json file = again.configJson();
  REQUIRE(again.putCanister({{"id", "spare"}, {"name", "Spare"}, {"stockMl", 50}}).status == 200);
  REQUIRE(again.importConfig(file).status == 200);
  CHECK(again.runtime().stockMl.count("spare") == 0);
  CHECK(again.runtime().stockMl.at("part_b") == doctest::Approx(left));
}

namespace {

// A nutrient round cut short: part A's pump blocks, so part B (first in the recipe) goes in alone;
// then a Stop ends the round and the robot works out what the pair misses
void cutPair(sim::Simulation& s) {
  s.control("channelFault", {{"channel", 2}, {"blocked", true}});
  REQUIRE(runUntil(s, [&] { return hasEvent(s, "ev.dose_incomplete"); }, 3 * 3600));
  s.control("channelFault", {{"channel", 2}, {"blocked", false}});
  REQUIRE(s.robot().stop(say("stop.app")).status == 200);
  REQUIRE(hasEvent(s, "ev.cycle.pair_short"));
}

double dosedOf(sim::Simulation& s, const std::string& canister) {
  double ml = 0;
  for (const auto& e : s.robot().events(0, std::numeric_limits<Epoch>::max(), "dose", 1000))
    if (e["data"].value("canister", "") == canister && e["data"]["ml"].is_number()) ml += e["data"]["ml"].get<double>();
  return ml;
}

}  // namespace

TEST_CASE("Pairs: a round cut short with a part missing catches it up first, alone, and learns nothing from it (SD-114, RAT-054, RAT-057)") {
  sim::Simulation s(demo());
  cutPair(s);
  json cu = st(s)["catchUp"];
  REQUIRE(cu.is_object());
  REQUIRE(cu["parts"].size() == 1);
  CHECK(cu["parts"][0]["canister"] == "part_a");
  // Part A misses what part B got beyond its own short run: the same ml/L (the recipe has 2 + 2 ml/L)
  const double missing = cu["parts"][0]["ml"], aBefore = dosedOf(s, "part_a");
  CHECK(aBefore < 0.5);
  CHECK(missing == doctest::Approx(dosedOf(s, "part_b") - aBefore).epsilon(0.01));
  // The next nutrient round doses part A alone, then the cycle goes on as usual
  REQUIRE(s.robot().resume().status == 200);
  REQUIRE(runUntil(s, [&] { return st(s)["cycle"]["line"]["key"] == "cycle.dose_catchup"; }, 2 * 3600));
  CHECK(st(s)["catchUp"].is_null());
  const double b = dosedOf(s, "part_b"), cal = dosedOf(s, "calmag");
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 10 * 60));
  CHECK(dosedOf(s, "part_a") == doctest::Approx(aBefore + missing).epsilon(0.01));
  CHECK(dosedOf(s, "part_b") == b);
  CHECK(dosedOf(s, "calmag") == cal);
  REQUIRE(runUntil(s, [&] { return phase(s) == "measure" || phase(s) == "dose" || phase(s) == "hold"; }, 2 * 3600));
  CHECK(s.robot().runtime().ecEffects.count("two_part") == 0);  // nothing learned from the catch-up
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
  // The pair keeps its ratio in the end
  CHECK(dosedOf(s, "part_a") == doctest::Approx(dosedOf(s, "part_b")).epsilon(0.02));
}

TEST_CASE("Pairs: a restart while a part doses counts its cut run as gone in; the other part is caught up (SD-114, R6)") {
  sim::Simulation s(demo());
  REQUIRE(runUntil(s, [&] { return dosing(s) && st(s)["dosing"]["canister"] == "part_b"; }, 3600));
  s.step(2 * kSecond);
  const double planned = s.robot().runtime().pairRound["parts"]["part_b"]["planned"];
  s.reboot();
  CHECK(hasEvent(s, "ev.boot.cut_dose"));
  // Part B counted in full, part A owed in full, once the round is settled
  REQUIRE(runUntil(s, [&] { return st(s)["cycle"]["line"]["key"] == "cycle.dose_catchup"; }, 3 * 3600));
  CHECK(hasEvent(s, "ev.cycle.pair_short"));
  REQUIRE(runUntil(s, [&] { return phase(s) == "mix"; }, 10 * 60));
  CHECK(dosedOf(s, "part_a") == doctest::Approx(planned).epsilon(0.03));
}

TEST_CASE("Pairs: a catch-up is dropped with a note when a person ends it, a new tank starts or the recipe changes; a part that cannot dose holds the pair (SD-114)") {
  SUBCASE("ended in the app") {
    sim::Simulation s(demo());
    cutPair(s);
    REQUIRE(s.robot().endCatchUp().status == 200);
    CHECK(st(s)["catchUp"].is_null());
    CHECK(hasEvent(s, "ev.cycle.catchup_dropped"));
    CHECK(s.robot().endCatchUp().status == 404);
  }
  SUBCASE("a new tank") {
    sim::Simulation s(demo());
    cutPair(s);
    REQUIRE(s.robot().newTank().status == 200);
    CHECK(st(s)["catchUp"].is_null());
    CHECK(hasEvent(s, "ev.cycle.catchup_dropped"));
  }
  SUBCASE("another recipe in force") {
    sim::Simulation s(demo());
    cutPair(s);
    REQUIRE(s.robot().putRecipe({{"id", "calmag_only"}, {"name", "CalMag only"}, {"ecTarget", 1.6},
                                 {"steps", json::array({{{"canister", "calmag"}, {"mlPerL", 2.0}}})}}).status == 200);
    REQUIRE(s.robot().setActive({{"recipe", "calmag_only"}}).status == 200);
    REQUIRE(s.robot().resume().status == 200);
    REQUIRE(runUntil(s, [&] { return hasEvent(s, "ev.cycle.catchup_dropped"); }, 3 * 3600));
    CHECK(st(s)["catchUp"].is_null());
  }
  SUBCASE("a missing part on no channel holds the other part") {
    sim::Simulation s(demo());
    cutPair(s);
    const double b = dosedOf(s, "part_b");
    REQUIRE(s.robot().putCanister({{"id", "part_a"}, {"channel", 0}}).status == 200);
    REQUIRE(s.robot().resume().status == 200);
    REQUIRE(runUntil(s, [&] { return st(s)["cycle"]["line"]["key"] == "cycle.wait.catchup_part"; }, 3 * 3600));
    CHECK(st(s)["cycle"]["line"]["args"]["name"] == "Part A");
    CHECK(st(s)["catchUp"].is_object());
    CHECK(dosedOf(s, "part_b") == b);
  }
}

TEST_CASE("Cycle: an EC round aims below its target by the reserve for the pH− dose that follows (RAT-053)") {
  sim::Simulation s(demo());
  // The fresh fill is above the pH range, so the rounds after the test dose keep a reserve
  REQUIRE(runUntil(s, [&] { return st(s)["cycle"]["line"]["key"] == "cycle.dose_ec"; }, 8 * 3600));
  const json j = st(s);
  const double ph = j["cycle"]["last"]["ph"], aim = j["cycle"]["line"]["args"]["aim"];
  REQUIRE(ph > 6.0 + 0.15);
  // Until the pH− effect is learned, at half the start effect, without a cap; never below the EC
  // floor and its tolerance (SD-121)
  const double eff = j["effects"]["ph"].is_number() ? j["effects"]["ph"].get<double>() : 2.5;
  CHECK(aim == doctest::Approx(std::max(1.6 - (ph - 6.0) / eff * 1.04, 0.5 + 0.1)).epsilon(0.001));
  CHECK(aim < 1.6);
}

TEST_CASE("Cycle: a nutrient round too small for a pump is not dosed; the check notes it and goes on to pH (SD-122, RAT-050)") {
  sim::Simulation s(demo());
  Robot& r = s.robot();
  REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
  // A trace part: a round for a small gap would run it well under 1 s
  REQUIRE(r.putRecipe({{"id", "trace"}, {"name", "Trace"}, {"ecTarget", 1.6}, {"ph", {5.8, 6.2}},
                       {"steps", json::array({{{"canister", "part_b"}, {"mlPerL", 2.0}},
                                              {{"canister", "part_a"}, {"mlPerL", 2.0}},
                                              {{"canister", "calmag"}, {"mlPerL", 0.03}}})}}).status == 200);
  REQUIRE(r.setActive({{"recipe", "trace"}}).status == 200);
  auto phDoses = [&] {
    int n = 0;
    for (const auto& e : r.events(0, std::numeric_limits<Epoch>::max(), "dose", 1000)) n += e["data"].value("canister", "") == "ph_down";
    return n;
  };
  const int before = phDoses();
  s.control("fill", {{"volumeL", 100}, {"ec", 1.2}, {"ph", 6.5}});
  auto noted = [&] {
    const json notes = st(s)["cycle"]["notes"];
    for (const auto& n : notes)
      if (n["key"] == "cycle.note.ec_too_small") return true;
    return false;
  };
  REQUIRE(runUntil(s, noted, 3 * 3600));
  REQUIRE(runUntil(s, [&] { return phDoses() > before; }, 30 * 60));
}

TEST_CASE("Cycle: pH waits below the EC floor and after a nutrient dose, and is dosed only on a fresh reading (RAT-046, RAT-043)") {
  SUBCASE("below the EC floor, and with readings that stopped") {
    sim::Simulation s(demo());
    Robot& r = s.robot();
    // pH only: a recipe without an EC target doses no nutrients (R5)
    REQUIRE(r.putRecipe({{"id", "ph_only"}, {"name", "pH only"}, {"ph", {5.8, 6.2}},
                         {"steps", json::array({{{"canister", "part_a"}, {"mlPerL", 2.0}}, {{"canister", "part_b"}, {"mlPerL", 2.0}}})}}).status == 200);
    REQUIRE(r.setActive({{"recipe", "ph_only"}}).status == 200);
    auto noted = [&](const char* key) {
      const json notes = st(s)["cycle"]["notes"];
      for (const auto& n : notes)
        if (n["key"] == key) return true;
      return false;
    };
    auto phDosed = [&] {
      for (const auto& e : r.events(0, std::numeric_limits<Epoch>::max(), "dose", 1000))
        if (e["data"].value("canister", "") == "ph_down") return true;
      return false;
    };
    // Weak solution: the probe reads unreliably, nothing is dosed
    s.control("fill", {{"volumeL", 100}, {"ec", 0.3}, {"ph", 7.6}});
    REQUIRE(r.newTank().status == 200);
    REQUIRE(runUntil(s, [&] { return noted("cycle.note.ph_gate"); }, 30 * 60));
    s.step(30 * kMinute);
    CHECK_FALSE(phDosed());
    // Enough ions, but the readings stopped: still nothing on the old values
    s.control("fill", {{"volumeL", 100}, {"ec", 1.2}, {"ph", 7.6}});
    s.control("probeFault", {{"fault", "offline"}});
    REQUIRE(r.newTank().status == 200);
    s.step(30 * kMinute);
    CHECK_FALSE(phDosed());
    CHECK(st(s)["readings"]["ph"]["usable"] == false);
    // Fresh readings again: pH is lowered
    s.control("probeFault", {{"fault", ""}});
    REQUIRE(runUntil(s, [&] { return phDosed(); }, 60 * 60));
  }
  SUBCASE("240 s after a nutrient dose") {
    sim::Simulation s(demo());
    Robot& r = s.robot();
    // Once the test dose found the volume, the shortest wait after nutrients: a pH dose could follow soon
    REQUIRE(runUntil(s, [&] { return hasEvent(s, "ev.cycle.test_ok"); }, 4 * 3600));
    REQUIRE(r.putTank({{"mixMinMin", 0}}).status == 200);
    // Right after a nutrient dose the checklist says pH rests
    REQUIRE(runUntil(s, [&] { const json c = st(s)["cycle"]; return phase(s) == "mix" && c["round"].is_object() && c["round"]["kind"] == "ec"; }, 2 * 3600));
    bool rests = false;
    const json checks = st(s)["cycle"]["checks"];
    for (const auto& x : checks["ph"]) rests |= x["key"] == "check.rest_left" && x["ok"] == false;
    CHECK(rests);
    REQUIRE(runUntil(s, [&] { return phase(s) == "hold"; }, 8 * 3600));
    CHECK(hasEvent(s, "ev.cycle.ec_round"));
    Epoch lastEc = 0;
    int ph = 0;
    const json ev = r.events(0, std::numeric_limits<Epoch>::max(), "dose", 1000);
    for (auto it = ev.rbegin(); it != ev.rend(); ++it) {  // oldest first
      const std::string canister = (*it)["data"].value("canister", "");
      if (canister == "ph_down") {
        ph++;
        CHECK((*it)["ts"].get<Epoch>() - lastEc >= 240);
      } else if (!canister.empty()) {
        lastEc = (*it)["ts"];
      }
    }
    CHECK(ph > 0);
  }
}

TEST_CASE("Flow by the device's method: an outlet level sensor proves it while it reports water and the feed pump runs; a flow meter at 0.5 L/min (SD-109)") {
  fake::Hardware hw;
  fake::Clock clock;
  MemoryStorage store;
  Robot r(hw, store, clock, [](std::uint8_t* p, size_t n) { std::fill(p, p + n, std::uint8_t{7}); });
  r.boot();
  // The feed pump as the hardware reports it after the tick (without a recipe the check switches it off)
  auto flowOk = [&](bool suction) {
    clock.ms += 1000;
    r.tick();
    hw.out = suction;
    return r.state()["flowOk"].get<bool>();
  };
  hw.prof.flow = "outlet_level";
  hw.water.outletWet = true;
  CHECK_FALSE(flowOk(false));  // water at the outlet, but not drawn by the feed pump
  CHECK(flowOk(true));
  hw.water.outletWet = false;
  CHECK_FALSE(flowOk(true));
  hw.water.outletWet.reset();  // no reading from the sensor: no flow (R5)
  CHECK_FALSE(flowOk(true));
  hw.prof.flow = "turbine";
  hw.set(Signal::Flow, 0.4, clock.ms + 1000);
  CHECK_FALSE(flowOk(true));
  hw.set(Signal::Flow, 6.0, clock.ms + 1000);
  CHECK(flowOk(true));
}
