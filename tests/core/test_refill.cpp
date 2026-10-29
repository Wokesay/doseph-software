// SPDX-License-Identifier: AGPL-3.0-or-later
// The refill valve (SD-132, PD-460): a fill opens on "top up water" or by hand and ends when the
// float switch says full; at the longest fill, or when the float loses its value, the refill
// latches until a person has looked (RAT-031, RAT-079). The gateway closes the valve on its own
// limits, whatever wants it open (RAT-032). Driven through the API against the twin's inlet: a
// Shelly relay whose output lets tap water in and whose input is the float switch at 105 L.
#include <doctest/doctest.h>

#include <string>

#include "doseph/api.hpp"
#include "harness.hpp"

using harness::Run;
using doseph::json;
using doseph::kSecond;

namespace {

// The twin's inlet relay: its output as the refill valve, its input as the float switch
void bindInlet(Run& r) {
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-valve.local"}, {"name", "Inlet"}});
  r.ok("PUT", "roles/refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.ok("PUT", "roles/tank_full", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.step(kSecond);
}

sim::FakeShelly& inlet(Run& r) { return *r.s.net().inlet(); }
bool open(Run& r) { return inlet(r).switches[0].on; }
json refill(Run& r) { return r.state()["refill"]; }
bool logged(Run& r, const std::string& type, const std::string& key) {
  for (const auto& e : r.events(type))
    if (e["title"]["key"] == key) return true;
  return false;
}
bool mixesFirst(Run& r) {
  const json c = r.state()["cycle"];
  return c["mixPending"] == true || (c["phase"] == "mix" && c["progress"]["first"] == true);
}

// Waits until a fill may start (no dose under way, the valve checked), then starts one
void startFill(Run& r) {
  r.waitUntil("a fill may start", [&] { return refill(r)["blocked"].is_null(); }, 900);
  r.ok("POST", "refill/start");
}

}  // namespace

TEST_CASE("Refill: a fill by hand opens the valve after its check and closes it when the float says full; the check waits meanwhile and mixes the water in first (SD-132, PD-460)") {
  Run r(harness::device());
  bindInlet(r);
  r.s.world().tank.volumeL = 100;  // below the float's line
  startFill(r);
  CHECK(refill(r)["filling"] == true);
  CHECK(refill(r)["why"] == "button");
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.step(2 * kSecond);
  CHECK(r.line() == "cycle.wait.refill");
  CHECK(r.call("POST", "refill/start").status == 409);  // one at a time
  r.waitUntil("the float says full", [&] { return !open(r); }, 5 * 60);
  CHECK(r.s.world().tank.volumeL >= 105);
  CHECK(r.s.world().tank.volumeL < 106);  // at most a poll late
  r.step(kSecond);
  CHECK(refill(r)["filling"] == false);
  CHECK(logged(r, "control", "ev.refill.done"));
  CHECK_FALSE(r.state()["latches"].contains("refill"));
  r.waitUntil("the check mixes the water in first", [&] { return mixesFirst(r); }, 60);
  // Full now: a fill by hand says why not
  const auto again = r.call("POST", "refill/start");
  CHECK(again.status == 409);
  CHECK(json::parse(again.body)["error"]["key"] == "refill.no.full");
}

TEST_CASE("Refill: a fill that reaches its longest time without \"full\" closes the valve and latches the refill; no fill opens until a person releases it (RAT-031, RAT-079)") {
  Run r(harness::device());
  bindInlet(r);
  r.s.world().tank.volumeL = 95;
  CHECK(r.call("PUT", "refill", {{"maxFillMin", 0.5}}).status == 422);
  CHECK(r.call("PUT", "refill", {{"maxFillMin", 26}}).status == 422);  // never past 25 min
  r.ok("PUT", "refill", {{"maxFillMin", 1}});
  r.act("netFloat", {{"stuck", true}});
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.waitUntil("the longest fill", [&] { return !open(r); }, 90);
  const json st = r.state();
  REQUIRE(st["latches"].contains("refill"));
  CHECK(st["latches"]["refill"]["reason"]["key"] == "refill.why_latch.time");
  CHECK(st["refill"]["filling"] == false);
  CHECK(logged(r, "alarm", "ev.refill.latched"));
  r.step(6 * kSecond);  // the watchdog looks every 5 s
  bool shown = false;
  const json wd = r.state()["watchdog"];
  for (const auto& i : wd["items"]) shown |= i["id"] == "latch.refill" && i["status"] == "problem";
  CHECK(shown);
  const auto refused = r.call("POST", "refill/start");
  CHECK(refused.status == 409);
  CHECK(json::parse(refused.body)["error"]["key"] == "refill.no.latched");
  r.step(60 * kSecond);
  CHECK_FALSE(open(r));
  // A person looked: the float is free again, the refill released, and a fill ends at full
  r.act("netFloat", {{"stuck", false}});
  r.ok("POST", "latches/refill/ack");
  r.ok("PUT", "refill", {{"maxFillMin", 15}});
  startFill(r);
  r.waitUntil("the float says full", [&] { return refill(r)["filling"] == false; }, 3 * 60);
  CHECK(r.s.world().tank.volumeL >= 105);
  CHECK_FALSE(r.state()["latches"].contains("refill"));
}

TEST_CASE("Refill: a float that loses its value during a fill closes the valve and latches the refill (RAT-031, R5)") {
  Run r(harness::device());
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-valve.local"}, {"name", "Inlet"}});
  r.ok("POST", "devices", {{"kind", "esphome"}, {"address", "esp-1.local"}, {"name", "Sensors"}});
  r.ok("PUT", "roles/refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.ok("PUT", "roles/tank_full", {{"source", "esphome"}, {"device", "d2"}, {"entity", "binary_sensor/tank_full"}});
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.act("netOnline", {{"address", "esp-1.local"}, {"online", false}});
  r.waitUntil("the valve closes", [&] { return !open(r); }, 60);
  const json st = r.state();
  REQUIRE(st["latches"].contains("refill"));
  CHECK(st["latches"]["refill"]["reason"]["key"] == "refill.why_latch.float");
}

TEST_CASE("Refill: Stop, maintenance and a hold close the valve at once; nothing latches and nothing resumes (SD-132, RAT-036)") {
  Run r(harness::device());
  bindInlet(r);
  r.s.world().tank.volumeL = 80;
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.ok("POST", "stop");
  r.step(2 * kSecond);
  CHECK_FALSE(open(r));
  CHECK(refill(r)["filling"] == false);
  CHECK_FALSE(r.state()["latches"].contains("refill"));
  CHECK(logged(r, "control", "ev.refill.ended"));
  CHECK(r.call("POST", "refill/start").status == 409);  // stopped
  r.ok("POST", "resume");
  r.step(30 * kSecond);
  CHECK_FALSE(open(r));
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.ok("POST", "maintenance", {{"minutes", 10}});
  r.step(2 * kSecond);
  CHECK_FALSE(open(r));
  CHECK(refill(r)["filling"] == false);
  r.ok("POST", "maintenance", {{"minutes", 0}});
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.act("leak", {{"on", true}});
  r.step(2 * kSecond);
  CHECK_FALSE(open(r));
  CHECK(refill(r)["filling"] == false);
  CHECK_FALSE(r.state()["latches"].contains("refill"));
}

TEST_CASE("Refill: the gateway opens the valve only while the float says \"not full\", closes it at full or past the longest fill, whatever wants it open; closed so, it opens only when wanted anew (RAT-032)") {
  Run r(harness::device());
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-valve.local"}, {"name", "Inlet"}});
  r.ok("PUT", "roles/refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.step(kSecond);
  r.s.world().tank.volumeL = 90;
  // No float: no fill, and the valve stays closed whatever wants it
  const auto none = r.call("POST", "refill/start");
  CHECK(none.status == 409);
  CHECK(json::parse(none.body)["error"]["key"] == "refill.no.float_unassigned");
  r.s.robot().wantOutput("refill_valve", true);
  r.step(20 * kSecond);
  CHECK_FALSE(open(r));
  // With a float that says "not full" it opens; at full it closes, and stays closed below it again
  r.ok("PUT", "roles/tank_full", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.s.robot().wantOutput("refill_valve", true);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.s.world().tank.volumeL = 110;
  r.waitUntil("the float closes it", [&] { return !open(r); }, 15);
  r.s.world().tank.volumeL = 90;
  r.step(30 * kSecond);
  CHECK_FALSE(open(r));
  // Past the longest fill it closes, though the float still says "not full"
  r.ok("PUT", "refill", {{"maxFillMin", 1}});
  r.s.world().tank.volumeL = 50;
  r.s.robot().wantOutput("refill_valve", true);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.waitUntil("the longest fill closes it", [&] { return !open(r); }, 75);
  r.step(30 * kSecond);
  CHECK_FALSE(open(r));
}

TEST_CASE("Refill: a restart of the robot during a fill switches the valve off, reports the cut fill, mixes first and does not resume it (SD-132, R6)") {
  Run r(harness::device());
  bindInlet(r);
  r.s.world().tank.volumeL = 80;
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.s.reboot();  // the robot alone: the relay keeps its power, and is on
  REQUIRE(r.login("demo-password"));
  CHECK(logged(r, "control", "ev.refill.cut"));
  CHECK(mixesFirst(r));
  r.waitUntil("the valve is switched off", [&] { return !open(r); }, 6);
  CHECK(refill(r)["filling"] == false);
  r.step(60 * kSecond);
  CHECK_FALSE(open(r));
}

TEST_CASE("Refill: a valve that does not open ends the fill without a latch, and the check goes on (SD-132, RAT-019)") {
  Run r(harness::device());
  bindInlet(r);
  r.s.world().tank.volumeL = 90;
  r.waitUntil("a fill may start", [&] { return refill(r)["blocked"].is_null(); }, 900);
  // Its settings changed in the maker's app since the check, and it no longer takes them
  inlet(r).switches[0].config["initial_state"] = "restore_last";
  inlet(r).ignoresConfig = true;
  r.ok("POST", "refill/start");
  r.waitUntil("the fill ends", [&] { return refill(r)["filling"] == false; }, 90);
  CHECK_FALSE(open(r));
  CHECK_FALSE(r.state()["latches"].contains("refill"));
  CHECK(logged(r, "control", "ev.refill.ended"));
  r.step(3 * kSecond);
  CHECK(r.line() != "cycle.wait.refill");
}

TEST_CASE("Refill: a change of the float's binding during a fill ends it without a latch; a backup is not restored during a fill (SD-132)") {
  Run r(harness::device());
  bindInlet(r);
  r.ok("POST", "devices", {{"kind", "esphome"}, {"address", "esp-1.local"}, {"name", "Sensors"}});
  r.s.world().tank.volumeL = 80;
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  CHECK(r.call("POST", "config/import", r.ok("GET", "config")).status == 409);
  CHECK(r.call("PUT", "refill", {{"maxFillMin", 5}}).status == 409);  // its limit stays as it began
  r.ok("PUT", "roles/tank_full", {{"source", "esphome"}, {"device", "d2"}, {"entity", "binary_sensor/tank_full"}});
  r.step(2 * kSecond);
  CHECK_FALSE(open(r));
  CHECK(refill(r)["filling"] == false);
  CHECK_FALSE(r.state()["latches"].contains("refill"));
  r.waitUntil("the check mixes the water in first", [&] { return mixesFirst(r); }, 30);
}

TEST_CASE("Refill: a valve that stops answering while it is open latches the refill (RAT-031)") {
  Run r(harness::device());
  // The float on a device of its own, which stays: it says "not full" all along
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-valve.local"}, {"name", "Inlet"}});
  r.ok("POST", "devices", {{"kind", "esphome"}, {"address", "esp-1.local"}, {"name", "Sensors"}});
  r.ok("PUT", "roles/refill_valve", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.ok("PUT", "roles/tank_full", {{"source", "esphome"}, {"device", "d2"}, {"entity", "binary_sensor/tank_full"}});
  r.s.world().tank.volumeL = 80;
  startFill(r);
  r.waitUntil("the valve opens", [&] { return open(r); }, 30);
  r.act("netReachable", {{"address", "shelly-valve.local"}, {"reachable", false}});
  r.waitUntil("the fill ends", [&] { return refill(r)["filling"] == false; }, 90);
  const json st = r.state();
  REQUIRE(st["latches"].contains("refill"));
  CHECK(st["latches"]["refill"]["reason"]["key"] == "refill.why_latch.valve");
}

TEST_CASE("Refill: only a session or an app token starts, stops or sets a fill; never an account token or the coupled device (SD-132, SD-129)") {
  for (const char* m : {"POST", "PUT"})
    for (const std::vector<std::string> p : {std::vector<std::string>{"refill", "start"}, {"refill", "stop"}, {"refill"}}) {
      CHECK(doseph::permitted(doseph::Role::App, m, p));
      CHECK_FALSE(doseph::permitted(doseph::Role::Account, m, p));
      CHECK_FALSE(doseph::permitted(doseph::Role::Peer, m, p));
    }
}

TEST_CASE("Refill: set to report only, a fill that ends without \"full\" is reported and holds nothing (SD-133, RAT-031)") {
  Run r(harness::device());
  bindInlet(r);
  r.s.world().tank.volumeL = 95;
  r.ok("PUT", "locks", {{"reportOnly", {"refill"}}});
  r.ok("PUT", "refill", {{"maxFillMin", 1}});
  r.act("netFloat", {{"stuck", true}});
  startFill(r);
  r.waitUntil("the longest fill", [&] { return refill(r)["filling"] == false; }, 90);
  const json st = r.state();
  REQUIRE(st["latches"].contains("refill"));
  CHECK(st["latches"]["refill"]["reportOnly"] == true);
  startFill(r);  // it holds nothing
  CHECK(refill(r)["filling"] == true);
}

TEST_CASE("Refill: \"top up water\" opens one fill each time it is raised; an account token cannot start one (SD-132, SD-115, SD-129)") {
  Run r(harness::device());
  bindInlet(r);
  r.s.world().tank.volumeL = 100;
  r.waitUntil("the check holds on target", [&] { return r.phase() == "hold"; }, 8 * 3600, 2000);
  r.s.world().tank.ec = 1.6;
  r.waitUntil("a check finds EC within its band", [&] { return r.s.robot().runtime().ecInBand.is_object(); }, 3 * 3600, 2000);
  // Water evaporates: EC rises without a dose, and the level falls
  r.s.world().tank.ec += 0.4;
  r.s.world().tank.volumeL = 90;
  r.waitUntil("top up water opens the valve", [&] { return open(r); }, 3 * 3600, 2000);
  CHECK(refill(r)["why"] == "top_up");
  r.waitUntil("the float says full", [&] { return !open(r); }, 5 * 60);
  CHECK(logged(r, "control", "ev.refill.done"));
  // "Top up water" still stands until a check finds EC back in its band: no second fill on its own
  r.s.world().tank.volumeL = 100;
  r.step(10 * 60 * kSecond, 2000);
  CHECK_FALSE(open(r));
  int starts = 0;
  for (const auto& e : r.events("control")) starts += e["title"]["key"] == "ev.refill.start";
  CHECK(starts == 1);
  // The account role reaches neither the button nor the settings
  const json tok = r.ok("POST", "tokens", {{"name", "Cloud"}, {"role", "account"}});
  r.token = tok["token"];
  CHECK(r.call("POST", "refill/start").status == 403);
  CHECK(r.call("PUT", "refill", {{"maxFillMin", 5}}).status == 403);
}
