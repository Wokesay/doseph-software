// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "folder_lock.hpp"
#include "simulation.hpp"

namespace fs = std::filesystem;
using doseph::json;

TEST_CASE("Simulator: one simulator per data folder; the lock ends with its holder, and a link gives none") {
  const fs::path dir = fs::temp_directory_path() / ("doseph-lock-test-" + std::to_string(::doctest::getContextOptions()->rand_seed));
  fs::remove_all(dir);
  {
    sim::FolderLock first(dir / "data");  // the folder is made
    CHECK(first.state() == sim::FolderLock::State::Held);
    sim::FolderLock second(dir / "data");
    CHECK(second.state() == sim::FolderLock::State::Taken);
    CHECK(fs::exists(dir / "data" / "simulator.lock"));
  }
  {
    sim::FolderLock again(dir / "data");  // the first one ended: free again, the file stays
    CHECK(again.state() == sim::FolderLock::State::Held);
  }
#ifndef _WIN32
  // A link under the lock's name is left as it is, and nothing is written where it points
  fs::create_directories(dir / "linked");
  const fs::path target = dir / "elsewhere.txt";
  std::ofstream(target) << "keep";
  fs::create_symlink(target, dir / "linked" / "simulator.lock");
  sim::FolderLock linked(dir / "linked");
  CHECK(linked.state() == sim::FolderLock::State::None);
  CHECK(linked.why().find("link") != std::string::npos);
  std::ifstream in(target);
  std::string text;
  in >> text;
  CHECK(text == "keep");
#endif
  fs::remove_all(dir);
}

TEST_CASE("Simulator: the example profiles Home and Pro are marked as assumptions; the twin has one pump per channel (SD-108)") {
  const auto home = sim::exampleProfile("home"), pro = sim::exampleProfile("pro");
  REQUIRE(home);
  REQUIRE(pro);
  CHECK_FALSE(sim::exampleProfile("Pro 30"));
  CHECK(home->example);
  CHECK(pro->example);
  CHECK(home->returnBy == "gravity");
  CHECK(pro->returnBy == "pump");
  CHECK(pro->network == std::vector<std::string>{"wifi", "lan"});
  for (const auto* p : {&*home, &*pro})
    for (const doseph::ChannelCaps& c : p->channels) {
      CHECK_FALSE(c.head.empty());
      CHECK(c.dropSensor);  // every outlet has a drop sensor (#9)
    }

  for (const std::string name : {"home", "pro"}) {
    sim::Options o;
    o.scenario = "demo";
    o.password = "demo-password";
    o.prefillHours = 0;
    o.profile = name;
    sim::Simulation s(o);
    const json info = s.robot().info();
    const auto p = sim::exampleProfile(name);
    CHECK(info["profile"]["line"] == name);
    CHECK(info["profile"]["example"] == true);
    CHECK(info["profile"]["channels"].size() == p->channels.size());
    CHECK(info["channelCount"] == static_cast<int>(p->channels.size()));
    CHECK(s.world().pumps.size() == p->channels.size());
    CHECK(s.robot().diagnostics()["info"]["profile"] == info["profile"]);
  }
}

TEST_CASE("Twin: the feed pump in the tank delivers once its hose has filled; a blocked inlet, a pump out of the water or a hose off gives no flow, and after 9 s the hardware cuts it until it is switched off (SD-127, PD-477)") {
  sim::World w;
  w.reset(4);
  w.fill(100, 0.45, 7.6);
  w.output = true;
  w.advance(1000);
  CHECK_FALSE(w.flowing());
  w.advance(3000);
  CHECK(w.flowing());
  CHECK(w.outletWet());
  CHECK(w.rawFlow() > 11.0);
  for (const char* fault : {"blocked", "dry", "hose"}) {
    w.flowFault = fault;
    w.advance(w.now() + 2000);
    CHECK_FALSE(w.flowing());
    CHECK_FALSE(w.outletWet());
    CHECK(w.rawFlow() == 0.0);
    CHECK(w.output);
    w.flowFault.clear();
    w.advance(w.now() + 1000);
  }
  // Without flow its sensor sees, the hardware switches the pump and the dosing pumps off
  w.flowFault = "sensor";
  std::string err;
  REQUIRE(w.startRun(1, 60000, "j1", err));
  w.advance(w.now() + 8500);
  CHECK(w.output);
  w.advance(w.now() + 1000);
  CHECK_FALSE(w.output);
  CHECK(w.noFlowCut);
  CHECK(w.runStatus(1).state == doseph::RunStatus::State::Failed);
  CHECK(w.runStatus(1).error == "no flow");
}

TEST_CASE("Twin: a feed pump in the device primes for 8 s before water flows (SD-109)") {
  sim::World w;
  w.reset(4);
  w.pumpInTank = false;
  w.fill(100, 0.45, 7.6);
  w.output = true;
  w.advance(4000);
  CHECK_FALSE(w.primed());
  CHECK_FALSE(w.flowing());
  w.advance(10000);
  CHECK(w.flowing());
  w.flowFault = "dry";
  w.advance(w.now() + 20000);
  CHECK(w.output);  // no cut of the hardware's for it
  CHECK_FALSE(w.noFlowCut);
}

TEST_CASE("Twin: the chamber keeps its water at the weir; a blocked return or a failed return pump fills it to high level, and the hardware release stops every pump (SD-109, SD-110)") {
  for (const bool byPump : {false, true}) {
    CAPTURE(byPump);
    sim::World w;
    w.reset(4);
    w.returnByPump = byPump;
    w.fill(100, 0.45, 7.6);
    w.output = true;
    w.advance(5 * 60 * 1000);  // a check: water through the chamber and back
    CHECK(w.flowing());
    CHECK_FALSE(w.highLevel());
    CHECK(w.chamberL >= 0.15);
    w.output = false;
    w.advance(w.now() + 60 * 1000);
    CHECK(w.chamberL == doctest::Approx(0.15));  // the weir keeps the probes wet
    CHECK_FALSE(w.returnPumpOn);
    // The return fails while the feed pump runs: the chamber fills up
    w.returnFault = byPump ? "failed" : "blocked";
    w.output = true;
    std::string err;
    REQUIRE(w.startRun(2, 20000, "dose-1", err));  // a run while the pump primes
    w.advance(w.now() + 12 * 1000);
    CHECK(w.highLevel());
    CHECK_FALSE(w.released());
    CHECK_FALSE(w.output);  // the feed pump is off, below the robot
    CHECK(w.runStatus(2).state == doseph::RunStatus::State::Failed);
    CHECK(w.runStatus(2).error == "hardware release off");
    CHECK_FALSE(w.startRun(3, 1000, "dose-2", err));
    CHECK(err == "hardware release off");
  }
}

TEST_CASE("Twin: a return pump switches by level while water comes (SD-109)") {
  sim::World w;
  w.reset(4);
  w.returnByPump = true;
  w.fill(100, 0.45, 7.6);
  w.output = true;
  bool on = false, off = false;
  for (int i = 0; i < 240; ++i) {
    w.advance(w.now() + 500);
    if (w.flowing()) (w.returnPumpOn ? on : off) = true;
    CHECK_FALSE(w.highLevel());
  }
  CHECK(on);
  CHECK(off);
}

TEST_CASE("Simulator: a leak or the stop button switches the hardware release off; the robot's state says why; flow by method (SD-109, SD-110)") {
  for (const std::string name : {"home", "pro"}) {
    CAPTURE(name);
    sim::Options o;
    o.scenario = "demo";
    o.password = "demo-password";
    o.prefillHours = 0;
    o.profile = name;
    sim::Simulation s(o);
    s.step(2 * 60 * 1000);
    json hw = s.robot().state()["hardware"];
    CHECK(hw["release"]["on"] == true);
    if (name == "home") {
      // No flow meter: the outlet's level sensor, and the flow estimated from the pump's low end
      CHECK(hw["outletWet"] == true);
      CHECK(hw["returnPump"].is_null());
      CHECK(s.robot().state()["readings"]["flow"]["value"] == 10.0);
    } else {
      CHECK(hw["outletWet"].is_null());
      CHECK(hw["returnPump"].is_boolean());
      CHECK(s.robot().state()["readings"]["flow"]["value"].get<double>() == doctest::Approx(12).epsilon(0.05));
    }
    s.control("leak", {{"on", true}});
    s.step(2000);
    hw = s.robot().state()["hardware"];
    CHECK(hw["release"]["on"] == false);
    CHECK(hw["release"]["leak"] == true);
    CHECK_FALSE(s.world().output);
    s.control("leak", {{"on", false}});
    s.control("stopButton", {{"pressed", true}});
    s.step(2000);
    hw = s.robot().state()["hardware"];
    CHECK(hw["release"]["on"] == false);
    CHECK(hw["release"]["stopButton"] == true);
    CHECK(hw["release"]["leak"] == false);
  }
}

namespace {
sim::World twinWithWater() {
  sim::World w;
  w.reset(4);
  w.fill(100, 0.45, 7.6);
  return w;
}
}  // namespace

TEST_CASE("Twin: the drop sensor counts each drop of a dose; a blocked outlet, an empty canister or a run into the cup gives none (SD-111)") {
  sim::World w = twinWithWater();
  std::string err;
  REQUIRE(w.startRun(2, 6000, "dose-1", err));
  w.advance(w.now() + 7000);
  const double ml = w.pumps[1].trueFlow * 6.0 / 60.0;
  CHECK(w.pumps[1].drops == static_cast<std::uint64_t>(ml / 0.05));
  CHECK(w.pumps[1].lastDropMs > 0);
  CHECK(w.tank.pendingEc > 0.0);
  for (const char* fault : {"outlet", "empty", "cup"}) {
    CAPTURE(fault);
    sim::World v = twinWithWater();
    v.pumps[1].outletBlocked = std::string(fault) == "outlet";
    v.pumps[1].empty = std::string(fault) == "empty";
    REQUIRE(v.startRun(2, 6000, std::string(fault) == "cup" ? "prime-1" : "dose-1", err));
    v.advance(v.now() + 7000);
    CHECK(v.runStatus(2).state == doseph::RunStatus::State::Done);  // the pump ran
    CHECK(v.pumps[1].drops == 0);
    CHECK(v.tank.pendingEc == 0.0);  // nothing went into the tank
  }
}

TEST_CASE("Twin: a line pulled back fills first, so the next run drips later and less (SD-111)") {
  sim::World w = twinWithWater();
  std::string err;
  REQUIRE(w.startRun(2, 1000, "back-1", err, true));
  w.advance(w.now() + 1500);
  const double back = w.pumps[1].trueFlow / 60.0;
  CHECK(w.pumps[1].lineMl == doctest::Approx(back).epsilon(0.01));
  CHECK(w.pumps[1].drops == 0);  // backwards nothing drips
  REQUIRE(w.startRun(2, 3000, "dose-1", err));
  w.advance(w.now() + 500);
  CHECK(w.pumps[1].drops == 0);  // the line fills first
  w.advance(w.now() + 3000);
  CHECK(w.pumps[1].lineMl == 0.0);
  CHECK(w.pumps[1].drops == static_cast<std::uint64_t>((w.pumps[1].trueFlow * 3.0 / 60.0 - back) / 0.05 + 1e-9));
}

TEST_CASE("Twin: a canister above the device siphons: drops without a running pump, into the tank (SD-111, PD-455)") {
  sim::World w = twinWithWater();
  w.pumps[1].siphon = true;
  w.advance(w.now() + 60 * 1000);
  CHECK(w.pumps[1].drops >= 11);  // 0.6 ml in a minute, 0.05 ml a drop
  CHECK(w.pumps[1].drops <= 12);
  CHECK(w.pumps[1].state == 0);
  CHECK(w.tank.pendingEc > 0.0);  // it went into the tank
}

TEST_CASE("Simulator: the robot's state shows each channel's drops; only a pump that can runs backwards (SD-108, SD-111)") {
  for (const std::string name : {"home", "pro"}) {
    CAPTURE(name);
    sim::Options o;
    o.scenario = "demo";
    o.password = "demo-password";
    o.prefillHours = 0;
    o.profile = name;
    sim::Simulation s(o);
    const json chs = s.robot().state()["channels"];
    REQUIRE(chs.size() == sim::exampleProfile(name)->channels.size());
    for (const auto& ch : chs) CHECK(ch["drops"].is_object());  // every outlet has a sensor in both examples
    CHECK(s.robot().info()["profile"]["channels"][0]["reverse"] == true);  // every pump runs both ways (SD-142)
    std::string err;
    CHECK(s.hardware().startRun(1, 500, "back-x", err, true));
  }
  // A slot whose pump cannot run backwards refuses it
  sim::World w;
  w.reset(4);
  doseph::Profile p = *sim::exampleProfile("home");
  p.channels[0].reverse = false;
  sim::SimHardware hw(w, p);
  std::string err;
  CHECK_FALSE(hw.startRun(1, 500, "back-y", err, true));
  CHECK(err == "this pump cannot run backwards");
}
