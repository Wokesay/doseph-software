// SPDX-License-Identifier: AGPL-3.0-or-later
#include "netdevices.hpp"

#include <algorithm>
#include <cmath>

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

json shellyLightDefaults() {
  return {{"name", nullptr}, {"initial_state", "restore_last"}, {"auto_on", false}, {"auto_on_delay", 60.0},
          {"auto_off", false}, {"auto_off_delay", 60.0}, {"in_mode", "dim"}};
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
    for (size_t i = 0; i < lights.size(); ++i) st["light:" + std::to_string(i)] = {{"id", i}, {"output", lights[i].on}, {"brightness", lights[i].brightness}};
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
  // A dimmer (SD-135)
  auto li = [&]() -> Light* {
    const int n = params.value("id", -1);
    return n >= 0 && n < static_cast<int>(lights.size()) ? &lights[n] : nullptr;
  };
  if (method == "Light.GetConfig") {
    Light* l = li();
    if (!l) return rpcError(req, id, -103, "Invalid argument 'id'");
    json c = l->config;
    c["id"] = params["id"];
    return result(c);
  }
  if (method == "Light.SetConfig") {
    Light* l = li();
    if (!l || !params.contains("config") || !params["config"].is_object()) return rpcError(req, id, -103, "Invalid argument");
    for (const auto& [k, v] : params["config"].items())
      if (!l->config.contains(k)) return rpcError(req, id, -103, "Invalid argument '" + k + "'");
    if (!ignoresConfig)
      for (const auto& [k, v] : params["config"].items()) l->config[k] = v;
    return result({{"restart_required", false}});
  }
  if (method == "Light.Set") {
    Light* l = li();
    if (!l || (params.contains("on") && !params["on"].is_boolean()) || (params.contains("brightness") && !params["brightness"].is_number()))
      return rpcError(req, id, -103, "Invalid argument");
    ++l->sets;
    if (params.contains("on")) l->on = params["on"].get<bool>();
    if (params.contains("brightness")) l->brightness = std::clamp(params["brightness"].get<double>(), 0.0, 100.0);
    return result(json::object());
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
  for (auto& l : lights) {
    const std::string init = l.config.value("initial_state", "off");
    if (init == "off") l.on = false;
    else if (init == "on") l.on = true;
  }
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
  FakeShelly dimmer;  // a dimmer on the lamps' 0–10 V line (SD-135)
  dimmer.address = "shelly-dimmer.local";
  dimmer.id = "shellyplus010v-sim000006";
  dimmer.model = "SNDM-00100WW";
  dimmer.lights.push_back({false, 100, shellyLightDefaults()});
  dimmer.inputs.push_back(false);
  shellies_.push_back(dimmer);
  FakeShelly pumps;
  pumps.address = "shelly-pump.local";
  pumps.id = "shellypro2-sim000005";
  pumps.model = "SPSW-202XE16EU";
  for (int i = 0; i < 2; ++i) pumps.switches.push_back({false, 0, shellySwitchDefaults(false)});
  pumps.waters = true;
  shellies_.push_back(pumps);
  FakeShelly room;  // a grow room's devices (SD-136)
  room.address = "shelly-climate.local";
  room.id = "shellypro4pm-sim000007";
  room.model = "SPSW-104PE16EU";
  // It measures power, so each output keeps a power limit (W)
  json metered = shellySwitchDefaults(false);
  metered["power_limit"] = 4480;
  for (int i = 0; i < 4; ++i) room.switches.push_back({false, 0, metered});
  shellies_.push_back(room);
  FakeShelly room2;
  room2.address = "shelly-climate2.local";
  room2.id = "shellyplus2pm-sim000008";
  room2.model = "SNSW-102P16EU";
  for (int i = 0; i < 2; ++i) room2.switches.push_back({false, 0, shellySwitchDefaults(false)});
  shellies_.push_back(room2);
  FakeEsphome air;
  air.address = "esp-climate.local";
  air.name = "room-sensors";
  air.entities["sensor/air_temp"] = 22.0;
  air.entities["sensor/air_rh"] = 60.0;
  air.entities["sensor/co2"] = 600.0;
  esps_.push_back(air);
  FakeEsphome esp;
  esp.address = "esp-1.local";
  esp.name = "tank-sensors";
  esp.entities["binary_sensor/tank_full"] = false;
  esp.entities["sensor/water_temp"] = 21.5;
  esp.entities["binary_sensor/tank_empty"] = false;
  esp.entities["sensor/tank_level"] = 100.0;
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
    if (s.address == address || std::find(s.aliases.begin(), s.aliases.end(), address) != s.aliases.end()) return &s;
  return nullptr;
}

FakeShelly* NetTwin::inlet() {
  for (auto& s : shellies_)
    if (s.inlet) return &s;
  return nullptr;
}

int NetTwin::watering() const {
  int n = 0;
  for (const auto& s : shellies_)
    if (s.waters && s.online)
      for (const auto& sw : s.switches) n += sw.on;
  return n;
}

FakeEsphome* NetTwin::sensors() { return esphome("esp-1.local"); }

void NetTwin::climate(Ms dt) {
  // The air outside and the room's own losses through its walls; the exhaust fan changes the air
  // in about 5 min (assumptions)
  constexpr double kOutC = 18, kOutRh = 55, kOutPpm = 420;
  constexpr double kWalls = 1.0 / 1800, kExhaust = 1.0 / 300;
  auto running = [&](const char* address, int n) {
    const FakeShelly* s = shelly(address);
    return s && s->online && n < static_cast<int>(s->switches.size()) && s->switches[n].on;
  };
  const FakeShelly* d = shelly("shelly-dimmer.local");
  const double lamps = d && d->online && !d->lights.empty() && d->lights[0].on ? d->lights[0].brightness / 100.0 : 0.0;
  const bool exhaust = running("shelly-climate.local", 0), heater = running("shelly-climate.local", 2);
  const bool dehumidifier = running("shelly-climate.local", 3), humidifier = running("shelly-climate2.local", 0);
  const bool valve = running("shelly-climate2.local", 1);
  const double k = kWalls + (exhaust ? kExhaust : 0);
  const double s = static_cast<double>(dt) / 1000.0;
  Room& r = room_;
  // Lamps at full brightness warm a closed room by 14 °C, the heater by 12 °C; the plants wet the air
  // more with light, the humidifier and dehumidifier move it by 25 %; plants take up CO2 with light,
  // and the valve gives 3 ppm/s (assumptions)
  r.tempC += s * ((kOutC - r.tempC) * k + lamps * 14 * kWalls + (heater ? 12 * kWalls : 0));
  r.rhPct += s * ((kOutRh - r.rhPct) * k + (8 + 12 * lamps) * kWalls + (humidifier ? 25 * kWalls : 0) - (dehumidifier ? 25 * kWalls : 0));
  r.rhPct = std::clamp(r.rhPct, 5.0, 100.0);
  r.co2Ppm += s * ((kOutPpm - r.co2Ppm) * k - 0.3 * lamps + (valve ? 3.0 : 0));
  r.co2Ppm = std::max(r.co2Ppm, 0.0);
  if (FakeEsphome* e = esphome("esp-climate.local")) {
    e->entities["sensor/air_temp"] = std::round(r.tempC * 10.0) / 10.0;
    e->entities["sensor/air_rh"] = std::round(r.rhPct);
    e->entities["sensor/co2"] = std::round(r.co2Ppm);
  }
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
  if (action == "climateAir") {
    // The grow room's air set, from where it goes on by its own (SD-136)
    for (const auto& [key, field] : {std::pair{"tempC", &room_.tempC}, std::pair{"rhPct", &room_.rhPct}, std::pair{"co2Ppm", &room_.co2Ppm}})
      if (body.contains(key) && body[key].is_number()) *field = body[key].get<double>();
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
