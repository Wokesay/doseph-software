// SPDX-License-Identifier: AGPL-3.0-or-later
// Large tanks (SD-150): how long a preparation takes and how far it is, and a tank mixed by hand
// that the robot only holds.
#include <doctest/doctest.h>

#include <functional>
#include <string>

#include "doseph/prep.hpp"
#include "harness.hpp"

using namespace doseph;

namespace {

struct Fix {
  Config cfg;
  RecipeEffect eff;  // the start effect throughout: 0.275 per ml/L
  explicit Fix(double flowMlPerMin) {
    cfg.canisters = {{"a", "Part A", "nutrient", 1, "AB", "#00c"}, {"b", "Part B", "nutrient", 2, "AB", "#0c0"}};
    for (int n = 1; n <= 2; ++n) cfg.channels[n] = {flowMlPerMin, 1};
    cfg.recipes.push_back(RecipeCfg{"r", "Test", "", {{"a", 1.0}, {"b", 1.0}}});
  }
  PrepInput input(double volumeL, double ec, double aim) const {
    PrepInput in;
    in.volumeL = volumeL;
    in.ec = ec;
    in.aim = aim;
    in.ecTol = 0.1;
    in.roundWaitMs = 10 * kMinute;
    return in;
  }
  PrepEstimate run(const PrepInput& in) const { return estimatePrep(cfg, cfg.recipes[0], eff, in); }
};

// Looks every 10 s of simulated time, which runs on in steps of 1 s; fails past its deadline
void waitFor(harness::Run& r, const std::string& what, const std::function<bool()>& done, double maxS) {
  for (double t = 0; t <= maxS; t += 10) {
    if (done()) return;
    r.step(10 * kSecond);
  }
  FAIL("not within " << maxS << " s: " << what);
}

}  // namespace

TEST_CASE("Preparation: the estimate runs the cycle's own rounds forward, each with its runs and the mixing wait; within the band there is nothing to do (SD-150, RAT-055)") {
  const Fix f(50);
  // 100 L from EC 0.4 to 1.6: 0.8 × 1.2 ÷ 0.275 = 3.49 ml/L, 349 ml in all, then 0.8 of what is left
  const PrepEstimate e = f.run(f.input(100, 0.4, 1.6));
  REQUIRE(e.ok);
  CHECK(e.rounds == 2);
  CHECK_FALSE(e.limited);
  CHECK_FALSE(e.daily);
  CHECK_FALSE(e.more);
  CHECK(e.reachEc == doctest::Approx(1.552).epsilon(0.001));
  CHECK(e.ml == doctest::Approx(349.1 + 69.8).epsilon(0.01));
  // The runs at 50 ml/min (about 8.4 min in all) and two mixing waits of 10 min
  CHECK(e.totalMs > 28 * kMinute);
  CHECK(e.totalMs < 29 * kMinute);
  // Pauses of the trough between runs and between channels add to it (SD-147)
  PrepInput paused = f.input(100, 0.4, 1.6);
  paused.runPauseMs = 3 * kSecond;
  paused.channelPauseMs = 30 * kSecond;
  paused.tailMs = 15 * kMinute;  // the pH round after
  const PrepEstimate p = f.run(paused);
  // Round 1: four runs a part, three pauses each; round 2: one run a part
  CHECK(p.totalMs - e.totalMs == 15 * kMinute + 2 * 30 * kSecond + 6 * 3 * kSecond);
  // Within the band: no round, no time, no pH round either
  const PrepEstimate none = f.run(f.input(100, 1.55, 1.6));
  REQUIRE(none.ok);
  CHECK(none.rounds == 0);
  CHECK(none.totalMs == 0);
}

TEST_CASE("Preparation: in a tank of 30 m³ the partial-run limit makes each round small, each check holds after its rounds, and the daily run time per channel stretches it over days (SD-150, SD-113, RAT-055)") {
  const Fix f(300);
  PrepInput in = f.input(30000, 0.0, 1.6);
  in.maxRounds = 10;
  in.checkMs = 70 * kMinute;
  // Six runs of 60 s a part: 3.6 L a round, 0.12 ml/L, about 0.033 mS/cm
  const PrepEstimate free = f.run(in);
  REQUIRE(free.ok);
  CHECK(free.limited);
  CHECK_FALSE(free.daily);
  CHECK(free.rounds >= 40);
  CHECK(free.reachEc >= 1.5);
  // The same with the code's 60 min a day per channel: about ten rounds a day
  in.dayCapMs = {{1, 60 * kMinute}, {2, 60 * kMinute}};
  const PrepEstimate capped = f.run(in);
  REQUIRE(capped.ok);
  CHECK(capped.daily);
  CHECK(capped.rounds == free.rounds);
  CHECK(capped.totalMs > free.totalMs);
  CHECK(capped.totalMs > 3 * kPrepDayMs);
  // 100 m³ with small pumps: beyond two weeks the estimate stops and says "more than"
  const Fix slow(50);
  PrepInput big = slow.input(100000, 0.0, 1.6);
  big.dayCapMs = {{1, 60 * kMinute}, {2, 60 * kMinute}};
  const PrepEstimate more = slow.run(big);
  REQUIRE(more.ok);
  CHECK(more.more);
  CHECK(more.totalMs >= kPrepMaxMs);
}

TEST_CASE("Preparation: runs of the last 24 h use up a channel's daily run time first; a round that never fits it gives no estimate (SD-150, SD-113)") {
  const Fix f(50);
  PrepInput in = f.input(100, 0.4, 1.6);
  in.dayCapMs = {{1, 60 * kMinute}, {2, 60 * kMinute}};
  in.dayRuns[1] = {{-kMinute * 60, 59 * kMinute}};  // an hour ago: 59 of its 60 min
  const PrepEstimate e = f.run(in);
  REQUIRE(e.ok);
  CHECK(e.daily);
  CHECK(e.totalMs > 23 * 60 * kMinute);  // it waits until that run falls out of the day
  in.dayRuns.clear();
  in.dayCapMs = {{1, kMinute}};  // the user's daily amount: 50 ml, one round needs about 175 ml
  const PrepEstimate never = f.run(in);
  CHECK_FALSE(never.ok);
  CHECK(never.reason.key == "prep.daily_short");
}

TEST_CASE("Preparation: without volume, EC or its target there is no estimate; nothing stands in for them (SD-150, R5)") {
  const Fix f(50);
  CHECK(f.run(f.input(kNaN, 0.4, 1.6)).reason.key == "prep.unknown");
  CHECK(f.run(f.input(100, kNaN, 1.6)).reason.key == "prep.unknown");
  CHECK(f.run(f.input(100, 0.4, kNaN)).reason.key == "prep.unknown");
  CHECK_FALSE(f.run(f.input(100, kNaN, 1.6)).ok);
}

TEST_CASE("Cycle: a preparation shows where it began, its rounds and when it should be done; it ends once EC is in its band, logged with how it went (SD-150)") {
  harness::Run r(harness::device());
  // The demo's fresh fill: EC 0.45 in 100 L
  waitFor(r, "the first round", [&] { return r.phase() == "dose"; }, 20 * 60);
  json c = r.state()["cycle"];
  REQUIRE(c["prep"].is_object());
  CHECK(c["prep"]["from"].get<double>() == doctest::Approx(0.45).epsilon(0.1));
  CHECK(c["prep"]["rounds"] == 1);
  const json est = c["prep"]["estimate"];
  REQUIRE(est["ok"] == true);
  CHECK(est["rounds"].get<int>() >= 1);
  CHECK(est["endAt"].get<long long>() > r.state()["now"].get<long long>());
  CHECK(est["leftMin"].get<int>() > 0);
  // A fresh fill of this tank, estimated at the check, to choose who prepares it
  REQUIRE(c["fresh"].is_object());
  CHECK(c["fresh"]["ok"] == true);
  CHECK(c["fresh"]["rounds"].get<int>() >= 1);
  waitFor(r, "on target", [&] { return r.phase() == "hold"; }, 8 * 3600);
  CHECK(r.state()["cycle"]["prep"].is_null());
  const json done = r.events("control");
  bool logged = false;
  for (const auto& e : done)
    if (e["title"]["key"] == "ev.prep.done") {
      logged = true;
      CHECK(e["data"]["rounds"].get<int>() >= 2);
      CHECK(e["data"]["to"].get<double>() > e["data"]["from"].get<double>());
    }
  CHECK(logged);
}

TEST_CASE("Cycle: mixed by hand, the robot only holds: EC far below its target is not dosed, pH neither, and it says so; near its target it corrects (SD-150, RAT-004)") {
  harness::Run r(harness::device());
  CHECK(r.call("PUT", "tank", {{"prepare", "both"}}).status == 422);
  r.ok("PUT", "tank", {{"prepare", "hand"}});
  CHECK(r.ok("GET", "config")["tank"]["prepare"] == "hand");
  // The demo's fresh fill, EC 0.45 against 1.6: the person's to mix
  waitFor(r, "the check's end", [&] { return r.phase() == "hold"; }, 30 * 60);
  json c = r.state()["cycle"];
  bool said = false;
  for (const auto& n : c["notes"]) said = said || n["key"] == "cycle.note.hand_mix";
  CHECK(said);
  CHECK(c["prep"].is_null());
  for (const auto& e : r.events("control")) {
    CHECK(e["title"]["key"] != "ev.cycle.ec_round");
    CHECK(e["title"]["key"] != "ev.cycle.ph_dose");
  }
  // Mixed by hand to near its targets: the robot corrects what is left
  r.act("fill", {{"volumeL", 100}, {"ec", 1.4}, {"ph", 6.3}});
  r.ok("POST", "tank/new");
  waitFor(r, "a round that corrects", [&] {
    for (const auto& e : r.events("control"))
      if (e["title"]["key"] == "ev.cycle.ec_round" || e["title"]["key"] == "ev.cycle.ph_dose") return true;
    return false;
  }, 3 * 3600);
}
