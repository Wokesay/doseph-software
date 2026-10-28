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

namespace {

// Both devices step together; the leader's calls reach the follower while the link is there
void together(sim::Simulation& a, sim::Simulation& b, Ms ms, bool link = true) {
  for (Ms t = 0; t < ms; t += 500) {
    a.step(500);
    b.step(500);
    if (link) carry(a, b);
    else lose(a);
  }
}

// The follower alone, called as the leader would every second, with its pump on or off
void beats(sim::Simulation& b, const std::string& leaderToken, Ms ms, bool pump) {
  for (Ms t = 0; t < ms; t += 1000) {
    REQUIRE(call(b, leaderToken, "POST", "peer/beat", {{"pump", pump}}).status == 200);
    b.step(1000);
  }
}

}  // namespace

TEST_CASE("Coupling: the follower runs its feed pump while the leader's runs, with every guard of a check, and doses nothing by itself; its readings reach the leader (SD-131)") {
  sim::Simulation a(demo()), b(demo());
  const std::string at = login(a), bt = login(b);
  couple(a, at, b, bt);
  const json stockBefore = get(b, bt, "state")["stock"];
  b.step(5 * kSecond);
  json st = get(b, bt, "state");
  CHECK(st["cycle"]["line"]["key"] == "cycle.follow.no_link");
  CHECK(st["feedPump"] != true);
  // The leader checks: its pump runs, and so does the follower's
  bool followed = false;
  for (int i = 0; i < 120 && !followed; ++i) {
    together(a, b, 1000);
    followed = a.robot().state()["feedPump"] == true && b.robot().state()["feedPump"] == true;
  }
  CHECK(followed);
  together(a, b, 30 * kSecond);
  const json link = get(a, at, "coupling")["link"];
  CHECK(link["ok"] == true);
  CHECK(link["peer"]["readings"]["ph"].is_number());
  // Through the leader's whole check and its hold, the follower doses nothing of its own
  together(a, b, 20 * kMinute);
  st = get(b, bt, "state");
  CHECK(st["stock"] == stockBefore);
  if (a.robot().state()["feedPump"] != true) CHECK(st["cycle"]["line"]["key"] == "cycle.follow.idle");
}

TEST_CASE("Coupling: the follower doses on the leader's request through its own gateway; 10 s without a call stop the run, book it and refuse new ones until the link is back (SD-131)") {
  sim::Simulation a(demo()), b(demo());
  const std::string at = login(a), bt = login(b);
  couple(a, at, b, bt);
  const std::string lt = peerToken(a);  // what the leader sends
  // Only the leader's token may ask; and only while it calls
  CHECK(call(b, bt, "POST", "peer/run", {{"job", "x"}, {"channel", 2}, {"ms", 2000}, {"purpose", "ec"}}).status == 403);
  CHECK(call(b, lt, "POST", "peer/run", {{"job", "x"}, {"channel", 2}, {"ms", 2000}, {"purpose", "ec"}}).status == 409);
  // The leader's pump runs: the follower's runs too and proves its flow
  beats(b, lt, 30 * kSecond, true);
  REQUIRE(b.robot().state()["flowOk"] == true);
  CHECK(call(b, lt, "POST", "peer/run", {{"job", "x"}, {"channel", 2}, {"ms", 2000}, {"purpose", "calibration"}}).status == 422);
  const double before = get(b, bt, "state")["stock"]["part_a"]["ml"];
  auto r = call(b, lt, "POST", "peer/run", {{"job", "j1"}, {"channel", 2}, {"ms", 60000}, {"purpose", "ec"}});
  REQUIRE(r.status == 200);
  CHECK(call(b, lt, "POST", "peer/run", {{"job", "j2"}, {"channel", 3}, {"ms", 2000}, {"purpose", "ec"}}).status == 409);  // one at a time
  beats(b, lt, 5 * kSecond, true);
  json beat = json::parse(call(b, lt, "POST", "peer/beat", {{"pump", true}}).body);
  CHECK(beat["run"]["job"] == "j1");
  CHECK(beat["run"]["state"] == "running");
  // The link drops: 10 s later the run stops and is booked
  b.step(12 * kSecond);
  const json lost = get(b, bt, "coupling")["link"];
  CHECK(lost["ok"] == false);
  CHECK(lost["run"]["state"] == "aborted");
  const double ran = lost["run"]["ms"];
  CHECK(ran > 10000);
  CHECK(ran < 25000);
  CHECK(get(b, bt, "state")["stock"]["part_a"]["ml"].get<double>() < before);
  CHECK(get(b, bt, "state")["cycle"]["line"]["key"] == "cycle.follow.no_link");
  CHECK(get(b, bt, "state")["feedPump"] != true);
  // Back: a new run is taken and done; its result comes with the next call
  beats(b, lt, 30 * kSecond, true);
  REQUIRE(call(b, lt, "POST", "peer/run", {{"job", "j3"}, {"channel", 2}, {"ms", 2000}, {"purpose", "ec"}}).status == 200);
  beats(b, lt, 15 * kSecond, true);
  beat = json::parse(call(b, lt, "POST", "peer/beat", {{"pump", true}}).body);
  CHECK(beat["run"]["job"] == "j3");
  CHECK(beat["run"]["state"] == "done");
  int lostEv = 0, backEv = 0;
  for (const auto& e : b.robot().events(0, b.robot().now() + 1, "coupling", 50)) {
    lostEv += e["title"]["key"] == "ev.coupling.link_lost";
    backEv += e["title"]["key"] == "ev.coupling.link_back";
  }
  CHECK(lostEv == 1);
  CHECK(backEv == 1);
  // A restart forgets the link: nothing runs until the leader calls again (R6)
  b.reboot();
  CHECK(call(b, lt, "POST", "peer/run", {{"job", "j4"}, {"channel", 2}, {"ms", 2000}, {"purpose", "ec"}}).status == 409);
}
