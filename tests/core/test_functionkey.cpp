// SPDX-License-Identifier: AGPL-3.0-or-later
// The function key (SD-149): a short press opens a window in which the code on the device's label
// sets the first password and pairs the device as the follower; the ring shows what stands.
#include <doctest/doctest.h>

#include <string>

#include "harness.hpp"

using namespace doseph;

namespace {

constexpr const char* kLabel = "ABCD-EFGH-JKLM-NPQR";

sim::Options labelled(const std::string& scenario) {
  sim::Options o = harness::device("home", scenario);
  o.labelCode = kLabel;
  return o;
}

std::string light(harness::Run& r) { return r.s.simState()["device"]["statusLight"]; }

}  // namespace

TEST_CASE("Function key: the label's code sets the first password only in the key's window; five wrong codes close it, and a restart or its end does too (SD-149)") {
  harness::Run r(labelled("new"));
  const json info = r.ok("GET", "info");
  CHECK(info["labelCode"] == true);
  CHECK(info["setupCode"] == true);
  CHECK(info["keyWindow"] == false);
  // Without a press: refused, whatever the code
  auto res = r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}});
  CHECK(res.status == 403);
  CHECK(json::parse(res.body)["error"]["key"] == "auth.key_window");
  // A press opens it for 5 min; the ring shows it
  r.act("keyPress");
  CHECK(r.ok("GET", "info")["keyWindow"] == true);
  CHECK(light(r) == "key_window");
  const json ev = r.s.robot().events(0, std::numeric_limits<Epoch>::max(), "auth", 20);
  CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.key.window"; }));
  // Five wrong codes close it
  for (int i = 0; i < 5; ++i) CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", "WRONGWRONGWRONG2"}}).status == 403);
  CHECK(r.ok("GET", "info")["keyWindow"] == false);
  CHECK(json::parse(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}}).body)["error"]["key"] == "auth.key_window");
  // Its end closes it
  r.act("keyPress");
  r.step(Robot::kKeyWindowMs + kSecond);
  CHECK(r.ok("GET", "info")["keyWindow"] == false);
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}}).status == 403);
  // A restart closes it
  r.act("keyPress");
  r.s.reboot();
  CHECK(r.ok("GET", "info")["keyWindow"] == false);
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}}).status == 403);
  // Pressed, the label's code works, in any case and with spaces
  r.act("keyPress");
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", "abcd efgh jklm npqr"}}).status == 200);
  CHECK(r.ok("GET", "info")["hasPassword"] == true);
}

TEST_CASE("Function key: the label's code survives a factory reset, and after an access reset it is needed again, in the key's window (SD-149, SD-139)") {
  harness::Run r(labelled("demo"));  // the demo set its password with the label's code
  REQUIRE(r.ok("GET", "info")["hasPassword"] == true);
  r.s.hardware().askReset(DeviceReset::Access);
  r.step(2 * kSecond);
  CHECK(r.ok("GET", "info")["hasPassword"] == false);
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}}).status == 403);
  r.act("keyPress");
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}}).status == 200);
  // A factory reset starts the device as new: the label stays
  r.s.hardware().askReset(DeviceReset::Factory);
  r.step(5 * kSecond);
  CHECK(r.s.hardware().labelCode() == std::optional<std::string>(kLabel));
}

TEST_CASE("Function key: a device with a label pairs as the follower with that code, in its key's window; it shows no code of its own (SD-149, SD-131)") {
  harness::Run leader(harness::device()), follower(labelled("demo"));
  // No code to show: the label has it
  const auto shown = follower.call("POST", "coupling/code");
  CHECK(shown.status == 409);
  CHECK(json::parse(shown.body)["error"]["key"] == "coupling.use_key");
  // Without a press the follower refuses the label's code
  CHECK(follower.call("POST", "coupling/join", {{"code", kLabel}, {"id", leader.s.robot().deviceId()}, {"name", "Leader"}, {"token", "x"}}).status == 403);
  // Pressed, the leader pairs with it
  follower.act("keyPress");
  REQUIRE(leader.call("POST", "coupling/pair", {{"address", "192.168.1.31"}, {"code", kLabel}}).status == 202);
  for (const auto& req : leader.s.robot().takePeerRequests()) {
    ApiRequest a;
    a.method = req.method;
    a.path = req.path;
    a.body = req.body.dump();
    a.token = req.token;
    const ApiResponse resp = follower.s.api().handle(a);
    CHECK(resp.status == 200);
    leader.s.robot().peerResponse(req.id, resp.status, json::parse(resp.body, nullptr, false));
  }
  CHECK(leader.ok("GET", "coupling")["peer"]["role"] == "leader");  // this device leads
  CHECK(follower.ok("GET", "coupling")["peer"]["role"] == "follower");
}

TEST_CASE("Function key: the ring shows the key's window, then a Stop, then a critical alarm, else all well (SD-149)") {
  harness::Run r(harness::device());
  r.step(6 * kSecond);
  CHECK(light(r) == "ok");
  r.ok("POST", "stop");
  r.step(kSecond);
  CHECK(light(r) == "stopped");
  r.act("keyPress");
  CHECK(light(r) == "key_window");
  r.step(Robot::kKeyWindowMs + kSecond);
  CHECK(light(r) == "stopped");
  r.ok("POST", "resume");
  r.act("leak", {{"on", true}});
  r.waitUntil("the leak's alarm", [&] { return light(r) == "alarm"; }, 120);
}
