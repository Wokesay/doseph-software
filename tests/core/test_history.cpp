// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <set>
#include <string>

#include "doseph/messages.hpp"
#include "doseph/events.hpp"
#include "doseph/history.hpp"

using namespace doseph;

TEST_CASE("History: finest level, gaps stay gaps") {
  History h;
  Epoch t0 = 1790000000 - 1790000000 % 3600;
  for (int i = 0; i < 360; ++i) h.add("ph", t0 + i * 10, i == 100 ? kNaN : 5.8 + 0.001 * i);
  auto p = h.query("ph", t0, t0 + 3599, 0);
  CHECK(p.stepS == 10);
  CHECK(p.t.size() == 360);
  CHECK(std::isnan(p.avg[100]));
  CHECK(p.avg[0] == doctest::Approx(5.8));
}

TEST_CASE("History: thinning keeps minimum and maximum") {
  History h;
  Epoch t0 = 1790000000;
  for (int i = 0; i < 600; ++i) h.add("ec", t0 + i * 10, i == 300 ? 3.0 : 1.5);
  auto p = h.query("ec", t0, t0 + 6000, 50);
  CHECK(p.t.size() <= 50);
  double mx = 0;
  for (double v : p.max) mx = std::max(mx, v);
  CHECK(mx == doctest::Approx(3.0));
}

TEST_CASE("History: older periods come from coarser levels") {
  History h;
  Epoch t0 = 1790000000 - 1790000000 % 900;
  for (int i = 0; i < 3 * 24 * 360; ++i) h.add("t", t0 + i * 10, 20.0);  // 3 Tage
  Epoch now = t0 + 3 * 24 * 3600;
  CHECK(h.query("t", now - 3600, now, 0).stepS == 10);
  CHECK(h.query("t", now - 2 * 24 * 3600, now, 0).stepS == 60);
}

namespace {

// A snapshot as it was before the hourly level: "GCH1", the three finer levels of each series
std::string withoutHourly(const std::string& dump) {
  const char* p = dump.data();
  std::string out = "GCH1";
  p += 4;
  std::uint32_t n = 0;
  std::memcpy(&n, p, 4);
  out.append(p, 4);
  p += 4;
  const size_t header = 8 + 8 + 8 + 8 + 8 + 4;
  const size_t sizes[4] = {header + 8640 * 4, header + 10080 * 12, header + 35040 * 12, header + 43800 * 12};
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint32_t len = 0;
    std::memcpy(&len, p, 4);
    const size_t keep = 4 + len + sizes[0] + sizes[1] + sizes[2];
    out.append(p, keep);
    p += keep + sizes[3];
  }
  out.append(p, static_cast<size_t>(dump.data() + dump.size() - p));
  return out;
}

}  // namespace

TEST_CASE("History: five years at one hour; a year at 15 min; the period asked for picks the level (SD-126)") {
  History h;
  const Epoch t0 = 1790000000 - 1790000000 % 3600;
  const int hours = 2 * 365 * 24;  // two years, one sample an hour
  for (int i = 0; i < hours; ++i) h.add("ph", t0 + i * 3600, 6.0 + (i % 24) * 0.01);
  const Epoch now = t0 + hours * 3600;
  auto old = h.query("ph", now - 700 * 86400, now - 400 * 86400, 0);
  CHECK(old.stepS == 3600);
  REQUIRE(old.t.size() == 300 * 24 + 1);
  CHECK(old.avg[0] == doctest::Approx(6.0));  // 700 days back is midnight of the samples' cycle
  CHECK(old.max[1] == doctest::Approx(6.01));
  CHECK(std::isfinite(old.avg[old.t.size() / 2]));
  CHECK(h.query("ph", now - 200 * 86400, now, 0).stepS == 900);
}

TEST_CASE("History: a snapshot from before the hourly level is read, and the hourly level is worked out from its 15-min level (SD-126)") {
  History h;
  const Epoch t0 = 1790000000 - 1790000000 % 3600;
  for (int i = 0; i < 200 * 24; ++i) h.add("ec", t0 + i * 3600, 1.0 + (i % 5) * 0.1);
  const Epoch now = t0 + 200 * 24 * 3600;
  History k;
  REQUIRE(k.load(withoutHourly(h.dump())));
  // Asked for more than a year back, the hourly level answers, with the same values
  auto a = h.query("ec", now - 2 * 365 * 86400, now, 0);
  auto b = k.query("ec", now - 2 * 365 * 86400, now, 0);
  CHECK(b.stepS == 3600);
  REQUIRE(a.t.size() == b.t.size());
  int held = 0, same = 0;
  for (size_t i = 0; i < a.t.size(); ++i) {
    if (!std::isfinite(a.avg[i])) continue;
    held++;
    same += a.t[i] == b.t[i] && std::isfinite(b.avg[i]) && std::fabs(a.avg[i] - b.avg[i]) < 1e-6;
  }
  CHECK(held == 200 * 24);
  CHECK(same >= held - 1);  // the hour being filled may differ in its last sample
  // Saved again, it is the new form
  History m;
  REQUIRE(m.load(k.dump()));
  CHECK(k.dump().compare(0, 4, "DPH2") == 0);
}

TEST_CASE("History: saving and loading") {
  History h;
  for (int i = 0; i < 100; ++i) h.add("ph", 1790000000 + i * 10, 6.0);
  History k;
  REQUIRE(k.load(h.dump()));
  CHECK(k.query("ph", 1790000000, 1790001000, 0).t.size() == h.query("ph", 1790000000, 1790001000, 0).t.size());
  CHECK_FALSE(k.load("kaputt"));
}

TEST_CASE("Ereignislog: neueste zuerst, Filter, Obergrenze") {
  EventLog log(3);
  for (int i = 0; i < 5; ++i) log.add(100 + i, i % 2 ? "dose" : "mix", "info", Msg{"", "E" + std::to_string(i)});
  auto all = log.query(0, 1000, "", 10);
  REQUIRE(all.size() == 3);
  CHECK(all[0].title.text == "E4");
  CHECK(log.query(0, 1000, "dose", 10).size() == 1);
  EventLog k;
  k.load(log.toJson());
  CHECK(k.lastId() == 5);
}

TEST_CASE("Event log: key and values survive a restart, old plain texts stay readable") {
  EventLog log;
  log.add(100, "control", "info", Msg{"ev.cycle.ec_round", "Nutrient round 1", {{"n", 1}}},
          Msg{"ev.cycle.ec_round.text", "EC 1.20 → 1.45 mS/cm with 12.0 ml", {{"from", 1.2}, {"to", 1.45}, {"ml", 12.0}}});
  EventLog k;
  k.load(log.toJson());
  auto e = k.query(0, 1000, "", 1).at(0);
  CHECK(e.title.key == "ev.cycle.ec_round");
  CHECK(e.text.args["to"] == 1.45);
  // Plain strings as title and text are read as texts without a key
  json old = {{"next", 2}, {"events", {{{"id", 1}, {"ts", 100}, {"type", "system"}, {"severity", "info"}, {"title", "Started"}, {"text", "Version 0.1"}}}}};
  EventLog o;
  o.load(old);
  auto oe = o.query(0, 1000, "", 1).at(0);
  CHECK(oe.title.key.empty());
  CHECK(oe.title.text == "Started");
  CHECK(oe.text.text == "Version 0.1");
  // A message saved under a template it no longer fits keeps its own text, not "–" for a value
  json jumped = {{"next", 2}, {"events", {{{"id", 1}, {"ts", 100}, {"type", "block"}, {"severity", "warn"}, {"title", {{"key", "ev.flow_cut"}, {"text", "Dose stopped"}}},
                                           {"text", {{"key", "truth.jump"}, {"text", "pH jumped from 6.01 to 7.23."}, {"args", {{"until", 900}, {"from", 6.01}, {"to", 7.23}}}}}}}}};
  EventLog jl;
  jl.load(jumped);
  const auto je = jl.query(0, 1000, "", 1).at(0);
  CHECK(je.text.key.empty());
  CHECK(je.text.text == "pH jumped from 6.01 to 7.23.");
  CHECK(doseph::fitsTemplate("truth.jump", {{"signal", "ec"}, {"until", 900}, {"from", nullptr}, {"to", 2.4}, {"unit", "\u00a0mS/cm"}}));
  CHECK_FALSE(doseph::fitsTemplate("truth.jump", {{"until", 900}, {"from", 1.2}, {"to", 2.4}}));
}

TEST_CASE("Event log: a damaged events.json loads as empty messages, never throws") {
  json events = json::array();
  int id = 1;
  for (const json& v : {json(5), json::array({"a"}), json(nullptr), json(true),
                        json{{"key", 7}, {"text", {{"toString", 1}}}, {"args", "x"}},
                        json{{"key", "ev.plain"}, {"text", "ok"}, {"args", json::array({1})}}})
    events.push_back({{"id", id++}, {"ts", 100}, {"type", "system"}, {"severity", "info"}, {"title", v}, {"text", v}});
  events.push_back({{"id", id++}, {"ts", 100}, {"type", "system"}, {"severity", "info"},
                    {"title", std::string(5000, 'x')}, {"text", {{"key", std::string(500, 'k')}, {"text", "t"}}}});
  EventLog log;
  REQUIRE_NOTHROW(log.load({{"next", id}, {"events", events}}));
  auto all = log.query(0, 1000, "", 100);
  REQUIRE(all.size() == 7);
  for (const auto& e : all) {
    CHECK(e.title.args.is_object());
    CHECK(e.text.args.is_object());
  }
  // newest first: the long one, then the one with array args, then the bad types
  CHECK(all[0].title.text.size() == 2048);
  CHECK(all[0].text.key.size() == 64);
  CHECK(all[1].title.key.empty());  // its arguments were damaged: shown with the text it was saved with
  CHECK(all[1].title.text == "ok");
  CHECK(all[1].title.args.empty());
  for (size_t i = 2; i < all.size(); ++i) {
    CHECK(all[i].title.key.empty());
    CHECK(all[i].title.text.empty());
  }
}

namespace {
bool sameSeries(const History& a, const History& b, const std::string& id, Epoch from, Epoch to) {
  const auto pa = a.query(id, from, to, 0), pb = b.query(id, from, to, 0);
  if (pa.t != pb.t || pa.avg.size() != pb.avg.size()) return false;
  for (size_t i = 0; i < pa.avg.size(); ++i)
    if (!(pa.avg[i] == pb.avg[i] || (std::isnan(pa.avg[i]) && std::isnan(pb.avg[i])))) return false;
  return true;
}
}  // namespace

TEST_CASE("History journal: snapshot plus journal gives the same history, never counted twice") {
  const Epoch t0 = 1790000000 - 1790000000 % 3600;
  const std::set<std::string> ids = {"tank.ph", "tank.ec"};
  History live;
  std::string journal, snapshot;
  for (int i = 0; i < 720; ++i) {  // two hours, one round every 10 s
    const Epoch t = t0 + i * 10;
    const std::vector<std::pair<std::string, double>> round = {{"tank.ph", 5.8 + 0.001 * i}, {"tank.ec", i == 50 ? kNaN : 1400.0 + i}};
    for (const auto& [id, v] : round) live.add(id, t, v);
    journal += History::record(t, round);
    if (i == 359) {  // the snapshot after one hour; its journal is emptied after it
      snapshot = live.dump();
      journal.clear();
    }
  }
  History back;
  REQUIRE(back.load(snapshot));
  CHECK(back.replay(journal, ids) == 360);
  CHECK(sameSeries(live, back, "tank.ph", t0, t0 + 7199));
  CHECK(sameSeries(live, back, "tank.ec", t0, t0 + 7199));
  CHECK(back.replay(journal, ids) == 0);  // read twice: nothing added twice

  SUBCASE("a journal that outlived its snapshot is skipped, not counted twice") {
    History full;
    std::string all;
    for (int i = 0; i < 720; ++i) all += History::record(t0 + i * 10, {{"tank.ph", 5.8 + 0.001 * i}});
    REQUIRE(full.load(live.dump()));
    CHECK(full.replay(all, ids) == 0);
    CHECK(sameSeries(live, full, "tank.ph", t0, t0 + 7199));
  }
  SUBCASE("a torn end from a power loss is skipped, everything before it counts") {
    History torn;
    REQUIRE(torn.load(snapshot));
    CHECK(torn.replay(journal.substr(0, journal.size() - 5), ids) == 359);
  }
  SUBCASE("a damaged record stops the replay; nothing after it is trusted") {
    std::string bad = journal;
    bad[History::record(t0, {{"tank.ph", 1.0}, {"tank.ec", 1.0}}).size() * 10 + 20] ^= 0x01;  // one bit in record 11
    History damaged;
    REQUIRE(damaged.load(snapshot));
    CHECK(damaged.replay(bad, ids) == 10);
  }
  SUBCASE("a snapshot from before the journal (no times in it) still loads; a cut one does not") {
    const std::string tag = "GCT2";
    const std::string old = snapshot.substr(0, snapshot.rfind(tag));
    History h;
    CHECK(h.load(old));
    CHECK(sameSeries(h, back, "tank.ph", t0, t0 + 3580));  // the snapshot's last slot was still open
    CHECK(h.replay(journal, ids) == 360);                  // without times in it, the journal is taken whole
    History cut;
    CHECK_FALSE(cut.load(snapshot.substr(0, snapshot.size() - 3)));
  }
}

TEST_CASE("History journal: a torn record followed by a whole one is never read as values") {
  const Epoch t0 = 1790000000;
  const std::set<std::string> ids = {"tank.ph", "tank.ec"};
  std::string good;
  for (int i = 0; i < 3; ++i) good += History::record(t0 + i * 10, {{"tank.ph", 6.0}, {"tank.ec", 1400.0}});
  const std::string torn = History::record(t0 + 30, {{"tank.ph", 6.1}, {"tank.ec", 1410.0}});
  const std::string whole = History::record(t0 + 40, {{"tank.ph", 6.2}, {"tank.ec", 1420.0}});
  for (size_t cut = 1; cut < torn.size(); ++cut) {
    CAPTURE(cut);
    History h;
    CHECK(h.replay(good + torn.substr(0, cut) + whole, ids) == 3);  // power loss mid-record, then one more record
    const auto ec = h.query("tank.ec", t0, t0 + 60, 0);
    for (double v : ec.avg) CHECK((std::isnan(v) || v == 1400.0));
    CHECK(h.ids() == std::vector<std::string>{"tank.ec", "tank.ph"});
  }
}

TEST_CASE("History journal: only known series and plausible times, each series by its own newest time") {
  const Epoch t0 = 1790000000;
  History h;
  const std::string journal = History::record(t0, {{"tank.ph", 6.0}, {"junk", 1.0}}) +  // an id no role has
                              History::record(kNotAfter + 1000, {{"tank.ph", 9.9}}) +   // a time no clock can have
                              History::record(t0 + 10, {{"tank.ph", 6.2}});
  CHECK(h.replay(journal, {"tank.ph"}) == 2);
  CHECK(h.ids() == std::vector<std::string>{"tank.ph"});
  const auto ph = h.query("tank.ph", t0, t0 + 19, 0);
  REQUIRE(ph.avg.size() == 2);
  CHECK(ph.avg[0] == doctest::Approx(6.0));
  CHECK(ph.avg[1] == doctest::Approx(6.2));  // the far-future record did not push the series ahead

  // One series ran ahead (a clock glitch); a series added later is still replayed
  History glitch;
  glitch.add("tank.ph", t0 + 86400, 6.0);
  History later;
  REQUIRE(later.load(glitch.dump()));
  CHECK(later.replay(History::record(t0 + 10, {{"tank.ec", 1400.0}}), {"tank.ph", "tank.ec"}) == 1);
  CHECK(later.query("tank.ec", t0, t0 + 60, 0).t.size() > 0);
}

TEST_CASE("Event journal: events after the snapshot come back by id, a torn line is skipped") {
  EventLog live;
  live.add(100, "system", "info", Msg{"ev.a", "A", json::object()});
  const json snapshot = live.toJson();
  const std::uint64_t saved = live.lastId();
  live.add(110, "system", "alarm", Msg{"ev.stop", "Stop", json::object()});
  live.add(120, "config", "info", Msg{"ev.b", "B", json::object()});
  const std::string journal = live.journalSince(saved);

  EventLog back;
  back.load(snapshot);
  CHECK(back.replay(journal) == 2);
  CHECK(back.lastId() == live.lastId());
  CHECK(back.query(0, 200, "", 10).front().title.key == "ev.b");
  CHECK(back.replay(journal) == 0);  // a journal that outlived its snapshot adds nothing twice

  EventLog torn;
  torn.load(snapshot);
  CHECK(torn.replay(journal.substr(0, journal.size() - 3)) == 1);  // the last line was cut
  CHECK(torn.query(0, 200, "", 10).front().title.key == "ev.stop");
  const Event& next = torn.add(130, "system", "info", Msg{"ev.c", "C", json::object()});
  CHECK(next.id == 3);  // ids go on after the journal
}

TEST_CASE("History: a time in UTC as a spreadsheet reads it (RAT-016)") {
  CHECK(utcText(0) == "1970-01-01 00:00:00");
  CHECK(utcText(946684799) == "1999-12-31 23:59:59");
  CHECK(utcText(1767225600) == "2026-01-01 00:00:00");
  CHECK(utcText(1835440496) == "2028-02-29 12:34:56");
  CHECK(utcText(4107542400) == "2100-03-01 00:00:00");
}
