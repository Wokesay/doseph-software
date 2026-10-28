// SPDX-License-Identifier: AGPL-3.0-or-later
// Local time in the device's time zone, from the rules of the IANA time zone
// database (core/src/tzdb.inc, tools/gen_tzdb.mjs). Pure arithmetic on a
// given epoch: the core still has no wall clock of its own (R8). Quiet hours,
// a morning report and maintenance dates are told in local time.
#pragma once

#include <optional>
#include <string>

#include "doseph/common.hpp"

namespace doseph {

// A POSIX TZ rule, such as "CET-1CEST,M3.5.0,M10.5.0/3". Offsets are east of
// UTC, in seconds; a transition is a day of the year and a local time.
struct TzRule {
  struct When {
    char kind = 'M';     // 'M' month.week.weekday, 'J' day 1–365 without 29 Feb, 'D' day 0–365
    int month = 0, week = 0, weekday = 0, day = 0;
    long timeS = 7200;   // local time of the change, may be negative or past 24 h
  };
  long stdOffsetS = 0;
  bool dst = false;
  long dstOffsetS = 0;
  When start, end;       // into and out of daylight saving time
};

std::optional<TzRule> parseTzRule(const std::string& rule);
// The rule of an IANA zone name; nullopt for a name the database does not have
std::optional<TzRule> zoneRule(const std::string& zone);

// Seconds east of UTC at an instant
long utcOffsetAt(const TzRule& r, Epoch utc);

struct LocalTime {
  int year = 1970, month = 1, day = 1;
  int weekday = 4;      // 0 Sunday … 6 Saturday
  int minuteOfDay = 0;  // 0 … 1439
  long offsetS = 0;     // east of UTC
  bool known = true;    // false: the zone is not in the database; UTC is used
};

LocalTime localTime(Epoch utc, const std::string& zone);

// The next instant at or after `from` that is the given local minute of the day
Epoch nextLocalMinute(Epoch from, const std::string& zone, int minuteOfDay);

}  // namespace doseph
