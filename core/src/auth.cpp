// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/auth.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include "doseph/messages.hpp"
#include "doseph/sha256.hpp"

namespace doseph {

namespace {
// Compares in constant time: does not tell where the hash differs.
bool sameHash(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  return diff == 0;
}
}  // namespace

Msg Auth::checkStrength(const std::string& pw) {
  if (pw.size() < 8) return say("auth.too_short");
  if (pw.size() > 128) return say("auth.too_long");
  return {};
}

std::string Auth::hashOf(const std::string& pw, const std::string& saltHex, std::uint32_t it) const {
  auto dk = pbkdf2Sha256(pw, fromHex(saltHex), it, 32);
  return toHex(dk.data(), dk.size());
}

// 16 characters of 32 without 0, 1, I and O (80 bits), shown in groups of four.
std::string Auth::armSetupCode(Ms now, Ms validFor) {
  if (hasPassword()) return {};
  static constexpr char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  std::uint8_t r[16];
  rng_(r, sizeof r);
  setupCode_.clear();
  for (std::uint8_t b : r) setupCode_ += kAlphabet[b & 31];  // 256 is a multiple of 32: no bias
  setupCodeUntil_ = validFor > 0 ? now + validFor : 0;
  std::string shown;
  for (size_t i = 0; i < setupCode_.size(); ++i) shown += (i > 0 && i % 4 == 0 ? "-" : "") + setupCode_.substr(i, 1);
  return shown;
}

bool Auth::logWrongSetupCode(Ms now) {
  if (wrongLogged_ && now - *wrongLogged_ < 15 * kMinute) return false;
  wrongLogged_ = now;
  return true;
}

Msg Auth::setInitialPassword(const std::string& pw, const std::string& code, Ms now) {
  if (hasPassword()) return say("auth.exists");
  if (setupCodeRequired()) {  // before the password's hash: a wrong code costs nothing
    std::string typed;  // as it comes from the log: any case, dashes or white space between the groups
    for (char ch : code)
      if (ch != '-' && !std::isspace(static_cast<unsigned char>(ch))) typed += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (!sameHash(typed, setupCode_) || (setupCodeUntil_ > 0 && now >= setupCodeUntil_)) {
      wrongCodes_++;
      return say("auth.code");
    }
  }
  Msg m = checkStrength(pw);
  if (!m.key.empty()) return m;
  std::uint8_t salt[16];
  rng_(salt, sizeof salt);
  salt_ = toHex(salt, sizeof salt);
  iterations_ = kIterations;
  hash_ = hashOf(pw, salt_, iterations_);
  setupCode_.clear();
  return {};
}

Msg Auth::changePassword(const std::string& oldPw, const std::string& newPw, Ms now, const std::string& client) {
  Attempts& a = attemptsOf(client);
  Msg err;
  if (locked(a, now, err)) return err;  // before hashing
  if (!hasPassword() || !sameHash(hashOf(oldPw, salt_, iterations_), hash_)) {
    failed(a, now);  // a stolen session must not guess the password freely
    return say("auth.wrong_old");
  }
  Msg m = checkStrength(newPw);
  if (!m.key.empty()) return m;
  std::uint8_t salt[16];
  rng_(salt, sizeof salt);
  salt_ = toHex(salt, sizeof salt);
  hash_ = hashOf(newPw, salt_, iterations_);
  sessions_.clear();  // every other session ends
  known_.clear();     // every other client signs in anew before it has a count of its own;
  remember(client, now);  // the caller just proved the old password
  return {};
}

Auth::Attempts& Auth::attemptsOf(const std::string& client) {
  auto it = client.empty() ? known_.end() : known_.find(client);
  return it == known_.end() ? shared_ : it->second.tries;
}

bool Auth::locked(const Attempts& a, Ms now, Msg& err) {
  if (now >= a.lockedUntil) return false;
  err = say("auth.locked", {{"waitS", (a.lockedUntil - now) / 1000 + 1}});
  return true;
}

void Auth::failed(Attempts& a, Ms now) {
  if (a.failures > 0 && now - a.lastFailure > kForgetFailures) a = {};  // a quiet day forgets the count
  a.failures++;
  a.lastFailure = now;
  // 30 s, then doubled up to 15 min (EN 18031-1 AUM-6)
  if (a.failures >= 5) a.lockedUntil = now + std::min<Ms>(30 * kSecond << std::min(a.failures - 5, 5), 15 * kMinute);
}

// A success gives the client a count of its own, fresh; the shared count
// stays, so a success from a new address does not lift a lockout for others.
void Auth::remember(const std::string& client, Ms now) {
  if (client.empty()) return;
  if (!known_.count(client) && known_.size() >= kKnownClients)
    known_.erase(std::min_element(known_.begin(), known_.end(),
                                  [](const auto& x, const auto& y) { return x.second.lastSeen < y.second.lastSeen; }));
  known_[client] = Known{{}, now};
}

std::optional<std::string> Auth::login(const std::string& pw, Ms now, Msg& err, const std::string& client) {
  if (!hasPassword()) {
    err = say("auth.no_password");
    return std::nullopt;
  }
  Attempts& a = attemptsOf(client);
  if (locked(a, now, err)) return std::nullopt;  // before hashing
  if (!sameHash(hashOf(pw, salt_, iterations_), hash_)) {
    failed(a, now);
    err = say("auth.wrong");
    return std::nullopt;
  }
  remember(client, now);
  std::uint8_t tok[24];
  rng_(tok, sizeof tok);
  std::string t = toHex(tok, sizeof tok);
  // Drop expired sessions; at most 16 at a time: a 17th ends the one used
  // longest ago (map order is the random token's, not age).
  for (auto it = sessions_.begin(); it != sessions_.end();)
    it = (now - it->second > kSessionIdle) ? sessions_.erase(it) : std::next(it);
  if (sessions_.size() >= kMaxSessions)
    sessions_.erase(std::min_element(sessions_.begin(), sessions_.end(),
                                     [](const auto& x, const auto& y) { return x.second < y.second; }));
  sessions_[t] = now;
  return t;
}

void Auth::seen(const std::string& client, Ms now) {
  auto it = client.empty() ? known_.end() : known_.find(client);
  if (it != known_.end()) it->second.lastSeen = now;
}

bool Auth::check(const std::string& token, Ms now) {
  auto it = sessions_.find(token);
  if (it == sessions_.end()) return false;
  if (now - it->second > kSessionIdle) {
    sessions_.erase(it);
    return false;
  }
  it->second = now;
  return true;
}

json Auth::toJson() const { return {{"salt", salt_}, {"hash", hash_}, {"iterations", iterations_}}; }

void Auth::load(const json& j) {
  salt_ = jstr(j, "salt");
  hash_ = jstr(j, "hash");
  double it = jnum(j, "iterations", kIterations);
  iterations_ = isNum(it) && it >= 1000 && it <= 1e7 ? static_cast<std::uint32_t>(it) : kIterations;
}

namespace {
// a.b.c.d, each 0–255 in 1–3 digits
bool parseIpv4(const std::string& s, std::uint8_t out[4]) {
  size_t pos = 0;
  for (int i = 0; i < 4; ++i) {
    if (i > 0) {
      if (pos >= s.size() || s[pos] != '.') return false;
      ++pos;
    }
    size_t start = pos;
    unsigned v = 0;
    while (pos < s.size() && pos - start < 3 && s[pos] >= '0' && s[pos] <= '9') v = v * 10 + static_cast<unsigned>(s[pos++] - '0');
    if (pos == start || v > 255) return false;
    out[i] = static_cast<std::uint8_t>(v);
  }
  return pos == s.size();
}

// Groups of 1–4 hex digits separated by ':'; the last one may be a.b.c.d
// only at the end of the whole address
bool parseGroups(const std::string& s, std::vector<std::uint16_t>& out, bool lastMayBeIpv4) {
  if (s.empty()) return true;
  size_t pos = 0;
  while (true) {
    size_t end = s.find(':', pos);
    std::string g = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (end == std::string::npos && lastMayBeIpv4 && g.find('.') != std::string::npos) {
      std::uint8_t b[4];
      if (!parseIpv4(g, b)) return false;
      out.push_back(static_cast<std::uint16_t>(b[0] << 8 | b[1]));
      out.push_back(static_cast<std::uint16_t>(b[2] << 8 | b[3]));
      return true;
    }
    if (g.empty() || g.size() > 4) return false;
    unsigned v = 0;
    for (char c : g) {
      const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      v = v * 16 + static_cast<unsigned>(d);
    }
    out.push_back(static_cast<std::uint16_t>(v));
    if (end == std::string::npos) return true;
    pos = end + 1;
  }
}

bool parseIpv6(const std::string& s, std::uint16_t out[8]) {
  const size_t gap = s.find("::");
  std::vector<std::uint16_t> head, tail;
  if (gap == std::string::npos) {
    if (!parseGroups(s, head, true) || head.size() != 8) return false;
  } else {
    if (s.find("::", gap + 1) != std::string::npos) return false;
    if (!parseGroups(s.substr(0, gap), head, false) || !parseGroups(s.substr(gap + 2), tail, true)) return false;
    if (head.size() + tail.size() > 7) return false;
  }
  std::fill(out, out + 8, 0);
  std::copy(head.begin(), head.end(), out);
  std::copy(tail.begin(), tail.end(), out + 8 - tail.size());
  return true;
}
}  // namespace

std::string clientKey(const std::string& address) {
  if (address.size() > 64) return "";
  std::string a = address.substr(0, address.find('%'));  // the zone names the interface, not the client
  std::uint8_t b[4];
  auto v4 = [](const std::uint8_t* x) {
    return std::to_string(x[0]) + "." + std::to_string(x[1]) + "." + std::to_string(x[2]) + "." + std::to_string(x[3]);
  };
  if (parseIpv4(a, b)) return v4(b);
  std::uint16_t g[8];
  if (!parseIpv6(a, g)) return "";
  if (g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 && g[5] == 0xffff) {
    const std::uint8_t m[4] = {static_cast<std::uint8_t>(g[6] >> 8), static_cast<std::uint8_t>(g[6] & 0xff), static_cast<std::uint8_t>(g[7] >> 8),
                               static_cast<std::uint8_t>(g[7] & 0xff)};
    return v4(m);
  }
  std::string out;
  for (int i = 0; i < 8; ++i) {
    char hex[5];
    std::snprintf(hex, sizeof hex, "%x", g[i]);
    out += (i ? ":" : "") + std::string(hex);
  }
  return out;
}

}  // namespace doseph
