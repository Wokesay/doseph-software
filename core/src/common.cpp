// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/common.hpp"

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

std::string jstr(const json& j, const char* key, const std::string& fallback) {
  if (!j.is_object()) return fallback;
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return fallback;
  return it->get<std::string>();
}

}  // namespace doseph
