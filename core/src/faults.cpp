// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/faults.hpp"

#include "doseph/messages.hpp"

namespace doseph {

FaultInfo classifyFault(const std::string& fault) {
  FaultInfo f;
  const auto dot = fault.find('.');
  const std::string name = fault.substr(0, dot);
  f.part = dot == std::string::npos ? "" : fault.substr(dot + 1);
  using K = FaultInfo::Kind;
  if (name == "self_test" || name == "board_description" || name == "power_trip" || name == "safety_cycle") {
    f.kind = K::Hold;
    f.reason = name;
  } else if (name == "supply_overvoltage_latched") {
    f.kind = K::Hold;  // the hardware holds it until unplugged: that is the person's step
    f.reason = "supply_overvoltage";
  } else if (name == "supply" || name == "current_limit" || name == "pump_protection" || name == "stop_mismatch" || name == "not_off" ||
             name == "moisture" || name == "sensor_supply" || name == "flow_implausible") {
    f.kind = K::Latch;
    f.reason = name;
  } else if (name == "leak_sensor_open") {
    f.kind = K::Latch;
    f.reason = "leak_sensor";
  } else if (name == "board_hot" || name == "safety_restart") {
    f.kind = K::Warning;
    f.reason = name;
  } else {
    f.kind = K::Hold;  // one the robot does not know holds while reported
    f.reason = "fault";
    f.part.clear();
  }
  // A part only where the ID documents one, and only a short name
  const bool parts = f.reason == "self_test" || f.reason == "power_trip" || f.reason == "current_limit" || f.reason == "pump_protection" ||
                     f.reason == "leak_sensor";
  if (!parts || f.part.size() > 24 || f.part.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) f.part.clear();
  return f;
}

std::vector<std::string> cleanFaults(const std::vector<std::string>& faults) {
  std::vector<std::string> out;
  for (const auto& f : faults) {
    if (out.size() >= 16) break;
    const bool plain = !f.empty() && f.size() <= 48 && f.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.") == std::string::npos;
    out.push_back(plain ? f : "?");
  }
  return out;
}

Msg holdText(const std::string& prefix, const std::string& hold) {
  if (hold == "leak" || hold == "high_level" || hold == "return" || hold == "stop_button" || hold == "release") return say(prefix + hold);
  return say(prefix + "fault", {{"name", say("lock.hw." + hold)}});
}

json faultArgs(const FaultInfo& f, const std::string& fault) {
  static const char* const kParts[] = {"dosing", "pump", "return", "switch_off", "pump_protection", "supply"};
  json part = say("hw.part.unknown");
  for (const char* k : kParts)
    if (f.part == k) part = say(std::string("hw.part.") + k);
  if (!f.part.empty() && part["key"] == "hw.part.unknown") part = f.part;
  // The ID only as far as it is a short name: it comes from the device
  const bool plain = !fault.empty() && fault.size() <= 48 && fault.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.") == std::string::npos;
  return {{"part", part}, {"id", plain ? fault : std::string("?")}};
}

}  // namespace doseph
