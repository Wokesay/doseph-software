// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>

#include "doseph/config.hpp"

using namespace doseph;

namespace {

Config sample() {
  Config c;
  c.tank.volumeL = 100;
  c.canisters = {{"down", "pH−", "ph_down", 1, "", "#c00"},
                 {"a", "Part A", "nutrient", 2, "AB", "#00c"},
                 {"b", "Part B", "nutrient", 3, "AB", "#0c0"},
                 {"cm", "CalMag", "nutrient", 4, "", "#cc0"}};
  RecipeCfg r;
  r.id = "r1";
  r.name = "Veg";
  r.steps = {{"b", 2.0, false}, {"a", 2.0, false}, {"cm", 0.5, false}};
  r.ecTarget = 1.6;
  r.phLow = 5.8;
  r.phHigh = 6.2;
  c.recipes = {r};
  c.channels[2] = {50.0, 1, "S"};
  return c;
}

bool has(const std::vector<Msg>& errs, const std::string& key) {
  return std::any_of(errs.begin(), errs.end(), [&](const Msg& m) { return m.key == key; });
}

}  // namespace

TEST_CASE("Configuration: there and back through JSON; a missing number stays missing (R5, RAT-006)") {
  Config c = sample();
  c.tank.waitEcMin = kNaN;
  c.recipes[0].phLow = c.recipes[0].phHigh = kNaN;
  json j = c;
  CHECK(j["tank"]["waitEcMin"].is_null());
  CHECK(j["recipes"][0]["ph"].is_null());
  Config back = configFromJson(j);
  CHECK(std::isnan(back.tank.waitEcMin));
  CHECK(std::isnan(back.recipes[0].phLow));
  CHECK(back.tank.volumeL == 100);
  CHECK(back.channelFlow(2) == 50.0);
  CHECK(back.channels.at(2).head == "S");
  CHECK(std::isnan(back.channelFlow(3)));
  CHECK(validateConfig(back, 4).empty());
  // A calibration from before SD-108 names no head: read, it has none, and the robot voids it
  j["channels"]["2"].erase("head");
  CHECK(configFromJson(j).channels.at(2).head.empty());
  CHECK_THROWS(configFromJson(json{{"tank", {{"volumeL", "a hundred"}}}}));
  // A circulation mode from before SD-109 is read over
  CHECK(validateConfig(configFromJson(json{{"tank", {{"volumeL", 100}, {"circulation", "external"}}}}), 4).empty());
  CHECK_FALSE(json(back)["tank"].contains("circulation"));
  CHECK_THROWS(configFromJson(json{{"schemaVersion", 99}}));
}

TEST_CASE("Configuration: a file of schema 2 names pumps; read, they are channels with their calibrations (SD-107)") {
  json v2 = {{"schemaVersion", 2},
             {"canisters", json::array({{{"id", "down"}, {"name", "pH−"}, {"role", "ph_down"}, {"pump", 1}},
                                        {{"id", "a"}, {"name", "Part A"}, {"role", "nutrient"}, {"pump", 3}}})},
             {"pumps", {{"1", {{"flowMlPerMin", 48.5}, {"at", 100}}}, {"3", {{"flowMlPerMin", 51.0}, {"at", 200}}}}}};
  const json m = migrateConfig(v2);
  CHECK(m["schemaVersion"] == kSchemaVersion);
  CHECK_FALSE(m.contains("pumps"));
  CHECK(m["canisters"][1]["channel"] == 3);
  CHECK_FALSE(m["canisters"][1].contains("pump"));
  const Config c = configFromJson(v2);
  REQUIRE(c.canisterOnChannel(3));
  CHECK(c.canisterOnChannel(3)->id == "a");
  CHECK(c.channelFlow(1) == 48.5);
  CHECK(c.channelFlow(3) == 51.0);
  CHECK(std::isnan(c.channelFlow(2)));
  CHECK(json(c)["channels"]["3"]["flowMlPerMin"] == 51.0);
  CHECK(validateConfig(c, 4).empty());
}

TEST_CASE("Configuration: a file of schema 1 names bottles; read, they are canisters with a role (SD-107)") {
  json v1 = {{"schemaVersion", 1},
             {"bottles", json::array({{{"id", "down"}, {"name", "pH−"}, {"kind", "ph_down"}, {"pump", 1}},
                                      {{"id", "a"}, {"name", "Part A"}, {"kind", "nutrient"}, {"pump", 2}, {"pair", "AB"}},
                                      {{"id", "b"}, {"name", "Part B"}, {"kind", "nutrient"}, {"pump", 3}, {"pair", "AB"}}})},
             {"recipes", json::array({{{"id", "r1"},
                                       {"name", "Veg"},
                                       {"steps", json::array({{{"bottle", "b"}, {"mlPerL", 2.0}}, {{"bottle", "a"}, {"mlPerL", 2.0}}})},
                                       {"ecTarget", 1.2},
                                       {"ph", {5.8, 6.2}}}})}};
  const json m = migrateConfig(v1);
  CHECK(m["schemaVersion"] == kSchemaVersion);
  CHECK_FALSE(m.contains("bottles"));
  CHECK(m["canisters"][0]["role"] == "ph_down");
  CHECK_FALSE(m["canisters"][0].contains("kind"));
  CHECK(m["recipes"][0]["steps"][0]["canister"] == "b");
  Config c = configFromJson(v1);
  REQUIRE(c.canisters.size() == 3);
  CHECK(c.canisters[0].role == "ph_down");
  CHECK(c.canisters[1].role == "nutrient");
  CHECK(c.canisters[2].pair == "AB");
  CHECK(c.canisters[1].channel == 2);  // a pump of schema 1 is a channel
  CHECK(c.recipes[0].steps[1].canister == "a");
  CHECK(c.phDown() == &c.canisters[0]);
  CHECK(validateConfig(c, 4).empty());
  // The current schema passes unchanged; an unknown or broken version is refused
  json now = sample();
  CHECK(migrateConfig(now) == now);
  CHECK_THROWS(migrateConfig(json{{"schemaVersion", 0}}));
  CHECK_THROWS(migrateConfig(json{{"schemaVersion", kSchemaVersion + 1}}));
  CHECK_THROWS(migrateConfig(json{{"schemaVersion", "1"}}));
  CHECK_THROWS(migrateConfig(json::array()));
}

TEST_CASE("Configuration: pH correction is never part of a recipe (RAT-004)") {
  Config c = sample();
  c.recipes[0].steps.push_back({"down", 0.1, false});
  CHECK(has(validateConfig(c, 4), "cfg.recipe.ph"));
}

TEST_CASE("Configuration: a tolerance of 0 is refused (RAT-008); settings only within their bounds (R7)") {
  Config c = sample();
  c.control.ecTolerance = 0;
  c.control.phTolerance = 0.01;
  c.limits.maxRunS = 120;
  auto errs = validateConfig(c, 4);
  CHECK(has(errs, "cfg.control.ec_tolerance"));
  CHECK(has(errs, "cfg.control.ph_tolerance"));
  CHECK(has(errs, "cfg.limits.max_run"));
  CHECK(c.limits.bounded().maxRunS == kHardMaxRunS);
}

TEST_CASE("Configuration: a pair keeps all its parts; none of them is fixed (RAT-054, RAT-085)") {
  Config c = sample();
  c.recipes[0].steps.erase(c.recipes[0].steps.begin());  // Part B out
  CHECK(has(validateConfig(c, 4), "cfg.recipe.pair"));
  c = sample();
  c.recipes[0].steps[0].fixed = true;
  CHECK(has(validateConfig(c, 4), "cfg.recipe.fixed_pair"));
  c = sample();
  c.recipes[0].steps[2].fixed = true;  // CalMag fixed: allowed
  CHECK(validateConfig(c, 4).empty());
  // A pair of one: a canister may wait for its partner, but no recipe uses it so
  c = sample();
  c.canisters[2].pair = "";
  c.recipes[0].steps.erase(c.recipes[0].steps.begin());
  CHECK(has(validateConfig(c, 4), "cfg.pair.single"));
  c.recipes.clear();
  CHECK(validateConfig(c, 4).empty());
}

TEST_CASE("Configuration: an EC target far from what the amounts give is refused, naming what fits (RAT-056)") {
  Config c = sample();
  c.recipes[0].ecTarget = 0.2;  // 0.2 / 4.5 ml/L = 0.044 per ml/L, below a quarter of 0.275
  auto errs = validateConfig(c, 4);
  REQUIRE(has(errs, "cfg.recipe.ec_fit"));
  CHECK(errs[0].args["low"].get<double>() == doctest::Approx(0.25 * 0.275 * 4.5));
  c.recipes[0].ecTarget = 21.0;
  CHECK(has(validateConfig(c, 4), "cfg.recipe.ec_range"));
}

TEST_CASE("Configuration: one canister per channel, a channel the device has; the mixing wait within its bounds") {
  Config c = sample();
  c.canisters[3].channel = 2;
  CHECK(has(validateConfig(c, 4), "cfg.canister.channel_twice"));
  c = sample();
  c.canisters[3].channel = 5;
  CHECK(has(validateConfig(c, 4), "cfg.canister.channel"));
  c = sample();
  c.tank.waitEcMin = 1;  // below 2 min (RAT-052)
  CHECK(has(validateConfig(c, 4), "cfg.tank.wait"));
  c.tank.waitEcMin = 30;
  c.tank.holdMin = 5;
  CHECK(has(validateConfig(c, 4), "cfg.tank.hold"));
  c = sample();
  c.channels[3] = {0, 1};  // a calibration of 0 is no calibration (RAT-003)
  CHECK(has(validateConfig(c, 4), "cfg.channel.flow"));
}

TEST_CASE("Configuration: one pH− canister at most, on a channel or not; the rest are nutrients (SD-108)") {
  Config c = sample();
  CHECK(validateConfig(c, 4).empty());
  c.canisters.push_back({"down2", "pH− 2", "ph_down", 0, "", "#c00"});
  CHECK(has(validateConfig(c, 4), "cfg.canister.ph_down_twice"));
  c.canisters.back().role = "nutrient";
  CHECK(validateConfig(c, 4).empty());
}

TEST_CASE("Configuration: the recipe in force exists; its end comes after its start") {
  Config c = sample();
  c.active = {"nope", 100, 0};
  CHECK(has(validateConfig(c, 4), "cfg.active.recipe"));
  c.active = {"r1", 100, 50};
  CHECK(has(validateConfig(c, 4), "cfg.active.until"));
  c.active = {"r1", 100, 0};
  CHECK(validateConfig(c, 4).empty());
  CHECK(phTarget(c.recipes[0]) == doctest::Approx(6.0));
}

TEST_CASE("Run-time state: latches, jump locks and learned effects survive; a damaged one throws (RAT-028, RAT-063)") {
  RuntimeState s;
  s.latches["ec.no_effect"] = {{"at", 5}};
  s.jumpLocks["ph"] = {{"until", 900}, {"from", 6.0}, {"to", 7.2}};
  s.ecEffects["r1"] = 0.31;
  s.phEffect = 3.2;
  s.stopped = true;
  s.newTank = false;
  s.pulledBackMs[2] = 240;
  json j = s;
  RuntimeState back = runtimeFromJson(j);
  CHECK(back.pulledBackMs.at(2) == 240);  // a line pulled back is filled after a restart too (SD-111)
  // A pull back of no sensible length counts as none
  CHECK(runtimeFromJson(json{{"pulledBackMs", {{"2", 5000}, {"x", 100}, {"3", -1}}}}).pulledBackMs.empty());
  CHECK(back.latches.count("ec.no_effect"));
  CHECK(back.jumpLocks["ph"]["until"] == 900);
  CHECK(back.ecEffects["r1"] == doctest::Approx(0.31));
  CHECK(back.phEffect == doctest::Approx(3.2));
  CHECK(back.stopped);
  CHECK_FALSE(back.newTank);
  // A learned value that is no positive number counts as nothing learned (R5)
  CHECK(runtimeFromJson(json{{"ecEffects", {{"r1", -1}}}, {"phEffect", 0}}).ecEffects.empty());
  CHECK_THROWS(runtimeFromJson(json{{"latches", 7}}));
}
