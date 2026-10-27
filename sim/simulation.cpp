// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simulation.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>  // getentropy
#endif
#endif

#include "doseph/messages.hpp"

namespace fs = std::filesystem;

namespace sim {

using doseph::json;
using doseph::Ms;

// ------------------------------------------------------------------ random bytes

void randomBytes(std::uint8_t* p, size_t n) {
#ifdef _WIN32
  static std::random_device rd;  // MSVC: the system's generator (rand_s)
  for (size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(rd());
#else
  while (n > 0) {
    const size_t k = std::min<size_t>(n, 256);  // getentropy's limit per call
    if (getentropy(p, k) != 0) {
      std::perror("getentropy");
      std::abort();
    }
    p += k;
    n -= k;
  }
#endif
}

// ------------------------------------------------------------------ files

namespace {

std::FILE* openForOwner(const fs::path& p, bool append) {
#ifdef _WIN32
  return _wfopen(p.c_str(), append ? L"ab" : L"wb");
#else
  const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0600);
  if (fd < 0) return nullptr;
  std::FILE* f = ::fdopen(fd, append ? "ab" : "wb");
  if (!f) ::close(fd);
  return f;
#endif
}

}  // namespace

std::optional<std::string> FileStorage::read(const std::string& name) {
  std::lock_guard<std::mutex> l(m_);
  std::ifstream f(fs::path(dir_) / name, std::ios::binary);
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool FileStorage::write(const std::string& name, const std::string& data) {
  std::lock_guard<std::mutex> l(m_);
  std::error_code ec;
  if (fs::create_directories(dir_, ec) && !ec) {
#ifndef _WIN32
    fs::permissions(dir_, fs::perms::owner_all, ec);
#endif
  }
  const fs::path target = fs::path(dir_) / name;
  const fs::path tmp = target.string() + ".tmp";
  std::FILE* f = openForOwner(tmp, false);
  if (!f) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && ::fsync(fileno(f)) == 0;
#endif
  ok = std::fclose(f) == 0 && ok;
  if (ok) fs::rename(tmp, target, ec);  // replaced atomically
  if (!ok || ec) {
    fs::remove(tmp, ec);
    return false;
  }
#ifndef _WIN32
  // The rename itself is on disk only with the folder
  int dir = ::open(dir_.c_str(), O_RDONLY | O_CLOEXEC);
  if (dir >= 0) {
    ::fsync(dir);
    ::close(dir);
  }
#endif
  return true;
}

bool FileStorage::append(const std::string& name, const std::string& data) {
  std::lock_guard<std::mutex> l(m_);
  std::error_code ec;
  fs::create_directories(dir_, ec);
  std::FILE* f = openForOwner(fs::path(dir_) / name, true);
  if (!f) return false;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  return std::fclose(f) == 0 && ok;
}

// ------------------------------------------------------------------ simulation

namespace {

doseph::Epoch realNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

Simulation::Simulation(Options o) : opts_(std::move(o)) {
  bool fresh = true;
  if (!opts_.dataDir.empty()) {
    std::error_code ec;
    fresh = !fs::exists(fs::path(opts_.dataDir) / "config.json", ec);
  }
  build(fresh);
}

Simulation::~Simulation() {
  if (robot_) robot_->flush();
}

void Simulation::createRobot() {
  robot_ = std::make_unique<doseph::Robot>(*hw_, *store_, *clock_, randomBytes);
  robot_->setPlatform({{"kind", "simulator"}, {"simulated", true}, {"scenario", opts_.scenario}});
  api_ = std::make_unique<doseph::Api>(*robot_, *clock_);
}

void Simulation::build(bool fresh) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  const bool demo = opts_.scenario == "demo";
  const double prefill = demo && fresh ? std::clamp(opts_.prefillHours, 0.0, 1000.0) : 0.0;
  const doseph::Epoch start = opts_.startEpoch > 0 ? opts_.startEpoch : realNow() - static_cast<doseph::Epoch>(prefill * 3600);
  clock_ = std::make_unique<SimClock>(start);
  world_.reset(4);
  // Tap water in a 100 L tank (assumption): EC 0.45 mS/cm, pH 7.6
  world_.fill(100, 0.45, 7.6);
  hw_ = std::make_unique<SimHardware>(world_);
  if (opts_.dataDir.empty()) store_ = std::make_unique<doseph::MemoryStorage>();
  else store_ = std::make_unique<FileStorage>(opts_.dataDir);
  createRobot();
  robot_->boot();
  if (demo && fresh) {
    configureDemo();
    if (prefill > 0) fastForward(prefill);
  }
}

void Simulation::configureDemo() {
  auto& r = *robot_;
  if (opts_.password.empty()) opts_.password = "doseph-demo";
  if (!r.auth().hasPassword()) {
    r.auth().setInitialPassword(opts_.password);
    r.saveAuth();
    r.markPasswordSet();
  }
  r.setSystem({{"name", "Doseph demo"}});
  // The bottles on the pumps as the twin has them, each pump calibrated near its true flow
  const char* names[] = {"pH−", "Part A", "Part B", "CalMag"};
  const char* kinds[] = {"ph_down", "nutrient", "nutrient", "nutrient"};
  const char* pairs[] = {"", "AB", "AB", ""};
  const char* colors[] = {"#c0392b", "#2e86c1", "#8e44ad", "#d4ac0d"};
  const char* ids[] = {"ph_down", "part_a", "part_b", "calmag"};
  for (int i = 0; i < 4; ++i)
    r.putBottle({{"id", ids[i]}, {"name", names[i]}, {"kind", kinds[i]}, {"pump", i + 1}, {"pair", pairs[i]}, {"color", colors[i]}});
  for (int i = 0; i < 4; ++i) {
    const double seconds = 30;
    r.startCalibration(i + 1, seconds);
    step(32 * doseph::kSecond, 500);
    r.calibrationResult(world_.pumps[static_cast<size_t>(i)].cupMl);
    r.cancelPumpJob();  // closed, as the app does
  }
  // Probes calibrated against the twin's own errors: pH 7 and 4 buffers, EC 1.413 mS/cm
  r.probe({{"action", "start"}, {"kind", "ph"}});
  world_.phBuffer = 7.0;
  step(4 * doseph::kSecond);
  r.probe({{"action", "point"}, {"reference", 7.0}});
  world_.phBuffer = 4.0;
  step(4 * doseph::kSecond);
  r.probe({{"action", "point"}, {"reference", 4.0}});
  r.probe({{"action", "save"}});
  world_.phBuffer.reset();
  r.probe({{"action", "start"}, {"kind", "ec"}});
  world_.ecBuffer = 1.413;
  step(4 * doseph::kSecond);
  r.probe({{"action", "point"}, {"reference", 1.413}});
  r.probe({{"action", "save"}});
  world_.ecBuffer.reset();
  r.putRecipe({{"id", "two_part"},
               {"name", "Two-part nutrient"},
               {"ecTarget", 1.6},
               {"ph", {5.8, 6.2}},
               {"steps", json::array({{{"bottle", "part_b"}, {"mlPerL", 2.0}},
                                      {{"bottle", "part_a"}, {"mlPerL", 2.0}},
                                      {{"bottle", "calmag"}, {"mlPerL", 0.5}}})}});
  r.putTank({{"volumeL", 100}, {"circulation", "cycle"}, {"holdMin", 60}});
  r.setActive({{"recipe", "two_part"}, {"days", 28}});
}

void Simulation::step(Ms dt, Ms tick) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  while (dt > 0) {
    const Ms d = std::min(dt, tick);
    clock_->advance(d);
    robot_->tick();
    dt -= d;
  }
}

void Simulation::fastForward(double hours, Ms tick) {
  step(static_cast<Ms>(hours * 3600.0 * 1000.0), tick);
}

void Simulation::reboot(Ms outageMs) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  // Power gone: the output and the pump driver lose it; nothing is saved on the way out
  world_.output = false;
  world_.stopAllPumps();
  robot_.reset();
  api_.reset();
  if (outageMs > 0) {
    clock_->advance(outageMs);
    world_.advance(clock_->nowMs());
  }
  createRobot();
  robot_->boot();
}

void Simulation::loadScenario(const std::string& name) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  opts_.scenario = name == "new" ? "new" : "demo";
  opts_.dataDir.clear();  // a scenario change runs in memory
  robot_.reset();
  api_.reset();
  build(true);
}

json Simulation::simState() {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  return {{"world", world_.toJson()}, {"speed", speed}, {"scenario", opts_.scenario}};
}

json Simulation::control(const std::string& action, const json& b) {
  std::lock_guard<std::recursive_mutex> l(mtx_);
  auto num = [&](const char* k, double d) { return doseph::jnum(b, k, d); };
  if (action == "speed") speed = std::clamp(num("speed", 1), 0.0, kMaxSpeed);
  else if (action == "fill") world_.fill(num("volumeL", 100), num("ec", 0.45), num("ph", 7.6));
  else if (action == "topup") {
    // Fresh tap water into the tank (assumption: EC 0.45, pH 7.6)
    const double v = world_.tank.volumeL, add = std::max(0.0, num("litres", 10));
    if (v + add > 0) {
      world_.tank.ec = (world_.tank.ec * v + 0.45 * add) / (v + add);
      world_.tank.ph = (world_.tank.ph * v + 7.6 * add) / (v + add);
      world_.tank.volumeL = v + add;
    }
  } else if (action == "flowFault") world_.flowFault = doseph::jstr(b, "fault");
  else if (action == "external") world_.external = doseph::jbool(b, "on", false);
  else if (action == "pumpLpm") world_.pumpLpm = std::clamp(num("lpm", 12), 0.0, 200.0);
  else if (action == "probeFault") world_.probeFault = doseph::jstr(b, "fault");
  else if (action == "pumpFault") {
    const int n = static_cast<int>(num("pump", 0));
    if (n >= 1 && n <= static_cast<int>(world_.pumps.size())) {
      world_.pumps[static_cast<size_t>(n - 1)].blocked = doseph::jbool(b, "blocked", false);
      world_.pumps[static_cast<size_t>(n - 1)].empty = doseph::jbool(b, "empty", false);
    }
  } else if (action == "buffer") {
    const std::string kind = doseph::jstr(b, "kind");
    const double v = num("value", doseph::kNaN);
    auto& slot = kind == "ec" ? world_.ecBuffer : world_.phBuffer;
    if (doseph::isNum(v)) slot = v;
    else slot.reset();
  } else if (action == "reboot") reboot(static_cast<Ms>(num("outageS", 0) * 1000));
  else if (action == "fastForward") fastForward(std::clamp(num("hours", 1), 0.0, 48.0));
  else if (action == "scenario") loadScenario(doseph::jstr(b, "name", "demo"));
  else return {{"error", doseph::say("api.not_found")}};
  return simState();
}

}  // namespace sim
