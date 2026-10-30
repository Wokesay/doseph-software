// SPDX-License-Identifier: AGPL-3.0-or-later
// Hardware abstraction of the mixing robot (layer 0). Implemented by the
// simulator (sim/simhw.cpp) and later by the device's firmware. The core
// calls only these interfaces. What the hardware is, is the product side's;
// this interface names only what the control needs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

// The signals the device measures in its measuring chamber. Raw values: pH on the
// raw pH scale (the pH an ideal probe would read, 7 − mV/59.16), EC in mS/cm
// at the water's own temperature (not compensated), water temperature in °C,
// flow through the device in L/min: measured (flow method "turbine"), or
// estimated from the feed pump's delivery while the outlet reports water
// ("outlet_level", SD-109).
enum class Signal { Ph, Ec, Temp, Flow };
inline constexpr Signal kSignals[] = {Signal::Ph, Signal::Ec, Signal::Temp, Signal::Flow};
const char* signalName(Signal c);  // "ph", "ec", "temp", "flow"

// A raw value with the monotonic time it was measured. Calibration and
// validity are decided by the sensor truth only.
struct Sample {
  double raw = kNaN;
  Ms ts = 0;
  Sample() = default;
  Sample(double raw_, Ms ts_) : raw(raw_), ts(ts_) {}
};

// What the device has (SD-108). The firmware reports it from its board
// profile, which stays private; the simulator has example profiles. Logic
// reads capabilities, never a channel number or a line name.
struct ChannelCaps {
  std::string pump;         // pump type: "brushed", "brushless", "stepper"
  std::string head;         // head size "S", "M", "L"; "" without a head
  bool dropSensor = false;  // drops at the outlet are seen
  bool reverse = false;     // the pump can run backwards, to pull the line back (SD-111)
  double dailyMaxMin = kNaN;  // the head's run time per 24 h (SD-113); NaN: the code's ceiling alone
  // The slot's pump delivers with its motor turning the other way (SD-141). The driver turns
  // "forwards" and "backwards" into the motor's direction; the core never reads it, and it is
  // never shown.
  bool inverted = false;
};

// What a channel's drop sensor has seen (SD-111): the drops counted since the
// device started, and when the last one fell (monotonic ms; -1: none yet).
struct Drops {
  std::uint64_t count = 0;
  Ms lastMs = -1;
};

struct Profile {
  std::string line;                   // "home", "pro": shown, never read by logic
  std::string variant;                // the name the device is sold under
  std::vector<ChannelCaps> channels;  // dosing channel n is channels[n - 1]
  std::string flow;                   // flow method: "turbine", "outlet_level"
  // Where the feed pump sits (SD-127): "tank", lying in the tank and pushing its water up (no
  // priming, a short start window); "device", drawing it through the device (SD-109)
  std::string pumpIn = "tank";
  std::string returnBy;               // "gravity", "pump"
  std::vector<std::string> network;   // "wifi", "lan"
  double pumpLpmLow = kNaN;           // the low end of the feed pump's delivery, L/min (SD-109)
  bool example = false;               // an example profile: its numbers are assumptions
  // The board description the profile comes from (SD-141): "signed" by the maker, or "own", one
  // the maker did not sign (shown as such, like own firmware). One missing, invalid or not
  // matching the hardware found is a fault (Release::faults).
  std::string board = "signed";
  // Whether the device tests its protection itself at every start ("auto"), or a person checks it
  // by hand ("manual", a prototype): then the robot doses on its own only once a person confirmed
  // the check after the start (SD-141)
  std::string selfTest = "auto";
  std::string pumpSpeed = "fixed";    // the feed pump's speed: "fixed", "adjustable"
  std::string returnFeedback;         // how a return pump is seen to run: "current", "" (none)
};

// The hardware release (SD-110): below the firmware, a leak, high level in
// the measuring chamber, the stop button and a hung controller switch every
// pump off. The core reads it and never overrides it.
struct Release {
  bool on = true;          // false: no pump runs, none starts
  bool leak = false;
  bool highLevel = false;  // the chamber is full up to its high-level sensor
  bool stopButton = false;
  // No flow within the hardware's window (PD-477): it switched the feed pump and dosing off and
  // holds them off until the feed pump is switched off again (SD-127)
  bool noFlow = false;
  // What the device's own protection reports, by ID (SD-141). The robot holds every pump while one
  // stands, and reports it; it never overrides it. Ended by a start that passes, while reported:
  //   "self_test.<part>"            the test at the start failed: switch_off, pump_protection, supply
  //   "board_description"           the board description is missing, invalid or does not match
  //   "power_trip.<rail>"           a supply rail switched off: dosing, pump, return
  //   "safety_cycle"                the safety supervisor restarts the device again and again
  // Latched until a person releases it, once no longer reported:
  //   "supply", "current_limit.<rail>", "pump_protection.<pump|return>", "stop_mismatch",
  //   "not_off", "leak_sensor_open", "moisture"
  // Warnings, which hold nothing: "board_hot"; "safety_restart" (this start came from the safety
  // supervisor; the device reports it from the start on). Any other ID holds while reported.
  std::vector<std::string> faults{};
};

// The water path as the device's sensors see it (SD-109); nullopt where the
// device has no such sensor.
struct WaterPath {
  std::optional<bool> outletWet;   // the outlet's level sensor (flow method "outlet_level")
  std::optional<bool> returnPump;  // the return pump runs (return by "pump")
};
json profileJson(const Profile& p);

struct RunStatus {
  enum class State { Idle, Running, Done, Failed } state = State::Idle;
  std::string jobId;
  Ms requestedMs = 0;
  Ms actualMs = 0;  // actual run time as the pump driver reports it (RAT-070)
  std::string error;
};

// A reset asked for at the device itself, which shows that a person stands at it (SD-139). How it
// is asked for (a button, how long it is held) is the device's.
enum class DeviceReset { None, Access, Factory };

class IHardware {
 public:
  virtual ~IHardware() = default;
  virtual void poll(Ms now) = 0;
  // The measuring part answers; without it no signal has a value.
  virtual bool online() const = 0;
  virtual std::optional<Sample> sample(Signal c) const = 0;
  virtual const Profile& profile() const = 0;
  virtual Release release() const = 0;
  virtual WaterPath waterPath() const = 0;
  // Dosing channels are numbered 1..channelCount(), as the profile lists them.
  int channelCount() const { return static_cast<int>(profile().channels.size()); }
  // A run of a channel's pump in ms with a job ID: a repeated order with the
  // same ID does not run twice. The driver allows one channel at a time.
  // `backwards` pulls the line back, where the pump can (ChannelCaps::reverse).
  virtual bool startRun(int channel, Ms ms, const std::string& jobId, std::string& err, bool backwards) = 0;
  virtual RunStatus runStatus(int channel) const = 0;
  // The channel's drop sensor; nullopt for a channel without one
  virtual std::optional<Drops> drops(int channel) const = 0;
  virtual void stopAllPumps() = 0;
  // The output that switches the feed pump. Switching it off also re-arms the hardware after a
  // cut for no flow (Release::noFlow).
  virtual bool setFeedPump(bool on, std::string& err) = 0;
  // The return pump runs by the chamber's level on its own; held off, it does not run (SD-109)
  virtual void allowReturnPump(bool allowed) = 0;
  virtual std::optional<bool> feedPump() const = 0;  // nullopt: state unknown
  // The feed pump's current in A while it runs; nullopt where the device does not measure it
  virtual std::optional<double> feedPumpA() const = 0;
  // A reset asked for at the device since the last call; each is taken once (SD-139)
  virtual DeviceReset takeReset() { return DeviceReset::None; }
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
  // Removes one file; true when it is gone, also when there was none
  virtual bool remove(const std::string& name) = 0;
  // Removes every file the robot keeps but `keep`: a factory reset (SD-139)
  virtual bool clear(const std::string& keep) = 0;
};

// The robot's storage, which a factory reset seals once it has cleared it: from then on nothing is
// written, removed or cleared but the one file it keeps, until the device has restarted (SD-139)
class SealableStorage : public IStorage {
 public:
  explicit SealableStorage(IStorage& inner) : inner_(inner) {}
  std::optional<std::string> read(const std::string& name) override { return inner_.read(name); }
  bool write(const std::string& name, const std::string& data) override { return open(name) && inner_.write(name, data); }
  bool append(const std::string& name, const std::string& data) override { return open(name) && inner_.append(name, data); }
  bool remove(const std::string& name) override { return open(name) && inner_.remove(name); }
  bool clear(const std::string& keep) override { return !sealed_ && inner_.clear(keep); }
  void seal(std::string keep) {
    sealed_ = true;
    keep_ = std::move(keep);
  }

 private:
  bool open(const std::string& name) const { return !sealed_ || name == keep_; }
  IStorage& inner_;
  bool sealed_ = false;
  std::string keep_;
};

class MemoryStorage : public IStorage {
 public:
  std::optional<std::string> read(const std::string& name) override;
  bool write(const std::string& name, const std::string& data) override;
  bool append(const std::string& name, const std::string& data) override;
  bool remove(const std::string& name) override;
  bool clear(const std::string& keep) override;

 private:
  std::vector<std::pair<std::string, std::string>> files_;
};

}  // namespace doseph
