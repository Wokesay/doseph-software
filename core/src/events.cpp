// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/events.hpp"

#include <algorithm>

#include "doseph/messages.hpp"

namespace doseph {

namespace {

// A message nested in a saved message's arguments (a reading's reason in a
// line, the parts of a list) that no longer fits its template loses its key
// too, down to the depth the web app shows
void dropUnfitting(json& v, int depth) {
  if (depth > 4) return;
  if (v.is_array()) {
    for (auto& x : v) dropUnfitting(x, depth + 1);
    return;
  }
  if (!v.is_object()) return;
  static const json kNone = json::object();
  const json& args = v.contains("args") && v["args"].is_object() ? v["args"] : kNone;
  if (v.contains("key") && v["key"].is_string()) {
    const std::string key = v["key"].get<std::string>();
    if (!key.empty() && !fitsTemplate(key, args)) v["key"] = "";
  }
  if (v.contains("args") && v["args"].is_object())
    for (auto& [name, x] : v["args"].items()) dropUnfitting(x, depth + 1);
}

// A saved message; events written before messages had keys hold plain text,
// which stays as it is (no key). Anything else of the wrong type loads as an
// empty message; key and text are capped (args and data are not yet).
Msg msgFromJson(const json& e, const char* field) {
  constexpr size_t kMaxKey = 64, kMaxText = 2048;
  if (!e.contains(field)) return {};
  const json& v = e[field];
  if (v.is_string()) return {"", utf8Prefix(v.get<std::string>(), kMaxText), json::object()};
  if (!v.is_object()) return {};
  Msg m{utf8Prefix(jstr(v, "key"), kMaxKey), utf8Prefix(jstr(v, "text"), kMaxText), json::object()};
  if (v.contains("args") && v["args"].is_object()) m.args = v["args"];
  // Saved under an older template: the page shows the text it was saved with
  // rather than today's template with values missing
  if (!m.key.empty() && !fitsTemplate(m.key, m.args)) m.key.clear();
  for (auto& [name, x] : m.args.items()) dropUnfitting(x, 1);
  return m;
}

}  // namespace

void to_json(json& j, const Event& e) {
  j = {{"id", e.id}, {"ts", e.ts}, {"type", e.type}, {"severity", e.severity},
       {"title", e.title}, {"text", e.text}, {"data", e.data}};
}

Epoch EventLog::newestTs() const {
  Epoch t = 0;
  for (const auto& e : events_) t = std::max(t, e.ts);
  return t;
}

const Event& EventLog::add(Epoch ts, std::string type, std::string severity, Msg title, Msg text, json data) {
  Event e;
  e.id = next_++;
  e.ts = ts;
  e.type = std::move(type);
  e.severity = std::move(severity);
  e.title = std::move(title);
  e.text = std::move(text);
  e.data = std::move(data);
  events_.push_back(std::move(e));
  while (events_.size() > cap_) events_.pop_front();
  return events_.back();
}

std::vector<Event> EventLog::query(Epoch from, Epoch to, const std::string& type, size_t limit) const {
  std::vector<Event> out;
  for (auto it = events_.rbegin(); it != events_.rend() && out.size() < limit; ++it) {
    if (it->ts < from || it->ts > to) continue;
    if (!type.empty() && it->type != type) continue;
    out.push_back(*it);
  }
  return out;  // newest first
}

json EventLog::toJson() const {
  json arr = json::array();
  for (const auto& e : events_) arr.push_back(e);
  return {{"next", next_}, {"events", arr}};
}

namespace {
// A file from outside: read with checks, damaged entries skipped (no crash at the start).
bool eventFromJson(const json& e, Event& ev) {
  if (!e.is_object()) return false;
  double id = jnum(e, "id", 0), ts = jnum(e, "ts", 0);
  ev.id = isNum(id) && id >= 0 && id < 9e15 ? static_cast<std::uint64_t>(id) : 0;
  // A time outside any plausible range (a broken file) is not carried
  // over: it would anchor the clock after a restart.
  ev.ts = isNum(ts) && ts >= 0 && ts <= static_cast<double>(kNotAfter) ? static_cast<Epoch>(ts) : 0;
  ev.type = jstr(e, "type");
  ev.severity = jstr(e, "severity");
  ev.title = msgFromJson(e, "title");
  ev.text = msgFromJson(e, "text");
  ev.data = e.contains("data") && e["data"].is_object() ? e["data"] : json::object();
  return true;
}
}  // namespace

void EventLog::load(const json& j) {
  events_.clear();
  double next = jnum(j, "next", 1);
  next_ = isNum(next) && next >= 1 ? static_cast<std::uint64_t>(next) : 1;
  const json events = j.is_object() && j.contains("events") && j["events"].is_array() ? j["events"] : json::array();
  for (const auto& e : events) {
    Event ev;
    if (eventFromJson(e, ev)) events_.push_back(ev);
  }
  while (events_.size() > cap_) events_.pop_front();
}

std::string EventLog::journalSince(std::uint64_t afterId) const {
  std::string out;
  for (const auto& e : events_)
    if (e.id > afterId) out += json(e).dump() + "\n";
  return out;
}

size_t EventLog::replay(const std::string& journal) {
  size_t added = 0, pos = 0;
  while (pos < journal.size()) {
    size_t nl = journal.find('\n', pos);
    const bool torn = nl == std::string::npos;  // the last line was cut by a power loss
    const json e = json::parse(journal.substr(pos, torn ? std::string::npos : nl - pos), nullptr, false);
    pos = torn ? journal.size() : nl + 1;
    Event ev;
    if (torn || !eventFromJson(e, ev) || ev.id < next_) continue;  // already in the snapshot
    next_ = ev.id + 1;
    events_.push_back(std::move(ev));
    while (events_.size() > cap_) events_.pop_front();
    ++added;
  }
  return added;
}

}  // namespace doseph
