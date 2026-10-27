// SPDX-License-Identifier: AGPL-3.0-or-later
// Event log: doses, mixes, locks, configuration changes, logins, updates.
// Replaces keeping a diary by hand. Security-relevant events are logged
// (CRA Annex I 2(l)).
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

struct Event {
  std::uint64_t id = 0;
  Epoch ts = 0;
  std::string type;      // dose, mix, calibration, block, unblock, device, config, auth, system, grow, alarm
  std::string severity;  // info | notice | warn | alarm
  Msg title;             // key, values and English text
  Msg text;              // empty when there is nothing to add
  json data = json::object();
};
void to_json(json& j, const Event& e);

class EventLog {
 public:
  explicit EventLog(size_t cap = 5000) : cap_(cap) {}
  const Event& add(Epoch ts, std::string type, std::string severity, Msg title, Msg text = {},
                   json data = json::object());
  std::vector<Event> query(Epoch from, Epoch to, const std::string& type, size_t limit) const;
  std::uint64_t lastId() const { return next_ - 1; }
  Epoch newestTs() const;  // 0 without events
  json toJson() const;
  void load(const json& j);
  // Storage: a daily snapshot (toJson) plus a journal, one event per line.
  std::string journalSince(std::uint64_t afterId) const;  // lines for the events newer than afterId
  // Adds the journal's events newer than what is held; a torn or damaged line is skipped.
  size_t replay(const std::string& journal);

 private:
  size_t cap_;
  std::uint64_t next_ = 1;
  std::deque<Event> events_;
};

}  // namespace doseph
