// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/clock.hpp"

#include <algorithm>

namespace doseph {

void to_json(json& j, const Stamp& s) {
  j = {{"at", s.at}, {"secured", s.secured}, {"operatingS", s.operatingS}, {"boot", s.boot}};
}

std::optional<Stamp> stampFromJson(const json& j) {
  if (!j.is_object() || !j.contains("at") || !j["at"].is_number_integer() || !j.contains("operatingS") ||
      !j["operatingS"].is_number_integer())
    return std::nullopt;
  Stamp s;
  s.at = j["at"].get<Epoch>();
  s.secured = jbool(j, "secured", false);
  s.operatingS = j["operatingS"].get<std::int64_t>();
  if (j.contains("boot") && j["boot"].is_number_unsigned()) s.boot = j["boot"].get<std::uint32_t>();
  if (s.operatingS < 0 || !plausibleEpoch(s.at)) return std::nullopt;
  return s;
}

std::int64_t elapsedS(const Stamp& from, const Stamp& to) {
  const std::int64_t op = to.operatingS - from.operatingS;
  std::int64_t d = op;
  if (from.boot != to.boot && from.secured && to.secured) d = std::max(op, to.at - from.at);
  return std::max<std::int64_t>(0, d);
}

void HubClock::start(const std::optional<Stamp>& saved, Epoch newestRecorded, std::uint32_t boot) {
  poll();
  startMs_ = continueMs_ = base_.nowMs();
  boot_ = boot;
  savedOperatingS_ = saved ? saved->operatingS : 0;
  continueFrom_.reset();
  if (saved && plausibleEpoch(saved->at)) {
    // Events are saved more often than the state; a newer event moves the
    // start forward, but not by more than an hour (anything newer is wrong).
    continueFrom_ = newestRecorded > saved->at && newestRecorded <= saved->at + 3600 ? newestRecorded : saved->at;
  } else if (plausibleEpoch(newestRecorded)) {
    continueFrom_ = newestRecorded;
  }
}

void HubClock::lose(Epoch at) {
  continueFrom_ = at;
  continueMs_ = base_.nowMs();
}

Epoch HubClock::epoch() const {
  if (secured_ || !continueFrom_) return base_.epoch();
  return *continueFrom_ + (base_.nowMs() - continueMs_) / 1000;
}

std::int64_t HubClock::operatingS() const { return savedOperatingS_ + (base_.nowMs() - startMs_) / 1000; }

const char* HubClock::source() const {
  if (secured_) return "secured";
  return continueFrom_ ? "continued" : "unset";
}

}  // namespace doseph
