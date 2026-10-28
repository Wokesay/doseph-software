// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/mqtt.hpp"

#include <cctype>
#include <cmath>

#include "doseph/messages.hpp"

namespace doseph {

namespace {

constexpr size_t kMaxField = 256;

bool plain(const std::string& s, size_t max = kMaxField) {
  if (s.size() > max) return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

// A host name or an address: letters, digits, dots, dashes and the colons of IPv6
bool hostName(const std::string& s) {
  if (s.empty() || s.size() > 253) return false;
  for (unsigned char c : s)
    if (!(std::isalnum(c) || c == '.' || c == '-' || c == ':')) return false;
  return true;
}

// A topic level of our own: no wildcard, no separator in the middle of a name
bool topicPrefix(const std::string& s) {
  if (s.empty() || s.size() > 64 || s.front() == '/' || s.back() == '/') return false;
  for (unsigned char c : s)
    if (c == '#' || c == '+' || c < 0x21 || c == 0x7f) return false;
  return true;
}

std::string u16(size_t n) { return {static_cast<char>((n >> 8) & 0xff), static_cast<char>(n & 0xff)}; }
std::string str(const std::string& s) { return u16(s.size()) + s; }

std::string remaining(size_t n) {
  std::string out;
  do {
    unsigned char b = n % 128;
    n /= 128;
    if (n > 0) b |= 0x80;
    out += static_cast<char>(b);
  } while (n > 0);
  return out;
}

std::string packet(unsigned char header, const std::string& body) { return std::string(1, static_cast<char>(header)) + remaining(body.size()) + body; }

// A canister's id as a topic level and a template key: letters, digits, "_" and "-" only
bool safeId(const std::string& s) {
  if (s.empty() || s.size() > 40) return false;
  for (unsigned char c : s)
    if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
  return true;
}

json number(const json& v) { return v.is_number() && std::isfinite(v.get<double>()) ? v : json(nullptr); }

}  // namespace

json mqttToJson(const MqttSettings& m, bool withSecrets) {
  json j = {{"enabled", m.enabled}, {"host", m.host}, {"port", m.port}, {"tls", m.tls}, {"user", m.user}, {"commands", m.commands}, {"discovery", m.discovery}};
  if (withSecrets) j["password"] = m.password;
  else j["passwordSet"] = !m.password.empty();
  return j;
}

MqttSettings mqttFromJson(const json& j) {
  MqttSettings m;
  if (!j.is_object()) return m;
  // Read through the same checks as a change: a damaged file gives no connection
  return applyMqtt(m, j).empty() ? m : MqttSettings{};
}

std::vector<Msg> applyMqtt(MqttSettings& m, const json& change) {
  if (!change.is_object()) return {say("api.bad_input")};
  MqttSettings n = m;
  std::vector<Msg> errors;
  auto text = [&](const char* key, std::string& into) {
    if (!change.contains(key)) return;
    if (!change[key].is_string() || !plain(change[key].get<std::string>())) return errors.push_back(say("cfg.mqtt.text", {{"field", key}}));
    into = change[key].get<std::string>();
  };
  auto flag = [&](const char* key, bool& into) {
    if (!change.contains(key)) return;
    if (!change[key].is_boolean()) return errors.push_back(say("api.bad_input"));
    into = change[key].get<bool>();
  };
  flag("enabled", n.enabled);
  flag("tls", n.tls);
  flag("commands", n.commands);
  text("host", n.host);
  text("user", n.user);
  text("password", n.password);
  text("discovery", n.discovery);
  if (change.contains("port")) {
    if (!change["port"].is_number_integer()) errors.push_back(say("cfg.mqtt.port"));
    else n.port = change["port"].get<int>();
  }
  if (!errors.empty()) return errors;
  if ((n.enabled || !n.host.empty()) && !hostName(n.host)) errors.push_back(say("cfg.mqtt.host"));
  if (n.port < 1 || n.port > 65535) errors.push_back(say("cfg.mqtt.port"));
  if (!topicPrefix(n.discovery)) errors.push_back(say("cfg.mqtt.discovery"));
  // Commands only over TLS with a user (PD-461)
  if (n.commands && (!n.tls || n.user.empty())) errors.push_back(say("cfg.mqtt.commands"));
  if (errors.empty()) m = n;
  return errors;
}

bool mqttCommands(const MqttSettings& m) { return m.enabled && m.commands && m.tls && !m.user.empty(); }

std::string mqttBase(const std::string& deviceId) { return "doseph/" + deviceId; }
std::string mqttAvailability(const std::string& deviceId) { return mqttBase(deviceId) + "/availability"; }

std::vector<std::string> mqttCommandTopics(const std::string& deviceId) {
  const std::string b = mqttBase(deviceId);
  return {b + "/stop/set", b + "/recipe/set", b + "/ec_target/set", b + "/ph_target/set"};
}

std::vector<MqttMessage> mqttDiscovery(const std::string& deviceId, const json& info, const Config& cfg, const MqttSettings& m) {
  const std::string base = mqttBase(deviceId);
  const std::string node = "doseph_" + deviceId;
  const json profile = info.value("profile", json::object());
  const json device = {{"identifiers", json::array({node})},
                       {"name", cfg.system.name},
                       {"manufacturer", "Doseph"},
                       {"model", jstr(profile, "variant", "Doseph")},
                       {"sw_version", jstr(info, "version")}};
  std::vector<MqttMessage> out;
  auto entity = [&](const std::string& component, const std::string& object, json conf) {
    conf["unique_id"] = node + "_" + object;
    conf["object_id"] = node + "_" + object;
    conf["device"] = device;
    conf["availability_topic"] = mqttAvailability(deviceId);
    out.push_back({m.discovery + "/" + component + "/" + node + "/" + object + "/config", conf.dump(), true});
  };
  auto removed = [&](const std::string& component, const std::string& object) {
    out.push_back({m.discovery + "/" + component + "/" + node + "/" + object + "/config", "", true});
  };
  const std::string state = base + "/state";
  auto sensor = [&](const std::string& object, const std::string& name, const std::string& field, json extra) {
    json conf = {{"name", name}, {"state_topic", state}, {"value_template", "{{ value_json." + field + " }}"}};
    for (auto& [k, v] : extra.items()) conf[k] = v;
    entity("sensor", object, conf);
  };
  sensor("ph", "pH", "ph", {{"device_class", "ph"}, {"state_class", "measurement"}, {"suggested_display_precision", 2}});
  sensor("ec", "EC", "ec", {{"device_class", "conductivity"}, {"unit_of_measurement", "mS/cm"}, {"state_class", "measurement"}, {"suggested_display_precision", 2}});
  sensor("temperature", "Temperature", "temp", {{"device_class", "temperature"}, {"unit_of_measurement", "°C"}, {"state_class", "measurement"}, {"suggested_display_precision", 1}});
  sensor("phase", "Cycle", "phase", json::object());
  sensor("recipe", "Recipe in force", "recipe", json::object());
  sensor("ec_target_now", "EC target", "ecTarget", {{"unit_of_measurement", "mS/cm"}});
  sensor("ph_target_now", "pH target", "phTarget", json::object());
  for (const auto& b : cfg.canisters)
    if (safeId(b.id))
      sensor("canister_" + b.id, b.name, "canisters['" + b.id + "']",
           {{"device_class", "volume_storage"}, {"unit_of_measurement", "mL"}, {"state_class", "measurement"}, {"suggested_display_precision", 0}});
  auto binary = [&](const std::string& object, const std::string& name, const std::string& field, json extra) {
    json conf = {{"name", name}, {"state_topic", state}, {"value_template", "{{ 'ON' if value_json." + field + " else 'OFF' }}"}};
    for (auto& [k, v] : extra.items()) conf[k] = v;
    entity("binary_sensor", object, conf);
  };
  binary("flow", "Flow proven", "flowOk", {{"device_class", "running"}});
  binary("stopped", "Stopped", "stopped", json::object());
  binary("problem", "Alarm", "problem", {{"device_class", "problem"}});
  // Commands: only over TLS with a user (PD-461); otherwise their entities are removed
  if (mqttCommands(m)) {
    entity("button", "stop", {{"name", "Stop"}, {"command_topic", base + "/stop/set"}, {"payload_press", "PRESS"}});
    json options = json::array();
    for (const auto& r : cfg.recipes) options.push_back(r.name);
    entity("select", "recipe_set", {{"name", "Recipe"}, {"command_topic", base + "/recipe/set"}, {"state_topic", state},
                                    {"value_template", "{{ value_json.recipe }}"}, {"options", options}});
    entity("number", "ec_target", {{"name", "EC target"}, {"command_topic", base + "/ec_target/set"}, {"state_topic", state},
                                   {"value_template", "{{ value_json.ecTarget }}"}, {"min", 0.1}, {"max", 5.0}, {"step", 0.05},
                                   {"unit_of_measurement", "mS/cm"}, {"mode", "box"}});
    entity("number", "ph_target", {{"name", "pH target"}, {"command_topic", base + "/ph_target/set"}, {"state_topic", state},
                                   {"value_template", "{{ value_json.phTarget }}"}, {"min", 4.5}, {"max", 7.5}, {"step", 0.05}, {"mode", "box"}});
  } else {
    removed("button", "stop");
    removed("select", "recipe_set");
    removed("number", "ec_target");
    removed("number", "ph_target");
  }
  return out;
}

MqttMessage mqttState(const std::string& deviceId, const json& state, const Config& cfg) {
  const json readings = state.value("readings", json::object());
  auto reading = [&](const char* k) {
    const json r = readings.value(k, json::object());
    return r.value("usable", false) ? number(r.value("value", json())) : json(nullptr);
  };
  const json plan = state.value("plan", json::object());
  const RecipeCfg* r = plan.contains("recipe") && plan["recipe"].is_string() ? cfg.recipe(plan["recipe"].get<std::string>()) : nullptr;
  bool problem = false;
  for (const auto& a : state.value("alarms", json::array())) problem |= jstr(a, "level") == "critical" || jstr(a, "level") == "warning";
  json canisters = json::object();
  const json stock = state.value("stock", json::object());
  for (const auto& b : cfg.canisters)
    if (safeId(b.id)) canisters[b.id] = stock.contains(b.id) ? number(stock[b.id].value("ml", json())) : json(nullptr);
  const json body = {{"ph", reading("ph")},
                     {"ec", reading("ec")},
                     {"temp", reading("temp")},
                     {"flowOk", state.value("flowOk", false)},
                     {"stopped", state.value("stopped", false)},
                     {"phase", state.contains("cycle") ? state["cycle"].value("phase", "") : ""},
                     {"recipe", r ? json(r->name) : json(nullptr)},
                     {"ecTarget", number(plan.value("ecTarget", json()))},
                     {"phTarget", number(plan.value("phTarget", json()))},
                     {"problem", problem},
                     {"canisters", canisters}};
  return {mqttBase(deviceId) + "/state", body.dump(), true};
}

std::string mqttConnectPacket(const std::string& clientId, const std::string& user, const std::string& password,
                              const std::string& willTopic, const std::string& willPayload, int keepAliveS) {
  unsigned char flags = 0x02;  // a clean session
  if (!willTopic.empty()) flags |= 0x04 | 0x20;  // a will, retained, QoS 0
  if (!user.empty()) flags |= 0x80;
  if (!user.empty() && !password.empty()) flags |= 0x40;
  std::string body = str("MQTT") + std::string(1, 4) + std::string(1, static_cast<char>(flags)) + u16(static_cast<size_t>(keepAliveS)) + str(clientId);
  if (!willTopic.empty()) body += str(willTopic) + str(willPayload);
  if (!user.empty()) body += str(user);
  if (!user.empty() && !password.empty()) body += str(password);
  return packet(0x10, body);
}

std::string mqttPublishPacket(const std::string& topic, const std::string& payload, bool retain) {
  return packet(static_cast<unsigned char>(0x30 | (retain ? 1 : 0)), str(topic) + payload);
}

std::string mqttSubscribePacket(std::uint16_t id, const std::vector<std::string>& topics) {
  std::string body = u16(id);
  for (const auto& t : topics) body += str(t) + std::string(1, 0);  // QoS 0
  return packet(0x82, body);
}

std::string mqttPingPacket() { return std::string("\xc0\x00", 2); }
std::string mqttDisconnectPacket() { return std::string("\xe0\x00", 2); }

std::optional<MqttPacket> MqttReader::next() {
  if (broken_ || buf_.size() < 2) return std::nullopt;
  size_t len = 0, mult = 1, pos = 1;
  while (true) {
    if (pos >= buf_.size()) return std::nullopt;
    const auto b = static_cast<unsigned char>(buf_[pos++]);
    len += (b & 0x7f) * mult;
    if (!(b & 0x80)) break;
    mult *= 128;
    if (pos > 4) {
      broken_ = true;
      return std::nullopt;
    }
  }
  if (len > kMaxPacket) {
    broken_ = true;
    return std::nullopt;
  }
  if (buf_.size() < pos + len) return std::nullopt;
  const auto header = static_cast<unsigned char>(buf_[0]);
  const std::string body = buf_.substr(pos, len);
  buf_.erase(0, pos + len);
  MqttPacket p;
  p.type = header >> 4;
  if (p.type == 2) {
    if (body.size() != 2) broken_ = true;
    else p.returnCode = static_cast<unsigned char>(body[1]);
  } else if (p.type == 3) {
    if (body.size() < 2) {
      broken_ = true;
      return std::nullopt;
    }
    const size_t tl = static_cast<size_t>(static_cast<unsigned char>(body[0])) << 8 | static_cast<unsigned char>(body[1]);
    size_t at = 2 + tl;
    if ((header & 0x06) != 0) at += 2;  // QoS 1 or 2: a packet id follows the topic
    if (at > body.size()) {
      broken_ = true;
      return std::nullopt;
    }
    p.topic = body.substr(2, tl);
    p.payload = body.substr(at);
  }
  return p;
}

}  // namespace doseph
