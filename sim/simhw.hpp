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
  // The wall clock is set, forward or back (a time server, a person); the monotonic one runs on
  void stepWall(doseph::Epoch s) { start_ += s; }

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
  doseph::Release release() const override { return {w_.released(), w_.leak, w_.highLevel(), w_.stopButton, w_.noFlowCut, w_.faults}; }
  doseph::WaterPath waterPath() const override;
  bool startRun(int channel, doseph::Ms ms, const std::string& jobId, std::string& err, bool backwards) override {
    if (backwards && (channel < 1 || channel > channelCount() || !profile_.channels[static_cast<size_t>(channel - 1)].reverse)) {
      err = "this pump cannot run backwards";
      return false;
    }
    return w_.startRun(channel, ms, jobId, err, backwards);
  }
  std::optional<doseph::Drops> drops(int channel) const override;
  doseph::RunStatus runStatus(int channel) const override { return w_.runStatus(channel); }
  void stopAllPumps() override { w_.stopAllPumps(); }
  bool setFeedPump(bool on, std::string& err) override;
  std::optional<bool> feedPump() const override { return w_.output; }
  std::optional<double> feedPumpA() const override { return w_.pumpA(); }
  void allowReturnPump(bool allowed) override { w_.returnLocked = !allowed; }
  // A reset asked for at the twin, as at the device (SD-139): the panel or the console
  void askReset(doseph::DeviceReset r) { reset_ = r; }
  doseph::DeviceReset takeReset() override {
    const doseph::DeviceReset r = reset_;
    reset_ = doseph::DeviceReset::None;
    return r;
  }
  // A head fitted, removed or changed: the device reports it, the twin pumps with it (SD-108)
  void setHead(int channel, const std::string& head) {
    profile_.channels[static_cast<size_t>(channel - 1)].head = head;
    w_.pumps[static_cast<size_t>(channel - 1)].head = head;
  }

 private:
  World& w_;
  doseph::DeviceReset reset_ = doseph::DeviceReset::None;
  doseph::Profile profile_;
};

}  // namespace sim
