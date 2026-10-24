// SPDX-License-Identifier: AGPL-3.0-or-later
// The simulator's hardware: translates the twin into what the robot's core
// would see from its measuring part, its pump driver and its output.
#pragma once

#include "doseph/hardware.hpp"
#include "world.hpp"

namespace sim {

class SimClock : public doseph::IClock {
 public:
  explicit SimClock(doseph::Epoch start) : start_(start) {}
  doseph::Ms nowMs() const override { return ms_; }
  doseph::Epoch epoch() const override { return start_ + ms_ / 1000; }
  bool secured() const override { return secured_; }
  void setSecured(bool s) { secured_ = s; }
  void advance(doseph::Ms dt) { ms_ += dt; }

 private:
  doseph::Epoch start_;
  doseph::Ms ms_ = 0;
  bool secured_ = true;
};

class SimHardware : public doseph::IHardware {
 public:
  explicit SimHardware(World& w) : w_(w) {}
  void poll(doseph::Ms now) override { w_.advance(now); }
  bool online() const override { return w_.probeFault != "offline"; }
  std::optional<doseph::Sample> sample(doseph::Channel c) const override;
  int pumpCount() const override { return static_cast<int>(w_.pumps.size()); }
  bool startRun(int pump, doseph::Ms ms, const std::string& jobId, std::string& err) override {
    return w_.startRun(pump, ms, jobId, err);
  }
  doseph::RunStatus runStatus(int pump) const override { return w_.runStatus(pump); }
  void stopAllPumps() override { w_.stopAllPumps(); }
  bool setCirculation(bool on, std::string& err) override;
  std::optional<bool> circulation() const override { return w_.output; }

 private:
  World& w_;
};

}  // namespace sim
