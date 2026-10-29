// SPDX-License-Identifier: AGPL-3.0-or-later
// Climate (SD-136): a room with an exhaust fan, a circulation fan, a heater with its own thermostat,
// a humidifier, a dehumidifier and a CO2 valve on the twin's relays, and its sensors on the twin's
// ESPHome device. Each output switches around its target with a band, at most once a minute; a
// missing value switches nothing on and runs the exhaust fan; humidifier and dehumidifier never run
// together, and the dehumidifier rests 5 min after it went off (RAT-034); CO2 only by day, while the
// exhaust fan is off, in pulses, and never above 2,000 ppm. The fans keep their rule through a Stop;
// the rest goes off at a Stop, not at a hold of the pumps. Driven through the API.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "doseph/api.hpp"
#include "doseph/climate.hpp"
#include "doseph/config.hpp"
#include "doseph/localtime.hpp"
#include "harness.hpp"

using harness::Run;
using doseph::json;
using doseph::kSecond;

namespace {

int nowMin(Run& r) {
  const std::string zone = r.ok("GET", "config")["system"]["timezone"];
  return doseph::localTime(r.s.clock().epoch(), zone).minuteOfDay;
}
int dayStart(Run& r) { return ((nowMin(r) - 60) % 1440 + 1440) % 1440; }  // a day of 12 h begun an hour ago

// The twin's room as the robot's room r1: its outputs on the two relays, its sensors on the ESPHome
// device
void keep(Run& r, double tempC, double rhPct, double co2Ppm, int seconds);
void addRoom(Run& r, json more = json::object()) {
  keep(r, 26, 70, 600, 2);  // the air at its targets: the first readings switch nothing
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-climate.local"}, {"name", "Room relay"}});
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-climate2.local"}, {"name", "Room relay 2"}});
  r.ok("POST", "devices", {{"kind", "esphome"}, {"address", "esp-climate.local"}, {"name", "Room sensors"}});
  json body = {{"name", "Tent"}, {"startMin", dayStart(r)}, {"hours", 12}};
  body.update(more);
  CHECK(r.ok("PUT", "climate/rooms", body)["id"] == "r1");
  const std::vector<std::pair<const char*, std::pair<const char*, int>>> outputs = {
      {"exhaust", {"d1", 0}}, {"circulation", {"d1", 1}}, {"heater", {"d1", 2}}, {"dehumidifier", {"d1", 3}}, {"humidifier", {"d2", 0}}, {"co2_valve", {"d2", 1}}};
  for (const auto& [kind, at] : outputs)
    r.ok("PUT", std::string("roles/climate.r1.") + kind, {{"source", "shelly"}, {"device", at.first}, {"channel", at.second}});
  for (const auto& [kind, entity] : {std::pair{"temp", "sensor/air_temp"}, std::pair{"rh", "sensor/air_rh"}, std::pair{"co2", "sensor/co2"}})
    r.ok("PUT", std::string("roles/climate.r1.") + kind, {{"source", "esphome"}, {"device", "d3"}, {"entity", entity}});
}

sim::NetTwin::Room& air(Run& r) { return r.s.net().room(); }
bool runs(Run& r, const char* address, int n) { return r.s.net().shelly(address)->switches[static_cast<size_t>(n)].on; }
bool exhaust(Run& r) { return runs(r, "shelly-climate.local", 0); }
bool circulation(Run& r) { return runs(r, "shelly-climate.local", 1); }
bool heater(Run& r) { return runs(r, "shelly-climate.local", 2); }
bool dehumidifier(Run& r) { return runs(r, "shelly-climate.local", 3); }
bool humidifier(Run& r) { return runs(r, "shelly-climate2.local", 0); }
bool valve(Run& r) { return runs(r, "shelly-climate2.local", 1); }

// The room's air held at these values for a while
void keep(Run& r, double tempC, double rhPct, double co2Ppm, int seconds) {
  for (int i = 0; i < seconds; ++i) {
    air(r) = {tempC, rhPct, co2Ppm};
    r.step(kSecond);
  }
}
// … until a condition holds; fails past its deadline
void keepUntil(Run& r, double tempC, double rhPct, double co2Ppm, const std::string& what, const std::function<bool()>& done, int maxS) {
  for (int i = 0; i <= maxS; ++i) {
    air(r) = {tempC, rhPct, co2Ppm};
    if (done()) return;
    r.step(kSecond);
  }
  FAIL("not within " << maxS << " s: " << what);
}
json room(Run& r) {
  const json all = r.state()["climate"];  // kept: a loop over a part of a temporary is undefined
  return all.empty() ? json(nullptr) : all[0];
}

}  // namespace

TEST_CASE("Climate: air VPD from temperature and humidity; a VPD target gives the humidity at the target temperature (SD-136, RAT-017, RAT-009)") {
  CHECK(doseph::airVpdKpa(27, 62) == doctest::Approx(1.35).epsilon(0.01));
  CHECK(doseph::airVpdKpa(26, 70) == doctest::Approx(1.01).epsilon(0.01));
  CHECK(doseph::rhForVpd(doseph::airVpdKpa(26, 70), 26) == doctest::Approx(70).epsilon(0.001));
  CHECK(doseph::rhForVpd(9, 20) == 0);  // never below 0 %
  // A room's own targets: those of RAT-009 by day, 4 °C less at night, 70 %
  doseph::ClimateRoomCfg r;
  CHECK(r.dayC == 26);
  CHECK(r.nightC == 22);
  CHECK(r.rhPct == 70);
  doseph::Config cfg;
  r.startMin = 6 * 60;
  r.hours = 12;
  doseph::ClimateTargets t = doseph::climateTargets(cfg, r, 12 * 3600);
  CHECK(t.day);
  CHECK(t.tempC == 26);
  CHECK(t.rhPct == 70);
  CHECK(std::isnan(t.co2Ppm));
  r.co2Ppm = 1000;
  r.byVpd = true;
  r.vpdKpa = 1.2;
  t = doseph::climateTargets(cfg, r, 12 * 3600);
  CHECK(t.co2Ppm == 1000);
  CHECK(t.rhPct == doctest::Approx(doseph::rhForVpd(1.2, 26)));
  t = doseph::climateTargets(cfg, r, 22 * 3600);  // night: no CO2, the night's temperature
  CHECK_FALSE(t.day);
  CHECK(t.tempC == 22);
  CHECK(std::isnan(t.co2Ppm));
}

TEST_CASE("Climate: a room's day is its first light circuit's; a plant's phase sets its targets where it has them, a VPD leading the humidity (SD-136, RAT-076, RAT-009)") {
  doseph::Config cfg;
  doseph::LightCircuitCfg l;
  l.id = "l1";
  l.startMin = 20 * 60;  // 20:00 to 08:00
  l.hours = 12;
  cfg.lighting.circuits = {l};
  doseph::PhaseCfg ph;
  ph.name = "Bloom";
  ph.dayC = 24;
  ph.vpdKpa = 1.2;
  ph.co2Ppm = 900;
  doseph::PlantCfg p;
  p.id = "p1";
  p.name = "Bed 1";
  p.phases = {ph};
  cfg.plants = {p};
  doseph::ClimateRoomCfg r;
  r.lights = {"l1"};
  r.plant = "p1";
  doseph::ClimateTargets t = doseph::climateTargets(cfg, r, 2 * 3600);
  CHECK(t.day);
  CHECK(t.tempC == 24);
  CHECK(t.vpdKpa == 1.2);
  CHECK(t.rhPct == doctest::Approx(doseph::rhForVpd(1.2, 24)));
  CHECK(t.co2Ppm == 900);
  t = doseph::climateTargets(cfg, r, 12 * 3600);
  CHECK_FALSE(t.day);
  CHECK(t.tempC == 22);  // the phase sets no night: the room's
  cfg.plants[0].completedAt = 1;  // no longer running: the room's own
  CHECK(doseph::climateTargets(cfg, r, 2 * 3600).tempC == 26);
}

TEST_CASE("Climate: too warm runs the exhaust fan, and back at its target it stops; too cold gives the heater power, its plug a power limit a fifth above its power; an output switches at most once a minute (SD-136, RAT-069)") {
  Run r(harness::device());
  addRoom(r, {{"heaterW", 370}});
  keepUntil(r, 30, 60, 600, "exhaust on", [&] { return exhaust(r); }, 60);
  CHECK_FALSE(heater(r));
  CHECK(circulation(r));  // by day
  // Back below the target at once: the fan runs out its minute
  keep(r, 25.5, 60, 600, 20);
  CHECK(exhaust(r));
  keepUntil(r, 25.5, 60, 600, "exhaust off", [&] { return !exhaust(r); }, 60);
  keepUntil(r, 24, 60, 600, "heater on", [&] { return heater(r); }, 90);
  const json heaterCfg = r.s.net().shelly("shelly-climate.local")->switches[2].config;
  CHECK(heaterCfg["auto_off"] == false);  // its own thermostat: no auto-off
  CHECK(heaterCfg["power_limit"] == 450);  // 370 W and a fifth, rounded up to 10 W
  CHECK(r.s.net().shelly("shelly-climate.local")->switches[0].config["power_limit"] == 4480);  // the fan's left as it was
  keepUntil(r, 26.5, 60, 600, "heater off", [&] { return !heater(r); }, 90);
  const json s = room(r);
  CHECK(s["day"] == true);
  CHECK(s["targets"]["tempC"] == 26);
  CHECK(s["air"]["tempC"] == 26.5);
  CHECK(s["air"]["vpdKpa"].is_number());
}

TEST_CASE("Climate: humidifier and dehumidifier never run together; the dehumidifier rests 5 min after a start and after it went off (SD-136, RAT-034)") {
  Run r(harness::device());
  addRoom(r);
  // Too humid right after the robot started: the exhaust fan runs, the dehumidifier rests its 5 min
  keep(r, 26, 80, 600, 120);
  CHECK(exhaust(r));
  CHECK_FALSE(dehumidifier(r));
  keepUntil(r, 26, 80, 600, "dehumidifier on", [&] { return dehumidifier(r); }, 5 * 60);
  // Back at its target it goes off at once; too humid again, it rests 5 min first
  keepUntil(r, 26, 68, 600, "dehumidifier off", [&] { return !dehumidifier(r); }, 30);
  keep(r, 26, 80, 600, 4 * 60);
  CHECK_FALSE(dehumidifier(r));
  keepUntil(r, 26, 80, 600, "dehumidifier on again", [&] { return dehumidifier(r); }, 90);
  // Too dry: the dehumidifier off, then the humidifier, never both
  bool both = false;
  keepUntil(r, 26, 60, 600, "humidifier on", [&] {
    both |= humidifier(r) && dehumidifier(r);
    return humidifier(r);
  }, 120);
  CHECK_FALSE(both);
  // The dehumidifier switched on at its device, as by a hand there: the humidifier goes off
  r.s.net().shelly("shelly-climate.local")->switches[3].on = true;
  keepUntil(r, 26, 60, 600, "humidifier off", [&] { return !humidifier(r); }, 30);
}

TEST_CASE("Climate: CO2 only by day, while the exhaust fan is off, in pulses of a minute; closed in maintenance, and above 2,000 ppm with an alarm (SD-136)") {
  Run r(harness::device());
  addRoom(r, {{"co2Ppm", 1000}});
  keepUntil(r, 26, 70, 700, "valve open", [&] { return valve(r); }, 90);
  // A pulse lasts a minute at most, then the valve rests a minute
  keepUntil(r, 26, 70, 700, "pulse over", [&] { return !valve(r); }, 70);
  keep(r, 26, 70, 700, 45);
  CHECK_FALSE(valve(r));
  keepUntil(r, 26, 70, 700, "next pulse", [&] { return valve(r); }, 30);
  // The exhaust fan runs: closed at once
  keepUntil(r, 30, 70, 700, "closed for the fan", [&] { return exhaust(r) && !valve(r); }, 60);
  keepUntil(r, 26, 70, 700, "fan off", [&] { return !exhaust(r); }, 90);
  // Maintenance: someone is in the room
  r.ok("POST", "maintenance", {{"minutes", 10}});
  keep(r, 26, 70, 700, 150);
  CHECK_FALSE(valve(r));
  r.ok("POST", "maintenance", {{"minutes", 0}});
  keepUntil(r, 26, 70, 700, "open after maintenance", [&] { return valve(r); }, 150);
  // Above 2,000 ppm: closed, with an alarm, until it is back below 1,500
  keepUntil(r, 26, 70, 2100, "closed at 2,100 ppm", [&] { return !valve(r); }, 5);
  CHECK(room(r)["co2High"] == true);
  bool alarm = false;
  for (const auto& e : r.events("alarm"))
    if (e["title"]["key"] == "ev.climate.co2_high") alarm = true;
  CHECK(alarm);
  keep(r, 26, 70, 1600, 150);
  CHECK_FALSE(valve(r));
  keepUntil(r, 26, 70, 800, "open again", [&] { return valve(r); }, 150);
  CHECK(room(r)["co2High"] == false);
  // At night, never
  r.ok("PUT", "climate/rooms", {{"id", "r1"}, {"name", "Tent"}, {"startMin", (nowMin(r) + 120) % 1440}, {"hours", 12}, {"co2Ppm", 1000}});
  keep(r, 26, 70, 700, 150);
  CHECK_FALSE(valve(r));
}

TEST_CASE("Climate: a sensor gone switches nothing on: the exhaust fan runs, heater, humidifier, dehumidifier and CO2 go off (SD-136, R5)") {
  Run r(harness::device());
  addRoom(r, {{"co2Ppm", 1000}});
  // Before the first values come, the exhaust fan runs; once they are in, it stops
  keepUntil(r, 24, 60, 700, "heater and humidifier on", [&] { return heater(r) && humidifier(r) && !exhaust(r); }, 120);
  r.act("netOnline", {{"address", "esp-climate.local"}, {"online", false}});
  keepUntil(r, 24, 60, 700, "safe side", [&] { return exhaust(r) && !heater(r) && !humidifier(r) && !valve(r); }, 90);
  const json s = room(r);
  CHECK(s["air"]["tempC"].is_null());
  CHECK(s["missing"].size() == 3);
}

TEST_CASE("Climate: a Stop leaves the fans to their rule and switches heater, humidifier and CO2 off until Resume; a hold of the pumps leaves climate on (SD-136)") {
  Run r(harness::device());
  addRoom(r);
  keepUntil(r, 24, 60, 600, "heater, humidifier and circulation on", [&] { return heater(r) && humidifier(r) && circulation(r); }, 120);
  r.ok("POST", "stop");
  keep(r, 24, 60, 600, 10);
  CHECK(circulation(r));
  CHECK_FALSE(heater(r));
  CHECK_FALSE(humidifier(r));
  keep(r, 24, 60, 600, 120);
  CHECK_FALSE(heater(r));
  r.ok("POST", "resume");
  keepUntil(r, 24, 60, 600, "heater on after Resume", [&] { return heater(r); }, 120);
  // A leak holds the pumps, not the room
  r.act("leak", {{"on", true}});
  keep(r, 24, 60, 600, 30);
  CHECK(heater(r));
  CHECK(humidifier(r));
  CHECK(circulation(r));
}

TEST_CASE("Climate: after a restart of the robot the fans run by their rule again, and the dehumidifier rests its 5 min (SD-136, R6, RAT-034)") {
  Run r(harness::device());
  addRoom(r);
  keep(r, 26, 80, 600, 6 * 60);
  REQUIRE(dehumidifier(r));
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  keep(r, 26, 80, 600, 90);
  CHECK(exhaust(r));
  CHECK(circulation(r));
  CHECK_FALSE(dehumidifier(r));
  keepUntil(r, 26, 80, 600, "dehumidifier after its rest", [&] { return dehumidifier(r); }, 5 * 60);
}

TEST_CASE("Climate: rooms are checked: a name, targets in their bounds, CO2 at most 1,500 ppm, a heat limit above the targets, light circuits that exist in one room only; a room removed lets its outputs go (SD-136, R7)") {
  Run r(harness::device());
  CHECK(r.call("PUT", "climate/rooms", {{"name", ""}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"dayC", 41}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"rhPct", 99}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"vpdKpa", 3}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"co2Ppm", 1600}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"heatC", 25}}).status == 422);  // not above the day's 26 °C
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"heaterW", 20}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"lights", {"l1"}}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"name", "A"}, {"lights", "l1"}}).status == 422);
  CHECK(r.call("PUT", "climate/rooms", {{"id", "r5"}, {"name", "A"}}).status == 404);
  r.ok("PUT", "lighting/circuits", {{"name", "Lamps"}});
  CHECK(r.ok("PUT", "climate/rooms", {{"name", "A"}, {"lights", {"l1"}}})["id"] == "r1");
  CHECK(r.call("PUT", "climate/rooms", {{"name", "B"}, {"lights", {"l1"}}}).status == 422);  // in one room only
  // CO2 over MQTT only with its sender's availability: a value kept on the broker outlives a sender that stopped
  CHECK(r.call("PUT", "roles/climate.r1.co2", {{"source", "mqtt"}, {"entity", "room/co2"}}).status == 422);
  r.ok("PUT", "roles/climate.r1.co2", {{"source", "mqtt"}, {"entity", "room/co2"}, {"availability", "room/status"}});
  r.ok("PUT", "roles/climate.r1.temp", {{"source", "mqtt"}, {"entity", "room/temp"}});
  // A light circuit removed leaves the room
  r.ok("DELETE", "lighting/circuits/l1");
  CHECK(r.ok("GET", "config")["climate"]["rooms"][0]["lights"].empty());
  // A room removed: its roles go, and its outputs are let go and switched off
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-climate.local"}, {"name", "Room relay"}});
  r.ok("PUT", "roles/climate.r1.circulation", {{"source", "shelly"}, {"device", "d1"}, {"channel", 1}});
  r.ok("PUT", "climate/rooms", {{"id", "r1"}, {"name", "A"}, {"circulateAlways", true}});
  r.waitUntil("circulation on", [&] { return circulation(r); }, 60);
  r.ok("DELETE", "climate/rooms/r1");
  r.waitUntil("off once removed", [&] { return !circulation(r); }, 15);
  CHECK_FALSE(r.ok("GET", "config")["roles"].contains("climate.r1.circulation"));
  // A phase's climate in its bounds
  json pr = {{"name", "Tomatoes"}, {"phases", json::array({{{"name", "Growth"}, {"days", 21}, {"recipe", "two_part"}, {"co2Ppm", 1600}}})}};
  CHECK(r.call("PUT", "plant-recipes", pr).status == 422);
  pr["phases"][0]["co2Ppm"] = 900;
  pr["phases"][0]["vpdKpa"] = 1.1;
  r.ok("PUT", "plant-recipes", pr);
}

TEST_CASE("Climate: only a session or an app token manages rooms (SD-136, SD-129)") {
  using doseph::Role;
  for (const auto& p : std::vector<std::vector<std::string>>{{"climate", "rooms"}, {"climate", "rooms", "r1"}}) {
    for (const char* m : {"PUT", "DELETE"}) {
      CHECK(doseph::permitted(Role::App, m, p));
      CHECK_FALSE(doseph::permitted(Role::Account, m, p));
      CHECK_FALSE(doseph::permitted(Role::Peer, m, p));
    }
  }
}
