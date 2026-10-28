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

TEST_CASE("Twin: the suction pump primes before water flows; a blocked filter or a suction line out of the water gives no flow (SD-109)") {
  sim::World w;
  w.reset(4);
  w.fill(100, 0.45, 7.6);
  w.output = true;
  w.advance(4000);
  CHECK_FALSE(w.primed());
  CHECK_FALSE(w.flowing());
  CHECK(w.rawFlow() == 0.0);
  w.advance(10000);
  CHECK(w.flowing());
  CHECK(w.outletWet());
  CHECK(w.rawFlow() > 11.0);
  for (const char* fault : {"blocked", "dry"}) {
    w.flowFault = fault;
    w.advance(w.now() + 2000);
    CHECK_FALSE(w.flowing());
    CHECK_FALSE(w.outletWet());
    CHECK(w.rawFlow() == 0.0);
  }
  w.flowFault.clear();
  w.output = false;  // off, the line runs empty: the next start primes again
  w.advance(w.now() + 1000);
  w.output = true;
  w.advance(w.now() + 2000);
  CHECK_FALSE(w.flowing());
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
    // The return fails while the suction pump runs: the chamber fills up
    w.returnFault = byPump ? "failed" : "blocked";
    w.output = true;
    std::string err;
    REQUIRE(w.startRun(2, 20000, "dose-1", err));  // a run while the pump primes
    w.advance(w.now() + 12 * 1000);
    CHECK(w.highLevel());
    CHECK_FALSE(w.released());
    CHECK_FALSE(w.output);  // the suction pump is off, below the robot
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
