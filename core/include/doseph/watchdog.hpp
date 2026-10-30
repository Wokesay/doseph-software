// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 9: watchdog. It only assesses (OK / problem / neutral) and switches
// nothing (RAT-074). This header includes only the read model and the
// configuration: no hardware, no gateway, no cycle. tools/arch_check.sh
// checks this in CI (rule R2).
#pragma once

#include <string>
#include <vector>

#include "doseph/config.hpp"
#include "doseph/readmodel.hpp"

namespace doseph {

struct WatchInput {
  const Config& cfg;
  const RuntimeState& rt;
  std::vector<Reading> readings;  // ph, ec, temp, flow
  bool needsFlow = false;         // the cycle needs flow now: none is a problem
  double lastPh = kNaN, lastEc = kNaN;  // the last stable measurement of the cycle
  Epoch lastAt = 0;
  Epoch epoch = 0;
  Epoch bootEpoch = 0;
  bool stopped = false;
  bool maintenance = false;  // someone works at tank or device: the targets are not judged
  int channelCount = 0;
  std::string hold{};  // what holds every pump below the robot, as Ctx::hold (SD-110)
  std::vector<std::string> faults{};  // what the device's protection reports, as Release::faults (SD-141)
  // Coupled (SD-131): the other device's name, which side this one is, whether the link is up, and
  // whether the readings of both devices differ
  struct Peer {
    bool coupled = false, leader = false, linkOk = false, deviation = false;
    std::string name;
  } peer{};
  // The roles bound to network devices (SD-132): whether each works, and the text that says so
  struct Role {
    std::string id;
    bool ok = false;
    Msg text;
    Msg label{};  // its name; a watering circuit's is its own (SD-134)
  };
  std::vector<Role> roles{};
  // Climate rooms (SD-136) where something is wrong: CO2 above 2,000 ppm, or heat protection on
  // their light (1 dimmed, 2 off), with their heat limit
  struct Room {
    std::string id, name;
    bool co2High = false;
    int heat = 0;
    double limitC = kNaN;
  };
  std::vector<Room> rooms{};
};

struct Assessment {
  std::string id;
  Msg label;
  std::string status;  // ok | problem | neutral
  Msg text;
};

struct WatchResult {
  Epoch evaluatedAt = 0;
  std::string overall;  // ok | problem | neutral
  int ok = 0, problems = 0, neutral = 0;
  Msg headline;
  std::vector<Assessment> items;
};
void to_json(json& j, const WatchResult& w);

WatchResult evaluate(const WatchInput& in);

// An assessment older than 3 min counts as missing and is red (RAT-073).
constexpr Epoch kWatchStaleS = 180;
// For 120 s after a start target checks are neutral (RAT-005).
constexpr Epoch kStartupGraceS = 120;
// The alarm band is wider than the control band (RAT-033).
constexpr double kAlarmMargin = 0.2;

}  // namespace doseph
