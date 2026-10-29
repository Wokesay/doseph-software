// SPDX-License-Identifier: AGPL-3.0-or-later
// Twins of network devices (SD-132). The simulation answers the robot's requests to their
// addresses itself, so the simulator and the tests need no network:
//   shelly-1.local     a Shelly Gen2 relay with one output and one input; out of the box it
//                      restores its last state after a power loss and its input switches it
//   shelly-plug.local  a Shelly Gen2 plug: one output, no input
//   shelly-old.local   a Shelly of the first generation, which has no RPC interface
//   esp-1.local        an ESPHome device: binary_sensor/tank_full, sensor/water_temp
// A Shelly's settings act as the real one's: the auto-off switches it off, and a power cycle
// sets its outputs by their initial state.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doseph/devices.hpp"

namespace sim {

struct FakeShelly {
  std::string address, id, model;
  int gen = 2;
  bool online = true;
  struct Switch {
    bool on = false;
    doseph::Ms onSince = 0;
    doseph::json config;  // as Switch.GetConfig gives it
  };
  std::vector<Switch> switches;
  std::vector<bool> inputs;
  bool ignoresConfig = false;  // takes Switch.SetConfig and keeps its settings, as a faulty one might
  // The RPC's answer body: {"id", "src", "result"} or {"id", "src", "error"}
  doseph::json handle(const doseph::json& request, doseph::Ms now);
  void advance(doseph::Ms now);  // the auto-off acts
  void powerCycle();             // the outputs as their initial state says
};

struct FakeEsphome {
  std::string address, name;
  bool online = true;
  std::map<std::string, doseph::json> entities;  // "binary_sensor/tank_full" → true
};

class NetTwin {
 public:
  NetTwin();
  // The answer to a request: HTTP status and body; status 0 when nothing answers at its address
  std::pair<int, doseph::json> answer(const doseph::NetRequest& r, doseph::Ms now);
  void advance(doseph::Ms now);
  FakeShelly* shelly(const std::string& address);
  FakeEsphome* esphome(const std::string& address);
  doseph::json toJson() const;
  // The simulator panel: a device away or back, an input, an ESPHome value
  bool control(const std::string& action, const doseph::json& body);

 private:
  std::vector<FakeShelly> shellies_;
  std::vector<FakeEsphome> esps_;
};

}  // namespace sim
