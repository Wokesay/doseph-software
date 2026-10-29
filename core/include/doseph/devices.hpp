// SPDX-License-Identifier: AGPL-3.0-or-later
// Network devices (SD-132): Shelly Gen2+ devices, whose outputs the robot switches and whose
// inputs it reads over their local RPC interface; ESPHome devices, whose web server gives values;
// and MQTT topics, which give values too. A role (the refill valve, the float switch in the tank)
// is bound to one of them. The core has no network (R8): it hands the platform the requests to
// send and takes back their answers, as it does for the coupled device (SD-131). Only the gateway
// switches an output (R1); this module polls, reads and keeps what the devices said.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

struct Config;

// A device the user added by its address
struct NetDeviceCfg {
  std::string id;       // the robot's name for it: d1, d2, …
  std::string kind;     // "shelly" | "esphome"
  std::string address;  // host or host:port in the home network
  std::string name;     // the user's name for it
};

// The roles a device can take, fixed in code. An output is switched only on a Shelly Gen2+; a
// value comes from any source.
struct RoleDef {
  std::string id;      // "refill_valve", "circulation_pump", "tank_full"
  std::string kind;    // "output" | "binary"
  bool pulse = false;  // an output with a longest run: the device switches it off on its own
  Ms maxRunMs = 0;     // a pulse output's longest run (hard limit)
};
const std::vector<RoleDef>& roleDefs();
const RoleDef* roleDef(const std::string& id);
// The auto-off a pulse output gets in the device: 1.11 × its longest run, rounded up to a whole
// minute (RAT-019, RAT-060); 0 for a continuous one, which gets none
int autoOffS(const RoleDef& r);

struct RoleBinding {
  std::string source;    // "shelly" | "esphome" | "mqtt"
  std::string device;    // NetDeviceCfg::id for "shelly" and "esphome"
  int channel = 0;       // shelly: the switch or input
  std::string entity;    // esphome: "binary_sensor/<id>" or "sensor/<id>"; mqtt: the topic
  std::string field;     // mqtt: a field of a JSON payload; "" for the plain payload
  bool invert = false;   // a binary value read the other way round
  std::string availability;  // mqtt: a topic that says "online" or "offline"; "" for none
};
json bindingToJson(const RoleBinding& b);
RoleBinding bindingFromJson(const json& j);  // throws on a structure error
json deviceToJson(const NetDeviceCfg& d);
NetDeviceCfg deviceFromJson(const json& j);  // throws on a structure error

// What a Shelly output gets written and read back before the robot switches it (RAT-019): off
// after a power loss, never on by itself, an auto-off for a pulse role, and, where the device has
// an input of its own (`current` from Switch.GetConfig says so), not switched by it. A read-back
// fits when these fields are as written.
json shellySwitchConfig(const RoleDef& r, const json& current);
bool shellyConfigFits(const json& written, const json& readBack);

// A binary value from a payload: "ON"/"OFF", "true"/"false", "1"/"0", "yes"/"no", a JSON boolean
// or 0/1; nothing when it reads as neither (R5)
std::optional<bool> binaryOf(const json& v);
// A value from an MQTT payload: the plain payload, or a field of it as JSON
std::optional<json> mqttValueOf(const std::string& payload, const std::string& field);

// A request the platform sends to a device and answers with NetDevices::response()
struct NetRequest {
  long long id = 0;
  std::string device;            // NetDeviceCfg::id
  std::string address, method, path;
  json body = nullptr;           // a JSON body for POST; null for GET
  std::string user, password;    // the device's credentials where it has some; digest for Shelly
};

constexpr size_t kMaxNetDevices = 16;
constexpr Ms kNetPollMs = 5 * kSecond;    // a polled device is asked this often
constexpr Ms kNetStaleMs = 30 * kSecond;  // a polled value is stale this long after its last good answer
constexpr Ms kNetTimeoutMs = 10 * kSecond;  // a request not answered by then counts as failed

// What came back for a request the gateway asked for (Switch.Set, Switch.SetConfig,
// Switch.GetConfig): its tag, whether it worked, and the result
struct NetAnswer {
  std::string device, tag;
  bool ok = false;
  json result = nullptr;
};

// A value of a role, with why there is none
struct RoleValue {
  std::optional<json> value;
  std::string state;  // "ok" | "not_assigned" | "offline" | "stale" | "unreadable" | "waiting"
  Ms at = 0;          // when it was read
};

class NetDevices {
 public:
  // Polls the devices, once every kNetPollMs, and fails requests not answered in time; returns
  // the gateway's requests that failed so
  std::vector<NetAnswer> tick(const Config& cfg, Ms now);
  // A request of the gateway's to a Shelly: Switch.Set and the like; its answer comes back from
  // response() as a NetAnswer with this tag
  long long rpc(const Config& cfg, const std::string& device, const std::string& method, const json& params, const std::string& tag, Ms now);
  std::vector<NetRequest> take();
  // The platform's answer: status 0 when the device was not reached. An answer for the gateway is
  // returned; the polls are taken in here.
  std::optional<NetAnswer> response(const Config& cfg, long long id, int status, const json& body, Ms now);
  // A message on an MQTT topic a role reads
  void mqttMessage(const Config& cfg, const std::string& topic, const std::string& payload, Ms now);
  // Whether the platform holds an MQTT connection
  void mqttConnected(bool on) { mqttUp_ = on; }
  // The MQTT topics roles read, with their availability topics
  std::vector<std::string> mqttTopics(const Config& cfg) const;

  // A role's value (R5: none, never a stand-in) and whether a device answers
  RoleValue value(const Config& cfg, const std::string& role, Ms now) const;
  bool online(const std::string& device, Ms now) const;
  // What a Shelly said last of a switch: on, off, or unknown
  std::optional<bool> output(const std::string& device, int channel) const;
  // The devices for the API: online, what they said they are, their last problem
  json status(const Config& cfg, Ms now) const;
  // A device's credentials: Shelly's user is always "admin"; "" as password removes them
  void setCredentials(const std::string& device, const std::string& user, const std::string& password);
  bool hasPassword(const std::string& device) const;
  // The passwords' file form; never in a backup or an answer of the API (SD-132)
  json secretsJson() const;
  void secretsFromJson(const json& j);
  // A device removed: what it said is forgotten
  void forget(const std::string& device);

 private:
  struct Seen {
    Ms lastOk = -1;       // the last good answer
    Ms nextPoll = 0;
    std::string ownId;    // what the device says it is (Shelly: its ID; ESPHome: its name)
    int gen = 0;          // a Shelly's generation; 1 is not used (SD-132)
    Msg problem;          // why its last request failed
    json status = nullptr;                    // Shelly.GetStatus
    std::map<std::string, json> entities;     // esphome: entity → {body, at} of its last answer
  };
  struct Pending {
    std::string device, kind, tag;  // kind: info | status | entity | gateway
    Ms sent = 0;
  };
  struct MqttSeen {
    std::string payload;
    Ms at = -1;
  };
  NetRequest request(const Config& cfg, const NetDeviceCfg& d, const std::string& method, const std::string& path, json body);
  std::map<std::string, Seen> seen_;
  std::map<long long, Pending> pending_;
  std::map<std::string, MqttSeen> mqtt_;
  struct Credentials {
    std::string user, password;
  };
  std::map<std::string, Credentials> credentials_;
  std::vector<NetRequest> out_;
  long long seq_ = 0;
  bool mqttUp_ = false;
};

}  // namespace doseph
