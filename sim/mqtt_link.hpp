// SPDX-License-Identifier: AGPL-3.0-or-later
// The simulator's MQTT connection (SD-130): a plain TCP client on a thread of
// its own, so the robot's loop never waits on the network. The core builds
// every packet and payload; this only carries them. The simulator has no TLS,
// so it sends values to a broker in the home network, and takes no commands:
// those need TLS, which the device's firmware has.
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "simulation.hpp"

namespace sim {

class MqttLink {
 public:
  explicit MqttLink(Simulation& s);
  ~MqttLink();
  MqttLink(const MqttLink&) = delete;
  MqttLink& operator=(const MqttLink&) = delete;

  static constexpr int kKeepAliveS = 60;

 private:
  void run();
  Simulation& sim_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace sim
