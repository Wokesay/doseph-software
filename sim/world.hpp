// SPDX-License-Identifier: AGPL-3.0-or-later
// Digital twin of the mixing robot at its tank: the tank, the feed pump
// that draws tank water through the robot's measuring chamber and back, the
// dosing pumps and the probes in the chamber. Numbers are measured where possible
// (docs/SIMULATOR.md), otherwise marked as assumptions. It models no
// particular hardware: what the device is, is the product side's.
#pragma once

#include <algorithm>
#include <cstdint>
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
  double trueFlow = 50; // ml/min, the real one with the head it came with
  // The pump head fitted ("S", "M", "L"; "" none) and the one trueFlow belongs to (SD-108)
  std::string head = "S", flowHead = "S";
  bool blocked = false; // the motor stalls: the run fails
  bool empty = false;   // the canister is empty: it runs, nothing comes
  bool outletBlocked = false;  // the outlet or line is blocked: it runs, nothing drips
  bool siphon = false;         // the canister stands above the device: it drips while the pump stands
  // The line and the drop sensor at the outlet (SD-111)
  double lineMl = 0;           // air in the line before the tip; a forward run fills it before it drips
  std::uint64_t drops = 0;
  Ms lastDropMs = -1;
  double dropAcc = 0;          // what came out since the last whole drop, ml
  bool backwards = false;      // the run in progress pulls back
  // The pump is mounted so that it delivers with its motor turning the other way (SD-141): the
  // board description says so, and the driver turns the motor accordingly
  bool inverted = false;
  bool motorReverse = false;   // the motor of the run in progress turns the other way
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

  // The feed pump: switched by the robot's output. In the tank (SD-127) it
  // pushes water up its hose, which fills in 2 s; in the device it primes
  // first (SD-109). `flowFault` "blocked" (its inlet), "dry" (out of the
  // water) or "hose" (the hose off) means it runs without flow, "sensor" that
  // water flows but the flow or outlet sensor sees none.
  bool output = false;
  bool pumpInTank = true;  // the profile's position of the feed pump
  double pumpLpm = 12.0;  // flow when it runs (assumption)
  // A circulation pump in the tank (SD-132): it mixes the tank as a flow of this many L/min would
  // (assumption)
  bool circulation = false;
  static constexpr double kCirculationLpm = 20.0;
  std::string flowFault;
  bool pumpRuns() const { return output; }
  bool primed() const;
  bool flowing() const;
  // The outlet's level sensor: water passes the weir on its way back
  bool outletWet() const { return flowing() && flowFault != "sensor"; }
  // The hardware's own cut (PD-477): a pump in the tank that runs 9 s without
  // flow its sensor sees is switched off with every dosing pump, and held off
  // until the robot switches its output off
  bool noFlowCut = false;
  // What the device's protection reports, by ID (SD-141): set from the panel or a test
  std::vector<std::string> faults;
  // The feed pump's current, A (assumptions): 0.80 moving water, 0.32 running
  // dry or against a blocked inlet, 0.86 with the hose off; 0 when it stands
  double pumpA() const;
  // The feed pump's cable leaks into the water: pH and EC read shifted while it runs (SD-127)
  bool cableFault = false;

  // The measuring chamber holds water up to its weir, which keeps the probes
  // wet; above it the water returns to the tank, by gravity or by a return
  // pump that switches by level (SD-109). `returnFault` "blocked" (the
  // return line) or "failed" (the return pump) lets the chamber fill up to
  // its high-level sensor. Volumes are assumptions.
  bool returnByPump = false;  // the profile's return
  double chamberL = 0.15;     // water in the chamber, L
  bool returnPumpOn = false;
  bool returnLocked = false;  // the robot holds the return pump off (SD-109)
  std::string returnFault;
  bool highLevel() const;
  // The hardware release (SD-110): a leak, high level or the stop button
  // switch the feed pump and every dosing pump off, below the robot
  bool leak = false;
  bool stopButton = false;
  bool released() const { return !leak && !highLevel() && !stopButton; }

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
  // What a pump moves with the head fitted now, ml/min: nothing without one
  static double flowOf(const Pump& p);
  // `motorReverse`: the motor turns the other way; whether that delivers or pulls back depends on
  // how the pump is mounted (Pump::inverted)
  bool startRun(int pump, Ms ms, const std::string& jobId, std::string& err, bool motorReverse = false);
  doseph::RunStatus runStatus(int pump) const;
  void stopAllPumps();
  // A new canister, or a line taken off: the whole line is air and fills before it drips (SD-111)
  void emptyLine(int pump);

  void fill(double volumeL, double ec, double ph);
  // Fresh tap water into the tank (assumption: EC 0.45, pH 7.6)
  void addWater(double litres);
  // Water out of the tank, as a watering pump takes it
  void takeWater(double litres) { tank.volumeL = std::max(0.0, tank.volumeL - std::max(0.0, litres)); }
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
  void outOfTip(Pump& p, double ml);  // what leaves a channel's tip: drops, and the tank
  double noise(double sigma) const;
  Ms now_ = 0;
  Ms lastSample_ = -100000;
  mutable std::mt19937 rng_;
  std::deque<std::pair<Ms, double>> phTrail_;  // dead time of the pH response
  int running_ = 0;
  Ms primedMs_ = 0;  // how long the feed pump has run since it started
  Ms noFlowMs_ = 0;  // how long it has run without flow its sensor sees
  bool frozenSet_ = false;
  double frozenPh_ = 0, frozenEc_ = 0;
};

}  // namespace sim
