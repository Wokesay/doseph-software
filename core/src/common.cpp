// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/common.hpp"

#include <cstdio>

namespace doseph {

void to_json(json& j, const Msg& m) {
  j = json{{"key", m.key}, {"text", m.text}};
  if (!m.args.empty()) j["args"] = m.args;
}

double jnum(const json& j, const char* key, double fallback) {
  if (!j.is_object()) return fallback;
  auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return fallback;
  return it->get<double>();
}

bool jbool(const json& j, const char* key, bool fallback) {
  if (!j.is_object()) return fallback;
  auto it = j.find(key);
  if (it == j.end() || !it->is_boolean()) return fallback;
  return it->get<bool>();
}

std::string utf8Prefix(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t n = maxBytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // a continuation byte: back to the character's start
  return s.substr(0, n);
}

std::string utcText(Epoch e) {
  // Days since 1970 to a civil date (proleptic Gregorian), without the C library's time zone state
  Epoch days = e >= 0 ? e / 86400 : (e - 86399) / 86400;
  const Epoch secs = e - days * 86400;
  days += 719468;
  const Epoch era = (days >= 0 ? days : days - 146096) / 146097;
  const Epoch doe = days - era * 146097;
  const Epoch yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const Epoch doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const Epoch mp = (5 * doy + 2) / 153;
  const Epoch d = doy - (153 * mp + 2) / 5 + 1;
  const Epoch m = mp < 10 ? mp + 3 : mp - 9;
  const Epoch y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  char buf[128];  // room for any 64-bit year, so the compiler sees no truncation
  std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld", static_cast<long long>(y), static_cast<long long>(m),
                static_cast<long long>(d), static_cast<long long>(secs / 3600), static_cast<long long>(secs / 60 % 60),
                static_cast<long long>(secs % 60));
  return buf;
}

std::string jstr(const json& j, const char* key, const std::string& fallback) {
  if (!j.is_object()) return fallback;
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return fallback;
  return it->get<std::string>();
}

}  // namespace doseph
