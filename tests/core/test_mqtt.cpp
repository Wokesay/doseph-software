// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "doseph/mqtt.hpp"
#include "simulation.hpp"

using namespace doseph;

namespace {

std::string bytes(std::initializer_list<int> b) {
  std::string s;
  for (int x : b) s += static_cast<char>(x);
  return s;
}

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

const json* find(const std::vector<MqttMessage>& all, const std::string& suffix, json& out) {
  for (const auto& m : all)
    if (m.topic.size() >= suffix.size() && m.topic.compare(m.topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
      out = m.payload.empty() ? json(nullptr) : json::parse(m.payload);
      return &out;
    }
  return nullptr;
}

}  // namespace

TEST_CASE("MQTT: settings are off by default; a host when on; commands only over TLS with a user; the password never read back (PD-461, SD-130)") {
  MqttSettings m;
  CHECK_FALSE(m.enabled);
  CHECK_FALSE(mqttCommands(m));
  CHECK(applyMqtt(m, {{"enabled", true}}).front().key == "cfg.mqtt.host");
  CHECK(applyMqtt(m, {{"enabled", true}, {"host", "broker.lan"}, {"port", 70000}}).front().key == "cfg.mqtt.port");
  CHECK(applyMqtt(m, {{"enabled", true}, {"host", "bad host"}}).front().key == "cfg.mqtt.host");
  CHECK(applyMqtt(m, {{"discovery", "home/#"}}).front().key == "cfg.mqtt.discovery");
  CHECK_FALSE(m.enabled);  // nothing changed on an error
  REQUIRE(applyMqtt(m, {{"enabled", true}, {"host", "broker.lan"}, {"user", "doseph"}, {"password", "s3cret"}}).empty());
  CHECK(applyMqtt(m, {{"commands", true}}).front().key == "cfg.mqtt.commands");  // no TLS
  REQUIRE(applyMqtt(m, {{"tls", true}, {"port", 8883}, {"commands", true}}).empty());
  CHECK(mqttCommands(m));
  CHECK(applyMqtt(m, {{"user", ""}}).front().key == "cfg.mqtt.commands");  // commands keep their user
  const json api = mqttToJson(m, false);
  CHECK(api.dump().find("s3cret") == std::string::npos);
  CHECK(api["passwordSet"] == true);
  CHECK(mqttFromJson(mqttToJson(m, true)).password == "s3cret");
  // A damaged file gives no connection
  json bad = mqttToJson(m, true);
  bad["port"] = "x";
  CHECK_FALSE(mqttFromJson(bad).enabled);
}

TEST_CASE("MQTT: the packets of MQTT 3.1.1, and a reader that waits for a whole packet and breaks on one it cannot read") {
  CHECK(mqttConnectPacket("dp", "", "", "", "", 60) == bytes({0x10, 14, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 60, 0, 2, 'd', 'p'}));
  CHECK(mqttConnectPacket("dp", "u", "p", "w", "x", 30) ==
        bytes({0x10, 26, 0, 4, 'M', 'Q', 'T', 'T', 4, 0xe6, 0, 30, 0, 2, 'd', 'p', 0, 1, 'w', 0, 1, 'x', 0, 1, 'u', 0, 1, 'p'}));
  CHECK(mqttPublishPacket("a/b", "on", true) == bytes({0x31, 7, 0, 3, 'a', '/', 'b', 'o', 'n'}));
  CHECK(mqttSubscribePacket(1, {"a/#"}) == bytes({0x82, 8, 0, 1, 0, 3, 'a', '/', '#', 0}));
  const std::string big = mqttPublishPacket("t", std::string(200, 'x'), false);
  CHECK(big.substr(0, 3) == bytes({0x30, 0xcb, 0x01}));  // 203 as a two-byte length

  MqttReader r;
  r.feed(bytes({0x20, 2, 0}));
  CHECK_FALSE(r.next());  // incomplete
  r.feed(bytes({0}) + big.substr(0, 10));
  auto ack = r.next();
  REQUIRE(ack);
  CHECK(ack->type == 2);
  CHECK(ack->returnCode == 0);
  CHECK_FALSE(r.next());
  r.feed(big.substr(10));
  auto pub = r.next();
  REQUIRE(pub);
  CHECK(pub->type == 3);
  CHECK(pub->topic == "t");
  CHECK(pub->payload == std::string(200, 'x'));
  r.feed(bytes({0x32, 7, 0, 1, 'q', 0, 9, 'h', 'i'}));  // QoS 1: a packet id after the topic
  auto q1 = r.next();
  REQUIRE(q1);
  CHECK(q1->payload == "hi");
  CHECK_FALSE(r.broken());
  r.feed(bytes({0x30, 0xff, 0xff, 0xff, 0x7f}));  // far over the size we take
  CHECK_FALSE(r.next());
  CHECK(r.broken());
}

TEST_CASE("MQTT: Home Assistant discovery names the device and its entities; command entities only while commands are on (PD-461, SD-130)") {
  sim::Simulation s(demo());
  s.step(5 * kMinute);
  Robot& robot = s.robot();
  const std::string id = robot.deviceId();
  const std::string node = "doseph_" + id;
  REQUIRE(robot.putMqtt({{"enabled", true}, {"host", "broker.lan"}}).status == 200);
  auto all = robot.mqttDiscoveryMessages();
  json c;
  REQUIRE(find(all, "/sensor/" + node + "/ph/config", c));
  CHECK(c["unique_id"] == node + "_ph");
  CHECK(c["state_topic"] == "doseph/" + id + "/state");
  CHECK(c["availability_topic"] == "doseph/" + id + "/availability");
  CHECK(c["device"]["identifiers"][0] == node);
  CHECK(c["device"]["name"] == "Doseph demo");
  REQUIRE(find(all, "/sensor/" + node + "/ec/config", c));
  CHECK(c["unit_of_measurement"] == "mS/cm");
  REQUIRE(find(all, "/sensor/" + node + "/canister_part_a/config", c));
  CHECK(c["value_template"] == "{{ value_json.canisters['part_a'] }}");
  REQUIRE(find(all, "/binary_sensor/" + node + "/problem/config", c));
  for (const auto& m : all) {
    CHECK(m.retain);
    CHECK(m.topic.rfind("homeassistant/", 0) == 0);
  }
  // Commands off: their entities are removed
  REQUIRE(find(all, "/button/" + node + "/stop/config", c));
  CHECK(c.is_null());
  REQUIRE(robot.putMqtt({{"tls", true}, {"user", "doseph"}, {"commands", true}}).status == 200);
  all = robot.mqttDiscoveryMessages();
  REQUIRE(find(all, "/button/" + node + "/stop/config", c));
  CHECK(c["command_topic"] == "doseph/" + id + "/stop/set");
  REQUIRE(find(all, "/select/" + node + "/recipe_set/config", c));
  CHECK(c["options"] == json::array({"Two-part nutrient"}));
  REQUIRE(find(all, "/number/" + node + "/ph_target/config", c));
  CHECK(c["command_topic"] == "doseph/" + id + "/ph_target/set");
}

TEST_CASE("MQTT: the state carries the readings, the plan and the canisters; a value the robot cannot use is null, never 0 (R5, SD-130)") {
  sim::Simulation s(demo());
  s.step(10 * kMinute);
  json st = json::parse(s.robot().mqttStateMessage().payload);
  CHECK(st["ph"].is_number());
  CHECK(st["ec"].is_number());
  CHECK(st["recipe"] == "Two-part nutrient");
  CHECK(st["ecTarget"] == 1.6);
  CHECK(st["canisters"]["part_a"].is_number());
  CHECK(st["stopped"] == false);
  CHECK(s.robot().mqttStateMessage().retain);
  // Without its probe calibration the pH is not usable: null
  json state = s.robot().state();
  state["readings"]["ph"]["usable"] = false;
  st = json::parse(mqttState(s.robot().deviceId(), state, s.robot().config()).payload);
  CHECK(st["ph"].is_null());
}

TEST_CASE("API: MQTT settings: the password is never read back; only the owner and an app change them; commands are the account role's and pass the same checks (PD-461, SD-129, SD-130)") {
  sim::Simulation s(demo());
  s.step(5 * kMinute);
  const std::string owner = login(s);
  REQUIRE_FALSE(owner.empty());
  const std::string base = "doseph/" + s.robot().deviceId() + "/";
  // Off: a command is refused
  CHECK(s.api().mqttCommand(base + "stop/set", "PRESS").status == 403);
  CHECK_FALSE(s.robot().runtime().stopped);
  auto r = call(s, owner, "PUT", "mqtt", {{"enabled", true}, {"host", "broker.lan"}, {"user", "doseph"}, {"password", "s3cret"}, {"commands", true}});
  CHECK(r.status == 422);  // commands without TLS
  r = call(s, owner, "PUT", "mqtt", {{"enabled", true}, {"host", "broker.lan"}, {"port", 8883}, {"tls", true}, {"user", "doseph"}, {"password", "s3cret"}, {"commands", true}});
  REQUIRE(r.status == 200);
  const json got = json::parse(call(s, owner, "GET", "mqtt").body);
  CHECK(got.dump().find("s3cret") == std::string::npos);
  CHECK(got["commandsOn"] == true);
  CHECK(got["topic"] == base.substr(0, base.size() - 1));
  CHECK(s.storage().read("config.json")->find("s3cret") == std::string::npos);  // never in a backup
  // An account token reads them but changes nothing
  const json tok = json::parse(call(s, owner, "POST", "tokens", {{"name", "Account"}, {"role", "account"}}).body);
  CHECK(call(s, tok["token"], "GET", "mqtt").status == 200);
  CHECK(call(s, tok["token"], "PUT", "mqtt", {{"enabled", false}}).status == 403);

  // The recipe by its name; the targets of the recipe in force, the pH range keeping its width
  CHECK(s.api().mqttCommand(base + "recipe/set", "No such recipe").status == 422);
  CHECK(s.api().mqttCommand(base + "recipe/set", "Two-part nutrient").status == 200);
  const auto ecSet = s.api().mqttCommand(base + "ec_target/set", "1.8");
  CHECK_MESSAGE(ecSet.status == 200, ecSet.body);
  CHECK(s.robot().config().recipe("two_part")->ecTarget == doctest::Approx(1.8));
  CHECK(s.api().mqttCommand(base + "ph_target/set", "6.1").status == 200);
  CHECK(s.robot().config().recipe("two_part")->phLow == doctest::Approx(5.9));
  CHECK(s.robot().config().recipe("two_part")->phHigh == doctest::Approx(6.3));
  CHECK(s.api().mqttCommand(base + "ec_target/set", "lots").status == 422);
  CHECK(s.api().mqttCommand(base + "ec_target/set", "99").status == 422);  // the recipe's own bounds
  // Never a limit, a release or a single pump: there is no such command
  CHECK(s.api().mqttCommand(base + "resume/set", "PRESS").status == 404);
  CHECK(s.api().mqttCommand("other/device/stop/set", "PRESS").status == 404);
  // Stop, said as coming over MQTT
  CHECK(s.api().mqttCommand(base + "stop/set", "PRESS").status == 200);
  CHECK(s.robot().runtime().stopped);
  int commands = 0, refused = 0;
  bool viaMqtt = false;
  for (const auto& e : s.robot().events(0, s.robot().now() + 1, "", 100)) {
    commands += e["title"]["key"] == "ev.mqtt.command";
    refused += e["title"]["key"] == "ev.mqtt.refused";
    viaMqtt |= e.dump().find("stop.mqtt") != std::string::npos;
  }
  CHECK(commands == 4);
  CHECK(refused == 3);
  CHECK(viaMqtt);
}
