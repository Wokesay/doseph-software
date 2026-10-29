// SPDX-License-Identifier: AGPL-3.0-or-later
// The refill valve (SD-132, PD-460): fresh water into the tank through a valve on a Shelly output,
// until the float switch in the tank says full. A fill opens when the robot raises "top up water"
// (SD-115), once for each time it does, or when a person asks for one. It ends when the float says
// full; at the longest fill, or when the float loses its value, it latches the refill until a
// person has looked (RAT-031, RAT-079). Only the gateway switches the valve (R1), and it closes it
// on its own where the float does not say "not full" (RAT-032).
#pragma once

#include <optional>
#include <string>

#include "doseph/common.hpp"

namespace doseph {

struct Ctx;
class NetOutputs;

constexpr Ms kRefillOpenMs = kMinute;  // a valve that did not open by then ends the fill

// What the robot knows around the refill now
struct RefillIn {
  std::optional<bool> full;   // the float: full or not; none without a value (R5)
  bool floatAssigned = false;
  std::string valve;          // the valve's state at the gateway: not_assigned | off | on | checking | …
  bool busy = false;          // a dose, a channel job or a probe calibration is under way
};

class RefillValve {
 public:
  // Why a fill may not start now; nothing where it may
  std::optional<Msg> blocked(const Ctx& c, const RefillIn& in) const;
  // A person's "Fill now": starts at once, or says why not
  std::optional<Msg> start(Ctx& c, const RefillIn& in, NetOutputs& out);
  // Ends a fill for a person's stop or a change of its valve or float, with why: nothing when no
  // fill is under way; otherwise whether the valve was open
  std::optional<bool> stop(Ctx& c, NetOutputs& out, Msg why);
  // Starts a fill "top up water" asked for, and ends one that is done or has to end. Returns true
  // when a fill ended in which the valve was open: the check mixes first (SD-112).
  bool tick(Ctx& c, const RefillIn& in, NetOutputs& out);
  bool filling() const { return since_ >= 0; }
  // A fill cut by a restart: reported, never resumed (R6). True when there was one.
  static bool reportCut(Ctx& c);
  // {filling, since, why, opened, asked, blocked}
  json status(const Ctx& c, const RefillIn& in) const;

 private:
  void open(Ctx& c, NetOutputs& out, const std::string& why);
  // Ends the fill: `latch` names why the refill latches ("time", "float", "valve"); otherwise the
  // event says why it ended
  bool end(Ctx& c, NetOutputs& out, const std::string& latch, Msg why);
  bool asked_ = false;     // "top up water" was raised: a fill waits for it
  bool seenTopUp_ = false;
  bool lastTopUp_ = false;
  Ms since_ = -1;          // a fill under way since then
  bool opened_ = false;    // the valve said on during this fill
  std::string why_;        // "top_up" | "button"
};

}  // namespace doseph
