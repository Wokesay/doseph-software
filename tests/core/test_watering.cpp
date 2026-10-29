// SPDX-License-Identifier: AGPL-3.0-or-later
// Watering from the tank (SD-134): circuits on Shelly outputs, one at a time, by a schedule or by
// hand; only with water fit for it (RAT-086), once a dose is mixed in, and while the tank keeps its
// reserve (RAT-067, RAT-068). Driven through the API against the twin: a Shelly with two watering
// pumps that take water from the tank, and an ESPHome device whose level and float "tank empty"
// follow it.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "doseph/api.hpp"
#include "doseph/config.hpp"
#include "doseph/watering.hpp"
#include "harness.hpp"

using harness::Run;
using doseph::json;
using doseph::kSecond;

namespace {

// The twin's pumps as circuits w1 and w2, counted at their real flow (2 L/min)
void addCircuits(Run& r, json w1 = json::object()) {
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-pump.local"}, {"name", "Pumps"}});
  json one = {{"name", "Tent"}, {"mode", "manual"}, {"rateLpm", 2}, {"longestS", 120}};
  one.update(w1);
  CHECK(r.ok("PUT", "watering/circuits", one)["id"] == "w1");
  CHECK(r.ok("PUT", "watering/circuits", {{"name", "Greenhouse"}, {"mode", "manual"}, {"rateLpm", 2}, {"longestS", 120}})["id"] == "w2");
  r.ok("PUT", "roles/water.w1", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.ok("PUT", "roles/water.w2", {{"source", "shelly"}, {"device", "d1"}, {"channel", 1}});
  r.step(kSecond);
}

bool runs(Run& r, int channel) { return r.s.net().shelly("shelly-pump.local")->switches[channel].on; }
json watering(Run& r) { return r.state()["watering"]; }
json circuitState(Run& r, const std::string& id) {
  const json w = watering(r);
  for (const auto& c : w["circuits"])
    if (c["id"] == id) return c;
  return nullptr;
}
std::string blockedKey(Run& r, const std::string& id) {
  const json c = circuitState(r, id);
  return c["blocked"].is_null() ? "" : c["blocked"]["key"].get<std::string>();
}
int count(Run& r, const std::string& key) {
  int n = 0;
  for (const auto& e : r.events("control")) n += e["title"]["key"] == key;
  return n;
}
std::string errorKey(const doseph::ApiResponse& a) { return json::parse(a.body)["error"]["key"]; }
// Until the first check has mixed its doses in and measured the water: it is judged by that
void waitFit(Run& r, const std::string& id = "w1") {
  r.waitUntil("the water may be judged", [&] { return blockedKey(r, id).empty(); }, 8 * 3600, 2000);
}

}  // namespace

TEST_CASE("Watering: shots in a window are spread from its start to its end; fixed times and an interval give their own (RAT-010, SD-134)") {
  doseph::WaterCircuitCfg w;
  w.mode = "window";
  w.fromMin = 8 * 60;
  w.toMin = 20 * 60;
  w.shotsPerDay = 7;
  w.shotS = 90;
  std::vector<int> at;
  for (int m = 0; m < 24 * 60; ++m)
    if (doseph::shotAt(w, m)) at.push_back(m);
  CHECK(at == std::vector<int>{480, 600, 720, 840, 960, 1080, 1200});
  CHECK(doseph::shotAt(w, 480) == 90);
  w.fromMin = 22 * 60;  // over midnight
  w.toMin = 2 * 60;
  w.shotsPerDay = 3;
  at.clear();
  for (int m = 0; m < 24 * 60; ++m)
    if (doseph::shotAt(w, m)) at.push_back(m);
  CHECK(at == std::vector<int>{0, 120, 1320});
  w.mode = "times";
  w.times = {{6 * 60, 30}, {18 * 60 + 30, 45}};
  CHECK(doseph::shotAt(w, 6 * 60) == 30);
  CHECK(doseph::shotAt(w, 18 * 60 + 30) == 45);
  CHECK_FALSE(doseph::shotAt(w, 12 * 60));
  w.mode = "interval";
  w.everyMin = 90;
  w.shotS = 20;
  CHECK(doseph::shotAt(w, 0) == 20);
  CHECK(doseph::shotAt(w, 90) == 20);
  CHECK_FALSE(doseph::shotAt(w, 100));
  w.mode = "manual";
  CHECK_FALSE(doseph::shotAt(w, 0));
}

TEST_CASE("Watering: water is fit within a wide band: pH 4.0 to 8.5, EC at most 1.0 above its target and never above 5.0, with a recent reading and no calibration; pH not corrected only warns (RAT-086, RAT-084, R5)") {
  doseph::WaterIn in;
  in.stale = false;
  in.ph = 7.9;  // tap water, not corrected
  in.ec = 1.2;
  in.ecTarget = 1.4;
  in.phTarget = 5.8;
  in.phTol = 0.2;
  CHECK_FALSE(doseph::Watering::unfit(in));
  REQUIRE(doseph::Watering::warning(in));
  CHECK(doseph::Watering::warning(in)->key == "water.warn.ph");
  in.ph = 8.6;
  CHECK(doseph::Watering::unfit(in)->key == "water.no.ph");
  in.ph = 3.9;
  CHECK(doseph::Watering::unfit(in)->key == "water.no.ph");
  in.ph = 5.8;
  CHECK_FALSE(doseph::Watering::warning(in));
  in.ec = 2.5;  // more than 1.0 above 1.4
  CHECK(doseph::Watering::unfit(in)->key == "water.no.ec");
  in.ecTarget = doseph::kNaN;
  CHECK_FALSE(doseph::Watering::unfit(in));  // no lower EC bound, and without a target only the ceiling
  in.ec = 5.1;
  CHECK(doseph::Watering::unfit(in)->key == "water.no.ec_max");
  in.ec = 0.3;
  CHECK_FALSE(doseph::Watering::unfit(in));
  in.stale = true;
  CHECK(doseph::Watering::unfit(in)->key == "water.no.reading");
  in.stale = false;
  in.ph = doseph::kNaN;
  CHECK(doseph::Watering::unfit(in)->key == "water.no.reading");
  in.ph = 6.0;
  in.calibrating = true;
  CHECK(doseph::Watering::unfit(in)->key == "water.no.calibrating");
}

TEST_CASE("Watering: circuits are checked: a name, a schedule within the longest shot, at most 24 h a shot, a reserve of 10 to 90 % (SD-134, R7)") {
  Run r(harness::device());
  CHECK(r.call("PUT", "watering/circuits", {{"name", ""}}).status == 422);
  CHECK(r.call("PUT", "watering/circuits", {{"name", "A"}, {"mode", "window"}, {"shotS", 400}, {"longestS", 300}}).status == 422);
  CHECK(r.call("PUT", "watering/circuits", {{"name", "A"}, {"longestS", 24 * 3600 + 1}}).status == 422);
  CHECK(r.call("PUT", "watering/circuits", {{"name", "A"}, {"mode", "times"}, {"times", json::array()}}).status == 422);
  CHECK(r.call("PUT", "watering/circuits", {{"name", "A"}, {"mode", "hourly"}}).status == 422);
  CHECK(r.ok("PUT", "watering/circuits", {{"name", "A"}, {"longestS", 24 * 3600}})["id"] == "w1");  // the user's choice (SD-134)
  CHECK(r.call("PUT", "watering", {{"reservePct", 5}}).status == 422);
  r.ok("PUT", "watering", {{"reservePct", 30}});
  CHECK(r.call("PUT", "watering/circuits", {{"id", "w9"}, {"name", "B"}}).status == 404);
  CHECK(r.call("DELETE", "watering/circuits/w9").status == 404);
}

TEST_CASE("Watering: a circuit waters by hand for its shot, counts the litres it gave and logs them; one circuit at a time (SD-134)") {
  Run r(harness::device());
  addCircuits(r);
  waitFit(r);
  const double before = r.s.world().tank.volumeL;
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 30}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  const auto other = r.call("POST", "watering/circuits/w2/now", {{"seconds", 10}});
  CHECK(other.status == 409);
  CHECK(errorKey(other) == "water.no.other");
  CHECK(r.call("POST", "watering/circuits/w1/now", {{"seconds", 200}}).status == 422);  // past its longest shot
  r.waitUntil("the shot ends", [&] { return !runs(r, 0); }, 60);
  r.step(2 * kSecond);
  CHECK(watering(r)["running"].is_null());
  CHECK(count(r, "ev.water.shot") == 1);
  CHECK(watering(r)["givenL"].get<double>() == doctest::Approx(1.0).epsilon(0.1));
  CHECK(before - r.s.world().tank.volumeL == doctest::Approx(1.0).epsilon(0.15));
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.end.done");
}

TEST_CASE("Watering: a schedule waters at its times, one circuit after the other; a paused one rests (SD-134, RAT-010)") {
  Run r(harness::device());
  addCircuits(r, {{"mode", "interval"}, {"everyMin", 10}, {"shotS", 20}});
  r.ok("PUT", "watering/circuits", {{"id", "w2"}, {"name", "Greenhouse"}, {"mode", "interval"}, {"everyMin", 10}, {"shotS", 20}, {"rateLpm", 2}, {"longestS", 120}});
  waitFit(r);
  const int before = count(r, "ev.water.shot");
  r.step(25 * 60 * kSecond, 1000);
  const int shots = count(r, "ev.water.shot") - before;
  CHECK(shots >= 4);  // both circuits, at least twice each, one after the other
  r.ok("POST", "watering/circuits/w1/pause", {{"paused", true}});
  r.ok("POST", "watering/circuits/w2/pause", {{"paused", true}});
  const int paused = count(r, "ev.water.shot");
  r.step(25 * 60 * kSecond, 1000);
  CHECK(count(r, "ev.water.shot") == paused);
}

TEST_CASE("Watering: water not fit keeps a circuit from watering and stops a shot; a person may water anyway, which is a warning (SD-134, RAT-086)") {
  Run r(harness::device());
  addCircuits(r);
  waitFit(r);
  // A gross overdose: EC far above its target
  r.s.world().tank.ec += 3.0;
  r.waitUntil("the check finds it", [&] { return blockedKey(r, "w1") == "water.no.ec" || blockedKey(r, "w1") == "water.no.ec_max"; }, 3 * 3600, 2000);
  const auto refused = r.call("POST", "watering/circuits/w1/now", {{"seconds", 20}});
  CHECK(refused.status == 409);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 20}, {"override", true}});
  r.waitUntil("the pump runs anyway", [&] { return runs(r, 0); }, 30);
  CHECK(watering(r)["override"] == true);
  CHECK(count(r, "ev.water.override") == 1);
  r.waitUntil("the shot ends", [&] { return !runs(r, 0); }, 60);
}

TEST_CASE("Watering: the count keeps the reserve: a shot that would go below it is not made, water is to be topped up, and a new tank starts the count anew (RAT-067, SD-134)") {
  Run r(harness::device());
  // 100 L entered, a reserve of 20 L: 80 L to give. Counted at 60 L/min, two shots of 30 s give 60 L.
  addCircuits(r, {{"mode", "interval"}, {"everyMin", 2}, {"shotS", 30}, {"rateLpm", 60}});
  waitFit(r);
  r.waitUntil("the reserve stops it", [&] { return r.state()["watering"]["low"] == true; }, 30 * 60, 1000);
  CHECK(watering(r)["givenL"].get<double>() <= 80.0 + 0.5);
  CHECK(watering(r)["availableL"].get<double>() < 30.0);
  r.step(6 * kSecond);
  bool shown = false;
  const json wd = r.state()["watchdog"];
  for (const auto& i : wd["items"]) shown |= i["id"] == "water.low" && i["status"] == "problem";
  CHECK(shown);
  r.ok("POST", "tank/new");
  CHECK(watering(r)["givenL"] == 0);
  CHECK(watering(r)["low"] == false);
}

TEST_CASE("Watering: a float \"tank empty\" or a level probe keeps it from an empty tank; without a value it blocks (RAT-068, R5)") {
  Run r(harness::device());
  addCircuits(r, {{"rateLpm", nullptr}});
  r.ok("POST", "devices", {{"kind", "esphome"}, {"address", "esp-1.local"}, {"name", "Sensors"}});
  waitFit(r, "w2");  // w1: its flow unknown, nothing tells how much water there is
  CHECK(errorKey(r.call("POST", "watering/circuits/w1/now", {{"seconds", 10}})) == "water.no.count");
  // A float "tank empty"
  r.ok("PUT", "roles/tank_empty", {{"source", "esphome"}, {"device", "d2"}, {"entity", "binary_sensor/tank_empty"}});
  r.waitUntil("the float says not empty", [&] { return blockedKey(r, "w1").empty(); }, 60);
  r.s.world().tank.volumeL = 15;  // below the float
  r.waitUntil("the float says empty", [&] { return blockedKey(r, "w1") == "water.no.empty"; }, 60);
  r.s.world().tank.volumeL = 100;
  r.waitUntil("water again", [&] { return blockedKey(r, "w1").empty(); }, 60);
  // A level probe: the reserve is checked against it
  r.ok("PUT", "roles/tank_level", {{"source", "esphome"}, {"device", "d2"}, {"entity", "sensor/tank_level"}});
  r.waitUntil("the level is read", [&] { return watering(r)["availableL"].is_number(); }, 60);
  CHECK(watering(r)["availableL"].get<double>() == doctest::Approx(80.0).epsilon(0.05));
  r.s.world().tank.volumeL = 22;  // with the 4 L a longest shot of w2 gives: below the 20 L reserve
  r.waitUntil("the level is at the reserve", [&] { return blockedKey(r, "w2") == "water.no.reserve"; }, 60);
  r.act("netOnline", {{"address", "esp-1.local"}, {"online", false}});
  r.waitUntil("the level has no value", [&] { return blockedKey(r, "w2") == "water.no.empty_float" || blockedKey(r, "w2") == "water.no.level"; }, 90);
}

TEST_CASE("Watering: Stop, maintenance and a dose stop a running shot; a restart reports a cut shot and does not repeat it (SD-134, R6, RAT-086)") {
  Run r(harness::device());
  addCircuits(r);
  waitFit(r);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.ok("POST", "stop");
  r.step(2 * kSecond);
  CHECK_FALSE(runs(r, 0));
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.end.stopped");
  CHECK(errorKey(r.call("POST", "watering/circuits/w1/now", {{"seconds", 10}})) == "water.no.stopped");
  r.ok("POST", "resume");
  waitFit(r);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.ok("POST", "maintenance", {{"minutes", 5}});
  r.step(2 * kSecond);
  CHECK_FALSE(runs(r, 0));
  r.ok("POST", "maintenance", {{"minutes", 0}});
  waitFit(r);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.s.reboot();  // the robot alone: the pump keeps its power
  REQUIRE(r.login("demo-password"));
  CHECK(count(r, "ev.water.cut_restart") == 1);
  r.waitUntil("the pump is switched off", [&] { return !runs(r, 0); }, 6);
  r.step(2 * 60 * kSecond);
  CHECK_FALSE(runs(r, 0));
  CHECK(watering(r)["running"].is_null());
}

TEST_CASE("Watering: the device's own auto-off is 1.11 times the circuit's longest shot, and the gateway ends a shot past it, whatever wants it on (RAT-019, SD-134)") {
  Run r(harness::device());
  addCircuits(r, {{"longestS", 60}});
  CHECK(r.ok("GET", "devices")["roles"]["water.w1"]["autoOffS"] == 120);  // 66.6 s, a whole minute up
  r.waitUntil("checked", [&] { return r.s.net().shelly("shelly-pump.local")->switches[0].config.value("auto_off", false); }, 30);
  CHECK(r.s.net().shelly("shelly-pump.local")->switches[0].config["auto_off_delay"] == 120);
  r.s.robot().wantOutput("water.w1", true);
  r.waitUntil("on", [&] { return runs(r, 0); }, 30);
  r.waitUntil("off past its longest shot", [&] { return !runs(r, 0); }, 70);
  r.step(30 * kSecond);
  CHECK_FALSE(runs(r, 0));
  // A circuit removed lets its output go
  r.ok("DELETE", "watering/circuits/w1");
  CHECK_FALSE(r.ok("GET", "config")["roles"].contains("water.w1"));
}

TEST_CASE("Watering: only a session or an app token manages circuits and waters; an account token only pauses a schedule (SD-134, SD-129)") {
  using doseph::Role;
  for (const auto& p : std::vector<std::vector<std::string>>{{"watering"}, {"watering", "circuits"}, {"watering", "circuits", "w1"}, {"watering", "circuits", "w1", "now"}, {"watering", "circuits", "w1", "stop"}}) {
    for (const char* m : {"POST", "PUT", "DELETE"}) {
      CHECK(doseph::permitted(Role::App, m, p));
      CHECK_FALSE(doseph::permitted(Role::Account, m, p));
      CHECK_FALSE(doseph::permitted(Role::Peer, m, p));
    }
  }
  CHECK(doseph::permitted(Role::Account, "POST", {"watering", "circuits", "w1", "pause"}));
  CHECK_FALSE(doseph::permitted(Role::Peer, "POST", {"watering", "circuits", "w1", "pause"}));
}
