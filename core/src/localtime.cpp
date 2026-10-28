// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/localtime.hpp"

#include <cctype>
#include <cstring>

namespace doseph {

namespace {

#include "tzdb.inc"

// Days since 1970-01-01 of a civil date, and back (proleptic Gregorian)
Epoch daysFromCivil(Epoch y, int m, int d) {
  y -= m <= 2;
  const Epoch era = (y >= 0 ? y : y - 399) / 400;
  const Epoch yoe = y - era * 400;
  const Epoch doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const Epoch doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(Epoch z, int& y, int& m, int& d) {
  z += 719468;
  const Epoch era = (z >= 0 ? z : z - 146096) / 146097;
  const Epoch doe = z - era * 146097;
  const Epoch yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const Epoch doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const Epoch mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

Epoch floorDiv(Epoch a, Epoch b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int weekdayOf(Epoch days) { return static_cast<int>(((days % 7) + 11) % 7); }  // 1970-01-01 was a Thursday

bool leap(Epoch y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// The day (since 1970) a transition falls on in a year
Epoch dayOf(Epoch y, const TzRule::When& w) {
  if (w.kind == 'J') return daysFromCivil(y, 1, 1) + w.day - 1 + (leap(y) && w.day >= 60 ? 1 : 0);
  if (w.kind == 'D') return daysFromCivil(y, 1, 1) + w.day;
  const Epoch first = daysFromCivil(y, w.month, 1);
  Epoch d = first + (w.weekday - weekdayOf(first) + 7) % 7 + (w.week - 1) * 7;
  const Epoch next = w.month == 12 ? daysFromCivil(y + 1, 1, 1) : daysFromCivil(y, w.month + 1, 1);
  while (d >= next) d -= 7;  // week 5 is the last of the month
  return d;
}

class Reader {
 public:
  explicit Reader(const std::string& s) : s_(s) {}
  bool done() const { return i_ >= s_.size(); }
  char peek() const { return done() ? '\0' : s_[i_]; }
  bool eat(char c) {
    if (peek() != c) return false;
    ++i_;
    return true;
  }
  // A zone abbreviation: three or more letters, or anything in <…>
  bool name() {
    if (eat('<')) {
      const size_t end = s_.find('>', i_);
      if (end == std::string::npos || end - i_ < 3) return false;
      i_ = end + 1;
      return true;
    }
    size_t n = 0;
    while (std::isalpha(static_cast<unsigned char>(peek()))) ++i_, ++n;
    return n >= 3;
  }
  bool number(long& v, long max) {
    if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
    v = 0;
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
      v = v * 10 + (s_[i_++] - '0');
      if (v > max) return false;
    }
    return true;
  }
  // [+|-]hh[:mm[:ss]] in seconds
  bool time(long& s, long maxHours) {
    const bool neg = eat('-');
    if (!neg) eat('+');
    long h = 0, m = 0, sec = 0;
    if (!number(h, maxHours)) return false;
    if (eat(':') && (!number(m, 59) || (eat(':') && !number(sec, 59)))) return false;
    s = (h * 3600 + m * 60 + sec) * (neg ? -1 : 1);
    return true;
  }
  bool when(TzRule::When& w) {
    long v = 0;
    if (eat('M')) {
      long week = 0, wd = 0;
      if (!number(v, 12) || v < 1 || !eat('.') || !number(week, 5) || week < 1 || !eat('.') || !number(wd, 6)) return false;
      w = {'M', static_cast<int>(v), static_cast<int>(week), static_cast<int>(wd), 0, 7200};
    } else if (eat('J')) {
      if (!number(v, 365) || v < 1) return false;
      w = {'J', 0, 0, 0, static_cast<int>(v), 7200};
    } else {
      if (!number(v, 365)) return false;
      w = {'D', 0, 0, 0, static_cast<int>(v), 7200};
    }
    return !eat('/') || time(w.timeS, 167);
  }

 private:
  const std::string& s_;
  size_t i_ = 0;
};

}  // namespace

std::optional<TzRule> parseTzRule(const std::string& rule) {
  Reader r(rule);
  TzRule t;
  long off = 0;
  if (!r.name() || !r.time(off, 24)) return std::nullopt;
  t.stdOffsetS = -off;  // POSIX counts west of UTC
  if (r.done()) return t;
  if (!r.name()) return std::nullopt;
  t.dst = true;
  t.dstOffsetS = t.stdOffsetS + 3600;
  const char c = r.peek();
  if (c == '+' || c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
    if (!r.time(off, 24)) return std::nullopt;
    t.dstOffsetS = -off;
  }
  if (r.done()) {
    // No rules given: those of the United States, as many systems assume
    t.start = {'M', 3, 2, 0, 0, 7200};
    t.end = {'M', 11, 1, 0, 0, 7200};
    return t;
  }
  if (!r.eat(',') || !r.when(t.start) || !r.eat(',') || !r.when(t.end) || !r.done()) return std::nullopt;
  return t;
}

std::optional<TzRule> zoneRule(const std::string& zone) {
  size_t lo = 0, hi = sizeof(kZones) / sizeof(kZones[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = std::strcmp(kZones[mid].zone, zone.c_str());
    if (c == 0) return parseTzRule(kZones[mid].rule);
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return std::nullopt;
}

long utcOffsetAt(const TzRule& r, Epoch utc) {
  if (!r.dst) return r.stdOffsetS;
  int y = 0, m = 0, d = 0;
  civilFromDays(floorDiv(utc + r.stdOffsetS, 86400), y, m, d);
  // Into daylight saving time at the local standard time, out of it at the local daylight time
  const Epoch start = dayOf(y, r.start) * 86400 + r.start.timeS - r.stdOffsetS;
  const Epoch end = dayOf(y, r.end) * 86400 + r.end.timeS - r.dstOffsetS;
  const bool inDst = start < end ? utc >= start && utc < end : utc < end || utc >= start;  // south of the equator it spans the new year
  return inDst ? r.dstOffsetS : r.stdOffsetS;
}

LocalTime localTime(Epoch utc, const std::string& zone) {
  LocalTime lt;
  const auto rule = zoneRule(zone);
  lt.known = rule.has_value();
  lt.offsetS = rule ? utcOffsetAt(*rule, utc) : 0;
  const Epoch local = utc + lt.offsetS;
  const Epoch days = floorDiv(local, 86400);
  civilFromDays(days, lt.year, lt.month, lt.day);
  lt.weekday = weekdayOf(days);
  lt.minuteOfDay = static_cast<int>((local - days * 86400) / 60);
  return lt;
}

Epoch nextLocalMinute(Epoch from, const std::string& zone, int minuteOfDay) {
  const LocalTime now = localTime(from, zone);
  Epoch day = daysFromCivil(now.year, now.month, now.day);
  for (int i = 0; i < 3; ++i, ++day) {
    const Epoch local = day * 86400 + static_cast<Epoch>(minuteOfDay) * 60;
    // The offset at that moment, which a change of time in between may have moved
    const Epoch t = local - localTime(local - now.offsetS, zone).offsetS;
    if (t >= from) return t;
  }
  return from + 86400;
}

}  // namespace doseph
