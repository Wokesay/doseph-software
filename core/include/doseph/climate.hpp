// SPDX-License-Identifier: AGPL-3.0-or-later
// Climate (SD-136): rooms, each with an exhaust fan, a circulation fan, a heater with its own
// thermostat (RAT-069), a humidifier, a dehumidifier and a CO2 valve on Shelly outputs, and sensors
// for air temperature, humidity and CO2. A room's day is its first light circuit's. Its targets are
// its own, or its plant's current phase's where it has them (RAT-076); a VPD target leads the
// humidity in place of a humidity target (RAT-009). VPD is air VPD (RAT-017).
//
// Each output switches on and off around its target with a band, at most once a minute; a value
// missing switches nothing on (R5), and the exhaust fan runs then. Humidifier and dehumidifier never
// run together, and the dehumidifier starts again only 5 min after it went off (RAT-034). CO2 only
// by day, while the exhaust fan is off, in pulses; above 2,000 ppm the valve closes with an alarm.
// The fans keep their rule through a Stop, a hold and maintenance; a Stop switches the rest off,
// and maintenance closes the CO2 valve. Only the gateway switches (R1).
#pragma once

#include <map>
#include <optional>
#include <string>

#include "doseph/common.hpp"

namespace doseph {

struct Ctx;
class NetOutputs;
class NetDevices;
struct Config;
struct PhaseCfg;
struct ClimateRoomCfg;

// Air VPD, kPa, from °C and % humidity, with FAO-56's saturation vapour pressure (RAT-017)
double saturationKpa(double tempC);
double airVpdKpa(double tempC, double rhPct);
// The humidity that gives a VPD at a temperature (RAT-009)
double rhForVpd(double vpdKpa, double tempC);

// The phase of the plant a room follows, while the plant runs; nothing otherwise
const PhaseCfg* followedClimatePhase(const Config& cfg, const ClimateRoomCfg& r);
// The room as its plan keeps it: the targets from the plant's phase where it has them
ClimateRoomCfg effectiveRoom(const Config& cfg, const ClimateRoomCfg& r);

// What a room keeps at a local second of the day
struct ClimateTargets {
  bool day = false;      // its first light circuit's day, or its own
  double tempC = kNaN;   // by day or by night
  double rhPct = kNaN;   // the humidity target: its own, or from its VPD target and target temperature
  double vpdKpa = kNaN;  // the VPD target where it leads; NaN otherwise
  double co2Ppm = kNaN;  // by day where set; NaN otherwise, and at night
};
ClimateTargets climateTargets(const Config& cfg, const ClimateRoomCfg& r, int secondOfDay);

class Climate {
 public:
  // The robot started: the dehumidifier waits its 5 min, and CO2 a value read since (RAT-034, R6)
  void start(Ms now);
  // Every room's outputs as their rules want them now
  void tick(Ctx& c, int secondOfDay, const NetDevices& dev, NetOutputs& out);
  // A room removed
  void forget(const std::string& room) { rooms_.erase(room); }
  // `outputs`: the gateway's status of the output roles
  json status(const Ctx& c, int secondOfDay, const NetDevices& dev, const json& outputs) const;

 private:
  static constexpr Ms kNever = -(Ms{1} << 60);
  struct Out {
    bool on = false;
    Ms changedAt = kNever;  // its last switching
    Ms onSince = -1;
  };
  struct Room {
    std::map<std::string, Out> outs;
    bool co2High = false;  // above 2,000 ppm, until back below the highest target
    double pulseFromPpm = kNaN;  // CO2 at the last pulse's start
    int noRise = 0;              // pulses in a row after which it had not risen
  };
  std::map<std::string, Room> rooms_;
  Ms startMs_ = 0;
};

}  // namespace doseph
