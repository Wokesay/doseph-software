// SPDX-License-Identifier: AGPL-3.0-or-later
// Digital twin of the mixing robot at its tank: the tank, the circulation
// pump that pushes tank water through the robot's measuring chamber, the dosing
// pumps and the probes in the chamber. Numbers are measured where possible
// (docs/SIMULATOR.md), otherwise marked as assumptions. It models no
// particular hardware: what the device is, is the product side's.
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "doseph/common.hpp"
#include "doseph/hardware.hpp"

namespace sim {

using doseph::json;
using doseph::Ms;

struct Liquid {
  std::string name;
  double ecPerMlL = 0;  // mS/cm per ml/L
  double phPerMlL = 0;  // pH change per ml/L (negative = acid)
};

struct Pump {
  std::string liquid;   // key in World::liquids; "" = nothing connected
  double trueFlow = 50; // ml/min, the real one
  bool blocked = false; // the motor stalls: the run fails
  bool empty = false;   // the canister is empty: it runs, nothing comes
  // Run state
  std::string jobId;
  Ms requested = 0, elapsed = 0;
  int state = 0;  // 0 idle, 1 running, 2 done, 3 failed
  std::string error;
  double cupMl = 0;  // what a calibration run pumped into the cup
};

struct Tank {
  double volumeL = 0;
  double ec = 0.02;     // the mixed state
  double ph = 7.0;
  double temp = 20.5;
  double pendingEc = 0; // a dose not mixed in yet
  double pendingPh = 0;
};

// The water in the robot's measuring chamber, what the probes see
struct Chamber {
  double ec = 0.02, ph = 7.0, temp = 20.5;
};

class World {
 public:
  World();
  void reset(int pumps = 4);
  void advance(Ms now);
  Ms now() const { return now_; }

  // The suction pump: switched by the robot's output, or by itself (an
  // external pump); `flowFault` "blocked" means it runs without flow,
  // "sensor" that water flows but the flow sensor reads nothing.
  bool output = false;
  bool external = false;
  double suctionLpm = 12.0;  // flow when it runs (assumption)
  std::string flowFault;
  bool suctionRuns() const { return external || output; }
  bool flowing() const { return suctionRuns() && flowFault != "blocked" && tank.volumeL > 1.0; }

  // The probes' faults: "" | "offline" | "frozen" | "jump" | "ec_zero"
  std::string probeFault;
  // A probe in a buffer or reference solution (calibration)
  std::optional<double> phBuffer, ecBuffer;

  // Raw values as the measuring part delivers them
  double rawPh() const;
  double rawEc() const;
  double rawTemp() const;
  double rawFlow() const;
  Ms sampleTs() const { return lastSample_; }

  // Pumps 1..n
  std::vector<Pump> pumps;
  bool startRun(int pump, Ms ms, const std::string& jobId, std::string& err);
  doseph::RunStatus runStatus(int pump) const;
  void stopAllPumps();

  void fill(double volumeL, double ec, double ph);
  json toJson() const;

  Tank tank;
  Chamber chamber;
  std::map<std::string, Liquid> liquids;
  double roomTemp = 22.0;
  // The probes' errors before a calibration (assumptions)
  double phOffset = 0.18;
  double phSlope = 0.97;
  double ecFactor = 1.08;

 private:
  void step(Ms dt);
  double noise(double sigma) const;
  Ms now_ = 0;
  Ms lastSample_ = -100000;
  mutable std::mt19937 rng_;
  std::deque<std::pair<Ms, double>> phTrail_;  // dead time of the pH response
  int running_ = 0;
  bool frozenSet_ = false;
  double frozenPh_ = 0, frozenEc_ = 0;
};

}  // namespace sim
