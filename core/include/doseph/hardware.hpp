// SPDX-License-Identifier: AGPL-3.0-or-later
// Hardware abstraction of the mixing robot (layer 0). Implemented by the
// simulator (sim/simhw.cpp) and later by the device's firmware. The core
// calls only these interfaces. What the hardware is, is the product side's;
// this interface names only what the control needs.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

// The channels the device measures in its flow cell. Raw values: pH on the
// raw pH scale (the pH an ideal probe would read, 7 − mV/59.16), EC in mS/cm
// at the water's own temperature (not compensated), water temperature in °C,
// flow through the device in L/min.
enum class Channel { Ph, Ec, Temp, Flow };
inline constexpr Channel kChannels[] = {Channel::Ph, Channel::Ec, Channel::Temp, Channel::Flow};
const char* channelName(Channel c);  // "ph", "ec", "temp", "flow"

// A raw value with the monotonic time it was measured. Calibration and
// validity are decided by the sensor truth only.
struct Sample {
  double raw = kNaN;
  Ms ts = 0;
  Sample() = default;
  Sample(double raw_, Ms ts_) : raw(raw_), ts(ts_) {}
};

struct RunStatus {
  enum class State { Idle, Running, Done, Failed } state = State::Idle;
  std::string jobId;
  Ms requestedMs = 0;
  Ms actualMs = 0;  // actual run time as the pump driver reports it (RAT-070)
  std::string error;
};

class IHardware {
 public:
  virtual ~IHardware() = default;
  virtual void poll(Ms now) = 0;
  // The measuring part answers; without it no channel has a value.
  virtual bool online() const = 0;
  virtual std::optional<Sample> sample(Channel c) const = 0;
  // Dosing pumps are numbered 1..pumpCount().
  virtual int pumpCount() const = 0;
  // A pump run in ms with a job ID: a repeated order with the same ID does
  // not run twice. The driver allows one pump at a time.
  virtual bool startRun(int pump, Ms ms, const std::string& jobId, std::string& err) = 0;
  virtual RunStatus runStatus(int pump) const = 0;
  virtual void stopAllPumps() = 0;
  // The output that switches the circulation pump, where the device switches it.
  virtual bool setCirculation(bool on, std::string& err) = 0;
  virtual std::optional<bool> circulation() const = 0;  // nullopt: state unknown
};

// Storage for configuration and state (device: flash, host: files). write()
// must be atomic (copy first, then rename) and is on disk when it returns.
// append() adds to the end of a journal; a torn end after a power loss is
// allowed, the reader skips it.
class IStorage {
 public:
  virtual ~IStorage() = default;
  virtual std::optional<std::string> read(const std::string& name) = 0;
  virtual bool write(const std::string& name, const std::string& data) = 0;
  // Without a real append the whole file is rewritten; storages that can append override this.
  virtual bool append(const std::string& name, const std::string& data) { return write(name, read(name).value_or("") + data); }
};

class MemoryStorage : public IStorage {
 public:
  std::optional<std::string> read(const std::string& name) override;
  bool write(const std::string& name, const std::string& data) override;
  bool append(const std::string& name, const std::string& data) override;

 private:
  std::vector<std::pair<std::string, std::string>> files_;
};

}  // namespace doseph
