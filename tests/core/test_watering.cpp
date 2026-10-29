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
#include "doseph/localtime.hpp"
#include "doseph/watering.hpp"
#include "harness.hpp"

using harness::Run;
using doseph::json;
using doseph::kSecond;

namespace {

// The twin's pumps as circuits w1 and w2, counted at their real flow (2 L/min). `fresh`: a new tank
// is announced, which starts the count of litres.
void addCircuits(Run& r, json w1 = json::object(), bool fresh = true) {
  if (fresh) r.ok("POST", "tank/new");
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
// Shots of a circuit skipped for this reason
int skipped(Run& r, const std::string& circuit, const std::string& why) {
  int n = 0;
  for (const auto& e : r.events("control"))
    n += e["title"]["key"] == "ev.water.skipped" && e["text"]["key"] == why && e["data"]["circuit"] == circuit;
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
  int both = 0, one = 0, two = 0;
  for (int i = 0; i < 25 * 60; ++i) {
    r.step(kSecond);
    both += runs(r, 0) && runs(r, 1);
    one += runs(r, 0);
    two += runs(r, 1);
  }
  CHECK(count(r, "ev.water.shot") - before >= 4);  // both circuits, at least twice each
  CHECK(one > 0);
  CHECK(two > 0);
  CHECK(both == 0);  // one after the other
  r.ok("POST", "watering/circuits/w1/pause", {{"paused", true}});
  r.ok("POST", "watering/circuits/w2/pause", {{"paused", true}});
  r.waitUntil("a shot under way ends", [&] { return watering(r)["running"].is_null(); }, 60);
  const int paused = count(r, "ev.water.shot");
  // A change of the circuit keeps it paused
  r.ok("PUT", "watering/circuits", {{"id", "w1"}, {"name", "Tent"}, {"mode", "interval"}, {"everyMin", 5}, {"shotS", 20}, {"rateLpm", 2}, {"longestS", 120}});
  r.step(25 * 60 * kSecond, 1000);
  CHECK(count(r, "ev.water.shot") == paused);
  CHECK(r.ok("GET", "config")["watering"]["circuits"][0]["paused"] == true);
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
  CHECK(errorKey(refused) == blockedKey(r, "w1"));
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 20}, {"override", true}});
  r.waitUntil("the pump runs anyway", [&] { return runs(r, 0); }, 30);
  CHECK(watering(r)["override"] == true);
  CHECK(count(r, "ev.water.override") == 1);
  r.waitUntil("the shot ends", [&] { return !runs(r, 0); }, 60);
  r.step(2 * kSecond);
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.end.done");  // the water was not fit all along
}

TEST_CASE("Watering: water that is no longer fit stops a shot that is not an override (RAT-086)") {
  Run r(harness::device());
  addCircuits(r);
  waitFit(r);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  // A probe goes into a buffer: the water cannot be judged
  CHECK(r.s.robot().probe({{"action", "start"}, {"kind", "ph"}}).status == 200);
  r.waitUntil("the shot ends", [&] { return !runs(r, 0); }, 10);
  r.step(2 * kSecond);
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.no.calibrating");
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
  r.s.world().tank.volumeL = 20.02;  // at the reserve of 20 L: not a second more
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
  const double given = watering(r)["givenL"].get<double>();
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.s.reboot();  // the robot alone: the pump keeps its power
  REQUIRE(r.login("demo-password"));
  CHECK(count(r, "ev.water.cut_restart") == 1);
  // Booked as if it ran until the device's own auto-off: 3 min at 2 L/min (RAT-019)
  CHECK(watering(r)["givenL"].get<double>() - given == doctest::Approx(6.0));
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

TEST_CASE("Watering: the count of litres starts with a new tank or the float \"full\"; before either, a counted circuit does not water (RAT-067, R5)") {
  Run r(harness::device());
  addCircuits(r, json::object(), false);
  r.waitUntil("its output is checked", [&] { return blockedKey(r, "w1") == "water.no.count_start"; }, 60);
  CHECK(watering(r)["givenL"].is_null());
  CHECK(watering(r)["availableL"].is_null());
  // The inlet's float, at the twin's line
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-valve.local"}, {"name", "Inlet"}});
  r.ok("PUT", "roles/tank_full", {{"source", "shelly"}, {"device", "d2"}, {"channel", 0}});
  r.s.world().tank.volumeL = 106;
  r.waitUntil("the float says full", [&] { return watering(r)["givenL"] == 0; }, 60);
  CHECK(blockedKey(r, "w1") != "water.no.count_start");
  CHECK(watering(r)["availableL"].get<double>() == doctest::Approx(80.0));
}

TEST_CASE("Watering: water at the reserve asks the refill valve for a fill, and the float \"full\" starts the count anew (SD-134, SD-132)") {
  Run r(harness::device());
  // Counted at 60 L/min: the reserve comes after a few shots, the twin's tank loses little
  addCircuits(r, {{"mode", "interval"}, {"everyMin", 2}, {"shotS", 30}, {"rateLpm", 60}});
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-valve.local"}, {"name", "Inlet"}});
  r.ok("PUT", "roles/refill_valve", {{"source", "shelly"}, {"device", "d2"}, {"channel", 0}});
  r.ok("PUT", "roles/tank_full", {{"source", "shelly"}, {"device", "d2"}, {"channel", 0}});
  waitFit(r);
  r.waitUntil("the reserve stops it", [&] { return watering(r)["low"] == true; }, 30 * 60, 1000);
  r.waitUntil("a fill opens", [&] { return r.state()["refill"]["filling"] == true; }, 15 * 60);
  CHECK(r.state()["refill"]["why"] == "top_up");
  r.waitUntil("the float says full", [&] { return watering(r)["givenL"] == 0; }, 30 * 60);
  CHECK(watering(r)["low"] == false);
}

TEST_CASE("Watering: a shot that brings the tank to its reserve ends there, and water is to be topped up until the tank has enough again (RAT-067, RAT-068)") {
  Run r(harness::device());
  addCircuits(r, {{"rateLpm", nullptr}, {"longestS", 600}});
  r.ok("POST", "devices", {{"kind", "esphome"}, {"address", "esp-1.local"}, {"name", "Sensors"}});
  r.ok("PUT", "roles/tank_level", {{"source", "esphome"}, {"device", "d2"}, {"entity", "sensor/tank_level"}});
  waitFit(r);
  r.s.world().tank.volumeL = 24;  // 4 L above the reserve: two minutes of the twin's pump
  r.waitUntil("the level is read", [&] { return watering(r)["availableL"].is_number() && watering(r)["availableL"].get<double>() < 5; }, 60);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 600}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.waitUntil("the reserve ends it", [&] { return !runs(r, 0); }, 300);
  r.step(2 * kSecond);
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.no.reserve");
  CHECK(watering(r)["low"] == true);
  CHECK(r.s.world().tank.volumeL >= 19.0);
  r.s.world().tank.volumeL = 100;
  r.waitUntil("water is no longer low", [&] { return watering(r)["low"] == false; }, 60);
}

TEST_CASE("Watering: a dose that starts ends a running shot, and no shot starts while one runs, not even anyway (RAT-086)") {
  Run r(harness::device());
  addCircuits(r, {{"longestS", 2 * 3600}, {"rateLpm", 0.1}});
  waitFit(r);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 2 * 3600}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  // A new tank: mixed in, measured, and its EC brought up by a dose
  r.s.world().tank.ec -= 0.5;
  r.ok("POST", "tank/new");
  r.waitUntil("a dose", [&] { return r.phase() == "dose"; }, 45 * 60);
  r.step(kSecond);
  const auto anyway = r.call("POST", "watering/circuits/w2/now", {{"seconds", 10}, {"override", true}});
  CHECK(anyway.status == 409);
  CHECK(errorKey(anyway) == "water.no.dosing");
  r.step(2 * kSecond);
  CHECK_FALSE(runs(r, 0));
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.end.dose");
}

TEST_CASE("Watering: a schedule goes by the clock's minutes: set back, no shot comes twice; set forward, nothing missed is made up (SD-134, R6)") {
  Run r(harness::device());
  addCircuits(r, {{"mode", "interval"}, {"everyMin", 60}, {"shotS", 20}});
  waitFit(r);
  const int before = count(r, "ev.water.shot");
  r.waitUntil("a shot at the full hour", [&] { return count(r, "ev.water.shot") > before; }, 2 * 3600, 5000);
  const int one = count(r, "ev.water.shot");
  // Back by half an hour: the hour comes again, its shot does not
  r.s.clock().stepWall(-30 * 60);
  r.step(80 * 60 * kSecond, 2000);
  CHECK(count(r, "ev.water.shot") == one);
  r.waitUntil("the next hour's shot", [&] { return count(r, "ev.water.shot") > one; }, 3600, 5000);
  const int two = count(r, "ev.water.shot");
  CHECK(two == one + 1);
  // Forward by 100 minutes, across a full hour: that shot is not made up
  r.s.clock().stepWall(100 * 60);
  r.step(2 * 60 * kSecond);
  CHECK(count(r, "ev.water.shot") == two);
  CHECK(watering(r)["running"].is_null());
  CHECK_FALSE(circuitState(r, "w1")["due"].get<bool>());
}

TEST_CASE("Watering: a shot that still waits when its circuit's next one comes is skipped, and said once (SD-134)") {
  Run r(harness::device());
  addCircuits(r, {{"mode", "interval"}, {"everyMin", 1}, {"shotS", 5}});
  r.ok("PUT", "watering/circuits", {{"id", "w2"}, {"name", "Greenhouse"}, {"mode", "manual"}, {"rateLpm", 2}, {"longestS", 300}});
  waitFit(r);
  // A shot of w1 first: a reason is said once until its circuit waters again
  const int shots = count(r, "ev.water.shot");
  r.waitUntil("w1 waters", [&] { return count(r, "ev.water.shot") > shots && watering(r)["running"].is_null(); }, 120);
  // w2 by hand, across three of w1's minutes: w1's shots wait for it
  const int before = skipped(r, "w1", "water.no.superseded");
  std::string why;
  INFO("refused: " << why);
  r.waitUntil("w2 may water", [&] {
    const auto a = r.call("POST", "watering/circuits/w2/now", {{"seconds", 180}});
    if (a.status != 200) why = errorKey(a);
    return a.status == 200;
  }, 120);
  r.waitUntil("w2 runs", [&] { return runs(r, 1); }, 30);
  r.waitUntil("w2 ends", [&] { return !runs(r, 1); }, 200);
  CHECK(skipped(r, "w1", "water.no.superseded") - before == 1);
  const int after = count(r, "ev.water.shot");
  r.waitUntil("w1 waters again", [&] { return count(r, "ev.water.shot") > after; }, 120);
}

TEST_CASE("Watering: a circuit's run time for the day is kept; a shot past it is not made (SD-134)") {
  Run r(harness::device());
  addCircuits(r, {{"dailyMaxS", 30}});
  waitFit(r);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 20}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.waitUntil("the shot ends", [&] { return !runs(r, 0) && watering(r)["running"].is_null(); }, 60);
  CHECK(circuitState(r, "w1")["dayS"].get<double>() == doctest::Approx(20.0).epsilon(0.1));
  r.waitUntil("its output is off again", [&] { return blockedKey(r, "w1").empty(); }, 30);
  CHECK(errorKey(r.call("POST", "watering/circuits/w1/now", {{"seconds", 20}})) == "water.no.daily");
  CHECK(blockedKey(r, "w2").empty());  // each circuit its own
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 5}});  // what is left of the day
}

TEST_CASE("Watering: a circuit removed while it waters ends its shot and switches its output off (SD-134, R1)") {
  Run r(harness::device());
  addCircuits(r);
  waitFit(r, "w2");
  r.ok("POST", "watering/circuits/w2/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 1); }, 30);
  r.step(30 * kSecond);
  r.ok("DELETE", "watering/circuits/w2");
  CHECK(watering(r)["running"].is_null());
  CHECK(watering(r)["givenL"].get<double>() >= 1.0);  // its 30 s at 2 L/min stay counted
  CHECK(count(r, "ev.water.cut") + count(r, "ev.water.shot") == 1);
  r.waitUntil("the pump stops", [&] { return !runs(r, 1); }, 10);
  r.step(2 * 60 * kSecond);
  CHECK_FALSE(runs(r, 1));
  CHECK(circuitState(r, "w2").is_null());
  // A circuit changed while it waters ends its shot too
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.ok("PUT", "watering/circuits", {{"id", "w1"}, {"name", "Tent"}, {"mode", "manual"}, {"rateLpm", 2}, {"longestS", 90}});
  CHECK(watering(r)["running"].is_null());
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.end.changed");
  r.waitUntil("the pump stops", [&] { return !runs(r, 0); }, 10);
}

TEST_CASE("Watering: water of a circuit whose flow is not known leaves the count without a value; the float \"tank empty\" guards it then (R5, RAT-068)") {
  Run r(harness::device());
  addCircuits(r, {{"rateLpm", nullptr}});
  r.ok("POST", "devices", {{"kind", "esphome"}, {"address", "esp-1.local"}, {"name", "Sensors"}});
  r.ok("PUT", "roles/tank_empty", {{"source", "esphome"}, {"device", "d2"}, {"entity", "binary_sensor/tank_empty"}});
  waitFit(r);
  CHECK(watering(r)["givenL"] == 0);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 20}});
  r.waitUntil("the shot ends", [&] { return circuitState(r, "w1")["last"].is_object() && watering(r)["running"].is_null(); }, 60);
  CHECK(watering(r)["givenL"].is_null());
  CHECK(watering(r)["availableL"].is_null());
  r.waitUntil("its output is off again", [&] { return blockedKey(r, "w2").empty(); }, 30);  // the float says there is water
  r.ok("POST", "tank/new");
  CHECK(watering(r)["givenL"] == 0);
}

TEST_CASE("Watering: a schedule starts afresh when the clock is set after a start, or set back far: the shots of the time it did not know are not made up (SD-134, R6)") {
  Run r(harness::device());
  addCircuits(r, {{"mode", "interval"}, {"everyMin", 60}, {"shotS", 20}});
  waitFit(r);
  // To 20 past a full hour, some hours back: the schedule starts from there, as it does nothing
  // until the next full hour
  const doseph::Epoch e = r.s.clock().epoch();
  r.s.clock().stepWall(-(e % 3600) + 20 * 60 - 3 * 3600);
  r.step(2 * 60 * kSecond);
  const int shots = count(r, "ev.water.shot"), skips = count(r, "ev.water.skipped");
  // A power cut of 45 min; the robot starts without a time server and goes on from its last time,
  // across no full hour
  r.s.clock().setSecured(false);
  r.s.reboot(45 * 60 * kSecond);
  REQUIRE(r.login("demo-password"));
  r.step(5 * kSecond);
  // The time server answers: the clock leaps across the full hour, whose shot does not come
  r.s.clock().setSecured(true);
  r.step(2 * 60 * kSecond);
  CHECK(count(r, "ev.water.shot") == shots);
  CHECK(count(r, "ev.water.skipped") == skips);
  CHECK_FALSE(circuitState(r, "w1")["due"].get<bool>());
  CHECK(watering(r)["running"].is_null());
}

TEST_CASE("Watering: a shot switched off at the device ends there (SD-134, RAT-019)") {
  Run r(harness::device());
  addCircuits(r);
  waitFit(r);
  r.ok("POST", "watering/circuits/w1/now", {{"seconds", 100}});
  r.waitUntil("the pump runs", [&] { return runs(r, 0); }, 30);
  r.step(10 * kSecond);  // the robot has read it on
  r.s.net().shelly("shelly-pump.local")->switches[0].on = false;  // a hand at the device
  r.waitUntil("the shot ends", [&] { return watering(r)["running"].is_null(); }, 15);
  CHECK(circuitState(r, "w1")["last"]["end"]["key"] == "water.end.output_off");
}

TEST_CASE("Watering: a due shot that waits an hour lapses; the next comes by the schedule (SD-134)") {
  Run r(harness::device());
  addCircuits(r);
  r.ok("PUT", "watering/circuits", {{"id", "w2"}, {"name", "Greenhouse"}, {"mode", "manual"}, {"rateLpm", 0.1}, {"longestS", 3 * 3600}});
  r.s.world().tank.volumeL = 400;  // the twin's pump takes 2 L/min whatever the count says
  waitFit(r);
  // w1 at fixed times in two minutes' time; w2 waters by hand for longer than an hour before
  const std::string zone = r.ok("GET", "config")["system"]["timezone"];
  const int now = doseph::localTime(r.s.clock().epoch(), zone).minuteOfDay;
  r.ok("PUT", "watering/circuits", {{"id", "w1"}, {"name", "Tent"}, {"mode", "times"}, {"times", {{{"atMin", (now + 2) % 1440}, {"s", 20}}}}, {"rateLpm", 2}, {"longestS", 120}});
  r.ok("POST", "watering/circuits/w2/now", {{"seconds", 2 * 3600}});
  r.waitUntil("w1 is due", [&] { return circuitState(r, "w1")["due"] == true; }, 4 * 60);
  r.waitUntil("it lapses", [&] { return skipped(r, "w1", "water.no.waited") == 1; }, 3700, 5000);
  CHECK_FALSE(circuitState(r, "w1")["due"].get<bool>());
  CHECK(watering(r)["running"] == "w2");
}
