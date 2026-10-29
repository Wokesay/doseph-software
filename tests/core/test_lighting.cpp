// SPDX-License-Identifier: AGPL-3.0-or-later
// Light (SD-135): circuits on Shelly outputs, switched or dimmed over 0–10 V, each with its day of
// light from its own start, sunrise and sunset ramps, the driver's threshold (RAT-013) and 0 % as
// off (RAT-029); a plant's phase sets the hours and the brightness (RAT-076). Light follows its plan
// through a restart, a Stop, a hold and maintenance; by hand until the plan's next change. Driven
// through the API against the twin's dimmer and relay.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "doseph/api.hpp"
#include "doseph/config.hpp"
#include "doseph/lighting.hpp"
#include "doseph/localtime.hpp"
#include "harness.hpp"

using harness::Run;
using doseph::json;
using doseph::kSecond;

namespace {

constexpr int kH = 3600;

doseph::LightCircuitCfg dimmer(int startMin, double hours) {
  doseph::LightCircuitCfg l;
  l.dimmed = true;
  l.startMin = startMin;
  l.hours = hours;
  l.pct = 80;
  l.thresholdPct = 10;
  l.sunriseMin = 30;
  l.sunsetMin = 30;
  return l;
}

sim::FakeShelly::Light& lamp(Run& r) { return r.s.net().shelly("shelly-dimmer.local")->lights[0]; }
json light(Run& r, const std::string& id = "l1") {
  const json all = r.state()["lighting"];  // kept: a loop over a part of a temporary is undefined
  for (const auto& c : all)
    if (c["id"] == id) return c;
  return nullptr;
}
int nowMin(Run& r) {
  const std::string zone = r.ok("GET", "config")["system"]["timezone"];
  return doseph::localTime(r.s.clock().epoch(), zone).minuteOfDay;
}
// The twin's dimmer as light l1, its day begun `ago` minutes before now
void addDimmer(Run& r, int ago, double hours = 12) {
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-dimmer.local"}, {"name", "Lamps"}});
  CHECK(r.ok("PUT", "lighting/circuits",
             {{"name", "Tent"}, {"dimmed", true}, {"startMin", ((nowMin(r) - ago) % 1440 + 1440) % 1440}, {"hours", hours}, {"pct", 80},
              {"thresholdPct", 10}, {"sunriseMin", 30}, {"sunsetMin", 30}})["id"] == "l1");
  r.ok("PUT", "roles/light.l1", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
}

}  // namespace

TEST_CASE("Light: a day from its start for its hours; a dimmer rises over its sunrise and falls over its sunset, never below the driver's threshold, and is off at night (SD-135, RAT-013, RAT-029)") {
  doseph::LightCircuitCfg l = dimmer(6 * 60, 18);  // 06:00 to 24:00
  doseph::LightPoint p = doseph::lightAt(l, 6 * kH - 1);
  CHECK_FALSE(p.day);
  CHECK_FALSE(p.on);
  CHECK(p.pct == 0);  // 0 %: off, the relay too (RAT-029)
  CHECK(p.changeInS == 1);
  p = doseph::lightAt(l, 6 * kH);
  CHECK(p.on);
  CHECK(p.pct == 10);  // the ramp's start, raised to the driver's threshold (RAT-013)
  CHECK(doseph::lightAt(l, 6 * kH + 15 * 60).pct == doctest::Approx(40).epsilon(0.02));
  CHECK(doseph::lightAt(l, 7 * kH).pct == 80);
  CHECK(doseph::lightAt(l, 24 * kH - 15 * 60).pct == doctest::Approx(40).epsilon(0.02));
  p = doseph::lightAt(l, 24 * kH - 1);
  CHECK(p.on);
  CHECK(p.changeInS == 1);
  CHECK_FALSE(doseph::lightAt(l, 0).on);
  // Over midnight: 20:00 to 08:00
  l = dimmer(20 * 60, 12);
  CHECK(doseph::lightAt(l, 2 * kH).on);
  CHECK_FALSE(doseph::lightAt(l, 12 * kH).on);
  // A switch: on by day, off by night
  l.dimmed = false;
  CHECK(doseph::lightAt(l, 21 * kH).on);
  CHECK(doseph::lightAt(l, 21 * kH).pct == 0);
  // 24 h always, 0 h never
  l.hours = 24;
  CHECK(doseph::lightAt(l, 12 * kH).on);
  CHECK(doseph::lightAt(l, 12 * kH).changeInS == 24 * kH);
  l.hours = 0;
  CHECK_FALSE(doseph::lightAt(l, 21 * kH).on);
}

TEST_CASE("Light: a circuit that follows a plant takes the hours and the brightness from its current phase where it has them; without a running plant its own (SD-135, RAT-076)") {
  doseph::Config cfg;
  doseph::PhaseCfg veg, bloom;
  veg.name = "Vegetative";
  veg.lightHours = 18;
  veg.lightPct = 60;
  bloom.name = "Bloom";
  bloom.lightHours = 12;
  doseph::PlantCfg p;
  p.id = "p1";
  p.name = "Bed 1";
  p.phases = {veg, bloom};
  cfg.plants = {p};
  doseph::LightCircuitCfg l = dimmer(6 * 60, 16);
  l.plant = "p1";
  doseph::LightCircuitCfg e = doseph::effectiveLight(cfg, l);
  CHECK(e.hours == 18);
  CHECK(e.pct == 60);
  cfg.plants[0].phase = 1;
  e = doseph::effectiveLight(cfg, l);
  CHECK(e.hours == 12);
  CHECK(e.pct == 80);  // the phase sets no brightness: the circuit's
  cfg.plants[0].completedAt = 1;
  CHECK(doseph::effectiveLight(cfg, l).hours == 16);
}

TEST_CASE("Light: a dimmer goes on by its plan at its brightness; Stop, a hold and maintenance leave it on; a restart of the robot does not switch it off (SD-135, R1)") {
  Run r(harness::device());
  addDimmer(r, 60);
  r.waitUntil("the lamps are on at 80 %", [&] { return lamp(r).on && lamp(r).brightness == 80; }, 60);
  CHECK(lamp(r).config["initial_state"] == "off");  // off after a power loss (RAT-019)
  CHECK(lamp(r).config["auto_off"] == false);       // light runs for hours: no auto-off
  r.waitUntil("the robot reads it back", [&] { return light(r)["output"]["brightness"] == 80; }, 15);  // a poll later
  r.ok("POST", "stop");
  r.step(15 * kSecond);
  CHECK(lamp(r).on);
  r.ok("POST", "resume");
  r.act("leak", {{"on", true}});
  r.step(15 * kSecond);
  CHECK(lamp(r).on);
  r.act("leak", {{"on", false}});
  r.ok("POST", "latches/hw.leak/ack");
  r.ok("POST", "maintenance", {{"minutes", 10}});
  r.step(15 * kSecond);
  CHECK(lamp(r).on);
  r.ok("POST", "maintenance", {{"minutes", 0}});
  // The robot alone restarts: the dimmer keeps its power and stays on, checked while on
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  bool off = false;
  for (int i = 0; i < 30; ++i) {
    r.step(kSecond);
    off |= !lamp(r).on;
  }
  CHECK_FALSE(off);
  CHECK(light(r)["output"]["state"] == "on");
  // A power cut at the dimmer: back, it is off, and the robot switches it on again by its plan
  r.act("netOnline", {{"address", "shelly-dimmer.local"}, {"online", false}});
  r.step(5 * kSecond);
  r.act("netOnline", {{"address", "shelly-dimmer.local"}, {"online", true}});
  CHECK_FALSE(lamp(r).on);
  r.waitUntil("on again", [&] { return lamp(r).on; }, 60);
}

TEST_CASE("Light: the sunrise raises the brightness from the driver's threshold, and night switches the lamps off (SD-135, RAT-013, RAT-029)") {
  Run r(harness::device());
  addDimmer(r, 0, 1);  // a day of one hour that begins now
  r.waitUntil("on at the threshold", [&] { return lamp(r).on; }, 60);
  CHECK(lamp(r).brightness <= 12);
  r.step(15 * 60 * kSecond);
  CHECK(lamp(r).brightness == doctest::Approx(40).epsilon(0.1));
  r.step(50 * 60 * kSecond);
  CHECK_FALSE(lamp(r).on);  // the day is over: 0 %, the relay off (RAT-029)
  CHECK(light(r)["day"] == false);
  CHECK(light(r)["nextChangeAt"].is_number());
}

TEST_CASE("Light: by hand on or off until the plan's next change, then by the plan again; on by hand at night is said (SD-135)") {
  Run r(harness::device());
  addDimmer(r, 60);
  r.waitUntil("on", [&] { return lamp(r).on; }, 60);
  r.ok("POST", "lighting/circuits/l1/hand", {{"on", false}});
  r.waitUntil("off by hand", [&] { return !lamp(r).on; }, 15);
  CHECK(light(r)["hand"]["on"] == false);
  CHECK(light(r)["hand"]["until"] == light(r)["nextChangeAt"]);
  r.ok("POST", "lighting/circuits/l1/hand", {{"on", nullptr}});
  r.waitUntil("on by the plan", [&] { return lamp(r).on; }, 30);
  CHECK(light(r)["hand"].is_null());
  // At night: on by hand, said, and off at the day's start would be its next change
  r.ok("PUT", "lighting/circuits", {{"id", "l1"}, {"name", "Tent"}, {"dimmed", true}, {"startMin", (nowMin(r) + 120) % 1440}, {"hours", 12}, {"pct", 80}});
  r.waitUntil("off at night", [&] { return !lamp(r).on; }, 30);
  r.ok("POST", "lighting/circuits/l1/hand", {{"on", true}});
  r.waitUntil("on by hand", [&] { return lamp(r).on; }, 30);
  CHECK(lamp(r).brightness == 80);
  CHECK(light(r)["darkOn"] == true);
  CHECK(r.call("POST", "lighting/circuits/l9/hand", {{"on", true}}).status == 404);
  CHECK(r.call("POST", "lighting/circuits/l1/hand", {{"on", "yes"}}).status == 422);
}

TEST_CASE("Light: a switched light on a relay is on by day and off by night; a light removed lets its output go (SD-135)") {
  Run r(harness::device());
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-1.local"}, {"name", "Relay"}});
  r.ok("PUT", "lighting/circuits", {{"name", "Shelf"}, {"startMin", ((nowMin(r) - 30) % 1440 + 1440) % 1440}, {"hours", 6}});
  r.ok("PUT", "roles/light.l1", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  auto relay = [&] { return r.s.net().shelly("shelly-1.local")->switches[0].on; };
  r.waitUntil("on by day", relay, 60);
  CHECK(light(r)["pct"].is_null());
  r.ok("DELETE", "lighting/circuits/l1");
  r.waitUntil("off once removed", [&] { return !relay(); }, 15);
  CHECK_FALSE(r.ok("GET", "config")["roles"].contains("light.l1"));
}

TEST_CASE("Light: circuits are checked: a name, a day of 0 to 24 h whose ramps fit in it, a brightness and a threshold in bounds, a plant that exists; a phase's light in bounds (SD-135, R7)") {
  Run r(harness::device());
  CHECK(r.call("PUT", "lighting/circuits", {{"name", ""}}).status == 422);
  CHECK(r.call("PUT", "lighting/circuits", {{"name", "A"}, {"hours", 25}}).status == 422);
  CHECK(r.call("PUT", "lighting/circuits", {{"name", "A"}, {"pct", 0}}).status == 422);
  CHECK(r.call("PUT", "lighting/circuits", {{"name", "A"}, {"thresholdPct", 60}}).status == 422);
  CHECK(r.call("PUT", "lighting/circuits", {{"name", "A"}, {"hours", 1}, {"sunriseMin", 40}, {"sunsetMin", 30}}).status == 422);
  CHECK(r.call("PUT", "lighting/circuits", {{"name", "A"}, {"plant", "nope"}}).status == 422);
  CHECK(r.call("PUT", "lighting/circuits", {{"id", "l7"}, {"name", "A"}}).status == 404);
  CHECK(r.ok("PUT", "lighting/circuits", {{"name", "A"}})["id"] == "l1");
  json pr = {{"name", "Tomatoes"}, {"phases", json::array({{{"name", "Growth"}, {"days", 21}, {"recipe", "two_part"}, {"lightHours", 25}}})}};
  CHECK(r.call("PUT", "plant-recipes", pr).status == 422);
  pr["phases"][0]["lightHours"] = 18;
  pr["phases"][0]["lightPct"] = 0;
  CHECK(r.call("PUT", "plant-recipes", pr).status == 422);
  pr["phases"][0]["lightPct"] = 70;
  const std::string prId = r.ok("PUT", "plant-recipes", pr)["id"];
  const std::string plant = r.ok("POST", "plants", {{"name", "Bed 1"}, {"plantRecipe", prId}})["id"];
  r.ok("PUT", "lighting/circuits", {{"id", "l1"}, {"name", "A"}, {"dimmed", true}, {"plant", plant}});
  const json c = light(r);
  CHECK(c["hours"] == 18);
  CHECK(c["plant"]["phase"] == "Growth");
  CHECK(c["plant"]["pct"] == 70);
  r.ok("DELETE", "plants/" + plant);
  CHECK(r.ok("GET", "config")["lighting"]["circuits"][0]["plant"] == "");
}

TEST_CASE("Light: only a session or an app token manages light and switches it by hand (SD-135, SD-129)") {
  using doseph::Role;
  for (const auto& p : std::vector<std::vector<std::string>>{{"lighting", "circuits"}, {"lighting", "circuits", "l1"}, {"lighting", "circuits", "l1", "hand"}}) {
    for (const char* m : {"POST", "PUT", "DELETE"}) {
      CHECK(doseph::permitted(Role::App, m, p));
      CHECK_FALSE(doseph::permitted(Role::Account, m, p));
      CHECK_FALSE(doseph::permitted(Role::Peer, m, p));
    }
  }
}
