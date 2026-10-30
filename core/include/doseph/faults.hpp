// SPDX-License-Identifier: AGPL-3.0-or-later
// What the device's own protection reports (SD-141): how the robot takes each fault the hardware
// names in Release::faults.
#pragma once

#include <string>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

struct FaultInfo {
  // Hold: every pump held while the device reports it, which ends it with a start that passes.
  // Latch: held until a person releases it, once no longer reported. Warning: holds nothing.
  enum class Kind { Hold, Latch, Warning } kind = Kind::Hold;
  std::string reason;  // the lock reason: "self_test", "power_trip", …; "fault" for an unknown ID
  std::string part;    // what it names after the dot: "dosing", "return", …; "" for none
  // The robot's ID for it: "hw.<reason>[.<part>]", a latch's or a watch item's
  std::string id() const { return "hw." + reason + (part.empty() ? "" : "." + part); }
};

FaultInfo classifyFault(const std::string& fault);
// What the device reports, as the robot takes it: at most 16, each a short name of [a-z0-9_.] or
// "?" in its place, which holds as an unknown fault
std::vector<std::string> cleanFaults(const std::vector<std::string>& faults);
// The arguments of its texts: the part by name ("the dosing pumps"), and the ID as the device gave it
json faultArgs(const FaultInfo& f, const std::string& fault);
// The text for a hold (Ctx::hold) under a prefix ("act.hold.", "cycle.wait.hold."): its own for
// the release's holds, one naming the fault for the device's protection
Msg holdText(const std::string& prefix, const std::string& hold);

}  // namespace doseph
