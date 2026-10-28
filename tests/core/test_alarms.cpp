// SPDX-License-Identifier: AGPL-3.0-or-later
// Alarms from the watchdog's problems: levels, quiet hours, repetition,
// acknowledgement and the morning report (SD-123, RAT-022).
#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "doseph/alarms.hpp"
#include "doseph/messages.hpp"

using namespace doseph;

namespace {

constexpr Epoch kNight = 1798761600;           // 2027-01-01 01:00 in Berlin
constexpr Epoch kDay = kNight + 11 * 3600;     // 12:00
constexpr Epoch kMorning = kNight + 6 * 3600;  // 07:00, the end of the quiet hours

WatchResult watch(const std::vector<std::pair<std::string, std::string>>& items) {
  WatchResult w;
  for (const auto& [id, status] : items) w.items.push_back({id, say("watch.latch"), status, say("watch.reading_ok")});
  return w;
}

struct Fix {
  NotifyCfg cfg;
  RuntimeState rt;
  Alarms alarms;
  std::vector<Notification> sent;
  // The watchdog's items, held for `minutes` with an update every minute from `from`
  void run(const WatchResult& w, Epoch from, int minutes, bool idle = true) {
    for (int m = 0; m < minutes; ++m) {
      alarms.update(w, {cfg, "Europe/Berlin", from + m * 60, idle}, rt);
      for (auto& n : alarms.takeOutbox()) sent.push_back(std::move(n));
    }
  }
  int count(const std::string& kind) const {
    int n = 0;
    for (const auto& s : sent) n += s.kind == kind;
    return n;
  }
};

}  // namespace

TEST_CASE("Alarms: the level of each watchdog item; a latch is raised at once, anything else once it stood 5 min (SD-123)") {
  CHECK(levelOf("latch.hw.leak") == Level::Critical);
  CHECK(levelOf("latch.hw.high_level") == Level::Critical);
  CHECK(levelOf("latch.channel.3.siphon") == Level::Critical);
  CHECK(levelOf("latch.flow.dry") == Level::Critical);
  CHECK(levelOf("latch.hw.return") == Level::Critical);
  CHECK(levelOf("target.ph") == Level::Critical);
  CHECK(levelOf("target.ec") == Level::Warning);
  CHECK(levelOf("latch.channel.3.no_drops") == Level::Warning);
  CHECK(levelOf("stock.part_a") == Level::Warning);
  CHECK(levelOf("water") == Level::Warning);
  CHECK(levelOf("probe.ph") == Level::Warning);
  CHECK(levelOf("maint.probe.ph") == Level::Info);
  Fix f;
  f.run(watch({{"latch.hw.leak", "problem"}, {"probe.ph", "problem"}, {"probe.ec", "ok"}}), kDay, 1);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].level == "critical");
  CHECK(f.sent[0].items == std::vector<std::string>{"latch.hw.leak"});
  f.run(watch({{"latch.hw.leak", "problem"}, {"probe.ph", "problem"}}), kDay + 60, 4);
  CHECK(f.count("alarm") == 1);  // the probe's problem has not stood 5 min yet
  f.run(watch({{"latch.hw.leak", "problem"}, {"probe.ph", "problem"}}), kDay + 5 * 60, 1);
  CHECK(f.count("alarm") == 2);
  for (const auto& n : f.sent)
    if (n.kind == "alarm" && n.items[0] == "probe.ph") CHECK(n.level == "warning");
  // A problem that went away before its 5 min is no alarm
  Fix g;
  g.run(watch({{"flow", "problem"}}), kDay, 4);
  g.run(watch({{"flow", "ok"}}), kDay + 4 * 60, 1);
  g.run(watch({{"flow", "problem"}}), kDay + 5 * 60, 4);
  CHECK(g.sent.empty());
}

TEST_CASE("Alarms: a critical alarm is pushed at night too and repeated 5, 15 and 60 min after, then no more; a warning by day once (RAT-022)") {
  Fix f;
  f.run(watch({{"latch.flow.dry", "problem"}}), kNight, 3 * 60);
  REQUIRE(f.sent.size() == 4);
  CHECK(f.sent[0].kind == "alarm");
  CHECK(f.sent[1].at - f.sent[0].at == 5 * 60);
  CHECK(f.sent[2].at - f.sent[0].at == 15 * 60);
  CHECK(f.sent[3].at - f.sent[0].at == 60 * 60);
  CHECK(f.sent[3].kind == "repeat");
  CHECK(f.sent[3].title.key == "notify.still");
  Fix g;
  g.run(watch({{"stock.part_a", "problem"}}), kDay, 3 * 60);
  CHECK(g.sent.size() == 1);
}

TEST_CASE("Alarms: at night a warning waits for the morning report when the quiet hours end; a note goes only there; the report comes once a day (RAT-022)") {
  Fix f;
  const auto night = watch({{"water", "problem"}, {"maint.probe.ph", "problem"}});
  f.run(night, kNight, 6 * 60 - 1);  // until 06:58
  CHECK(f.sent.empty());
  f.run(night, kMorning - 60, 3);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].kind == "report");
  CHECK(f.sent[0].at == kMorning);
  CHECK(f.sent[0].level == "warning");
  CHECK(f.sent[0].title.key == "notify.report");
  CHECK(f.sent[0].lines.size() == 2);
  // Neither comes again, not even in the next report
  f.run(night, kMorning + 3 * 60, 24 * 60);
  CHECK(f.sent.size() == 1);
  // A note raised by day waits for the next morning too
  Fix g;
  g.run(watch({{"maint.probe.ec", "problem"}}), kDay, 19 * 60 + 2);  // 12:00 to 07:01 the next day
  REQUIRE(g.sent.size() == 1);
  CHECK(g.sent[0].at == kMorning + 86400);
  CHECK(g.sent[0].level == "info");
  // Without quiet hours a warning is pushed at night
  Fix h;
  h.cfg.quietFromMin = h.cfg.quietToMin = 7 * 60;
  h.run(watch({{"water", "problem"}}), kNight, 6);
  CHECK(h.sent.size() == 1);
}

TEST_CASE("Alarms: acknowledging pauses the repetition, never the alarm; the pause ends after 12 h with a new round, at once when another problem appears, and with the problem (RAT-022)") {
  Fix f;
  const auto leak = watch({{"latch.hw.leak", "problem"}});
  f.run(leak, kDay, 2);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.alarms.ack("latch.hw.leak", kDay + 2 * 60, f.rt));
  CHECK_FALSE(f.alarms.ack("latch.flow.dry", kDay, f.rt));  // none raised
  f.run(leak, kDay + 2 * 60, 3 * 60);
  CHECK(f.sent.size() == 1);  // no repetition while paused
  const json listed = f.alarms.list(leak, f.rt);
  REQUIRE(listed.size() == 1);  // still raised, still red
  CHECK(listed[0]["ackUntil"] == kDay + 2 * 60 + 12 * 3600);
  // Another problem ends the pause: a reminder at once, with a new round of repetitions
  const auto both = watch({{"latch.hw.leak", "problem"}, {"latch.hw.high_level", "problem"}});
  f.run(both, kDay + 4 * 3600, 1);
  REQUIRE(f.sent.size() == 3);  // the new alarm, and the leak's reminder with it
  CHECK(f.sent[1].items[0] == "latch.hw.high_level");
  CHECK(f.sent[2].kind == "repeat");
  CHECK(f.sent[2].items[0] == "latch.hw.leak");
  // 12 h end a pause: a reminder then, not before
  Fix g;
  g.run(leak, kDay, 1);
  g.alarms.ack("", kDay, g.rt);
  g.run(leak, kDay, 12 * 60);
  CHECK(g.sent.size() == 1);
  g.run(leak, kDay + 12 * 3600, 1);
  CHECK(g.sent.size() == 2);
  // The problem ends: its alarm and its pause end; when it comes back it is new
  g.alarms.ack("latch.hw.leak", kDay + 12 * 3600 + 60, g.rt);
  g.run(watch({{"latch.hw.leak", "ok"}}), kDay + 12 * 3600 + 120, 1);
  CHECK(g.rt.alarms.empty());
  g.run(leak, kDay + 12 * 3600 + 180, 1);
  CHECK(g.sent.back().kind == "alarm");
}

TEST_CASE("Alarms: a target out of range counts only while the check holds or waits, not while it doses and mixes (SD-123)") {
  Fix f;
  const auto low = watch({{"target.ec", "problem"}});
  f.run(low, kDay, 60, false);  // an hour of correcting
  CHECK(f.sent.empty());
  f.run(low, kDay + 3600, 4, true);
  CHECK(f.sent.empty());
  f.run(low, kDay + 3600 + 4 * 60, 2, true);
  CHECK(f.sent.size() == 1);
}

TEST_CASE("Alarms: their state lives in the run-time state, so a restart does not push again what was pushed (SD-123, R6)") {
  Fix f;
  const auto leak = watch({{"latch.hw.leak", "problem"}});
  f.run(leak, kDay, 1);
  REQUIRE(f.sent.size() == 1);
  json saved = f.rt;
  RuntimeState back = runtimeFromJson(saved);
  Alarms fresh;
  fresh.update(leak, {f.cfg, "Europe/Berlin", kDay + 60, true}, back);
  CHECK(fresh.takeOutbox().empty());
  CHECK(back.alarms.count("latch.hw.leak"));
}

TEST_CASE("Quiet hours: across midnight, within a day, or none") {
  NotifyCfg c;
  CHECK(Alarms::quiet(c, 22 * 60));
  CHECK(Alarms::quiet(c, 3 * 60));
  CHECK_FALSE(Alarms::quiet(c, 7 * 60));
  CHECK_FALSE(Alarms::quiet(c, 21 * 60 + 59));
  c.quietFromMin = 13 * 60;
  c.quietToMin = 15 * 60;
  CHECK(Alarms::quiet(c, 14 * 60));
  CHECK_FALSE(Alarms::quiet(c, 15 * 60));
  c.quietToMin = 13 * 60;
  CHECK_FALSE(Alarms::quiet(c, 13 * 60));
}
