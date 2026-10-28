// SPDX-License-Identifier: AGPL-3.0-or-later
// The scenario harness (#10, step 3.4; docs/LATER.md): the real core against
// the twin in simulated time, driven only through the API, as the web app
// does. What the core cannot see is read from the twin's truth.
#pragma once

#include <doctest/doctest.h>

#include <functional>
#include <map>
#include <string>
#include <utility>

#include "simulation.hpp"

namespace harness {

using doseph::json;
using doseph::Ms;

// A device of an example profile, set up as the demo (or new), no history
// computed ahead, a fixed start time, everything in memory
inline sim::Options device(const std::string& profile = "home", const std::string& scenario = "demo") {
  sim::Options o;
  o.scenario = scenario;
  o.password = "demo-password";
  o.prefillHours = 0;
  o.profile = profile;
  o.startEpoch = 1798761600;  // 2027-01-01 00:00 UTC
  return o;
}

class Run {
 public:
  explicit Run(const sim::Options& o) : s(o) {
    if (o.scenario == "demo") REQUIRE(login(o.password));
  }

  sim::Simulation s;
  std::string token;
  std::string ip = "192.168.1.20";

  // A request as the HTTP server hands it to the core, without a socket
  doseph::ApiResponse call(const std::string& method, const std::string& path, const json& body = nullptr,
                           std::map<std::string, std::string> query = {}) {
    doseph::ApiRequest r;
    r.method = method;
    r.path = "/api/v1/" + path;
    r.body = body.is_null() ? "" : body.dump();
    r.token = token;
    r.client = doseph::clientKey(ip);
    r.query = std::move(query);
    return s.api().handle(r);
  }
  // Fails the test on any answer but 200, naming the request and the answer
  json ok(const std::string& method, const std::string& path, const json& body = nullptr,
          std::map<std::string, std::string> query = {}) {
    const auto r = call(method, path, body, std::move(query));
    INFO(method << " " << path << " -> " << r.status << " " << r.body);
    REQUIRE(r.status == 200);
    return r.body.empty() ? json(nullptr) : json::parse(r.body);
  }
  bool login(const std::string& pw) {
    const auto r = call("POST", "auth/login", {{"password", pw}});
    for (const auto& [k, v] : r.headers)
      if (k == "Set-Cookie") token = doseph::sessionToken("", v.substr(0, v.find(';')));
    return r.status == 200;
  }

  json state() { return ok("GET", "state"); }
  std::string phase() { return state()["cycle"]["phase"]; }
  std::string line() { return state()["cycle"]["line"]["key"]; }
  // Events of one type, newest first, as the API lists them
  json events(const std::string& type) { return ok("GET", "events", nullptr, {{"type", type}, {"limit", "1000"}}); }

  // The twin: its truth, which the core cannot see, and its faults
  json twin() { return s.simState()["world"]; }
  void act(const std::string& action, const json& body = json::object()) {
    const json out = s.control(action, body);
    INFO("sim " << action << " " << body.dump() << " -> " << out.dump());
    REQUIRE_FALSE(out.contains("error"));
  }

  // Time moves only when the test moves it
  void step(Ms ms, Ms tick = 1000) { s.step(ms, tick); }
  // Polls a condition in simulated time; fails the test past its deadline
  void waitUntil(const std::string& what, const std::function<bool()>& done, double maxS, Ms tick = 1000) {
    for (double t = 0; t <= maxS; t += static_cast<double>(tick) / 1000.0) {
      if (done()) return;
      s.step(tick, tick);
    }
    FAIL("not within " << maxS << " s: " << what);
  }
};

}  // namespace harness
