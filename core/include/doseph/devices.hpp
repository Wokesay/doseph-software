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

// The roles a device can take: those fixed in code, an output for each watering circuit
// ("water.<id>", SD-134) and light circuit ("light.<id>", SD-135), and a room's outputs and sensors
// ("climate.<room>.<kind>", SD-136). An output is switched only on a Shelly Gen2+; a value comes
// from any source.
struct RoleDef {
  std::string id;      // "refill_valve", "circulation_pump", "tank_full", "tank_empty", "tank_level", "water.<id>", …
  std::string kind;    // "output" | "binary" | "level" (litres) | "temp" (°C) | "rh" (%) | "co2" (ppm)
  bool pulse = false;  // an output with a longest run: the device switches it off on its own
  Ms maxRunMs = 0;     // a pulse output's longest run (hard limit)
  bool light = false;  // a light circuit's (SD-135): no Stop, hold or maintenance switches it
  bool dimmer = false; // on a Shelly "Light" component with a brightness, not a "Switch"
  bool fan = false;    // a room's fan (SD-136): no Stop, hold or maintenance switches it either
  bool climate = false;  // a room's other output (SD-136): Stop switches it off, a hold of the pumps does not
  double powerLimitW = 0;  // written into the device where it has one: a net for a thermostat that sticks (RAT-069)
};
// Whether a Stop leaves an output to its rule: light and a room's fans (SD-135, SD-136)
bool throughStop(const std::string& role);
const std::vector<RoleDef>& roleDefs();  // the fixed ones
const RoleDef* roleDef(const std::string& id);
std::vector<RoleDef> roleDefs(const Config& cfg);  // with the watering circuits
std::optional<RoleDef> roleDef(const Config& cfg, const std::string& id);
// A role's name for people: a circuit's is its own
Msg roleLabel(const Config& cfg, const std::string& id);
// The auto-off a pulse output gets in the device: 1.11 × its longest run, rounded up to a whole
// minute (RAT-019); 0 for a continuous one, which gets none
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

// A request the platform sends to a device and answers with NetDevices::response(). A Shelly's
// RPC body carries the request's ID.
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
// Switch.GetConfig): the request's ID, its tag, whether it worked, and the result
struct NetAnswer {
  long long id = 0;
  std::string device, tag;
  bool ok = false;
  json result = nullptr;
  bool other = false;  // another device answered: what was asked reached the wrong one
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
  // A request of the gateway's to a Shelly the configuration names: Switch.Set and the like. Its
  // answer comes back from response() as a NetAnswer with the ID returned here (0: no such device).
  long long rpc(const Config& cfg, const std::string& device, const std::string& method, const json& params, const std::string& tag, Ms now);
  // A request of the gateway's for an output it let go, to a device the configuration may no longer
  // name, with the credentials and own ID it had. Its answer counts only from that device, and
  // touches nothing else this module keeps; it comes back with the tag "release".
  long long releaseRpc(const NetDeviceCfg& d, const std::string& user, const std::string& password, const std::string& ownId,
                       const std::string& method, const json& params, Ms now);
  std::vector<NetRequest> take();
  // The platform's answer: status 0 when the device was not reached. An answer for the gateway is
  // returned; the polls are taken in here.
  std::optional<NetAnswer> response(const Config& cfg, long long id, int status, const json& body, Ms now);
  // A message on an MQTT topic a role reads
  void mqttMessage(const Config& cfg, const std::string& topic, const std::string& payload, Ms now);
  // Whether the platform holds an MQTT connection. Without one, what came before is forgotten: a
  // value from before an outage counts only once it comes again (R5).
  void mqttConnected(bool on);
  // The MQTT topics roles read, with their availability topics
  std::vector<std::string> mqttTopics(const Config& cfg) const;

  // A role's value (R5: none, never a stand-in) and whether a device answers as itself
  RoleValue value(const Config& cfg, const std::string& role, Ms now) const;
  bool online(const std::string& device, Ms now) const;
  // What a Shelly's last status said of a switch: on, off, or unknown (none, or stale)
  std::optional<bool> output(const std::string& device, int channel, Ms now, bool dimmer = false) const;
  // A dimmer's brightness as last read, %; nothing without a fresh one (R5)
  std::optional<double> brightness(const std::string& device, int channel, Ms now) const;
  // The devices for the API: online, what they said they are, their last problem
  json status(const Config& cfg, Ms now) const;
  // A device's credentials: Shelly's user is always "admin"; "" as password removes them
  void setCredentials(const std::string& device, const std::string& user, const std::string& password);
  bool hasPassword(const std::string& device) const;
  std::pair<std::string, std::string> credentials(const std::string& device) const;
  // The ID a Shelly said it has; "" before it said
  std::string ownId(const std::string& device) const;
  // Another device answers at its address
  bool isOther(const std::string& device) const;
  // The file of what is kept apart from the configuration: passwords, and the ID each Shelly said
  // it has (a device is known by it; its address is only where it is reached now). Never in a
  // backup or an answer of the API (SD-132).
  json secretsJson() const;
  void secretsFromJson(const json& j);
  // Whether a Shelly's own ID was learned since the last call: the file is to be saved
  bool takeLearned();
  // A device removed, or moved to another address or kind: all it said and its credentials go
  void forget(const std::string& device);
  // What MQTT topics no role reads any more is dropped
  void pruneMqtt(const Config& cfg);

 private:
  struct Seen {
    Ms lastOk = -1;       // the last good answer, as itself
    Ms statusAt = -1;     // when its status (Shelly) was last read
    Ms nextPoll = 0;
    int gen = 0;          // a Shelly's generation; 1 is not used (SD-132)
    bool other = false;   // another device answered at its address
    Msg problem;          // why its last request failed
    json status = nullptr;                    // Shelly.GetStatus
    std::map<std::string, json> entities;     // esphome: entity → {body, at} or {error, at} of its last answer
  };
  struct Pending {
    std::string device, kind, tag;  // kind: info | status | entity | gateway | release
    Ms sent = 0;
    std::string ownId{};  // release: the device it must be
  };
  struct MqttSeen {
    std::string payload;
    Ms at = -1;
  };
  struct Credentials {
    std::string user, password;
  };
  NetRequest request(const NetDeviceCfg& d, const std::string& user, const std::string& password, const std::string& method,
                     const std::string& path, json body);
  NetRequest request(const NetDeviceCfg& d, const std::string& method, const std::string& path, json body);
  long long rpcAt(const NetDeviceCfg& d, const std::string& user, const std::string& password, const std::string& method, const json& params,
                  const std::string& tag, Ms now);
  std::map<std::string, Seen> seen_;
  std::map<long long, Pending> pending_;
  std::map<std::string, MqttSeen> mqtt_;
  std::map<std::string, Credentials> credentials_;
  std::map<std::string, std::string> ownIds_;  // device → the ID it said it has
  std::vector<NetRequest> out_;
  long long seq_ = 0;
  bool mqttUp_ = false;
  bool learned_ = false;
};

}  // namespace doseph
