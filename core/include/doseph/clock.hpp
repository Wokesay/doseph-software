// SPDX-License-Identifier: AGPL-3.0-or-later
// Time base of the robot (gc PD-069, gc PD-073, SD-028). The platform clock says
// whether its wall time is secured. Without a secured time after a start,
// the robot continues from the time it saved last; the outage counts as 0.
// The operating time is saved too and counts only while the robot runs.
#pragma once

#include <optional>

#include "doseph/common.hpp"

namespace doseph {

class HubClock : public IClock {
 public:
  explicit HubClock(const IClock& base) : base_(base) {}
  Ms nowMs() const override { return base_.nowMs(); }
  Epoch epoch() const override;
  // The platform's flag as taken by the last poll(): all readings within a
  // tick agree, even if the flag changes on another task meanwhile.
  bool secured() const override { return secured_; }
  void poll() { secured_ = base_.secured(); }

  // At the start: the moment saved last (nullopt: nothing saved), the
  // newest wall time recorded anywhere (e.g. the event log) and the number
  // of this start. The continued clock starts at the saved time, or at the
  // newest record if that is at most an hour later; implausible times are
  // ignored.
  void start(const std::optional<Stamp>& saved, Epoch newestRecorded, std::uint32_t boot);
  // The secured time was lost while running: continue from `at` without a
  // jump; the operating time goes on.
  void lose(Epoch at);
  std::int64_t operatingS() const;
  Stamp stamp() const { return {epoch(), secured_, operatingS(), boot_}; }
  // "secured", "continued" (from a saved or last secured time) or "unset"
  // (no secured time and nothing saved: the platform's own clock).
  const char* source() const;

 private:
  const IClock& base_;
  bool secured_ = false;
  Ms startMs_ = 0;  // base of the operating time
  std::optional<Epoch> continueFrom_;
  Ms continueMs_ = 0;
  std::int64_t savedOperatingS_ = 0;
  std::uint32_t boot_ = 0;
};

}  // namespace doseph
