// SPDX-License-Identifier: AGPL-3.0-or-later
// Test doubles: a clock and hardware whose readings a test sets by hand.
#pragma once

#include <map>
#include <string>

#include "doseph/hardware.hpp"

namespace fake {

using doseph::Channel;
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
  std::map<Channel, doseph::Sample> samples;
  int pumps = 4;
  std::map<int, doseph::RunStatus> runs;
  int started = 0;
  bool out = false;

  void set(Channel c, double raw, Ms ts) { samples[c] = doseph::Sample(raw, ts); }
  void poll(Ms) override {}
  bool online() const override { return isOnline; }
  std::optional<doseph::Sample> sample(Channel c) const override {
    auto it = samples.find(c);
    if (it == samples.end()) return std::nullopt;
    return it->second;
  }
  int pumpCount() const override { return pumps; }
  bool startRun(int pump, Ms ms, const std::string& jobId, std::string&) override {
    runs[pump] = {doseph::RunStatus::State::Running, jobId, ms, 0, ""};
    started++;
    return true;
  }
  doseph::RunStatus runStatus(int pump) const override {
    auto it = runs.find(pump);
    return it == runs.end() ? doseph::RunStatus{} : it->second;
  }
  void stopAllPumps() override {
    for (auto& [n, r] : runs)
      if (r.state == doseph::RunStatus::State::Running) r.state = doseph::RunStatus::State::Failed;
  }
  bool setCirculation(bool on, std::string&) override {
    out = on;
    return true;
  }
  std::optional<bool> circulation() const override { return out; }
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
