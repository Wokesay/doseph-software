// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/devices.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "doseph/config.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

const RoleBinding* bindingOf(const Config& cfg, const std::string& role) {
  const auto it = cfg.roles.find(role);
  return it == cfg.roles.end() ? nullptr : &it->second;
}

}  // namespace

const std::vector<RoleDef>& roleDefs() {
  static const std::vector<RoleDef> defs = {
      {"refill_valve", "output", true, 25 * kMinute},  // RAT-079: the longest fill
      {"circulation_pump", "output", false, 0},        // runs for hours: no auto-off (RAT-019)
      {"tank_full", "binary", false, 0},               // the float switch in the tank (PD-460)
  };
  return defs;
}

const RoleDef* roleDef(const std::string& id) {
  for (const auto& r : roleDefs())
    if (r.id == id) return &r;
  return nullptr;
}

int autoOffS(const RoleDef& r) {
  if (!r.pulse || r.maxRunMs <= 0) return 0;
  const double s = 1.11 * static_cast<double>(r.maxRunMs) / 1000.0;
  return static_cast<int>(std::ceil(s / 60.0 - 1e-9)) * 60;
}

json bindingToJson(const RoleBinding& b) {
  return {{"source", b.source},  {"device", b.device}, {"channel", b.channel},           {"entity", b.entity},
          {"field", b.field},    {"invert", b.invert}, {"availability", b.availability}};
}

RoleBinding bindingFromJson(const json& j) {
  if (!j.is_object()) throw std::invalid_argument("a role's binding is not an object");
  RoleBinding b;
  b.source = jstr(j, "source");
  b.device = jstr(j, "device");
  if (j.contains("channel") && !j["channel"].is_null()) {
    if (!j["channel"].is_number_integer()) throw std::invalid_argument("a role's channel is not a whole number");
    b.channel = j["channel"].get<int>();
  }
  b.entity = jstr(j, "entity");
  b.field = jstr(j, "field");
  b.invert = jbool(j, "invert", false);
  b.availability = jstr(j, "availability");
  return b;
}

json deviceToJson(const NetDeviceCfg& d) { return {{"id", d.id}, {"kind", d.kind}, {"address", d.address}, {"name", d.name}}; }

NetDeviceCfg deviceFromJson(const json& j) {
  if (!j.is_object()) throw std::invalid_argument("a device is not an object");
  return {jstr(j, "id"), jstr(j, "kind"), jstr(j, "address"), jstr(j, "name")};
}

json shellySwitchConfig(const RoleDef& r, const json& current) {
  json c = {{"initial_state", "off"}, {"auto_on", false}, {"auto_off", r.pulse}};
  if (r.pulse) c["auto_off_delay"] = autoOffS(r);
  // A device with an input of its own must not switch the output by it: the robot reads the input
  if (current.is_object() && current.contains("in_mode")) c["in_mode"] = "detached";
  return c;
}

bool shellyConfigFits(const json& written, const json& readBack) {
  if (!written.is_object() || !readBack.is_object()) return false;
  for (const auto& [k, v] : written.items()) {
    if (!readBack.contains(k)) return false;
    const json& got = readBack[k];
    if (v.is_number()) {
      if (!got.is_number() || std::fabs(got.get<double>() - v.get<double>()) > 0.5) return false;
    } else if (got != v) {
      return false;
    }
  }
  return true;
}

std::optional<bool> binaryOf(const json& v) {
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number_integer()) {
    const auto n = v.get<long long>();
    if (n == 0 || n == 1) return n == 1;
    return std::nullopt;
  }
  if (v.is_string()) {
    const std::string s = lower(v.get<std::string>());
    if (s == "on" || s == "true" || s == "1" || s == "yes") return true;
    if (s == "off" || s == "false" || s == "0" || s == "no") return false;
  }
  return std::nullopt;
}

std::optional<json> mqttValueOf(const std::string& payload, const std::string& field) {
  json j = json::parse(payload, nullptr, false);
  if (field.empty()) {
    if (j.is_discarded() || j.is_object() || j.is_array()) return json(payload);
    return j;
  }
  if (!j.is_object()) return std::nullopt;
  // A dotted field reaches into nested objects: "sensor.temp"
  const json* at = &j;
  size_t from = 0;
  while (true) {
    const size_t dot = field.find('.', from);
    const std::string key = field.substr(from, dot == std::string::npos ? std::string::npos : dot - from);
    if (!at->is_object() || !at->contains(key)) return std::nullopt;
    at = &(*at)[key];
    if (dot == std::string::npos) break;
    from = dot + 1;
  }
  if (at->is_object() || at->is_array() || at->is_null()) return std::nullopt;
  return *at;
}

// ---------------------------------------------------------------- NetDevices

NetRequest NetDevices::request(const Config& cfg, const NetDeviceCfg& d, const std::string& method, const std::string& path, json body) {
  (void)cfg;
  NetRequest r;
  r.id = ++seq_;
  r.device = d.id;
  r.address = d.address;
  r.method = method;
  r.path = path;
  r.body = std::move(body);
  if (const auto it = credentials_.find(d.id); it != credentials_.end()) {
    r.user = d.kind == "shelly" ? "admin" : it->second.user;
    r.password = it->second.password;
  }
  return r;
}

std::vector<NetAnswer> NetDevices::tick(const Config& cfg, Ms now) {
  std::vector<NetAnswer> failed;
  // A request not answered in time fails, and its device says so
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (now - it->second.sent <= kNetTimeoutMs) {
      ++it;
      continue;
    }
    seen_[it->second.device].problem = say("device.no_answer");
    if (it->second.kind == "gateway") failed.push_back({it->second.device, it->second.tag, false, nullptr});
    it = pending_.erase(it);
  }
  for (const auto& d : cfg.devices) {
    Seen& s = seen_[d.id];
    if (now < s.nextPoll) continue;
    const bool busy = std::any_of(pending_.begin(), pending_.end(), [&](const auto& p) { return p.second.device == d.id && p.second.kind != "gateway"; });
    if (busy) continue;
    s.nextPoll = now + kNetPollMs;
    if (d.kind == "shelly") {
      // Who it is first: only the second generation and later are used (SD-132)
      const std::string method = s.gen == 0 ? "Shelly.GetDeviceInfo" : "Shelly.GetStatus";
      NetRequest r = request(cfg, d, "POST", "/rpc", {{"id", seq_ + 1}, {"method", method}});
      pending_[r.id] = {d.id, s.gen == 0 ? "info" : "status", "", now};
      out_.push_back(std::move(r));
    } else if (d.kind == "esphome") {
      for (const auto& [role, b] : cfg.roles) {
        if (b.source != "esphome" || b.device != d.id || b.entity.empty()) continue;
        NetRequest r = request(cfg, d, "GET", "/" + b.entity, nullptr);
        pending_[r.id] = {d.id, "entity", b.entity, now};
        out_.push_back(std::move(r));
      }
    }
  }
  return failed;
}

long long NetDevices::rpc(const Config& cfg, const std::string& device, const std::string& method, const json& params, const std::string& tag, Ms now) {
  const auto d = std::find_if(cfg.devices.begin(), cfg.devices.end(), [&](const NetDeviceCfg& x) { return x.id == device; });
  if (d == cfg.devices.end()) return 0;
  NetRequest r = request(cfg, *d, "POST", "/rpc", {{"id", seq_ + 1}, {"method", method}, {"params", params}});
  const long long id = r.id;
  pending_[id] = {device, "gateway", tag, now};
  out_.push_back(std::move(r));
  return id;
}

std::vector<NetRequest> NetDevices::take() {
  std::vector<NetRequest> out;
  out.swap(out_);
  return out;
}

std::optional<NetAnswer> NetDevices::response(const Config& cfg, long long id, int status, const json& body, Ms now) {
  (void)cfg;
  const auto it = pending_.find(id);
  if (it == pending_.end()) return std::nullopt;  // late, after its time ran out
  const Pending p = it->second;
  pending_.erase(it);
  Seen& s = seen_[p.device];
  auto fail = [&](Msg why) -> std::optional<NetAnswer> {
    s.problem = std::move(why);
    if (p.kind == "gateway") return NetAnswer{p.device, p.tag, false, nullptr};
    return std::nullopt;
  };
  if (status == 0) return fail(say("device.unreachable"));
  if (status == 401) return fail(say("device.auth"));
  if (status == 404 && p.kind == "info") return fail(say("device.shelly_gen1"));  // no RPC: the first generation
  if (status < 200 || status >= 300) return fail(say("device.http", {{"status", status}}));
  if (p.kind == "entity") {
    // ESPHome's web server: {"id": "binary_sensor-float", "value": true, "state": "ON"}
    if (!body.is_object()) return fail(say("device.unreadable"));
    s.entities[p.tag] = {{"body", body}, {"at", now}};
    s.lastOk = now;
    s.problem = {};
    return std::nullopt;
  }
  // Shelly's RPC: {"id", "src", "result"} or {"id", "src", "error": {"code", "message"}}
  if (!body.is_object()) return fail(say("device.unreadable"));
  if (body.contains("error")) return fail(say("device.rpc_error", {{"message", jstr(body["error"], "message")}}));
  const json result = body.contains("result") ? body["result"] : json(nullptr);
  if (p.kind == "info") {
    s.ownId = jstr(result, "id");
    s.gen = result.is_object() && result.contains("gen") && result["gen"].is_number_integer() ? result["gen"].get<int>() : 1;
    if (s.gen < 2) return fail(say("device.shelly_gen1"));
    s.lastOk = now;
    s.problem = {};
    s.nextPoll = now;  // its status at once
    return std::nullopt;
  }
  if (s.gen < 2) return fail(say("device.shelly_gen1"));
  s.lastOk = now;
  s.problem = {};
  if (p.kind == "status") {
    s.status = result;
    return std::nullopt;
  }
  return NetAnswer{p.device, p.tag, true, result};
}

void NetDevices::mqttMessage(const Config& cfg, const std::string& topic, const std::string& payload, Ms now) {
  const auto topics = mqttTopics(cfg);
  if (std::find(topics.begin(), topics.end(), topic) == topics.end()) return;
  mqtt_[topic] = {payload, now};
}

std::vector<std::string> NetDevices::mqttTopics(const Config& cfg) const {
  std::vector<std::string> out;
  for (const auto& [role, b] : cfg.roles) {
    if (b.source != "mqtt") continue;
    if (!b.entity.empty()) out.push_back(b.entity);
    if (!b.availability.empty()) out.push_back(b.availability);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool NetDevices::online(const std::string& device, Ms now) const {
  const auto it = seen_.find(device);
  if (it == seen_.end() || it->second.lastOk < 0) return false;
  return now - it->second.lastOk <= kNetStaleMs;
}

std::optional<bool> NetDevices::output(const std::string& device, int channel) const {
  const auto it = seen_.find(device);
  if (it == seen_.end() || !it->second.status.is_object()) return std::nullopt;
  const std::string key = "switch:" + std::to_string(channel);
  if (!it->second.status.contains(key)) return std::nullopt;
  const json& sw = it->second.status[key];
  if (!sw.is_object() || !sw.contains("output") || !sw["output"].is_boolean()) return std::nullopt;
  return sw["output"].get<bool>();
}

RoleValue NetDevices::value(const Config& cfg, const std::string& role, Ms now) const {
  RoleValue v;
  const RoleBinding* b = bindingOf(cfg, role);
  const RoleDef* def = roleDef(role);
  if (!b || !def) {
    v.state = "not_assigned";
    return v;
  }
  std::optional<json> raw;
  if (b->source == "mqtt") {
    const auto avail = b->availability.empty() ? mqtt_.end() : mqtt_.find(b->availability);
    if (!mqttUp_ || (!b->availability.empty() && (avail == mqtt_.end() || lower(avail->second.payload) != "online"))) {
      v.state = "offline";
      return v;
    }
    const auto it = mqtt_.find(b->entity);
    if (it == mqtt_.end()) {
      v.state = "waiting";
      return v;
    }
    v.at = it->second.at;
    raw = mqttValueOf(it->second.payload, b->field);
  } else {
    if (!online(b->device, now)) {
      v.state = "offline";
      return v;
    }
    const Seen& s = seen_.at(b->device);
    if (b->source == "shelly") {
      const std::string key = "input:" + std::to_string(b->channel);
      v.at = s.lastOk;
      if (s.status.is_object() && s.status.contains(key) && s.status[key].is_object() && s.status[key].contains("state"))
        raw = s.status[key]["state"];
    } else {
      const auto it = s.entities.find(b->entity);
      if (it == s.entities.end()) {
        v.state = "waiting";
        return v;
      }
      v.at = jnum(it->second, "at") >= 0 ? static_cast<Ms>(jnum(it->second, "at")) : 0;
      if (now - v.at > kNetStaleMs) {
        v.state = "stale";
        return v;
      }
      const json& e = it->second["body"];
      if (e.contains("value") && !e["value"].is_null()) raw = e["value"];
      else if (e.contains("state")) raw = e["state"];
    }
  }
  if (!raw) {
    v.state = "unreadable";
    return v;
  }
  if (def->kind == "binary") {
    const auto bit = binaryOf(*raw);
    if (!bit) {
      v.state = "unreadable";
      return v;
    }
    v.value = json(b->invert ? !*bit : *bit);
  } else {
    if (!raw->is_number()) {
      v.state = "unreadable";
      return v;
    }
    v.value = *raw;
  }
  v.state = "ok";
  return v;
}

json NetDevices::status(const Config& cfg, Ms now) const {
  json out = json::array();
  for (const auto& d : cfg.devices) {
    json j = deviceToJson(d);
    const auto it = seen_.find(d.id);
    j["online"] = online(d.id, now);
    j["hasPassword"] = hasPassword(d.id);
    j["ownId"] = nullptr;
    j["problem"] = nullptr;
    j["switches"] = json::array();
    j["inputs"] = json::array();
    if (it != seen_.end()) {
      const Seen& s = it->second;
      if (!s.ownId.empty()) j["ownId"] = s.ownId;
      if (!s.problem.key.empty()) j["problem"] = s.problem;
      if (s.status.is_object())
        for (const auto& [k, v] : s.status.items()) {
          if (!v.is_object()) continue;
          if (k.rfind("switch:", 0) == 0 && v.contains("output"))
            j["switches"].push_back({{"channel", std::atoi(k.c_str() + 7)}, {"on", v["output"]}});
          if (k.rfind("input:", 0) == 0 && v.contains("state"))
            j["inputs"].push_back({{"channel", std::atoi(k.c_str() + 6)}, {"state", v["state"]}});
        }
    }
    out.push_back(j);
  }
  return out;
}

void NetDevices::setCredentials(const std::string& device, const std::string& user, const std::string& password) {
  if (password.empty()) credentials_.erase(device);
  else credentials_[device] = {user, password};
}

bool NetDevices::hasPassword(const std::string& device) const { return credentials_.count(device) > 0; }

json NetDevices::secretsJson() const {
  json j = json::object();
  for (const auto& [d, c] : credentials_) j[d] = {{"user", c.user}, {"password", c.password}};
  return j;
}

void NetDevices::secretsFromJson(const json& j) {
  credentials_.clear();
  if (!j.is_object()) return;
  for (const auto& [d, v] : j.items()) {
    const std::string p = jstr(v, "password");
    if (!p.empty()) credentials_[d] = {jstr(v, "user"), p};
  }
}

void NetDevices::forget(const std::string& device) {
  seen_.erase(device);
  credentials_.erase(device);
  for (auto it = pending_.begin(); it != pending_.end();)
    it = it->second.device == device ? pending_.erase(it) : std::next(it);
}

}  // namespace doseph
