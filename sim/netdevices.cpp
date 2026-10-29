// SPDX-License-Identifier: AGPL-3.0-or-later
#include "netdevices.hpp"

namespace sim {

using doseph::json;
using doseph::Ms;

namespace {

json shellySwitchDefaults(bool withInput) {
  json c = {{"name", nullptr}, {"initial_state", "restore_last"}, {"auto_on", false}, {"auto_on_delay", 60.0},
            {"auto_off", false}, {"auto_off_delay", 60.0}};
  if (withInput) c["in_mode"] = "follow";
  return c;
}

json rpcError(const json& req, const std::string& src, int code, const std::string& message) {
  return {{"id", req.value("id", 0)}, {"src", src}, {"error", {{"code", code}, {"message", message}}}};
}

}  // namespace

json FakeShelly::handle(const json& req, Ms now) {
  const std::string method = req.value("method", "");
  const json params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();
  auto result = [&](json r) { return json{{"id", req.value("id", 0)}, {"src", id}, {"result", std::move(r)}}; };
  auto sw = [&]() -> Switch* {
    const int n = params.value("id", -1);
    return n >= 0 && n < static_cast<int>(switches.size()) ? &switches[n] : nullptr;
  };
  if (method == "Shelly.GetDeviceInfo") return result({{"id", id}, {"model", model}, {"gen", gen}, {"app", "Sim"}});
  if (method == "Shelly.GetStatus") {
    json st = {{"sys", {{"uptime", now / 1000}}}};
    for (size_t i = 0; i < switches.size(); ++i) st["switch:" + std::to_string(i)] = {{"id", i}, {"output", switches[i].on}};
    for (size_t i = 0; i < inputs.size(); ++i) st["input:" + std::to_string(i)] = {{"id", i}, {"state", static_cast<bool>(inputs[i])}};
    return result(st);
  }
  if (method == "Switch.GetConfig") {
    Switch* s = sw();
    if (!s) return rpcError(req, id, -103, "Invalid argument 'id'");
    json c = s->config;
    c["id"] = params["id"];
    return result(c);
  }
  if (method == "Switch.SetConfig") {
    Switch* s = sw();
    if (!s || !params.contains("config") || !params["config"].is_object()) return rpcError(req, id, -103, "Invalid argument");
    for (const auto& [k, v] : params["config"].items())
      if (!s->config.contains(k)) return rpcError(req, id, -103, "Invalid argument '" + k + "'");
    if (!ignoresConfig)
      for (const auto& [k, v] : params["config"].items()) s->config[k] = v;
    return result({{"restart_required", false}});
  }
  if (method == "Switch.Set") {
    Switch* s = sw();
    if (!s || !params.contains("on") || !params["on"].is_boolean()) return rpcError(req, id, -103, "Invalid argument");
    const bool was = s->on;
    s->on = params["on"].get<bool>();
    if (s->on && !was) s->onSince = now;
    return result({{"was_on", was}});
  }
  return rpcError(req, id, -114, "Method " + method + " not found");
}

void FakeShelly::setInput(int n, bool state) {
  if (n < 0 || n >= static_cast<int>(inputs.size())) return;
  inputs[n] = state;
  if (n < static_cast<int>(switches.size()) && switches[n].config.value("in_mode", "") == "follow") switches[n].on = state;
}

void FakeShelly::advance(Ms now) {
  for (auto& s : switches) {
    const bool autoOff = s.config.value("auto_off", false);
    const double delayS = s.config.value("auto_off_delay", 60.0);
    if (s.on && autoOff && now - s.onSince >= static_cast<Ms>(delayS * 1000)) s.on = false;
  }
}

void FakeShelly::powerCycle() {
  for (size_t i = 0; i < switches.size(); ++i) {
    Switch& s = switches[i];
    const std::string init = s.config.value("initial_state", "off");
    if (init == "off") s.on = false;
    else if (init == "on") s.on = true;
    else if (init == "match_input") s.on = i < inputs.size() && inputs[i];
    // restore_last: as it was
  }
}

NetTwin::NetTwin() {
  FakeShelly relay;
  relay.address = "shelly-1.local";
  relay.id = "shellyplus1-sim000001";
  relay.model = "SNSW-001X16EU";
  relay.switches.push_back({false, 0, shellySwitchDefaults(true)});
  relay.inputs.push_back(false);
  shellies_.push_back(relay);
  FakeShelly plug;
  plug.address = "shelly-plug.local";
  plug.id = "shellyplusplugs-sim000002";
  plug.model = "SNPL-00112EU";
  plug.switches.push_back({false, 0, shellySwitchDefaults(false)});
  plug.circulates = true;
  shellies_.push_back(plug);
  FakeShelly old;
  old.address = "shelly-old.local";
  old.id = "shelly1-sim000003";
  old.gen = 1;
  shellies_.push_back(old);
  FakeShelly valve;
  valve.address = "shelly-valve.local";
  valve.id = "shellyplus1-sim000004";
  valve.model = "SNSW-001X16EU";
  // Set up by the installer: its input, the float switch, does not switch the valve
  json valveConfig = shellySwitchDefaults(true);
  valveConfig["in_mode"] = "detached";
  valve.switches.push_back({false, 0, valveConfig});
  valve.inputs.push_back(false);
  valve.inlet = true;
  shellies_.push_back(valve);
  FakeEsphome esp;
  esp.address = "esp-1.local";
  esp.name = "tank-sensors";
  esp.entities["binary_sensor/tank_full"] = false;
  esp.entities["sensor/water_temp"] = 21.5;
  esps_.push_back(esp);
}

std::pair<int, json> NetTwin::answer(const doseph::NetRequest& r, Ms now) {
  if (FakeShelly* s = shelly(r.address)) {
    if (!s->online || !s->reachable) return {0, nullptr};
    if (s->gen < 2 || r.path != "/rpc") return {404, nullptr};  // the first generation has no RPC
    return {200, s->handle(r.body, now)};
  }
  if (FakeEsphome* e = esphome(r.address)) {
    if (!e->online) return {0, nullptr};
    const std::string entity = r.path.size() > 1 ? r.path.substr(1) : "";
    const auto it = e->entities.find(entity);
    if (r.method != "GET" || it == e->entities.end()) return {404, nullptr};
    const std::string domain = entity.substr(0, entity.find('/'));
    const std::string object = entity.substr(entity.find('/') + 1);
    json body = {{"id", domain + "-" + object}, {"value", it->second}};
    if (it->second.is_boolean()) body["state"] = it->second.get<bool>() ? "ON" : "OFF";
    else body["state"] = it->second.dump();
    return {200, body};
  }
  return {0, nullptr};
}

void NetTwin::advance(Ms now) {
  for (auto& s : shellies_) s.advance(now);
}

FakeShelly* NetTwin::shelly(const std::string& address) {
  for (auto& s : shellies_)
    if (s.address == address) return &s;
  return nullptr;
}

FakeShelly* NetTwin::inlet() {
  for (auto& s : shellies_)
    if (s.inlet) return &s;
  return nullptr;
}

bool NetTwin::circulating() const {
  for (const auto& s : shellies_)
    if (s.circulates && s.online && !s.switches.empty() && s.switches[0].on) return true;
  return false;
}

FakeEsphome* NetTwin::esphome(const std::string& address) {
  for (auto& e : esps_)
    if (e.address == address) return &e;
  return nullptr;
}

json NetTwin::toJson() const {
  json shellies = json::array();
  for (const auto& s : shellies_) {
    json sw = json::array();
    for (const auto& x : s.switches) sw.push_back({{"on", x.on}, {"config", x.config}});
    json in = json::array();
    for (bool b : s.inputs) in.push_back(b);
    json one = {{"address", s.address}, {"gen", s.gen}, {"online", s.online}, {"switches", sw}, {"inputs", in}};
    if (s.inlet) one["inlet"] = {{"floatStuck", s.floatStuck}};
    shellies.push_back(one);
  }
  json esps = json::array();
  for (const auto& e : esps_) esps.push_back({{"address", e.address}, {"online", e.online}, {"entities", e.entities}});
  return {{"shellies", shellies}, {"esphome", esps}};
}

bool NetTwin::control(const std::string& action, const json& body) {
  const std::string address = body.value("address", "");
  if (action == "netOnline") {
    const bool on = body.value("online", true);
    if (FakeShelly* s = shelly(address)) {
      if (on && !s->online) s->powerCycle();  // back from a power loss
      s->online = on;
      return true;
    }
    if (FakeEsphome* e = esphome(address)) {
      e->online = on;
      return true;
    }
    return false;
  }
  if (action == "netReachable") {
    FakeShelly* s = shelly(address);
    if (!s) return false;
    s->reachable = body.value("reachable", true);
    return true;
  }
  if (action == "netInput") {
    FakeShelly* s = shelly(address);
    const int n = body.value("channel", 0);
    if (!s || s->inlet || n < 0 || n >= static_cast<int>(s->inputs.size())) return false;  // the float follows the tank
    s->setInput(n, body.value("state", false));
    return true;
  }
  if (action == "netFloat") {
    // The float switch at the inlet sticks, or is free again
    FakeShelly* s = inlet();
    if (!s) return false;
    s->floatStuck = body.value("stuck", false);
    return true;
  }
  if (action == "netId") {
    // Another device at the address, as after the router gave its address to another one
    FakeShelly* s = shelly(address);
    if (!s || !body.contains("id") || !body["id"].is_string()) return false;
    s->id = body["id"].get<std::string>();
    return true;
  }
  if (action == "netValue") {
    FakeEsphome* e = esphome(address);
    const std::string entity = body.value("entity", "");
    if (!e || !e->entities.count(entity) || !body.contains("value")) return false;
    e->entities[entity] = body["value"];
    return true;
  }
  return false;
}

}  // namespace sim
