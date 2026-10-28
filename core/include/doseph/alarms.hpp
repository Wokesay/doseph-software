// SPDX-License-Identifier: AGPL-3.0-or-later
// Alarms (SD-123, RAT-022): what the watchdog finds a problem becomes an
// alarm with a level, and the alarm becomes notifications by rule:
//   critical  pushed at once, also at night, repeated 5, 15 and 60 min later
//   warning   pushed once by day; at night it waits for the morning report
//   info      only in the morning report and the app
// Acknowledging pauses the repetition, never the alarm. The notifications
// wait in an outbox the platform delivers (#10, step 5.2); the core sends
// nothing itself. The alarms' state is part of the run-time state, so a
// restart does not push again what is already known.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "doseph/common.hpp"
#include "doseph/config.hpp"
#include "doseph/watchdog.hpp"

namespace doseph {

enum class Level { Critical, Warning, Info };
const char* levelName(Level l);
// The level of a watchdog item, by its ID
Level levelOf(const std::string& itemId);

constexpr Epoch kAlarmStandS = 5 * 60;        // a problem stands this long before it is an alarm; a latch at once
constexpr Epoch kAckPauseS = 12 * 3600;       // acknowledging pauses the repetition at most this long
constexpr Epoch kRepeatS[] = {5 * 60, 15 * 60, 60 * 60};  // a critical alarm's repetitions after its first push
constexpr size_t kRecentNotifications = 50;

struct Notification {
  long long id = 0;
  Epoch at = 0;
  std::string kind;   // "alarm", "repeat", "report"
  std::string level;  // "critical", "warning", "info"
  Msg title;
  std::vector<Msg> lines;
  std::vector<std::string> items;  // the watchdog items it is about
};
void to_json(json& j, const Notification& n);

struct AlarmInput {
  const NotifyCfg& cfg;
  std::string zone;      // the device's time zone, for quiet hours and the report
  Epoch epoch = 0;
  bool cycleIdle = true;  // the check holds or waits: a target out of range is not being corrected
};

class Alarms {
 public:
  // After each watchdog evaluation: raises, repeats and ends alarms and queues
  // the notifications; returns whether the run-time state changed
  bool update(const WatchResult& w, const AlarmInput& in, RuntimeState& rt);
  // Pauses the repetition of one alarm, or of all ("") that are raised
  bool ack(const std::string& id, Epoch now, RuntimeState& rt);
  // Whether the local minute lies within the quiet hours
  static bool quiet(const NotifyCfg& cfg, int minuteOfDay);

  std::vector<Notification> takeOutbox();
  const std::deque<Notification>& recent() const { return recent_; }
  // The alarms raised, for the state: ID, level, since, pushes, paused until
  json list(const WatchResult& w, const RuntimeState& rt) const;

 private:
  void push(Notification n, Epoch at);
  std::vector<Notification> outbox_;
  std::deque<Notification> recent_;
  long long nextId_ = 1;
};

}  // namespace doseph
