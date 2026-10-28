// SPDX-License-Identifier: AGPL-3.0-or-later
// Test doubles: a clock and hardware whose readings a test sets by hand.
#pragma once

#include <map>
#include <string>

#include "doseph/hardware.hpp"

namespace fake {

using doseph::Signal;
using doseph::Ms;

class Clock : public doseph::IClock {
 public:
  Ms ms = 0;
  doseph::Epoch start = 1798761600;  // 2027-01-01
  bool isSecured = true;
  Ms nowMs() const override { return ms; }
  doseph::Epoch epoch() const override { return start + ms / 1000; }
  bool secured() const override { return isSecured; }
};

class Hardware : public doseph::IHardware {
 public:
  bool isOnline = true;
  std::map<Signal, doseph::Sample> samples;
  doseph::Profile prof{"home", "Test", std::vector<doseph::ChannelCaps>(4, {"brushed", "S", true, true}), "turbine", "gravity", {"wifi"}, doseph::kNaN, false};
  std::map<int, doseph::RunStatus> runs;
  int started = 0;
  bool out = false;

  void set(Signal c, double raw, Ms ts) { samples[c] = doseph::Sample(raw, ts); }
  void poll(Ms) override {}
  bool online() const override { return isOnline; }
  std::optional<doseph::Sample> sample(Signal c) const override {
    auto it = samples.find(c);
    if (it == samples.end()) return std::nullopt;
    return it->second;
  }
  const doseph::Profile& profile() const override { return prof; }
  doseph::Release rel;
  doseph::Release release() const override { return rel; }
  doseph::WaterPath water;
  doseph::WaterPath waterPath() const override { return water; }
  std::map<int, doseph::Drops> dropsSeen;  // the channels with a drop sensor and what it counted
  std::optional<doseph::Drops> drops(int channel) const override {
    auto it = dropsSeen.find(channel);
    if (it == dropsSeen.end()) return std::nullopt;
    return it->second;
  }
  bool backwards = false;  // the last run started ran backwards
  bool startRun(int channel, Ms ms, const std::string& jobId, std::string&, bool back) override {
    runs[channel] = {doseph::RunStatus::State::Running, jobId, ms, 0, ""};
    backwards = back;
    started++;
    return true;
  }
  doseph::RunStatus runStatus(int channel) const override {
    auto it = runs.find(channel);
    return it == runs.end() ? doseph::RunStatus{} : it->second;
  }
  void stopAllPumps() override {
    for (auto& [n, r] : runs)
      if (r.state == doseph::RunStatus::State::Running) r.state = doseph::RunStatus::State::Failed;
  }
  bool setSuctionPump(bool on, std::string&) override {
    out = on;
    return true;
  }
  std::optional<bool> suctionPump() const override { return out; }
};

// A storage whose writes fail on demand
class FailingStorage : public doseph::MemoryStorage {
 public:
  bool fail = false;
  bool write(const std::string& name, const std::string& data) override {
    return fail ? false : doseph::MemoryStorage::write(name, data);
  }
  bool append(const std::string& name, const std::string& data) override {
    return fail ? false : doseph::MemoryStorage::append(name, data);
  }
};

}  // namespace fake
