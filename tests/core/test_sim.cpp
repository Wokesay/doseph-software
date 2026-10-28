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
    CHECK(info["pumpCount"] == static_cast<int>(p->channels.size()));
    CHECK(s.world().pumps.size() == p->channels.size());
    CHECK(s.robot().diagnostics()["info"]["profile"] == info["profile"]);
  }
}
