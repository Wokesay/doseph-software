// SPDX-License-Identifier: AGPL-3.0-or-later
// Maintenance dates: the items a configuration has, their intervals, the
// bundling into dates, and the notes they become (SD-125).
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "doseph/localtime.hpp"
#include "doseph/service.hpp"
#include "simulation.hpp"

using namespace doseph;

namespace {

constexpr Epoch kT0 = 1798761600;  // 2027-01-01 00:00 UTC, 01:00 in Berlin
constexpr Epoch kDay = 86400;

Config withChannels() {
  Config c;
  c.canisters = {{"a", "Part A", "nutrient", 1, "AB", "#00c"}, {"down", "pH−", "ph_down", 4, "", "#c00"}};
  return c;
}

const ServiceItem& item(const ServicePlan& p, const std::string& id) {
  auto it = std::find_if(p.items.begin(), p.items.end(), [&](const ServiceItem& i) { return i.id == id; });
  REQUIRE(it != p.items.end());
  return *it;
}

}  // namespace

TEST_CASE("Maintenance: the items a configuration has; a new one counts from when it is first seen, one no longer there is forgotten (SD-125)") {
  Config bare;
  std::vector<std::string> ids;
  for (const auto& i : serviceItems(bare)) ids.push_back(i.id);
  CHECK(ids == std::vector<std::string>{"probe.ph.cal", "probe.ec.cal", "chamber.clean", "probe.ph.replace"});
  Config c = withChannels();
  ids.clear();
  for (const auto& i : serviceItems(c)) ids.push_back(i.id);
  CHECK(ids == std::vector<std::string>{"probe.ph.cal", "probe.ec.cal", "channel.1.cal", "channel.4.cal", "chamber.clean", "heads.check", "probe.ph.replace"});
  RuntimeState rt;
  CHECK(noteService(c, rt, kT0));
  CHECK_FALSE(noteService(c, rt, kT0 + kDay));  // nothing new
  CHECK(rt.serviceSince.at("channel.1.cal") == kT0);
  c.canisters.pop_back();
  CHECK(noteService(c, rt, kT0 + 2 * kDay));
  CHECK_FALSE(rt.serviceSince.count("channel.4.cal"));
}

TEST_CASE("Maintenance: each item is due its interval after it was last done or calibrated; items due within a week of the earliest join its date, the local midnight of that day (SD-125)") {
  Config c = withChannels();
  RuntimeState rt;
  noteService(c, rt, kT0);
  // The EC probe calibrated 7 days before: due at 83 days; the chamber cleaned 55 days in: due at 85
  c.ecCal = {{"at", kT0 - 7 * kDay}};
  rt.serviceDone["chamber.clean"] = kT0 + 55 * kDay;
  // The pH probe calibrated 20 days in: due at 50 days, then every 30 days
  c.phCal = {{"at", kT0 + 20 * kDay}};
  // Channel 1 was calibrated 5 days before: its 85 days come with the pH probe's at 80 (SD-144)
  c.channels[1] = {50.0, kT0 - 5 * kDay};
  ServicePlan p = planService(c, rt, kT0, "Europe/Berlin");
  CHECK(item(p, "probe.ph.cal").due == kT0 + 50 * kDay);
  CHECK(item(p, "channel.1.cal").due == kT0 + 80 * kDay);
  CHECK(item(p, "channel.4.cal").due == kT0 + 80 * kDay);
  CHECK(item(p, "probe.ec.cal").due == kT0 + 83 * kDay);
  CHECK(item(p, "chamber.clean").due == kT0 + 85 * kDay);
  CHECK(item(p, "heads.check").due == kT0 + 180 * kDay);
  CHECK(item(p, "probe.ph.replace").due == kT0 + 365 * kDay);
  // Dates: 50 days, 80 (with the EC probe at 83 and the chamber at 85), 180, 365; each the local midnight
  REQUIRE(p.dates.size() == 4);
  const Epoch d80 = p.dates[1];
  CHECK(item(p, "probe.ec.cal").date == d80);
  CHECK(item(p, "chamber.clean").date == d80);
  CHECK(item(p, "channel.4.cal").date == d80);
  const LocalTime lt = localTime(d80, "Europe/Berlin");
  CHECK(lt.minuteOfDay == 0);
  CHECK(localTime(item(p, "channel.1.cal").due, "Europe/Berlin").day == lt.day);
  CHECK_FALSE(item(p, "probe.ec.cal").dueNow);
  // On that date all are due, the EC probe 3 days early
  p = planService(c, rt, d80, "Europe/Berlin");
  CHECK(item(p, "channel.1.cal").dueNow);
  CHECK(item(p, "probe.ec.cal").dueNow);
  CHECK_FALSE(item(p, "heads.check").dueNow);
  // Done by hand, or calibrated: it moves on by its interval
  rt.serviceDone["chamber.clean"] = kT0 + 31 * kDay;
  c.ecCal = {{"at", kT0 + 86 * kDay}};
  p = planService(c, rt, kT0 + 86 * kDay, "Europe/Berlin");
  CHECK(item(p, "chamber.clean").due == kT0 + 61 * kDay);
  CHECK(item(p, "probe.ec.cal").due == kT0 + 176 * kDay);
}

TEST_CASE("Maintenance: an interval can be set per kind, from 7 to 730 days; a kind there is none of is refused (SD-125)") {
  Config c = withChannels();
  c.service.days["chamber.clean"] = 14;
  RuntimeState rt;
  noteService(c, rt, kT0);
  CHECK(item(planService(c, rt, kT0, "UTC"), "chamber.clean").due == kT0 + 14 * kDay);
  CHECK(validateConfig(c, 4).empty());
  c.service.days["chamber.clean"] = 3;
  CHECK_FALSE(validateConfig(c, 4).empty());
  c.service.days["chamber.clean"] = 14;
  c.service.days["window.clean"] = 30;
  CHECK_FALSE(validateConfig(c, 4).empty());
  // Kept in the configuration and in the run-time state across a save
  c.service.days.erase("window.clean");
  CHECK(configFromJson(json(c)).service.days.at("chamber.clean") == 14);
  rt.serviceDone["chamber.clean"] = kT0 + kDay;
  CHECK(runtimeFromJson(json(rt)).serviceDone.at("chamber.clean") == kT0 + kDay);
}

TEST_CASE("Maintenance over the API: the plan, an item done, the intervals; an item on its date is a note that waits for the morning report, and done it ends (SD-125, SD-123)") {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  o.startEpoch = kT0;
  sim::Simulation s(o);
  s.step(10 * kSecond);
  json plan = s.robot().service();
  REQUIRE(plan["items"].size() == 9);  // two probes, four channels, chamber, heads, a new pH probe
  CHECK(s.robot().serviceDone("no.such").status == 404);
  REQUIRE(s.robot().putService({{"days", {{"chamber.clean", 7}}}}).status == 200);
  CHECK(s.robot().putService({{"days", {{"chamber.clean", 3}}}}).status == 422);
  CHECK(s.robot().putService({{"days", {{"chamber.clean", nullptr}}}}).status == 200);
  // The chamber was last seen to 40 days ago: after a restart it is due, and after 5 min it is a note
  json st = json::parse(*s.storage().read("state.json"));
  st["serviceSince"]["chamber.clean"] = kT0 - 40 * kDay;
  s.storage().write("state.json", st.dump());
  s.control("reboot", json::object());
  s.step(6 * kMinute, 1000);
  bool noted = false;
  const json alarms = s.robot().state()["alarms"];  // a copy: the state is a temporary
  for (const auto& a : alarms)
    if (a["id"] == "maint.chamber.clean") noted = a["level"] == "info";
  CHECK(noted);
  REQUIRE(s.robot().serviceDone("chamber.clean").status == 200);
  s.step(10 * kSecond);
  const json after = s.robot().state()["alarms"];
  for (const auto& a : after) CHECK(a["id"] != "maint.chamber.clean");
}

TEST_CASE("Maintenance: a pump's calibration comes with the pH probe's: it is due with the last one of those before it; with none before it, at its own interval (SD-144)") {
  Config c = withChannels();
  RuntimeState rt;
  noteService(c, rt, kT0);
  c.phCal = {{"at", kT0 + 10 * kDay}};   // pH due at 40, 70, 100 …
  c.channels[1] = {50.0, kT0};           // due at 90 → 70
  c.channels[4] = {50.0, kT0 - 60 * kDay};  // due at 30, before the pH probe's: stays
  ServicePlan p = planService(c, rt, kT0, "UTC");
  CHECK(item(p, "channel.1.cal").due == kT0 + 70 * kDay);
  CHECK(item(p, "channel.4.cal").due == kT0 + 30 * kDay);
  // Calibrated together with the probe on its day, both are due together again
  c.phCal = {{"at", kT0 + 70 * kDay}};
  c.channels[1] = {50.0, kT0 + 70 * kDay};
  p = planService(c, rt, kT0 + 70 * kDay, "UTC");
  CHECK(item(p, "channel.1.cal").due == kT0 + 160 * kDay);
  CHECK(item(p, "channel.1.cal").date == item(p, "probe.ph.cal").date + 60 * kDay);
}
