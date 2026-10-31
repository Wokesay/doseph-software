// SPDX-License-Identifier: AGPL-3.0-or-later
// The function key (SD-149): a short press opens a window in which the code on the device's label
// sets the first password and pairs the device as the follower; the ring shows what stands.
#include <doctest/doctest.h>

#include <string>

#include "doseph/coupling.hpp"
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

TEST_CASE("Function key: the label's code sets the first password only over the setup network and in the key's window; wrong codes are logged and close nothing; its use, its end or a restart close it (SD-149)") {
  harness::Run r(labelled("new"));
  const json info = r.ok("GET", "info");
  CHECK(info["labelCode"] == true);
  CHECK(info["setupCode"] == true);
  CHECK_FALSE(info.contains("keyWindow"));  // nobody on the network learns when it is open
  // Over the home network: never, so the label's code does not cross it
  r.act("keyPress");
  auto res = r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}});
  CHECK(res.status == 403);
  CHECK(json::parse(res.body)["error"]["key"] == "auth.setup_network");
  r.s.robot().endKeyWindow();
  r.setupNetwork = true;
  // Without a press: refused, whatever the code, and logged
  res = r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}});
  CHECK(res.status == 403);
  CHECK(json::parse(res.body)["error"]["key"] == "auth.key_window");
  auto logged = [&](const std::string& text) {
    const json ev = r.s.robot().events(0, std::numeric_limits<Epoch>::max(), "auth", 50);
    return std::any_of(ev.begin(), ev.end(), [&](const json& e) { return e["title"]["key"] == "ev.key.wrong" && e["text"]["key"] == text; });
  };
  CHECK(logged("ev.key.outside.text"));
  // A press opens it for 5 min; the ring shows it
  r.act("keyPress");
  CHECK(r.s.robot().keyWindowOpen());
  CHECK(light(r) == "key_window");
  const json ev = r.s.robot().events(0, std::numeric_limits<Epoch>::max(), "auth", 50);
  CHECK(std::any_of(ev.begin(), ev.end(), [](const json& e) { return e["title"]["key"] == "ev.key.window"; }));
  // Wrong codes close nothing: anyone on the network could otherwise block the owner
  for (int i = 0; i < 8; ++i) CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", "WRONGWRONGWRONG2"}}).status == 403);
  CHECK(r.s.robot().keyWindowOpen());
  // Its end closes it
  r.step(Robot::kKeyWindowMs + kSecond);
  CHECK_FALSE(r.s.robot().keyWindowOpen());
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}}).status == 403);
  // A restart closes it
  r.act("keyPress");
  r.s.reboot();
  CHECK_FALSE(r.s.robot().keyWindowOpen());
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", kLabel}}).status == 403);
  // Pressed, the label's code works, in any case and with spaces; used, the window closes
  r.act("keyPress");
  CHECK(r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", "abcd efgh jklm npqr"}}).status == 200);
  CHECK(r.ok("GET", "info")["hasPassword"] == true);
  CHECK_FALSE(r.s.robot().keyWindowOpen());
}

TEST_CASE("Function key: a label that is no code fails closed: no first password and no pairing, even in the key's window (SD-149)") {
  for (const std::string bad : {"----", "SHORT", "ABCD-EFGH-JKLM-NPQ0"}) {
    INFO(bad);
    sim::Options o = harness::device("home", "new");
    o.labelCode = bad;
    harness::Run r(o);
    r.setupNetwork = true;
    r.act("keyPress");
    for (const std::string code : {std::string(), std::string("----"), bad}) {
      const auto res = r.call("POST", "auth/setup", {{"password", "a-long-password"}, {"code", code}});
      CHECK(res.status == 403);
      CHECK(json::parse(res.body)["error"]["key"] == "auth.label_invalid");
    }
    CHECK(r.ok("GET", "info")["hasPassword"] == false);
  }
}

TEST_CASE("Function key: the label's code survives a factory reset, and after an access reset it is needed again, over the setup network in the key's window (SD-149, SD-139)") {
  harness::Run r(labelled("demo"));  // the demo set its password with the label's code
  REQUIRE(r.ok("GET", "info")["hasPassword"] == true);
  r.setupNetwork = true;
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

TEST_CASE("Function key: a device with a label pairs as the follower once its key is pressed and its owner allows it; the leader sends a proof, never the code, and the window closes when used (SD-149, SD-131)") {
  harness::Run leader(harness::device()), follower(labelled("demo"));
  // Without a press the owner cannot allow it, and nothing is armed
  const auto early = follower.call("POST", "coupling/code");
  CHECK(early.status == 409);
  CHECK(json::parse(early.body)["error"]["key"] == "coupling.use_key");
  // A press alone arms nothing: the leader's proof is refused
  follower.act("keyPress");
  auto carry = [&] {
    int status = 0;
    for (const auto& req : leader.s.robot().takePeerRequests()) {
      CHECK_FALSE(req.body.contains("code"));
      CHECK(req.body.dump().find("ABCD") == std::string::npos);  // the label's code is nowhere in it
      ApiRequest a;
      a.method = req.method;
      a.path = req.path;
      a.body = req.body.dump();
      a.token = req.token;
      const ApiResponse resp = follower.s.api().handle(a);
      status = resp.status;
      leader.s.robot().peerResponse(req.id, resp.status, json::parse(resp.body, nullptr, false));
    }
    return status;
  };
  REQUIRE(leader.call("POST", "coupling/pair", {{"address", "192.168.1.31"}, {"code", kLabel}}).status == 202);
  CHECK(carry() == 403);
  // Pressed and allowed by the owner: it pairs
  const json allowed = follower.ok("POST", "coupling/code");
  CHECK(allowed["label"] == true);
  CHECK(allowed["code"].is_null());
  REQUIRE(leader.call("POST", "coupling/pair", {{"address", "192.168.1.31"}, {"code", kLabel}}).status == 202);
  CHECK(carry() == 200);
  CHECK(leader.ok("GET", "coupling")["peer"]["role"] == "leader");  // this device leads
  CHECK(follower.ok("GET", "coupling")["peer"]["role"] == "follower");
  CHECK_FALSE(follower.s.robot().keyWindowOpen());
  // The proof belongs to the leader's ID and token: changed, it is refused
  CHECK(pairingProof(kLabel, "a", "t") != pairingProof(kLabel, "b", "t"));
  CHECK(pairingProof(kLabel, "a", "t") != pairingProof(kLabel, "a", "u"));
  CHECK(pairingProof("abcd efgh jklm npqr", "a", "t") == pairingProof(kLabel, "a", "t"));
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
