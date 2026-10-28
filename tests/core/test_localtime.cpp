// SPDX-License-Identifier: AGPL-3.0-or-later
// Local time from the time zone database's rules.
#include <doctest/doctest.h>

#include <string>

#include "doseph/localtime.hpp"

using namespace doseph;

namespace {

#include "../../core/src/tzdb.inc"

long offsetAt(const char* zone, Epoch t) { return localTime(t, zone).offsetS; }

}  // namespace

TEST_CASE("Time zone rules: names in letters or <…>, offsets west of UTC in hours and minutes, transitions by month, week and weekday or by day, with times past 24 h or below zero") {
  auto berlin = parseTzRule("CET-1CEST,M3.5.0,M10.5.0/3");
  REQUIRE(berlin);
  CHECK(berlin->stdOffsetS == 3600);
  CHECK(berlin->dstOffsetS == 7200);
  CHECK(berlin->start.month == 3);
  CHECK(berlin->start.week == 5);
  CHECK(berlin->start.timeS == 7200);
  CHECK(berlin->end.timeS == 3 * 3600);
  auto tehran = parseTzRule("<+0330>-3:30");
  REQUIRE(tehran);
  CHECK(tehran->stdOffsetS == 3 * 3600 + 1800);
  CHECK_FALSE(tehran->dst);
  auto nuuk = parseTzRule("<-02>2<-01>,M3.5.0/-1,M10.5.0/0");
  REQUIRE(nuuk);
  CHECK(nuuk->start.timeS == -3600);
  CHECK(nuuk->dstOffsetS == -3600);
  CHECK(parseTzRule("IST-2IDT,M3.4.4/26,M10.5.0")->start.timeS == 26 * 3600);
  CHECK(parseTzRule("<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45")->end.timeS == 3 * 3600 + 45 * 60);
  CHECK(parseTzRule("XXX3YYY,J60/1,300")->start.kind == 'J');
  for (const char* bad : {"", "C-1", "CET", "CET-1CEST,M13.1.0,M10.5.0", "CET-1CEST,M3.5.0", "CET-1CEST,M3.5.0,M10.5.0/x", "CET-1CEST,M3.6.0,M10.5.0"})
    CHECK_FALSE(parseTzRule(bad));
}

TEST_CASE("Local time: the offset changes at the transitions of each zone, north and south of the equator, and a zone the database lacks is UTC") {
  // Berlin, 2026: summer time from 29 Mar 01:00 UTC to 25 Oct 01:00 UTC
  CHECK(offsetAt("Europe/Berlin", 1774746000 - 1) == 3600);
  CHECK(offsetAt("Europe/Berlin", 1774746000) == 7200);
  CHECK(offsetAt("Europe/Berlin", 1792890000 - 1) == 7200);
  CHECK(offsetAt("Europe/Berlin", 1792890000) == 3600);
  // New York, 2026: 8 Mar 07:00 UTC to 1 Nov 06:00 UTC
  CHECK(offsetAt("America/New_York", 1772953200 - 1) == -5 * 3600);
  CHECK(offsetAt("America/New_York", 1772953200) == -4 * 3600);
  CHECK(offsetAt("America/New_York", 1793512800 - 1) == -4 * 3600);
  CHECK(offsetAt("America/New_York", 1793512800) == -5 * 3600);
  // Sydney: summer time spans the new year; it ends 4 Apr 16:00 UTC and starts 3 Oct 16:00 UTC 2026
  CHECK(offsetAt("Australia/Sydney", 1767225600) == 11 * 3600);
  CHECK(offsetAt("Australia/Sydney", 1775318400 - 1) == 11 * 3600);
  CHECK(offsetAt("Australia/Sydney", 1775318400) == 10 * 3600);
  CHECK(offsetAt("Australia/Sydney", 1791043200 - 1) == 10 * 3600);
  CHECK(offsetAt("Australia/Sydney", 1791043200) == 11 * 3600);
  // Transitions at negative and late times: Nuuk at −1:00, Jerusalem at 26:00
  CHECK(offsetAt("America/Nuuk", 1774746000 - 1) == -7200);
  CHECK(offsetAt("America/Nuuk", 1774746000) == -3600);
  CHECK(offsetAt("Asia/Jerusalem", 1774569600 - 1) == 7200);
  CHECK(offsetAt("Asia/Jerusalem", 1774569600) == 10800);
  CHECK(offsetAt("Asia/Kolkata", 1774569600) == 5 * 3600 + 1800);
  CHECK(offsetAt("UTC", 1774569600) == 0);
  // Past 2038: the arithmetic is 64-bit on every platform (25 Mar 2040, 01:00 UTC)
  CHECK(offsetAt("Europe/Berlin", 2216250000 - 1) == 3600);
  CHECK(offsetAt("Europe/Berlin", 2216250000) == 7200);
  CHECK(localTime(2216250000, "Europe/Berlin").year == 2040);
  const LocalTime none = localTime(1774569600, "Mars/Olympus_Mons");
  CHECK_FALSE(none.known);
  CHECK(none.offsetS == 0);
}

TEST_CASE("Local time: date, weekday and minute of the day; the next local minute, across a change of time") {
  // 2027-01-01 00:00 UTC is a Friday, 01:00 in Berlin
  const LocalTime t = localTime(1798761600, "Europe/Berlin");
  CHECK(t.year == 2027);
  CHECK(t.month == 1);
  CHECK(t.day == 1);
  CHECK(t.weekday == 5);
  CHECK(t.minuteOfDay == 60);
  const LocalTime ny = localTime(1798761600, "America/New_York");  // still 31 Dec, 19:00
  CHECK(ny.year == 2026);
  CHECK(ny.day == 31);
  CHECK(ny.weekday == 4);
  CHECK(ny.minuteOfDay == 19 * 60);
  // 07:00 in Berlin is 06:00 UTC in winter; asked at 07:00 it is that very minute
  CHECK(nextLocalMinute(1798761600, "Europe/Berlin", 7 * 60) == 1798761600 + 6 * 3600);
  CHECK(nextLocalMinute(1798761600 + 6 * 3600, "Europe/Berlin", 7 * 60) == 1798761600 + 6 * 3600);
  CHECK(nextLocalMinute(1798761600 + 6 * 3600 + 1, "Europe/Berlin", 7 * 60) == 1798761600 + 30 * 3600);
  // Over the night summer time starts (29 Mar 2026): 07:00 is then 05:00 UTC
  const Epoch evening = 1774746000 - 6 * 3600;  // 28 Mar, 20:00 in Berlin
  CHECK(nextLocalMinute(evening, "Europe/Berlin", 7 * 60) == 1774746000 + 4 * 3600);
}

TEST_CASE("Time zone database: every zone's rule parses and is found by its name, in strcmp order; old names such as Asia/Calcutta are there too") {
  const size_t n = sizeof(kZones) / sizeof(kZones[0]);
  REQUIRE(n > 400);
  for (size_t i = 0; i < n; ++i) {
    CAPTURE(kZones[i].zone);
    CHECK(parseTzRule(kZones[i].rule));
    CHECK(zoneRule(kZones[i].zone));
    if (i > 0) CHECK(std::string(kZones[i - 1].zone) < std::string(kZones[i].zone));
  }
  CHECK(offsetAt("Asia/Calcutta", 1774569600) == 5 * 3600 + 1800);
  CHECK(offsetAt("Europe/Kiev", 1774569600) == offsetAt("Europe/Kyiv", 1774569600));
}
