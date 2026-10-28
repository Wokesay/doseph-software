// SPDX-License-Identifier: AGPL-3.0-or-later
// Scenarios of the new water path (#10, step 3): priming, flow, dose, drops,
// blocked outlet, high level, return pump; and of the device's parts (#10,
// step 4): the pump heads. They run the core against the
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

}  // namespace

TEST_CASE("Scenario priming: the check switches the suction pump on; water flows once it has primed, and flow is proven before the chamber is flushed (SD-105, SD-109)") {
  Run r(device("home"));
  r.waitUntil("the suction pump is switched on", [&] { return r.twin()["output"] == true; }, 60);
  CHECK(r.twin()["primed"] == false);
  CHECK(r.twin()["flowing"] == false);
  CHECK(r.state()["flowOk"] == false);
  r.waitUntil("water flows", [&] { return r.twin()["flowing"] == true; }, 15);
  CHECK(r.state()["hardware"]["outletWet"] == true);
  r.waitUntil("the chamber is flushed", [&] { return r.phase() == "flush"; }, 30);
  CHECK(r.state()["flowOk"] == true);
}

TEST_CASE("Scenario flow: a suction line out of the water gives no flow; the suction pump goes off and the check waits, then proves flow again (SD-105, SD-109)") {
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
    if (std::string(profile) == "home") CHECK(line == doctest::Approx(0.2).epsilon(0.1));
    else CHECK(line == 0);  // its pumps cannot run backwards
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

TEST_CASE("Scenario calibration: a channel calibration stops the check and the suction pump first; the check starts anew once it ends (SD-109)") {
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
