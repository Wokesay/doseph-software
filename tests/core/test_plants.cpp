// SPDX-License-Identifier: AGPL-3.0-or-later
// Plants and their phases: the water plan from the leading plant, the phases
// moving on at local midnight, the harvest and the end, and the notes they
// leave for the user (SD-128).
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "doseph/alarms.hpp"
#include "doseph/localtime.hpp"
#include "doseph/plants.hpp"
#include "simulation.hpp"

using namespace doseph;

namespace {

constexpr Epoch kT0 = 1798761600;  // 2027-01-01 00:00 UTC, 01:00 in Berlin
constexpr Epoch kDay = 86400;
const std::string kZone = "Europe/Berlin";

PhaseCfg phase(const std::string& name, int days, bool confirm, const std::string& recipe = "r1", double ec = kNaN) {
  PhaseCfg p;
  p.name = name;
  p.days = days;
  p.confirm = confirm;
  p.recipe = recipe;
  p.ecTarget = ec;
  return p;
}

Config withRecipes() {
  Config c;
  RecipeCfg r1;
  r1.id = "r1";
  r1.name = "Grow";
  r1.ecTarget = 1.6;
  r1.phLow = 5.8;
  r1.phHigh = 6.2;
  RecipeCfg r2 = r1;
  r2.id = "r2";
  r2.name = "Bloom";
  r2.ecTarget = 2.0;
  c.recipes = {r1, r2};
  return c;
}

PlantCfg plant(const std::string& id, Epoch start, std::vector<PhaseCfg> phases) {
  PlantCfg p;
  p.id = id;
  p.name = "Plant " + id;
  p.phases = std::move(phases);
  p.start = p.phaseStart = localMidnight(start, kZone);
  return p;
}

bool hasNote(const std::vector<PlantNote>& notes, const std::string& id) {
  return std::any_of(notes.begin(), notes.end(), [&](const PlantNote& n) { return n.id == id; });
}

sim::Options demo() {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  o.startEpoch = kT0;
  return o;
}

}  // namespace

TEST_CASE("Plants: the leading plant's phase sets the water plan, with its own EC target and pH range where it has them; without a running plant the recipe chosen by hand applies (SD-128)") {
  Config c = withRecipes();
  c.active.recipe = "r2";
  WaterPlan w = waterPlan(c, kT0);
  CHECK(w.source == "hand");
  CHECK(w.recipe->id == "r2");
  CHECK(w.ecTarget == doctest::Approx(2.0));
  PhaseCfg own = phase("A", 7, false, "r1", 1.2);
  own.phLow = 5.6;
  own.phHigh = 6.0;
  c.plants = {plant("p1", kT0, {own, phase("B", 7, false)})};
  w = waterPlan(c, kT0);
  CHECK(w.source == "plant");
  CHECK(w.plant->id == "p1");
  CHECK(w.recipe->id == "r1");
  CHECK(w.ecTarget == doctest::Approx(1.2));
  CHECK(w.phTarget == doctest::Approx(5.8));
  // The next phase has none of its own: the recipe's
  c.plants[0].phase = 1;
  w = waterPlan(c, kT0);
  CHECK(w.ecTarget == doctest::Approx(1.6));
  CHECK(w.phTarget == doctest::Approx(6.0));
  // Completed, it leads no more: the hand choice again
  c.plants[0].completedAt = kT0;
  CHECK(waterPlan(c, kT0).source == "hand");
}

TEST_CASE("Plants: a phase moves on by itself at the local midnight after its days, also after the device was off; day 1 is the day it started (SD-128)") {
  Config c = withRecipes();
  // Started in the evening: that day counts as day 1
  c.plants = {plant("p1", kT0 + 20 * 3600, {phase("A", 3, false), phase("B", 2, false), phase("C", 5, false, "r2")})};
  const Epoch day1 = c.plants[0].start;
  CHECK(localTime(day1, kZone).minuteOfDay == 0);
  const Epoch endA = phaseEnd(c.plants[0], kZone);
  CHECK(endA == localDaysLater(day1, 3, kZone));
  CHECK(advancePlants(c, endA - 1, kZone).empty());
  CHECK(c.plants[0].phase == 0);
  CHECK(advancePlants(c, endA, kZone) == std::vector<std::string>{"p1"});
  CHECK(c.plants[0].phase == 1);
  CHECK(c.plants[0].phaseStart == endA);
  // Off for a week: both ends passed, it catches up and stays in the last phase
  c.plants[0].phase = 0;
  c.plants[0].phaseStart = day1;
  advancePlants(c, day1 + 12 * kDay, kZone);
  CHECK(c.plants[0].phase == 2);
  CHECK(c.plants[0].phaseStart == localDaysLater(day1, 5, kZone));
  // Across the change to summer time a day is 23 h, and the end still falls on a local midnight
  Config s = withRecipes();
  s.plants = {plant("p2", 1806192000, {phase("A", 3, false), phase("B", 3, false)})};  // 2027-03-27
  const Epoch end = phaseEnd(s.plants[0], kZone);
  CHECK(localTime(end, kZone).minuteOfDay == 0);
  CHECK(end - s.plants[0].phaseStart == 3 * kDay - 3600);
}

TEST_CASE("Plants: a phase that waits for its confirmation stays after its days and reminds daily; the last phase reminds to record the harvest; a phase begun today and a plant with other targets are said (SD-128, RAT-002)") {
  Config c = withRecipes();
  c.plants = {plant("p1", kT0, {phase("A", 2, true), phase("B", 2, false)})};
  const Epoch end = phaseEnd(c.plants[0], kZone);
  CHECK(advancePlants(c, end + 3 * kDay, kZone).empty());  // never on its own
  CHECK(c.plants[0].phase == 0);
  auto notes = plantNotes(c, end + 3 * kDay, kZone);
  REQUIRE(hasNote(notes, "plant.p1.confirm"));
  CHECK(notes[0].text.args["days"] == 4);
  CHECK_FALSE(hasNote(plantNotes(c, end - 1, kZone), "plant.p1.confirm"));
  // The last phase over: the harvest is the user's, a reminder until it is recorded
  c.plants[0].phase = 1;
  c.plants[0].phaseStart = localMidnight(end + 3 * kDay, kZone);
  CHECK(hasNote(plantNotes(c, end + 3 * kDay, kZone), "plant.p1.phase"));  // begun today
  const Epoch last = phaseEnd(c.plants[0], kZone);
  notes = plantNotes(c, last + kDay, kZone);
  CHECK(hasNote(notes, "plant.p1.harvest"));
  CHECK_FALSE(hasNote(notes, "plant.p1.confirm"));
  c.plants[0].harvestAt = last + kDay;
  CHECK_FALSE(hasNote(plantNotes(c, last + 2 * kDay, kZone), "plant.p1.harvest"));
  // A second plant on the same tank that asks for other water is said; the notes are notes
  c.plants.push_back(plant("p2", kT0 + kDay, {phase("X", 10, false, "r2")}));
  notes = plantNotes(c, last + 2 * kDay, kZone);
  CHECK(hasNote(notes, "plant.p2.differs"));
  CHECK(levelOf("plant.p2.differs") == Level::Info);
  CHECK(levelOf("plant.p1.harvest") == Level::Info);
}

TEST_CASE("Plants: renaming a phase changes nothing the robot does; logic reads a phase's parameters, never its name (RAT-076)") {
  Config c = withRecipes();
  c.plants = {plant("p1", kT0, {phase("Vegetative", 3, false), phase("Flowering", 3, false, "r2")})};
  Config d = c;
  d.plants[0].phases[0].name = "Erste";
  d.plants[0].phases[1].name = "Zweite";
  for (Epoch t : {kT0, kT0 + 4 * kDay}) {
    advancePlants(c, t, kZone);
    advancePlants(d, t, kZone);
    CHECK(c.plants[0].phase == d.plants[0].phase);
    const WaterPlan a = waterPlan(c, t), b = waterPlan(d, t);
    CHECK(a.recipe == c.recipe(b.recipe->id));
    CHECK(a.ecTarget == doctest::Approx(b.ecTarget));
  }
}

TEST_CASE("Plants over the API: a plant started from a plant recipe copies its phases, leads and ends the hand choice; the next phase, the harvest and the end are the user's; once none runs, the robot holds (SD-128, RAT-077)") {
  sim::Simulation s(demo());
  Robot& r = s.robot();
  s.step(10 * kSecond);
  json pr = {{"name", "Tomatoes"},
             {"phases", json::array({{{"name", "Growth"}, {"days", 21}, {"recipe", "two_part"}, {"ecTarget", 1.2}},
                                     {{"name", "Fruit"}, {"days", 56}, {"confirm", true}, {"recipe", "two_part"}}})}};
  Result res = r.putPlantRecipe(pr);
  REQUIRE(res.status == 200);
  const std::string prId = res.body["id"];
  // A phase without a recipe there is, or without days, is refused
  json bad = pr;
  bad["phases"][0]["recipe"] = "none";
  CHECK(r.putPlantRecipe(bad).status == 422);
  bad = pr;
  bad["phases"][0]["days"] = 0;
  CHECK(r.putPlantRecipe(bad).status == 422);
  CHECK(r.configJson()["active"]["recipe"] == "two_part");  // the demo's hand choice
  res = r.startPlant({{"name", "Bed 1"}, {"plantRecipe", prId}});
  REQUIRE(res.status == 200);
  const std::string p1 = res.body["id"];
  json pl = r.plants();
  CHECK(pl["lead"] == p1);
  CHECK(pl["plan"]["source"] == "plant");
  CHECK(pl["plants"][0]["day"] == 1);
  CHECK(pl["plants"][0]["ecTarget"] == 1.2);
  CHECK(r.configJson()["active"]["recipe"] == "");  // the hand choice ended with it
  CHECK(r.state()["cycle"]["settings"]["ecTarget"] == 1.2);
  CHECK(r.setActive({{"recipe", "two_part"}}).status == 409);
  // A change of the plant recipe does not move the plant
  pr["id"] = prId;
  pr["phases"][0]["days"] = 7;
  REQUIRE(r.putPlantRecipe(pr).status == 200);
  CHECK(r.plants()["plants"][0]["phases"][0]["days"] == 21);
  // A recipe a plant names stays
  CHECK(r.deleteRecipe("two_part").status == 409);
  // The next phase by hand: today is its day 1; the last one has no next
  REQUIRE(r.plantNext(p1).status == 200);
  CHECK(r.plants()["plants"][0]["phase"] == 1);
  CHECK(r.state()["cycle"]["settings"]["ecTarget"] == 1.6);
  CHECK(r.plantNext(p1).status == 409);
  // A second plant: the first still leads; leading passes on request
  res = r.startPlant({{"name", "Bed 2"}, {"plantRecipe", prId}, {"start", kT0 - 30 * kDay}});
  REQUIRE(res.status == 200);
  const std::string p2 = res.body["id"];
  CHECK(r.plants()["lead"] == p1);
  // Started 30 days ago: its 21 days of growth are over, it moved on by itself
  CHECK(r.plants()["plants"][1]["phase"] == 1);
  CHECK(r.startPlant({{"name", "Later"}, {"plantRecipe", prId}, {"start", kT0 + kDay}}).status == 422);
  // Harvest and the end: the user's; the next running plant leads
  CHECK(r.plantHarvest(p1, {{"at", kT0 + 3600}}).status == 422);  // not in the future
  REQUIRE(r.plantHarvest(p1, json::object()).status == 200);
  CHECK(r.plants()["plants"][0]["harvestAt"].get<Epoch>() > 0);
  CHECK(r.plants()["plants"][0]["running"] == true);  // harvested, not yet completed
  REQUIRE(r.plantComplete(p1).status == 200);
  CHECK(r.plants()["lead"] == p2);
  CHECK(r.plantNext(p1).status == 409);
  // The last plant completed: the robot holds, no old hand choice comes back
  REQUIRE(r.plantComplete(p2).status == 200);
  CHECK(r.plants()["plan"]["source"] == "");
  s.step(10 * kSecond);
  CHECK(r.state()["cycle"]["phase"] == "off");
  CHECK(r.state()["cycle"]["line"]["key"] == "cycle.off.no_recipe");
  // A completed plant is kept until removed
  REQUIRE(r.deletePlant(p1).status == 200);
  CHECK(r.plants()["plants"].size() == 1);
  CHECK(r.deletePlant(p1).status == 404);
}

TEST_CASE("Plants over the API: at the local midnight after its days a phase moves on, the event says so, and the morning's notes name it (SD-128, SD-123)") {
  sim::Simulation s(demo());
  Robot& r = s.robot();
  s.step(10 * kSecond);
  json pr = {{"name", "Herbs"},
             {"phases", json::array({{{"name", "First"}, {"days", 1}, {"recipe", "two_part"}, {"ecTarget", 1.2}},
                                     {{"name", "Second"}, {"days", 30}, {"recipe", "two_part"}}})}};
  const std::string prId = r.putPlantRecipe(pr).body["id"];
  REQUIRE(r.startPlant({{"name", "Pot"}, {"plantRecipe", prId}}).status == 200);
  // kT0 is 01:00 in Berlin: the first day ends at the next local midnight
  const Epoch midnight = localDaysLater(localMidnight(kT0, kZone), 1, kZone);
  s.step((midnight - r.now() - 60) * kSecond, 5000);
  CHECK(r.plants()["plants"][0]["phase"] == 0);
  s.step(2 * kMinute, 1000);
  CHECK(r.plants()["plants"][0]["phase"] == 1);
  CHECK(r.state()["cycle"]["settings"]["ecTarget"] == 1.6);
  const json ev = r.events(0, std::numeric_limits<Epoch>::max(), "", 200);
  CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.plant.phase"; }));
  bool noted = false;
  const json alarms = r.state()["alarms"];
  for (const auto& a : alarms)
    if (a["id"].get<std::string>().find(".phase") != std::string::npos) noted = true;
  s.step(6 * kMinute, 1000);
  const json later = r.state()["alarms"];
  for (const auto& a : later)
    if (a["id"].get<std::string>().find(".phase") != std::string::npos) noted = a["level"] == "info";
  CHECK(noted);
}

TEST_CASE("Plants: the calendar lists the next weeks by local day: phases that begin, a phase that waits for confirmation, the planned end, and the maintenance dates; one past its plan stands at today (SD-128, SD-125)") {
  Config c = withRecipes();
  c.plants = {plant("p1", kT0, {phase("A", 3, false), phase("B", 4, true), phase("C", 5, false, "r2")})};
  const Epoch day1 = c.plants[0].start;
  const json d = plantDates(c, kT0, localDaysLater(day1, 56, kZone), kZone);
  REQUIRE(d.size() == 3);
  CHECK(d[0]["kind"] == "phase");
  CHECK(d[0]["phase"] == "B");
  CHECK(d[0]["date"] == localDaysLater(day1, 3, kZone));
  CHECK(d[1]["kind"] == "confirm");
  CHECK(d[1]["phase"] == "C");
  CHECK(d[1]["date"] == localDaysLater(day1, 7, kZone));
  CHECK(d[2]["kind"] == "harvest");
  CHECK(d[2]["date"] == localDaysLater(day1, 12, kZone));
  // B over by two days and not confirmed: it stands at today, overdue, and C follows from today
  c.plants[0].phase = 1;
  c.plants[0].phaseStart = localDaysLater(day1, 3, kZone);
  const Epoch now = localDaysLater(day1, 9, kZone) + 3600;
  const json o = plantDates(c, now, localDaysLater(day1, 60, kZone), kZone);
  REQUIRE(o.size() == 2);
  CHECK(o[0]["date"] == localMidnight(now, kZone));
  CHECK(o[0]["overdue"] == true);
  CHECK(o[1]["date"] == localDaysLater(localMidnight(now, kZone), 5, kZone));
  // Over the API, with the maintenance dates and the water plan in the state
  sim::Simulation s(demo());
  s.step(10 * kSecond);
  const json cal = s.robot().calendar(56);
  CHECK(std::any_of(cal["entries"].begin(), cal["entries"].end(), [](const json& e) { return e["kind"] == "maintenance"; }));
  CHECK(s.robot().state()["plan"]["source"] == "hand");
  CHECK(s.robot().state()["plan"]["recipe"] == "two_part");
}
