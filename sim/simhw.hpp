// SPDX-License-Identifier: AGPL-3.0-or-later
// The simulator's hardware: translates the twin into what the robot's core
// would see from its measuring part, its pump driver and its output.
#pragma once

#include <optional>
#include <string>

#include "doseph/hardware.hpp"
#include "world.hpp"

namespace sim {

// The example profiles "home" and "pro" (SD-108). Their numbers are
// assumptions, not a board's data; nullopt for another name.
std::optional<doseph::Profile> exampleProfile(const std::string& name);

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
  SimHardware(World& w, doseph::Profile p) : w_(w), profile_(std::move(p)) {}
  void poll(doseph::Ms now) override { w_.advance(now); }
  bool online() const override { return w_.probeFault != "offline"; }
  std::optional<doseph::Sample> sample(doseph::Signal c) const override;
  const doseph::Profile& profile() const override { return profile_; }
  bool startRun(int channel, doseph::Ms ms, const std::string& jobId, std::string& err) override {
    return w_.startRun(channel, ms, jobId, err);
  }
  doseph::RunStatus runStatus(int channel) const override { return w_.runStatus(channel); }
  void stopAllPumps() override { w_.stopAllPumps(); }
  bool setSuctionPump(bool on, std::string& err) override;
  std::optional<bool> suctionPump() const override { return w_.output; }

 private:
  World& w_;
  doseph::Profile profile_;
};

}  // namespace sim
