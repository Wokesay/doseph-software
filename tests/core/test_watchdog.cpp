// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>

#include "doseph/messages.hpp"
#include "doseph/watchdog.hpp"

using namespace doseph;

namespace {

Reading ok(const char* ch, double v) {
  Reading r;
  r.signal = ch;
  r.value = v;
  r.quality = Quality::Ok;
  r.reason = say("truth.ok");
  return r;
}

struct Fix {
  Config cfg;
  RuntimeState rt;
  Fix() {
    cfg.tank.volumeL = 100;
    cfg.canisters = {{"a", "Part A", "nutrient", 2, "", "#00c"}, {"down", "pH−", "ph_down", 1, "", "#c00"}};
    cfg.pumps[1] = {40, 1};
    cfg.pumps[2] = {50, 1};
    RecipeCfg r{"r", "Veg", "", {{"a", 2.0}}};
    r.ecTarget = 1.6;
    r.phLow = 5.8;
    r.phHigh = 6.2;
    cfg.recipes = {r};
    cfg.active = {"r", 1000, 0};
  }
  WatchInput input(double ph, double ec) {
    WatchInput in{cfg, rt, {ok("ph", ph), ok("ec", ec), ok("temp", 21), ok("flow", 12)}};
    in.needsFlow = true;
    in.lastPh = ph;
    in.lastEc = ec;
    in.lastAt = 5000;
    in.epoch = 5000;
    in.bootEpoch = 1000;
    in.pumpCount = 4;
    return in;
  }
};

const Assessment& item(const WatchResult& w, const std::string& id) {
  auto it = std::find_if(w.items.begin(), w.items.end(), [&](const Assessment& a) { return a.id == id; });
  REQUIRE(it != w.items.end());
  return *it;
}

}  // namespace

TEST_CASE("Watchdog: the alarm band is wider than the control band (RAT-033)") {
  Fix f;
  // pH 6.0 ± 0.15 control, ± 0.35 alarm
  auto w = evaluate(f.input(6.3, 1.6));
  CHECK(item(w, "target.ph").status == "ok");
  w = evaluate(f.input(6.4, 1.6));
  CHECK(item(w, "target.ph").status == "problem");
  CHECK(item(w, "target.ph").text.key == "watch.target_high");
  // EC 1.6 ± 0.1 control, ± 0.3 alarm
  CHECK(item(evaluate(f.input(6.0, 1.35)), "target.ec").status == "ok");
  CHECK(item(evaluate(f.input(6.0, 1.25)), "target.ec").status == "problem");
}

TEST_CASE("Watchdog: a reading that is not delivered is a problem, never neutral; not calibrated only while a recipe needs it (RAT-015, RAT-021)") {
  Fix f;
  {
    auto in = f.input(6.0, 1.6);
    in.readings[1].quality = Quality::Stale;
    in.readings[1].reason = say("truth.stale", {{"ageS", 90}, {"limitS", 60}});
    CHECK(item(evaluate(in), "probe.ec").status == "problem");
  }
  {
    auto in = f.input(6.0, 1.6);
    in.readings[0].quality = Quality::Uncalibrated;
    CHECK(item(evaluate(in), "probe.ph").status == "problem");
  }
  f.cfg.active = {};
  auto in = f.input(6.0, 1.6);
  in.readings[0].quality = Quality::Uncalibrated;
  CHECK(item(evaluate(in), "probe.ph").status == "neutral");
}

TEST_CASE("Watchdog: neutral with a reason during the start, while stopped and without a recipe (RAT-005, RAT-073)") {
  Fix f;
  {
    auto in = f.input(7.5, 0.5);
    in.bootEpoch = in.epoch - 60;
    CHECK(item(evaluate(in), "target.ph").status == "neutral");
  }
  auto in = f.input(7.5, 0.5);
  in.stopped = true;
  auto w = evaluate(in);
  CHECK(item(w, "target.ph").status == "neutral");
  CHECK(item(w, "stop").status == "neutral");
  f.cfg.active = {};
  CHECK(item(evaluate(f.input(7.5, 0.5)), "target.ph").status == "neutral");
}

TEST_CASE("Watchdog: no flow is a problem only while the cycle needs it") {
  Fix f;
  auto in = f.input(6.0, 1.6);
  in.readings[3].value = 0.0;
  CHECK(item(evaluate(in), "flow").status == "problem");
  in.needsFlow = false;
  CHECK(item(evaluate(in), "flow").status == "neutral");
}

TEST_CASE("Watchdog: latches, an ended recipe and an uncalibrated pump are problems; the headline names the first and counts the rest (RAT-015, RAT-072)") {
  Fix f;
  auto w = evaluate(f.input(6.0, 1.6));
  CHECK(w.overall == "ok");
  CHECK(w.headline.key == "watch.headline_ok");
  f.rt.latches["ec.no_effect"] = {{"at", 4000}};
  f.cfg.pumps.erase(2);
  w = evaluate(f.input(6.0, 1.6));
  CHECK(w.overall == "problem");
  CHECK(w.problems == 2);
  CHECK(item(w, "latch.ec.no_effect").text.key == "latch.ec.no_effect");
  CHECK(item(w, "canister.a").text.key == "watch.pump_uncalibrated");
  CHECK(w.headline.key == "watch.headline_more");
  f.cfg.active.until = 4000;
  CHECK(item(evaluate(f.input(6.0, 1.6)), "recipe").status == "problem");
}
