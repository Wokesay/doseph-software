// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/alarms.hpp"

#include <map>
#include <set>

#include "doseph/localtime.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool endsWith(const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
Epoch epochAt(const json& j, const char* key) { return j.contains(key) && j[key].is_number_integer() ? j[key].get<Epoch>() : 0; }
int intAt(const json& j, const char* key) { return j.contains(key) && j[key].is_number_integer() ? j[key].get<int>() : 0; }

}  // namespace

const char* levelName(Level l) {
  switch (l) {
    case Level::Critical: return "critical";
    case Level::Warning: return "warning";
    case Level::Info: return "info";
  }
  return "warning";
}

// Critical: water where it must not be, a pump that runs dry or moves nothing, a canister that runs
// into the tank on its own, pH out of its range (SD-123), CO2 above 2,000 ppm (SD-136), and a fault
// of the device's protection (SD-141). Info: what can wait for a date. Everything else is a warning.
Level levelOf(const std::string& id) {
  if (id == "hw.board_hot" || id == std::string("latch.") + kSafetyRestartNote) return Level::Warning;
  if (startsWith(id, "hw.") || startsWith(id, "latch.hw.")) return Level::Critical;
  if (id == "latch.hw.leak" || id == "latch.hw.high_level" || id == "latch.hw.return" || id == "latch.flow.dry" || id == "latch.pump.cable" ||
      id == "target.ph")
    return Level::Critical;
  if (startsWith(id, "latch.channel.") && endsWith(id, ".siphon")) return Level::Critical;
  if (startsWith(id, "climate.") && endsWith(id, ".co2")) return Level::Critical;  // gas where people work (SD-136)
  if (startsWith(id, "maint.") || startsWith(id, "plant.") || id == "ec.precipitation") return Level::Info;
  return Level::Warning;
}

void to_json(json& j, const Notification& n) {
  j = {{"id", n.id}, {"at", n.at}, {"kind", n.kind}, {"level", n.level}, {"title", n.title}, {"lines", n.lines}, {"items", n.items}};
}

bool Alarms::quiet(const NotifyCfg& cfg, int m) {
  const int from = cfg.quietFromMin, to = cfg.quietToMin;
  if (from == to) return false;
  return from < to ? m >= from && m < to : m >= from || m < to;
}

void Alarms::push(Notification n, Epoch at) {
  n.id = nextId_++;
  n.at = at;
  recent_.push_front(n);
  while (recent_.size() > kRecentNotifications) recent_.pop_back();
  outbox_.push_back(std::move(n));
}

std::vector<Notification> Alarms::takeOutbox() {
  std::vector<Notification> out;
  out.swap(outbox_);
  return out;
}

bool Alarms::update(const WatchResult& w, const AlarmInput& in, RuntimeState& rt) {
  const Epoch now = in.epoch;
  bool changed = false;
  std::map<std::string, const Assessment*> problems;
  for (const auto& a : w.items)
    if (a.status == "problem") problems[a.id] = &a;

  // A problem that is over ends its alarm, and with it the alarm's pause
  for (auto it = rt.alarms.begin(); it != rt.alarms.end();) {
    if (problems.count(it->first)) {
      ++it;
    } else {
      it = rt.alarms.erase(it);
      changed = true;
    }
  }

  const bool night = quiet(in.cfg, localTime(now, in.zone).minuteOfDay);
  auto notify = [&](const char* kind, const std::string& level, const Assessment& a) {
    Notification n;
    n.kind = kind;
    n.level = level;
    n.title = std::string(kind) == "repeat" ? say("notify.still", {{"label", a.label}}) : a.label;
    n.lines = {a.text};
    n.items = {a.id};
    push(std::move(n), now);
  };

  // New problems: a latch is an alarm at once, anything else once it has stood a while; a target
  // out of range counts only while the check holds or waits, not while it corrects it
  std::set<std::string> raised;
  for (const auto& [id, a] : problems) {
    auto it = rt.alarms.find(id);
    if (it == rt.alarms.end()) {
      it = rt.alarms.emplace(id, json{{"since", now}, {"raised", false}}).first;
      changed = true;
    }
    json& s = it->second;
    if (jbool(s, "raised", false)) continue;
    if (startsWith(id, "target.") && !in.cycleIdle) {
      s["since"] = now;
      continue;
    }
    // A latch, and the device's protection, alarm at once (SD-141)
    if (!startsWith(id, "latch.") && !startsWith(id, "hw.") && now - epochAt(s, "since") < kAlarmStandS) continue;
    const Level lv = levelOf(id);
    const bool pushNow = lv == Level::Critical || (lv == Level::Warning && !night);
    s = {{"since", epochAt(s, "since")}, {"raised", true},     {"level", levelName(lv)},
         {"round", now},                 {"sent", pushNow ? 1 : 0}, {"ackUntil", 0},
         {"reported", pushNow}};
    if (pushNow) notify("alarm", levelName(lv), *a);
    raised.insert(id);
    changed = true;
  }

  // Another problem ends every pause at once (RAT-022)
  if (!raised.empty())
    for (auto& [id, s] : rt.alarms)
      if (!raised.count(id) && epochAt(s, "ackUntil") > now) s["ackUntil"] = now;

  // A critical alarm is repeated 5, 15 and 60 min after its first push; a pause that ends starts
  // a new round of reminders, at once
  for (auto& [id, s] : rt.alarms) {
    if (!jbool(s, "raised", false) || jstr(s, "level") != "critical") continue;
    const Epoch ack = epochAt(s, "ackUntil");
    if (ack > 0) {
      if (now < ack) continue;
      s["ackUntil"] = 0;
      s["round"] = now;
      s["sent"] = 0;
      changed = true;
    }
    const int sent = intAt(s, "sent");
    const Epoch round = epochAt(s, "round");
    const bool due = sent == 0 || (sent <= 3 && now >= round + kRepeatS[sent - 1]);
    if (!due) continue;
    notify("repeat", "critical", *problems.at(id));
    s["sent"] = sent + 1;
    changed = true;
  }

  // The morning report when the quiet hours end: what waited through the night, and the notes
  if (rt.lastReport == 0) {
    rt.lastReport = now;
    changed = true;
  } else if (now >= nextLocalMinute(rt.lastReport + 1, in.zone, in.cfg.quietToMin)) {
    Notification n;
    n.kind = "report";
    n.level = "info";
    for (auto& [id, s] : rt.alarms) {
      if (!jbool(s, "raised", false) || jbool(s, "reported", false)) continue;
      const Assessment& a = *problems.at(id);
      n.lines.push_back(say("notify.item", {{"label", a.label}, {"text", a.text}}));
      n.items.push_back(id);
      if (jstr(s, "level") == "warning") n.level = "warning";
      s["reported"] = true;
    }
    if (!n.items.empty()) {
      n.title = say("notify.report", {{"n", n.items.size()}});
      push(std::move(n), now);
    }
    rt.lastReport = now;
    changed = true;
  }
  return changed;
}

bool Alarms::ack(const std::string& id, Epoch now, RuntimeState& rt) {
  bool any = false;
  for (auto& [key, s] : rt.alarms) {
    if ((!id.empty() && key != id) || !jbool(s, "raised", false)) continue;
    s["ackUntil"] = now + kAckPauseS;
    any = true;
  }
  return any;
}

json Alarms::list(const WatchResult& w, const RuntimeState& rt) const {
  json out = json::array();
  for (const auto& a : w.items) {
    auto it = rt.alarms.find(a.id);
    if (a.status != "problem" || it == rt.alarms.end() || !jbool(it->second, "raised", false)) continue;
    const json& s = it->second;
    out.push_back({{"id", a.id},
                   {"level", jstr(s, "level", "warning")},
                   {"since", epochAt(s, "since")},
                   {"sent", intAt(s, "sent")},
                   {"ackUntil", epochAt(s, "ackUntil")},
                   {"label", a.label},
                   {"text", a.text}});
  }
  return out;
}

}  // namespace doseph
