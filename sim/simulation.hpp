// SPDX-License-Identifier: AGPL-3.0-or-later
// Simulation = twin + clock + hardware + storage + robot + API in one
// package. Used by the host server (main.cpp) and by the scenario tests.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "doseph/api.hpp"
#include "simhw.hpp"

namespace sim {

// Random bytes from the operating system for salts, session tokens and the
// setup code: getentropy(), on Windows the system's generator. Never
// std::random_device on the others: libstdc++ tries RDSEED first and takes the
// zero a Zen 5 processor reports as success (AMD-SB-7055). Aborts rather
// than going on without.
void randomBytes(std::uint8_t* p, size_t n);

// Files in a data folder; without a folder in memory only. write() is on
// disk when it returns true: a temporary file, fsync, rename. append() adds
// to the end at once, without fsync (journals: a torn end is skipped when
// read). Files are the owner's only.
class FileStorage : public doseph::IStorage {
 public:
  explicit FileStorage(std::string dir) : dir_(std::move(dir)) {}
  std::optional<std::string> read(const std::string& name) override;
  bool write(const std::string& name, const std::string& data) override;
  bool append(const std::string& name, const std::string& data) override;

 private:
  std::string dir_;
  std::mutex m_;
};

struct Options {
  std::string scenario = "demo";  // demo | new
  std::string profile = "home";   // an example profile: home | pro
  std::string dataDir;            // empty: in memory only
  std::string password;           // the demo's password
  double prefillHours = 24;       // history the demo computes ahead
  doseph::Epoch startEpoch = 0;   // 0: now minus the prefill
};

class Simulation {
 public:
  explicit Simulation(Options o);
  ~Simulation();

  // Moves the clock on; the robot ticks every `tick` at most (a coarser tick makes long tests cheaper)
  void step(doseph::Ms dt, doseph::Ms tick = 1000);
  void fastForward(double hours, doseph::Ms tick = 5000);
  // A power cut: the robot restarts, the output and the pumps lose power; the
  // tank lives on for `outageMs` without circulation.
  void reboot(doseph::Ms outageMs = 0);
  void loadScenario(const std::string& name);  // everything anew, storage empty
  doseph::json simState();
  doseph::json control(const std::string& action, const doseph::json& body);

  doseph::Robot& robot() { return *robot_; }
  doseph::Api& api() { return *api_; }
  World& world() { return world_; }
  SimHardware& hardware() { return *hw_; }
  SimClock& clock() { return *clock_; }
  doseph::IStorage& storage() { return *store_; }
  std::recursive_mutex& mutex() { return mtx_; }
  const std::string& scenario() const { return opts_.scenario; }
  std::string demoPassword() const { return opts_.password; }
  double speed = 1.0;
  static constexpr double kMaxSpeed = 600;

 private:
  void build(bool fresh);
  void configureDemo();
  void createRobot();

  Options opts_;
  std::recursive_mutex mtx_;
  World world_;
  std::unique_ptr<SimClock> clock_;
  std::unique_ptr<SimHardware> hw_;
  std::unique_ptr<doseph::IStorage> store_;
  std::unique_ptr<doseph::Robot> robot_;
  std::unique_ptr<doseph::Api> api_;
};

}  // namespace sim
