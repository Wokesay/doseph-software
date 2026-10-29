// SPDX-License-Identifier: AGPL-3.0-or-later
// The circulation pump (SD-132, PD-421): a pump in the tank on a Shelly output. By default it runs
// with each dosing round and fill until the check has measured again after its mixing wait; set to
// always, all the time. Never during Stop, maintenance or a hold, and after a restart only once the
// robot switches it on again (R6). It does not shorten the mixing wait (#1). Driven through the API
// against the twin's plug, into which the circulation pump is plugged.
#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include "doseph/api.hpp"
#include "harness.hpp"

using harness::Run;
using doseph::json;
using doseph::kSecond;
using doseph::Ms;

namespace {

void bindPump(Run& r) {
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-plug.local"}, {"name", "Circulation"}});
  r.ok("PUT", "roles/circulation_pump", {{"source", "shelly"}, {"device", "d1"}, {"channel", 0}});
  r.step(kSecond);
}

bool runs(Run& r) { return r.s.net().shelly("shelly-plug.local")->switches[0].on; }

// The mixing wait of the first dosing round, as the check worked it out
double mixWait(Run& r) {
  r.waitUntil("the mixing wait", [&] { return r.phase() == "mix"; }, 3600);
  return r.state()["cycle"]["settings"]["mixMin"].get<double>();
}

}  // namespace

TEST_CASE("Circulation: by default the pump runs during each dose and its mixing wait, and is off while the check measures and once it holds; the mixing wait stays as long (SD-132, PD-421, RAT-044)") {
  Run without(harness::device());
  const double alone = mixWait(without);
  Run r(harness::device());
  bindPump(r);
  CHECK(r.state()["circulation"]["mode"] == "mixing");
  CHECK_FALSE(runs(r));
  r.waitUntil("a dose", [&] { return r.phase() == "dose"; }, 3600);
  r.waitUntil("the pump runs", [&] { return runs(r); }, 15);
  CHECK(mixWait(r) == alone);  // stability decides, not the pump (#1)
  CHECK(runs(r));
  // Through the check: in every measuring window the pump is off (a switch lags a poll at most)
  std::map<std::string, int> on, seen;
  std::string phase;
  Ms since = 0;
  r.waitUntil("the check holds", [&] {
    const std::string ph = r.phase();
    if (ph != phase) phase = ph, since = 0;
    else since += 2000;
    if (since >= 10 * kSecond) {
      seen[ph]++;
      on[ph] += runs(r);
    }
    return ph == "hold";
  }, 8 * 3600, 2000);
  CHECK(seen["remeasure"] > 0);
  CHECK(on["remeasure"] == 0);
  CHECK(on["measure"] == 0);
  CHECK(on["mix"] == seen["mix"]);
  r.waitUntil("the pump stops", [&] { return !runs(r); }, 15);
  r.step(10 * 60 * kSecond, 2000);
  CHECK_FALSE(runs(r));
}

TEST_CASE("Circulation: set to always, the pump runs; Stop, maintenance and a hold switch it off; after a restart it is off until the robot switches it on again (SD-132, R6)") {
  Run r(harness::device());
  bindPump(r);
  CHECK(r.call("PUT", "circulation", {{"mode", "sometimes"}}).status == 422);
  r.ok("PUT", "circulation", {{"mode", "always"}});
  r.waitUntil("the pump runs", [&] { return runs(r); }, 30);
  r.ok("POST", "stop");
  r.step(2 * kSecond);
  CHECK_FALSE(runs(r));
  r.ok("POST", "resume");
  r.waitUntil("the pump runs again", [&] { return runs(r); }, 30);
  r.ok("POST", "maintenance", {{"minutes", 10}});
  r.waitUntil("maintenance switches it off", [&] { return !runs(r); }, 10);
  r.ok("POST", "maintenance", {{"minutes", 0}});
  r.waitUntil("the pump runs again", [&] { return runs(r); }, 30);
  r.act("leak", {{"on", true}});
  r.waitUntil("a hold switches it off", [&] { return !runs(r); }, 10);
  r.act("leak", {{"on", false}});
  r.ok("POST", "latches/hw.leak/ack");
  r.waitUntil("the pump runs again", [&] { return runs(r); }, 30);
  // A short power cut at the plug: back, it is off (its initial state), and the robot finds it off
  // and switches it on again after its check
  r.act("netOnline", {{"address", "shelly-plug.local"}, {"online", false}});
  r.step(5 * kSecond);
  r.act("netOnline", {{"address", "shelly-plug.local"}, {"online", true}});
  CHECK_FALSE(runs(r));
  r.waitUntil("the pump runs again", [&] { return runs(r); }, 60);
  // The robot alone restarts: the plug keeps its power and is on, the robot switches it off first
  r.s.reboot();
  REQUIRE(r.login("demo-password"));
  bool wasOff = false;
  r.waitUntil("off, then on again", [&] {
    wasOff |= !runs(r);
    return wasOff && runs(r);
  }, 60);
}

TEST_CASE("Circulation: in its default mode the pump runs while the refill valve fills (SD-132)") {
  Run r(harness::device());
  bindPump(r);
  r.ok("POST", "devices", {{"kind", "shelly"}, {"address", "shelly-valve.local"}, {"name", "Inlet"}});
  r.ok("PUT", "roles/refill_valve", {{"source", "shelly"}, {"device", "d2"}, {"channel", 0}});
  r.ok("PUT", "roles/tank_full", {{"source", "shelly"}, {"device", "d2"}, {"channel", 0}});
  r.s.world().tank.volumeL = 80;
  r.waitUntil("a fill may start, with the pump off", [&] { return r.state()["refill"]["blocked"].is_null() && !runs(r); }, 3600);
  r.ok("POST", "refill/start");
  r.waitUntil("the pump runs", [&] { return runs(r); }, 15);
  CHECK(r.state()["refill"]["filling"] == true);
}

TEST_CASE("Circulation: only a session or an app token sets its mode (SD-132, SD-129)") {
  const std::vector<std::string> p = {"circulation"};
  CHECK(doseph::permitted(doseph::Role::App, "PUT", p));
  CHECK_FALSE(doseph::permitted(doseph::Role::Account, "PUT", p));
  CHECK_FALSE(doseph::permitted(doseph::Role::Peer, "PUT", p));
}
