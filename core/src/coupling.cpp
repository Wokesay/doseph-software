// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/coupling.hpp"

#include <algorithm>
#include <cctype>

namespace doseph {

namespace {

bool hex(const std::string& s, size_t n) {
  return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

bool peerAddress(const std::string& s) {
  if (s.empty() || s.size() > 260) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']'; });
}

json peerToJson(const Peer& p, bool withSecret) {
  json j = {{"role", p.role}, {"id", p.id}, {"name", p.name}, {"address", p.address}, {"since", p.since}};
  if (withSecret) {
    j["token"] = p.token;
    j["tokenId"] = p.tokenId;
  }
  return j;
}

std::optional<Peer> peerFromJson(const json& j) {
  if (!j.is_object()) return std::nullopt;
  Peer p;
  p.role = jstr(j, "role");
  p.id = jstr(j, "id");
  p.name = jstr(j, "name");
  p.address = jstr(j, "address");
  p.token = jstr(j, "token");
  p.tokenId = jstr(j, "tokenId");
  p.since = j.contains("since") && j["since"].is_number_integer() ? j["since"].get<Epoch>() : 0;
  const bool ok = (p.role == "leader" || p.role == "follower") && hex(p.id, 16) && !p.name.empty() && p.name.size() <= 40 &&
                  p.token.rfind("dpt_", 0) == 0 && hex(p.token.substr(4), 64) && !p.tokenId.empty() &&
                  (p.role == "follower" ? p.address.empty() : peerAddress(p.address));
  return ok ? std::optional<Peer>(p) : std::nullopt;
}

}  // namespace doseph
