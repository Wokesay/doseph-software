// SPDX-License-Identifier: AGPL-3.0-or-later
// Scenarios of the new water path (#10, step 3): priming, flow, dose, drops,
// blocked outlet, high level, return pump; of the device's parts (#10,
// step 4): the pump heads; and step 4's acceptance (4.9). They run the core against the
// twin through the API. Where the core does not react yet, they say which
// step brings it (#10, step 4).
#include <cmath>

#include "harness.hpp"

using harness::Run;
using harness::device;
using doseph::json;
using doseph::kMinute;
using doseph::kSecond;

namespace {

// The drops each channel's sensor has seen so far, from the robot's state
std::map<int, double> dropsOf(Run& r) {
  std::map<int, double> d;
  const json channels = r.state()["channels"];  // a copy: the state is a temporary
  for (const auto& ch : channels)
    if (ch["drops"].is_object()) d[ch["channel"].get<int>()] = ch["drops"]["count"].get<double>();
  return d;
}

// Every dose dripped through its outlet: the drop sensor saw, channel by
// channel, what the journal booked since `before` (SD-111)
void dropsSawEveryDose(Run& r, const std::map<int, double>& before) {
  std::map<int, double> ml;
  std::map<int, int> runs;
  const json doses = r.events("dose");
  for (const auto& e : doses) {
    ml[e["data"]["channel"].get<int>()] += e["data"]["ml"].get<double>();
    runs[e["data"]["channel"].get<int>()] += e["data"]["runs"].get<int>();
  }
  REQUIRE_FALSE(ml.empty());
  const auto after = dropsOf(r);
  for (const auto& [ch, m] : ml) {
    CAPTURE(ch);
    const double seen = (after.at(ch) - before.at(ch)) * 0.05;
    CHECK(seen > 0);
    // Booked from the run time with the calibrated flow; the drops see the true flow:
    // within 3 % of what was booked, and a drop per run that stayed in the tip
    CHECK(std::abs(seen - m) <= 0.03 * m + 0.05 * runs[ch]);
  }
}

// Step 4's acceptance in the twin (#10, step 4.9): from tap water (EC 0.45,
// pH 7.6) to the demo recipe (EC 1.6 ± 0.1, pH 5.8–6.2), then held there for
// 12 h. The tank's own values, which the robot cannot see, are taken every
// minute: on the way pH falls at most 0.2 below its target; once the recipe
// is reached, pH stays within ± 0.2 and EC within ± 0.1, though the pH−
// dose adds EC (RAT-053, SD-121); the drop sensors saw every dose. 5 L waits
// for the smallest tank the device is meant for (#48).
void acceptance(const std::string& profile, double volumeL) {
  Run r(device(profile));
  r.act("fill", {{"volumeL", volumeL}, {"ec", 0.45}, {"ph", 7.6}});
  r.ok("PUT", "tank", {{"volumeL", volumeL}});
  const auto before = dropsOf(r);
  const double phTarget = 6.0, ecTarget = 1.6;
  double phMin = 99;
  int minutes = 0;
  for (; minutes < 12 * 60 && r.phase() != "hold"; ++minutes) {
    r.step(kMinute, 2000);
    phMin = std::min(phMin, r.twin()["tank"]["ph"].get<double>());
  }
  INFO(profile << ", " << volumeL << " L, the recipe reached after " << minutes << " min");
  REQUIRE(r.phase() == "hold");
  double phLo = 99, phHi = 0, ecLo = 99, ecHi = 0;
  for (int m = 0; m < 12 * 60; ++m) {
    r.step(kMinute, 2000);
    const json tank = r.twin()["tank"];
    const double ph = tank["ph"].get<double>(), ec = tank["ec"].get<double>();
    phMin = std::min(phMin, ph);
    phLo = std::min(phLo, ph);
    phHi = std::max(phHi, ph);
    ecLo = std::min(ecLo, ec);
    ecHi = std::max(ecHi, ec);
  }
  INFO("pH " << phLo << "–" << phHi << ", lowest " << phMin << "; EC " << ecLo << "–" << ecHi);
  CHECK(phMin >= phTarget - 0.2);  // the overshoot
  CHECK(phLo >= phTarget - 0.2);
  CHECK(phHi <= phTarget + 0.2);
  CHECK(ecLo >= ecTarget - 0.1);
  CHECK(ecHi <= ecTarget + 0.1);
  CHECK(r.state()["latches"].empty());
  dropsSawEveryDose(r, before);
}

}  // namespace

TEST_CASE("Scenario flow at the start: the check switches the feed pump on; water flows once its hose has filled, and flow is proven before the chamber is flushed (SD-105, SD-127)") {
  Run r(device("home"));
  r.waitUntil("the feed pump is switched on", [&] { return r.twin()["output"] == true; }, 60);
  CHECK(r.twin()["primed"] == false);
  CHECK(r.twin()["flowing"] == false);
  CHECK(r.state()["flowOk"] == false);
  r.waitUntil("water flows", [&] { return r.twin()["flowing"] == true; }, 15);
  CHECK(r.state()["hardware"]["outletWet"] == true);
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 30);
  CHECK(r.state()["flowOk"] == true);
}

TEST_CASE("Scenario flow: a feed pump out of the water gives no flow; it goes off within seconds and the check waits, then proves flow again (SD-105, SD-127)") {
  Run r(device("home"));
  r.act("flowFault", {{"fault", "dry"}});
  r.waitUntil("the check waits for flow", [&] { return r.line() == "cycle.wait.no_flow"; }, 4 * 60);
  CHECK(r.twin()["output"] == false);  // a pump without flow may run dry
  CHECK(r.events("alarm").size() >= 1);
  CHECK(r.state()["dosing"].is_null());
  r.act("flowFault", {{"fault", ""}});
  r.waitUntil("flow is proven again", [&] { return r.phase() == "flush"; }, 10 * 60);
}

TEST_CASE("Scenario dose and drops: every dose drips through its outlet; the drop sensor sees what went in, channel by channel (SD-111, #10 acceptance)") {
  Run r(device("home"));
  const auto before = dropsOf(r);
  REQUIRE(before.size() == 4);
  r.waitUntil("the check holds", [&] { return r.phase() == "hold"; }, 8 * 3600, 2000);
  dropsSawEveryDose(r, before);
}

TEST_CASE("Scenario blocked outlet: the pump runs but nothing drips; the run is cut within the wait for a drop and the channel locked until a person releases it (SD-111)") {
  Run r(device("home"));
  r.act("channelFault", {{"channel", 2}, {"outletBlocked", true}});
  r.waitUntil("channel 2 is locked", [&] { return r.state()["latches"].contains("channel.2.no_drops"); }, 2 * 3600, 2000);
  CHECK(r.events("alarm")[0]["title"]["key"] == "ev.channel.no_drops");
  CHECK(r.twin()["channels"][1]["drops"] == 0);
  const json cut = r.events("dose")[0];
  CHECK(cut["title"]["key"] == "ev.dose_incomplete");
  CHECK(cut["data"]["channel"] == 2);
  CHECK(cut["data"]["ms"].get<double>() <= 8000);  // 5 s and the ticks around it
  // Part A waits for its channel, with the reason
  r.waitUntil("the check waits for channel 2", [&] { return r.line() == "cycle.wait.channel_locked"; }, 2 * 3600, 2000);
  // Cleared and released, part A drips again
  r.act("channelFault", {{"channel", 2}, {"outletBlocked", false}});
  r.ok("POST", "latches/channel.2.no_drops/ack", json::object());
  r.waitUntil("part A drips", [&] { return r.twin()["channels"][1]["drops"].get<double>() > 0; }, 2 * 3600, 2000);
  CHECK_FALSE(r.state()["latches"].contains("channel.2.no_drops"));
}

TEST_CASE("Scenario empty canister: it runs and nothing comes; no drops lock its channel at once (SD-111)") {
  Run r(device("home"));
  r.act("channelFault", {{"channel", 3}, {"empty", true}});
  r.waitUntil("channel 3 is locked", [&] { return r.state()["latches"].contains("channel.3.no_drops"); }, 2 * 3600, 2000);
  CHECK(r.events("dose")[0]["data"]["ms"].get<double>() <= 8000);
}

TEST_CASE("Scenario siphon: a canister above the device drips while its pump stands; the channel is locked with an alarm and the check mixes first (SD-111, PD-455)") {
  Run r(device("home"));
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 3 * 60);
  r.act("channelFault", {{"channel", 4}, {"siphon", true}});
  r.waitUntil("channel 4 is locked", [&] { return r.state()["latches"].contains("channel.4.siphon"); }, 60);
  CHECK(r.events("alarm")[0]["title"]["key"] == "ev.channel.siphon");
  CHECK(r.state()["cycle"]["mixPending"] == true);
  // It goes on dripping: one alarm, not one per drop
  r.step(2 * kMinute);
  CHECK(r.events("alarm").size() == 1);
  r.act("channelFault", {{"channel", 4}, {"siphon", false}});
  r.ok("POST", "latches/channel.4.siphon/ack", json::object());
  r.step(2 * kMinute);
  CHECK(r.state()["latches"].empty());
}

TEST_CASE("Scenario high level: a blocked return fills the chamber; the hardware release stops every pump, the robot latches it with the reason, and a person releases it once the chamber has drained (SD-110)") {
  Run r(device("home"));
  r.waitUntil("water flows", [&] { return r.twin()["flowing"] == true; }, 60);
  r.act("returnFault", {{"fault", "blocked"}});
  r.waitUntil("high level", [&] { return r.state()["hardware"]["release"]["highLevel"] == true; }, 30);
  r.step(5 * kSecond);
  json st = r.state();
  CHECK(st["hardware"]["release"]["on"] == false);
  CHECK(st["latches"].contains("hw.high_level"));
  CHECK(st["cycle"]["line"]["key"] == "cycle.wait.hold.high_level");
  CHECK(r.events("alarm")[0]["title"]["key"] == "ev.hw.high_level");
  CHECK(r.twin()["output"] == false);
  r.step(2 * kMinute);
  const json channels = r.twin()["channels"];
  for (const auto& ch : channels) CHECK(ch["state"] != 1);  // no pump runs
  CHECK(r.state()["dosing"].is_null());
  // While the chamber is full, the latch stays
  const auto refused = r.call("POST", "latches/hw.high_level/ack", json::object());
  CHECK(refused.status == 409);
  CHECK(json::parse(refused.body)["error"]["key"] == "latch.cause_present");
  // The return is cleared and the chamber drains; the latch still holds until a person releases it
  r.act("returnFault", {{"fault", ""}});
  r.waitUntil("the chamber has drained", [&] { return r.state()["hardware"]["release"]["on"] == true; }, 60);
  r.step(30 * kSecond);
  CHECK(r.line() == "cycle.wait.hold.high_level");
  CHECK(r.twin()["output"] == false);
  r.ok("POST", "latches/hw.high_level/ack", json::object());
  r.waitUntil("flow is proven again", [&] { return r.phase() == "flush"; }, 3 * 60);
}

TEST_CASE("Scenario leak: everything goes off at once and stays off, latched across a restart, until the leak is fixed and a person releases it (SD-110, RAT-063)") {
  Run r(device("pro"));
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 3 * 60);
  r.act("leak", {{"on", true}});
  r.step(2 * kSecond);
  CHECK(r.twin()["output"] == false);
  CHECK(r.twin()["returnPump"] == false);
  CHECK(r.state()["latches"].contains("hw.leak"));
  CHECK(r.line() == "cycle.wait.hold.leak");
  // A channel job is refused with the reason
  const auto prime = r.call("POST", "channels/1/prime", {{"seconds", 3}});
  CHECK(prime.status != 200);
  CHECK(prime.body.find("act.hold.leak") != std::string::npos);
  CHECK(r.call("POST", "latches/hw.leak/ack", json::object()).status == 409);
  // A restart releases nothing (RAT-063)
  r.act("reboot", {{"outageS", 10}});
  REQUIRE(r.login("demo-password"));
  r.step(5 * kSecond);
  CHECK(r.state()["latches"].contains("hw.leak"));
  CHECK(r.twin()["output"] == false);
  r.act("leak", {{"on", false}});
  r.step(10 * kSecond);
  CHECK(r.line() == "cycle.wait.hold.leak");  // fixed, but nothing resumes on its own
  r.ok("POST", "latches/hw.leak/ack", json::object());
  r.waitUntil("the check starts again", [&] { return r.phase() == "flush"; }, 3 * 60);
}

TEST_CASE("Scenario stop button: it is the Stop of the app; it holds after a restart, and Resume is in the app (SD-110, RAT-036)") {
  Run r(device("home"));
  r.waitUntil("water flows", [&] { return r.twin()["flowing"] == true; }, 60);
  r.act("stopButton", {{"pressed", true}});
  r.step(2 * kSecond);
  CHECK(r.state()["stopped"] == true);
  CHECK(r.twin()["output"] == false);
  r.act("stopButton", {{"pressed", false}});
  r.act("reboot", {{"outageS", 10}});
  REQUIRE(r.login("demo-password"));
  r.step(30 * kSecond);
  CHECK(r.state()["stopped"] == true);  // a restart never resumes (R6)
  CHECK(r.twin()["output"] == false);
  r.ok("POST", "resume", json::object());
  r.waitUntil("the check starts again", [&] { return r.phase() == "flush"; }, 3 * 60);
}

TEST_CASE("Scenario return pump: it runs while water comes back and stops below its level; when it fails, the chamber rises to high level (SD-109, SD-110)") {
  Run r(device("pro"));
  bool ran = false, stopped = false;
  r.waitUntil("the return pump ran and stopped", [&] {
    const json w = r.state()["hardware"];
    if (w["returnPump"] == true) ran = true;
    if (ran && w["returnPump"] == false) stopped = true;
    return ran && stopped;
  }, 3 * 60);
  CHECK(r.state()["hardware"]["release"]["highLevel"] == false);
  r.act("returnFault", {{"fault", "failed"}});
  r.waitUntil("high level", [&] { return r.state()["hardware"]["release"]["highLevel"] == true; }, 60);
  CHECK(r.state()["hardware"]["release"]["on"] == false);
  CHECK(r.twin()["output"] == false);
}

TEST_CASE("Scenario pump head: a changed head voids the channel's calibration until it is calibrated again; without a head the channel runs nothing (SD-108)") {
  Run r(device("home"));
  const auto channel2 = [&] { return r.state()["channels"][1]; };
  const auto headEvents = [&](const std::string& key) {
    int n = 0;
    const json events = r.events("calibration");
    for (const auto& e : events) n += e["title"]["key"] == key;
    return n;
  };
  REQUIRE(channel2()["head"] == "S");
  REQUIRE(channel2()["flowMlPerMin"].is_number());
  const double small = r.twin()["channels"][1]["trueFlow"];
  r.act("channelHead", {{"channel", 2}, {"head", "M"}});
  r.step(5 * kSecond);
  CHECK(channel2()["head"] == "M");
  CHECK(channel2()["flowMlPerMin"].is_null());
  CHECK(headEvents("ev.channel.head_changed") == 1);  // once, not every tick
  // Calibrated again, the channel doses with the new head's flow
  r.ok("POST", "channels/2/calibrate", {{"seconds", 10}});
  r.waitUntil("the run into the cup", [&] { return r.state()["channelJob"]["state"] == "waiting_result"; }, 30);
  r.ok("POST", "channels/result", {{"ml", r.twin()["channels"][1]["cupMl"]}});
  r.ok("POST", "channels/cancel", json::object());
  CHECK(channel2()["flowMlPerMin"].get<double>() == doctest::Approx(2.5 * small).epsilon(0.02));
  r.step(5 * kSecond);
  CHECK(channel2()["flowMlPerMin"].is_number());
  CHECK(r.s.control("channelHead", {{"channel", 2}, {"head", "XL"}}).contains("error"));  // no such head
  // Taken off: the calibration is void, and nothing runs on the channel
  r.act("channelHead", {{"channel", 2}, {"head", ""}});
  r.step(5 * kSecond);
  CHECK(channel2()["head"].is_null());
  CHECK(channel2()["flowMlPerMin"].is_null());
  CHECK(headEvents("ev.channel.head_removed") == 1);
  const auto prime = r.call("POST", "channels/2/prime", {{"seconds", 3}});
  CHECK(prime.status == 409);
  CHECK(prime.body.find("act.no_head") != std::string::npos);
}

TEST_CASE("Scenario pull back: after a dose the line is pulled back about 0.2 ml where the pump can; the next run fills it first, and every dose still drips in full (SD-111)") {
  for (const char* profile : {"home", "pro"}) {
    CAPTURE(profile);
    Run r(device(profile));
    r.waitUntil("the first dose", [&] { return !r.events("dose").empty(); }, 2 * 3600, 2000);
    r.step(5 * kSecond);
    const int ch = r.events("dose")[0]["data"]["channel"];
    const double line = r.twin()["channels"][ch - 1]["lineMl"];
    CHECK(line == doctest::Approx(0.2).epsilon(0.1));  // every pump runs both ways (SD-142)
  }
}

TEST_CASE("Scenario line fill: a line of air is filled into the tank until its first drops fall; it is booked like a dose, and the check mixes it in first (SD-111)") {
  Run r(device("home"));
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 3 * 60);
  r.act("lineEmpty", {{"channel", 4}});
  const double before = r.state()["stock"]["calmag"]["ml"].is_number() ? r.state()["stock"]["calmag"]["ml"].get<double>() : doseph::kNaN;
  r.ok("POST", "channels/4/prime", json::object());
  CHECK(r.state()["channelJob"]["type"] == "fill");
  r.waitUntil("the line is full", [&] { return r.state()["channelJob"]["state"] == "done"; }, 60, 250);
  CHECK(r.state()["channelJob"]["message"]["key"] == "job.filled");
  const json ch = r.twin()["channels"][3];
  CHECK(ch["lineMl"] == 0);
  CHECK(ch["drops"].get<double>() >= 1);
  CHECK(ch["drops"].get<double>() <= 6);  // it stopped at its first drops, within a tick of 250 ms
  CHECK(r.events("dose")[0]["title"]["key"] == "ev.fill");
  CHECK(r.state()["cycle"]["mixPending"] == true);
  if (std::isfinite(before)) CHECK(r.state()["stock"]["calmag"]["ml"].get<double>() < before - 5);  // the line's 6 ml
  r.ok("POST", "channels/cancel", json::object());
}

TEST_CASE("Scenario line fill without drops: a blocked outlet ends the fill after 5 min with the reason (SD-111)") {
  Run r(device("home"));
  r.act("channelFault", {{"channel", 4}, {"outletBlocked", true}});
  r.ok("POST", "channels/4/prime", json::object());
  r.waitUntil("the fill gives up", [&] { return r.state()["channelJob"]["state"] == "failed"; }, 6 * 60, 2000);
  CHECK(r.state()["channelJob"]["message"]["key"] == "dose.fill_no_drops");
  CHECK(r.state()["channelJob"]["actualS"].get<double>() == doctest::Approx(300).epsilon(0.01));
  CHECK(r.events("dose")[0]["title"]["key"] == "ev.fill_incomplete");
}

TEST_CASE("Scenario line fill cut by a restart: reported as a cut dose, and the check mixes first (SD-106, SD-111, R6)") {
  Run r(device("home"));
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 3 * 60);
  r.act("lineEmpty", {{"channel", 4}});
  r.ok("POST", "channels/4/prime", json::object());
  r.step(3 * kSecond);
  r.act("reboot", {{"outageS", 10}});
  REQUIRE(r.login("demo-password"));
  CHECK(r.events("dose")[0]["title"]["key"] == "ev.boot.cut_dose");
  CHECK(r.state()["cycle"]["mixPending"] == true);
  CHECK(r.state()["channelJob"].is_null());  // nothing resumes
}

TEST_CASE("Scenario calibration: a channel calibration stops the check and the feed pump first; the check starts anew once it ends (SD-109)") {
  Run r(device("home"));
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 3 * 60);
  REQUIRE(r.twin()["output"] == true);
  r.ok("POST", "channels/2/calibrate", {{"seconds", 10}});
  r.step(2 * kSecond);
  CHECK(r.line() == "cycle.wait.channel_job");
  CHECK(r.twin()["output"] == false);
  r.waitUntil("the run into the cup", [&] { return r.state()["channelJob"]["state"] == "waiting_result"; }, 30);
  r.step(30 * kSecond);
  CHECK(r.line() == "cycle.wait.channel_job");  // the outlet is still over the cup
  r.ok("POST", "channels/result", {{"ml", r.twin()["channels"][1]["cupMl"]}});
  r.waitUntil("the check starts anew", [&] { return r.phase() == "flush"; }, 3 * 60);
}

TEST_CASE("Scenario return pump dry or blocked: it runs a minute without pause; it is held off and latched across a restart, everything holds, and a person releases it once fixed (SD-109)") {
  Run r(device("pro"));
  r.waitUntil("water flows", [&] { return r.twin()["flowing"] == true; }, 60);
  r.act("returnFault", {{"fault", "failed"}});  // it runs and moves nothing
  r.waitUntil("the return pump is held off", [&] { return r.state()["latches"].contains("hw.return"); }, 3 * 60);
  bool alarmed = false;
  const json alarms = r.events("alarm");
  for (const auto& e : alarms) alarmed |= e["title"]["key"] == "ev.hw.return";
  CHECK(alarmed);
  r.step(2 * kSecond);
  CHECK(r.twin()["returnLocked"] == true);
  CHECK(r.twin()["returnPump"] == false);
  CHECK(r.twin()["output"] == false);
  // A restart: the device forgets it, the robot holds it off again (R6)
  r.act("reboot", {{"outageS", 10}});
  REQUIRE(r.login("demo-password"));
  r.step(2 * kSecond);
  CHECK(r.state()["latches"].contains("hw.return"));
  CHECK(r.twin()["returnLocked"] == true);
  // Fixed: the return pump is released first, the chamber drains, then high level is released
  r.act("returnFault", {{"fault", ""}});
  CHECK(r.call("POST", "latches/hw.high_level/ack", json::object()).status == 409);
  r.ok("POST", "latches/hw.return/ack", json::object());
  r.waitUntil("the chamber has drained", [&] { return r.state()["hardware"]["release"]["on"] == true; }, 60);
  r.ok("POST", "latches/hw.high_level/ack", json::object());
  r.waitUntil("the check starts anew", [&] { return r.phase() == "flush"; }, 3 * 60);
}

TEST_CASE("Scenario daily amount: a canister's ml per day holds its doses back with the reason; the state shows each channel's day (SD-113)") {
  Run r(device("home"));
  r.ok("POST", "canisters", {{"id", "part_b"}, {"maxMlPerDay", 1}});
  r.waitUntil("the check waits for part B's day", [&] { return r.line() == "cycle.wait.daily"; }, 2 * 3600, 2000);
  CHECK(r.state()["cycle"]["line"]["args"]["reason"]["key"] == "act.daily_ml");
  const json day = r.state()["channels"][2]["day"];  // channel 3: part B
  CHECK(day["capS"] == 40 * 60);                     // the example head's value
  CHECK(day["usedS"] == 0);
}

TEST_CASE("Scenario top up water: EC above its band with no nutrient in since a check found it within raises \"top up water\" once; nutrients wait, and it goes when EC is back (SD-115, RAT-012)") {
  Run r(device("home"));
  r.waitUntil("the check holds on target", [&] { return r.phase() == "hold"; }, 8 * 3600, 2000);
  CHECK(r.state()["topUp"] == false);
  const auto ecDoses = [&] {
    int n = 0;
    const json doses = r.events("dose");
    for (const auto& e : doses) n += e["data"]["purpose"] == "ec";
    return n;
  };
  // A check finds EC within its band, and nothing is dosed after it
  r.s.world().tank.ec = 1.6;
  r.waitUntil("a check finds EC within its band", [&] { return r.s.robot().runtime().ecInBand.is_object(); }, 3 * 3600, 2000);
  const int before = ecDoses();
  // Water evaporates: EC rises without a dose
  r.s.world().tank.ec += 0.4;
  r.waitUntil("top up water", [&] { return r.state()["topUp"] == true; }, 2 * 3600, 2000);
  r.step(6 * doseph::kSecond);  // the watchdog looks every 5 s
  const json st = r.state();
  bool watched = false, noted = false;
  for (const auto& i : st["watchdog"]["items"]) watched |= i["id"] == "water" && i["status"] == "problem";
  for (const auto& n : st["cycle"]["notes"]) noted |= n["key"] == "cycle.note.top_up";
  CHECK(watched);
  CHECK(noted);
  int raised = 0;
  const json control = r.events("control");
  for (const auto& e : control) raised += e["title"]["key"] == "ev.water.top_up";
  CHECK(raised == 1);
  r.step(3 * 3600 * doseph::kSecond, 2000);  // further checks raise it no second time
  int again = 0;
  const json later = r.events("control");
  for (const auto& e : later) again += e["title"]["key"] == "ev.water.top_up";
  CHECK(again == 1);
  CHECK(ecDoses() == before);  // nutrients wait (RAT-012)
  // Topped up: back within the band, the message goes
  r.s.world().tank.ec -= 0.4;
  r.waitUntil("EC back within its band", [&] { return r.state()["topUp"] == false; }, 3 * 3600, 2000);
  bool ok = false;
  const json done = r.events("control");
  for (const auto& e : done) ok |= e["title"]["key"] == "ev.water.ok";
  CHECK(ok);
}

TEST_CASE("Scenario large tank: above 150 L the wait is 60 min until a clean dose's settling is learned from the history, then 1.5 × the longest of the last three (SD-112)") {
  Run r(device("home"));
  r.s.world().tank.volumeL = 200;
  r.ok("PUT", "tank", {{"volumeL", 200}});
  json set = r.state()["cycle"]["settings"];
  CHECK(set["mixWhy"] == "settling_default");
  CHECK(set["mixMin"] == 60);
  r.waitUntil("a dose's settling is learned", [&] { return !r.s.robot().runtime().settleMin.empty(); }, 6 * 3600, 2000);
  const double settled = r.s.robot().runtime().settleMin.back();
  CHECK(settled > 0);
  CHECK(settled < 60);
  set = r.state()["cycle"]["settings"];
  CHECK(set["mixWhy"] == "settling");
  CHECK(set["mixOwnMin"].get<double>() == doctest::Approx(std::max(5.0, 1.5 * settled)));
}

TEST_CASE("Acceptance 50 L: tap water to the recipe and held there; pH within ± 0.2, EC within ± 0.1, an overshoot of at most 0.2 pH, every dose seen by its drop sensor (#10, step 4.9, SD-121)") {
  acceptance("home", 50);
}

TEST_CASE("Acceptance 100 L: tap water to the recipe and held there; pH within ± 0.2, EC within ± 0.1, an overshoot of at most 0.2 pH, every dose seen by its drop sensor (#10, step 4.9, SD-121)") {
  acceptance("home", 100);
}

TEST_CASE("Acceptance 100 L with the Pro example: a flow meter, a return pump and larger pump heads hold the recipe the same way (#10, step 4.9, SD-121)") {
  acceptance("pro", 100);
}

TEST_CASE("Scenario alarms: a leak is a critical alarm, pushed at once and repeated; acknowledging pauses the reminders and keeps the alarm; quiet hours are set and checked (SD-123, RAT-022)") {
  Run r(device("home"));
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 3 * 60);
  r.act("leak", {{"on", true}});
  r.waitUntil("the leak is an alarm", [&] { return !r.state()["alarms"].empty(); }, 30);
  json a = r.state()["alarms"][0];
  CHECK(a["id"] == "latch.hw.leak");
  CHECK(a["level"] == "critical");
  json sent = r.ok("GET", "notifications")["notifications"];
  REQUIRE(sent.size() == 1);
  CHECK(sent[0]["kind"] == "alarm");
  CHECK(sent[0]["items"][0] == "latch.hw.leak");
  r.step(5 * kMinute + 10 * kSecond);
  CHECK(r.ok("GET", "notifications")["notifications"].size() == 2);  // the first reminder
  r.ok("POST", "alarms/latch.hw.leak/ack", json::object());
  a = r.state()["alarms"][0];
  CHECK(a["ackUntil"].get<doseph::Epoch>() > r.state()["now"].get<doseph::Epoch>());
  r.step(15 * kMinute);
  CHECK(r.ok("GET", "notifications")["notifications"].size() == 2);  // paused
  CHECK(r.call("POST", "alarms/latch.flow.dry/ack", json::object()).status == 404);
  // Quiet hours as local minutes of the day; a time outside the day is refused
  r.ok("PUT", "notify", {{"quietFromMin", 21 * 60}, {"quietToMin", 6 * 60 + 30}});
  CHECK(r.ok("GET", "config")["notify"]["quietToMin"] == 6 * 60 + 30);
  CHECK(r.call("PUT", "notify", {{"quietFromMin", 24 * 60}}).status == 422);
  CHECK(r.call("PUT", "notify", {{"quietFromMin", "22:00"}}).status == 422);
  // The leak is fixed and released: the alarm ends
  r.act("leak", {{"on", false}});
  r.ok("POST", "latches/hw.leak/ack", json::object());
  r.step(10 * kSecond);
  CHECK(r.state()["alarms"].empty());
}
