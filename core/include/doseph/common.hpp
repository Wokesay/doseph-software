// SPDX-License-Identifier: AGPL-3.0-or-later
// Common types of the core. The core is platform-neutral: no ESP-IDF, POSIX
// or network headers in core/ (tools/arch_check.sh checks this).
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace doseph {

using json = nlohmann::json;

// Monotonic time in milliseconds (device: since start; simulator: virtual time).
using Ms = std::int64_t;
// Wall clock in seconds since 1970 (history and events).
using Epoch = std::int64_t;

constexpr Ms kSecond = 1000;
constexpr Ms kMinute = 60 * kSecond;
constexpr Ms kHour = 60 * kMinute;

// A missing value is never 0 (RAT-006). Missing numbers are std::nullopt or
// NaN and appear in JSON as null.
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// The ideal slope of a pH electrode at 25 °C (Nernst, RT·ln10/F), in mV per
// pH step. The raw pH scale is the pH an ideal probe would read, 7 − E/59.16,
// so RAT-025's slope band 0.8–1.25 is 47.3–74.0 mV/pH.
inline constexpr double kNernstMvPerPh = 59.16;
inline bool isNum(double v) { return std::isfinite(v); }

// The clock is injected (rule R8): deterministic tests, time-lapse in the simulator.
class IClock {
 public:
  virtual ~IClock() = default;
  virtual Ms nowMs() const = 0;
  virtual Epoch epoch() const = 0;
  // The wall clock was set from a trusted source since the start and has
  // not been interrupted since: network time, later a buffered clock.
  virtual bool secured() const = 0;
};

// A plausible wall time: not before this software existed (2026-01-01),
// not absurdly far ahead. A saved time outside is unknown (R5).
inline constexpr Epoch kNotBefore = 1767225600;
inline constexpr Epoch kNotAfter = kNotBefore + 50LL * 365 * 86400;
inline bool plausibleEpoch(Epoch e) { return e >= kNotBefore && e <= kNotAfter; }

// A moment as the device records it: wall time, whether that wall time was
// secured, the operating time in which every outage counts as 0, and the
// start (boot) it belongs to.
struct Stamp {
  Epoch at = 0;
  bool secured = false;
  std::int64_t operatingS = 0;
  std::uint32_t boot = 0;
};
void to_json(json& j, const Stamp& s);
// nullopt if missing, malformed or implausible: an unknown moment is
// never 0 (R5).
std::optional<Stamp> stampFromJson(const json& j);
// Seconds from `from` to `to`, never negative. Within one start: operating
// time, which no clock step can stretch. Across a restart: the wall time
// if both moments were secured (it counts the outage), never less than
// the operating time; otherwise operating time.
std::int64_t elapsedS(const Stamp& from, const Stamp& to);

// A text the device sends: a stable key, its values and the English text.
// The web app shows German from its own table by the key.
struct Msg {
  std::string key;
  std::string text;
  json args = json::object();
};
void to_json(json& j, const Msg& m);

// JSON helpers: a number or null, never 0 as a stand-in.
inline json numOrNull(double v) { return isNum(v) ? json(v) : json(nullptr); }
inline json numOrNull(const std::optional<double>& v) {
  return v && isNum(*v) ? json(*v) : json(nullptr);
}
double jnum(const json& j, const char* key, double fallback = kNaN);
std::string jstr(const json& j, const char* key, const std::string& fallback = "");
bool jbool(const json& j, const char* key, bool fallback);
// At most maxBytes bytes, without cutting a UTF-8 character.
std::string utf8Prefix(const std::string& s, size_t maxBytes);
// A wall time as "YYYY-MM-DD hh:mm:ss" in UTC, as spreadsheets read it.
std::string utcText(Epoch e);

}  // namespace doseph
