// SPDX-License-Identifier: AGPL-3.0-or-later
// Coupling two devices (PD-457, SD-131): two simulators in one process, and a carrier that does
// what the platform does: it takes one device's requests to the other and brings the answers back.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "simulation.hpp"

using namespace doseph;

namespace {

sim::Options demo() {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  return o;
}

ApiResponse call(sim::Simulation& s, const std::string& token, const std::string& method, const std::string& path, const json& body = nullptr) {
  ApiRequest r;
  r.method = method;
  r.path = "/api/v1/" + path;
  r.body = body.is_null() ? "" : body.dump();
  r.token = token;
  return s.api().handle(r);
}

std::string login(sim::Simulation& s) {
  auto r = call(s, "", "POST", "auth/login", {{"password", "demo-password"}});
  for (const auto& [k, v] : r.headers)
    if (k == "Set-Cookie") return sessionToken("", v.substr(0, v.find(';')));
  return "";
}

// What the platform does: each request of `from` to `to`, and its answer back. Returns how many.
size_t carry(sim::Simulation& from, sim::Simulation& to) {
  const auto reqs = from.robot().takePeerRequests();
  for (const auto& r : reqs) {
    ApiRequest a;
    a.method = r.method;
    a.path = r.path;
    a.body = r.body.dump();
    a.token = r.token;
    const ApiResponse resp = to.api().handle(a);
    from.robot().peerResponse(r.id, resp.status, json::parse(resp.body, nullptr, false));
  }
  return reqs.size();
}

// The other device is not there: every request fails without an answer
void lose(sim::Simulation& from) {
  for (const auto& r : from.robot().takePeerRequests()) from.robot().peerResponse(r.id, 0, nullptr);
}

json get(sim::Simulation& s, const std::string& token, const std::string& path) { return json::parse(call(s, token, "GET", path).body); }

std::string peerToken(sim::Simulation& s) { return json::parse(*s.storage().read("peer.json"))["token"]; }

// Couples `leader` with `follower` as the owner would: a code on the follower, typed into the leader
void couple(sim::Simulation& leader, const std::string& lt, sim::Simulation& follower, const std::string& ft) {
  const std::string code = json::parse(call(follower, ft, "POST", "coupling/code").body)["code"];
  REQUIRE(call(leader, lt, "POST", "coupling/pair", {{"address", "192.168.1.31"}, {"code", code}}).status == 202);
  REQUIRE(carry(leader, follower) == 1);
}

}  // namespace

TEST_CASE("Coupling: the second device shows a code, the leader pairs with it, and each holds a peer token for the other; a backup carries none of it (SD-131)") {
  sim::Simulation a(demo()), b(demo());
  const std::string at = login(a), bt = login(b);
  REQUIRE(a.robot().deviceId() != b.robot().deviceId());
  const json code = json::parse(call(b, bt, "POST", "coupling/code").body);
  CHECK(code["code"].get<std::string>().size() == 9);  // XXXX-XXXX
  CHECK(get(b, bt, "coupling")["code"]["until"].is_number());
  CHECK(call(a, at, "POST", "coupling/pair", {{"address", "not an address!"}, {"code", "x"}}).status == 422);
  REQUIRE(call(a, at, "POST", "coupling/pair", {{"address", "192.168.1.31:8080"}, {"code", code["code"]}}).status == 202);
  CHECK(get(a, at, "coupling")["pairing"]["state"] == "pairing");
  CHECK(call(a, at, "POST", "coupling/pair", {{"address", "192.168.1.31"}, {"code", "x"}}).status == 409);  // one at a time
  REQUIRE(carry(a, b) == 1);

  const json ca = get(a, at, "coupling"), cb = get(b, bt, "coupling");
  CHECK(ca["peer"]["role"] == "leader");
  CHECK(ca["peer"]["id"] == b.robot().deviceId());
  CHECK(ca["peer"]["address"] == "192.168.1.31:8080");
  CHECK(ca["pairing"].is_null());
  CHECK(cb["peer"]["role"] == "follower");
  CHECK(cb["peer"]["id"] == a.robot().deviceId());
  CHECK(cb["code"].is_null());  // used up
  CHECK(ca.dump().find("dpt_") == std::string::npos);  // never a token in the API
  CHECK(get(a, at, "tokens")["tokens"][0]["role"] == "peer");
  CHECK(get(b, bt, "tokens")["tokens"][0]["role"] == "peer");

  // Each opens the other as a peer: it reads, and changes nothing
  CHECK(call(a, peerToken(b), "GET", "state").status == 200);
  CHECK(call(b, peerToken(a), "GET", "state").status == 200);
  CHECK(call(a, peerToken(b), "POST", "stop").status == 403);
  // A backup carries neither the peer nor a token; a restart keeps the coupling
  const std::string backup = call(a, at, "GET", "config/export").body;
  CHECK(backup.find(b.robot().deviceId()) == std::string::npos);
  CHECK(backup.find("dpt_") == std::string::npos);
  b.reboot();
  CHECK(get(b, login(b), "coupling")["peer"]["id"] == a.robot().deviceId());
  // Coupled once is enough: a second pairing is refused on both sides
  CHECK(call(a, at, "POST", "coupling/pair", {{"address", "192.168.1.40"}, {"code", "ABCD-EFGH"}}).status == 409);
  CHECK(call(b, login(b), "POST", "coupling/code").status == 409);
}

TEST_CASE("Coupling: a wrong code is refused, the fifth voids it, an old one is refused; a failed pairing shows on the leader and takes its token back (SD-131)") {
  sim::Simulation a(demo()), b(demo());
  const std::string at = login(a), bt = login(b);
  // Wrong: the leader says why, and the token it made for the other is gone again
  const std::string code = json::parse(call(b, bt, "POST", "coupling/code").body)["code"];
  REQUIRE(call(a, at, "POST", "coupling/pair", {{"address", "192.168.1.31"}, {"code", "WRON-GCOD"}}).status == 202);
  REQUIRE(carry(a, b) == 1);
  json ca = get(a, at, "coupling");
  CHECK(ca["peer"].is_null());
  CHECK(ca["pairing"]["state"] == "failed");
  CHECK(ca["pairing"]["error"]["key"] == "coupling.code");
  CHECK(get(a, at, "tokens")["tokens"].empty());
  // Typed in lower case with a space instead of the dash, it is the same code; but the fifth wrong
  // try before it voids it
  for (int i = 0; i < 4; ++i) CHECK(call(b, "", "POST", "coupling/join", {{"code", "WRONGCOD"}}).status == 403);
  CHECK(get(b, bt, "coupling")["code"].is_null());
  CHECK(call(b, "", "POST", "coupling/join", {{"code", code}, {"id", a.robot().deviceId()}, {"name", "x"}, {"token", "dpt_" + std::string(64, 'a')}}).status == 403);
  // An old code is refused
  const std::string late = json::parse(call(b, bt, "POST", "coupling/code").body)["code"];
  b.step(11 * kMinute);
  CHECK(call(b, "", "POST", "coupling/join", {{"code", late}, {"id", a.robot().deviceId()}, {"name", "x"}, {"token", "dpt_" + std::string(64, 'a')}}).status == 403);
  // Unreachable: said as such
  const std::string fresh = json::parse(call(b, bt, "POST", "coupling/code").body)["code"];
  REQUIRE(call(a, at, "POST", "coupling/pair", {{"address", "192.168.1.99"}, {"code", fresh}}).status == 202);
  lose(a);
  ca = get(a, at, "coupling");
  CHECK(ca["pairing"]["error"]["key"] == "coupling.unreachable");
  CHECK(get(a, at, "tokens")["tokens"].empty());
  // Typed as people type it: lower case, a space for the dash
  std::string typed = fresh.substr(0, 4) + " " + fresh.substr(5);
  for (auto& ch : typed) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  REQUIRE(call(a, at, "POST", "coupling/pair", {{"address", "192.168.1.31"}, {"code", typed}}).status == 202);
  REQUIRE(carry(a, b) == 1);
  CHECK(get(a, at, "coupling")["peer"]["role"] == "leader");
  // Wrong codes are events
  int wrong = 0;
  for (const auto& e : b.robot().events(0, b.robot().now() + 1, "coupling", 50)) wrong += e["title"]["key"] == "ev.coupling.code_wrong";
  CHECK(wrong == 5);
}

TEST_CASE("Coupling: ended on the leader, the follower is told and both take their tokens back; only the coupled device's own token may end it (SD-131)") {
  sim::Simulation a(demo()), b(demo());
  const std::string at = login(a), bt = login(b);
  couple(a, at, b, bt);
  const std::string aCallsB = peerToken(a), bCallsA = peerToken(b);
  // An account token, or a peer token the owner made by hand, does not end it
  const std::string manual = json::parse(call(b, bt, "POST", "tokens", {{"name", "Other"}, {"role", "peer"}}).body)["token"];
  CHECK(call(b, manual, "POST", "coupling/leave").status == 403);
  const std::string account = json::parse(call(b, bt, "POST", "tokens", {{"name", "Account"}, {"role", "account"}}).body)["token"];
  CHECK(call(b, account, "POST", "coupling/leave").status == 403);
  CHECK_FALSE(get(b, bt, "coupling")["peer"].is_null());

  REQUIRE(call(a, at, "DELETE", "coupling").status == 200);
  CHECK(get(a, at, "coupling")["peer"].is_null());
  REQUIRE(carry(a, b) == 1);
  CHECK(get(b, bt, "coupling")["peer"].is_null());
  CHECK(call(a, bCallsA, "GET", "state").status == 401);
  CHECK(call(b, aCallsB, "GET", "state").status == 401);
  bool ended = false, left = false;
  for (const auto& e : a.robot().events(0, a.robot().now() + 1, "coupling", 20)) ended |= e["title"]["key"] == "ev.coupling.ended";
  for (const auto& e : b.robot().events(0, b.robot().now() + 1, "coupling", 20)) left |= e["title"]["key"] == "ev.coupling.left";
  CHECK(ended);
  CHECK(left);

  // Ended on the follower: it takes its token back at once; the leader learns it on its next call
  couple(a, at, b, bt);
  const std::string again = peerToken(a);
  REQUIRE(call(b, bt, "DELETE", "coupling").status == 200);
  CHECK(carry(b, a) == 0);  // the follower does not know where the leader is
  CHECK(call(b, again, "GET", "state").status == 401);
  CHECK(call(a, at, "DELETE", "coupling").status == 200);
  CHECK(call(a, at, "DELETE", "coupling").status == 404);
}
