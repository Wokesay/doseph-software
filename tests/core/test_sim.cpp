// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "folder_lock.hpp"

namespace fs = std::filesystem;

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
