// SPDX-License-Identifier: AGPL-3.0-or-later
// Sign-in (EN 18031-1 ACM/AUM): a mandatory password at the first start, no
// default password, no skipping; a salted PBKDF2 hash; a lockout after
// failed attempts. One sign-in model for the web app and the API.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

using RandomFn = std::function<void(std::uint8_t*, size_t)>;

// Who a request comes from (SD-129): a session signed in with the password is
// the owner; an access token has one of three fixed roles that say what it may do.
enum class Role { Owner, App, Account, Peer };
const char* roleName(Role r);
// "app", "account" or "peer"; nothing for anything else, the owner included
std::optional<Role> tokenRole(const std::string& name);

// An access token for the app, the account relay or a coupled device. The
// robot keeps its SHA-256 hash only: the token itself is shown once.
struct AccessToken {
  std::string id, name, hash;
  Role role = Role::App;
  Epoch created = 0;
  std::optional<Ms> used;  // the last use since the start, in memory only
};

class Auth {
 public:
  explicit Auth(RandomFn rng) : rng_(std::move(rng)) {}

  bool hasPassword() const { return !hash_.empty(); }
  // First password: a platform that can show a secret to its owner
  // alone (the app's log in Home Assistant) arms a one-time setup code, and
  // the first password then needs it. Kept in memory only; arming again
  // voids the one before; used up by the password; refused from `validFor`
  // after `now` on (0: no time limit). "" when a
  // password is set.
  std::string armSetupCode(Ms now, Ms validFor = 0);
  bool setupCodeRequired() const { return !setupCode_.empty() && !hasPassword(); }
  // Wrong setup codes are counted, never braked: 80 bits leave nothing to
  // guess, and a count shared by every new client would let anyone block the
  // owner's setup. Whether to log a refusal now: the first one, then
  // at most one per 15 min, so a loop cannot fill the event log.
  std::int64_t wrongSetupCodes() const { return wrongCodes_; }
  bool logWrongSetupCode(Ms now);
  // An empty key on success, else the reason.
  Msg setInitialPassword(const std::string& pw, const std::string& code = "", Ms now = 0);
  // A wrong old password counts as a failed attempt of `client`.
  Msg changePassword(const std::string& oldPw, const std::string& newPw, Ms now, const std::string& client);
  // Signs in, with a lockout after failed attempts. A session token on success.
  // `client` is the peer's key (clientKey), "" when unknown.
  std::optional<std::string> login(const std::string& pw, Ms now, Msg& err, const std::string& client);
  bool check(const std::string& token, Ms now);
  // Who a credential is: a session is the owner, an access token has its role; nothing for neither.
  // A use counts as now.
  std::optional<Role> access(const std::string& credential, Ms now);
  // A new access token: its secret on success, to be shown once; else the reason in `err`.
  // Never a token for the owner's role.
  std::optional<std::string> createToken(const std::string& name, Role role, Epoch created, Msg& err);
  bool revokeToken(const std::string& id);
  // The id of the access token a credential is; nothing for a session or an unknown one
  std::optional<std::string> tokenIdOf(const std::string& credential) const;
  const std::vector<AccessToken>& tokens() const { return tokens_; }
  // A known client using its session counts as seen now: the one seen
  // longest ago is the first to lose its own count.
  void seen(const std::string& client, Ms now);
  void logout(const std::string& token) { sessions_.erase(token); }
  void logoutAll() { sessions_.clear(); }
  // The password again, for a step that needs it (a factory reset, SD-139): counts as a sign-in
  // attempt of `client`, with its lockout, and opens no session
  bool confirm(const std::string& pw, Ms now, Msg& err, const std::string& client);
  // An access reset (SD-139): no password, no session, no access token, no count; the next
  // password is a first one again
  void resetAccess();

  json toJson() const;  // hash and salt, and the tokens' hashes; never sessions
  void load(const json& j);

  static constexpr std::uint32_t kIterations = 10000;
  static constexpr Ms kSessionIdle = 12 * kHour;
  static constexpr size_t kMaxSessions = 16;  // a further sign-in ends the session used longest ago
  // Lockout per client: a client that signed in before has a count
  // of its own; every other client shares one, which also brakes guessing
  // from many addresses. Kept in memory only.
  static constexpr size_t kKnownClients = 16;
  static constexpr Ms kForgetFailures = 24 * kHour;
  static constexpr size_t kMaxTokens = 16;
  // Tokens carry a prefix of their own: never taken for a session, and easy to find in a leak
  static constexpr const char* kTokenPrefix = "dpt_";

 private:
  struct Attempts {
    int failures = 0;
    Ms lockedUntil = 0;
    Ms lastFailure = 0;
  };
  struct Known {
    Attempts tries;
    Ms lastSeen = 0;  // signed in or used its session
  };
  static Msg checkStrength(const std::string& pw);
  Attempts& attemptsOf(const std::string& client);
  static bool locked(const Attempts& a, Ms now, Msg& err);
  static void failed(Attempts& a, Ms now);
  void remember(const std::string& client, Ms now);
  std::string hashOf(const std::string& pw, const std::string& saltHex, std::uint32_t it) const;
  RandomFn rng_;
  std::string salt_, hash_;
  std::string setupCode_;  // without dashes; never in toJson()
  Ms setupCodeUntil_ = 0;  // 0: no time limit
  std::int64_t wrongCodes_ = 0;  // a loop of days stays far below its end
  std::optional<Ms> wrongLogged_;
  std::uint32_t iterations_ = kIterations;
  std::map<std::string, Ms> sessions_;  // token → last used
  Attempts shared_;                    // every client that has not signed in
  std::map<std::string, Known> known_;  // clientKey → its own count
  std::vector<AccessToken> tokens_;
};

// The key of a peer's address for the lockout: IPv4 as a.b.c.d, IPv6 as
// eight groups in lowercase hex without its zone, an IPv4-mapped IPv6 address
// as IPv4; anything else is "", an unknown client.
std::string clientKey(const std::string& address);

}  // namespace doseph
