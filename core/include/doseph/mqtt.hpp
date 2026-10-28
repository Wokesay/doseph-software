// SPDX-License-Identifier: AGPL-3.0-or-later
// MQTT with Home Assistant discovery (PD-461, SD-130). Off by default. Its
// settings live in a file of their own, apart from the configuration, as the
// push channels' do (SD-124): a backup, the diagnostics and GET /config never
// carry the password. Values and states go to the broker also without TLS;
// commands only over TLS with a user (decided by the owner). A command is a
// call of the account role (SD-129) and passes the same checks as one from the
// app. The core builds the topics, the payloads and the packets of MQTT 3.1.1;
// the platform holds the connection (R8).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "doseph/common.hpp"
#include "doseph/config.hpp"

namespace doseph {

struct MqttSettings {
  bool enabled = false;
  std::string host;
  int port = 1883;
  bool tls = false;
  std::string user;
  std::string password;  // secret: never read back, never in a backup
  bool commands = false;  // only over TLS with a user
  std::string discovery = "homeassistant";  // Home Assistant's discovery prefix
};

// The file's form, with the password; the API's form, with only whether it is set
json mqttToJson(const MqttSettings& m, bool withSecrets);
MqttSettings mqttFromJson(const json& j);
// A change from the app: a field left out stays, a password given as "" is cleared. On an error
// nothing changes and the errors are returned.
std::vector<Msg> applyMqtt(MqttSettings& m, const json& change);
// Whether commands are taken: asked for, over TLS, with a user
bool mqttCommands(const MqttSettings& m);

struct MqttMessage {
  std::string topic, payload;
  bool retain = false;
};

// The device's own topics: doseph/<device ID>/…
std::string mqttBase(const std::string& deviceId);
// "online" here while connected, "offline" as the broker's last will
std::string mqttAvailability(const std::string& deviceId);
// What Home Assistant needs to know of the device's entities, retained. With commands off, the
// command entities get an empty config, which removes them.
std::vector<MqttMessage> mqttDiscovery(const std::string& deviceId, const json& info, const Config& cfg, const MqttSettings& m);
// The values and states as one retained JSON payload; a missing value is null (R5)
MqttMessage mqttState(const std::string& deviceId, const json& state, const Config& cfg);
// The topics commands come on: …/stop/set, …/recipe/set, …/ec_target/set, …/ph_target/set
std::vector<std::string> mqttCommandTopics(const std::string& deviceId);

// MQTT 3.1.1 packets (OASIS standard), QoS 0 only
std::string mqttConnectPacket(const std::string& clientId, const std::string& user, const std::string& password,
                              const std::string& willTopic, const std::string& willPayload, int keepAliveS);
std::string mqttPublishPacket(const std::string& topic, const std::string& payload, bool retain);
std::string mqttSubscribePacket(std::uint16_t id, const std::vector<std::string>& topics);
std::string mqttPingPacket();
std::string mqttDisconnectPacket();

// A packet from the broker: CONNACK with its return code, PUBLISH with topic and payload, others
// by type only
struct MqttPacket {
  int type = 0;  // 2 CONNACK, 3 PUBLISH, 9 SUBACK, 13 PINGRESP
  int returnCode = -1;
  std::string topic, payload;
};
// Reads packets from the bytes of a connection; an incomplete one waits for more. A packet that
// cannot be read breaks the reader: the platform closes the connection.
class MqttReader {
 public:
  void feed(const std::string& bytes) { buf_ += bytes; }
  std::optional<MqttPacket> next();
  bool broken() const { return broken_; }

  static constexpr size_t kMaxPacket = 64 * 1024;

 private:
  std::string buf_;
  bool broken_ = false;
};

}  // namespace doseph
