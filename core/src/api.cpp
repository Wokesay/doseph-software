// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/api.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

#include "doseph/messages.hpp"
#include "doseph/plants.hpp"

namespace doseph {

namespace {

std::vector<std::string> split(const std::string& path) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : path) {
    if (c == '/') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

ApiResponse jsonResp(int status, const json& body) {
  ApiResponse r;
  r.status = status;
  r.body = body.dump();
  return r;
}

ApiResponse fromResult(const Result& r) { return jsonResp(r.status, r.body); }

ApiResponse fail(int status, const Msg& m) { return jsonResp(status, {{"error", m}}); }

// The series a request names, separated by commas: known ids only, none twice.
std::optional<Msg> parseSeries(const std::string& list, std::vector<std::string>& out) {
  out.clear();
  size_t pos = 0;
  while (pos <= list.size()) {
    const size_t comma = list.find(',', pos);
    const std::string s = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (s != "ph" && s != "ec" && s != "temp" && s != "flow") return say("history.series_id");
    if (std::find(out.begin(), out.end(), s) != out.end()) return say("history.series_twice", {{"id", s}});
    out.push_back(s);
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return std::nullopt;
}

Epoch qnum(const ApiRequest& r, const char* k, Epoch def) {
  auto it = r.query.find(k);
  if (it == r.query.end() || it->second.empty()) return def;
  try {
    return std::stoll(it->second);
  } catch (...) {
    return def;
  }
}

std::string sessionCookie(const std::string& token, int maxAge) {
  return "dp_session=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(maxAge);
}
// The browser keeps the cookie as long as the robot keeps an unused session
constexpr int kCookieMaxAgeS = static_cast<int>(Auth::kSessionIdle / kSecond);

int channelOf(const std::string& s) {
  if (s.empty() || s.size() > 2 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return 0;
  return std::stoi(s);
}

}  // namespace

std::optional<Role> Api::access(const ApiRequest& req) {
  std::lock_guard<std::recursive_mutex> l(robot_.mutex());
  const Ms now = clock_.nowMs();
  if (req.token.empty()) return std::nullopt;
  const auto role = robot_.auth().access(req.token, now);
  if (role) robot_.auth().seen(req.client, now);  // a phone that stays signed in keeps its own count
  return role;
}

bool permitted(Role role, const std::string& method, const std::vector<std::string>& p) {
  if (role == Role::Owner) return true;
  const std::string first = p.empty() ? "" : p[0];
  // A factory reset is the owner's alone, with the password (SD-139)
  if (role == Role::App)
    return first != "tokens" && !(first == "auth" && p.size() > 1 && p[1] == "password") && !(first == "system" && p.size() > 1 && p[1] == "factory-reset");
  if (method == "GET") return first != "tokens";
  // The coupled device's calls (SD-131); the robot checks that the token is the one made for it
  if (role == Role::Peer) return method == "POST" && p.size() == 2 && ((first == "coupling" && p[1] == "leave") || first == "peer");
  if (role != Role::Account) return false;
  const size_t n = p.size();
  if (method == "POST" && n == 1 && (first == "stop" || first == "recipes" || first == "plants")) return true;
  if (first == "alarms") return method == "POST" && p.back() == "ack" && (n == 2 || n == 3);
  if (first == "recipes") return method == "DELETE" && n == 2;
  if (first == "active") return method == "PUT" && n == 1;
  if (first == "plant-recipes") return (method == "PUT" && n == 1) || (method == "DELETE" && n == 2);
  if (first == "plants") return (n == 2 && (method == "PUT" || method == "DELETE")) || (n == 3 && method == "POST");
  // Watering (SD-134): an account only pauses and resumes a circuit's schedule
  if (first == "watering") return method == "POST" && n == 4 && p[3] == "pause";
  return false;
}

std::string sessionToken(const std::string& authorization, const std::string& cookie) {
  if (authorization.rfind("Bearer ", 0) == 0) return authorization.substr(7);
  // name=value pairs split by ';' (RFC 6265, 4.2.1); the name must match whole
  auto trim = [](std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
  };
  std::optional<std::string_view> found;
  std::string_view rest = cookie;
  while (!rest.empty()) {
    auto semi = rest.find(';');
    std::string_view pair = rest.substr(0, semi);
    rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
    auto eq = pair.find('=');
    if (eq == std::string_view::npos || trim(pair.substr(0, eq)) != "dp_session") continue;
    if (found) return {};  // more than one: their order says nothing about which is ours
    found = trim(pair.substr(eq + 1));
  }
  return found ? std::string(*found) : std::string();
}

bool hostAllowed(const std::string& hostHeader, const std::string& extraHost) {
  std::string h = hostHeader;
  if (h.empty()) return true;  // HTTP/1.0 without Host: no browser
  if (h.front() == '[') return h.find(']') != std::string::npos;  // an IPv6 literal
  auto colon = h.rfind(':');
  if (colon != std::string::npos) h.resize(colon);
  for (auto& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (h == "localhost" || (!extraHost.empty() && h == extraHost)) return true;
  // Names that exist only in the home network (mDNS, routers); public domains never.
  for (const char* suffix : {".local", ".lan", ".home.arpa", ".internal", ".fritz.box"}) {
    const size_t n = std::strlen(suffix);
    if (h.size() > n && h.compare(h.size() - n, n, suffix) == 0) return true;
  }
  bool ipv4 = !h.empty();
  for (char ch : h) ipv4 = ipv4 && (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.');
  return ipv4;
}

bool writeAllowed(const std::string& method, const std::string& secFetchSite, const std::string& origin,
                  const std::string& hostHeader) {
  if (method == "GET" || method == "HEAD" || method == "OPTIONS") return true;
  if (!secFetchSite.empty()) return secFetchSite == "same-origin" || secFetchSite == "none";
  if (!origin.empty()) {
    auto p = origin.find("://");
    return p != std::string::npos && origin.substr(p + 3) == hostHeader;
  }
  return true;
}

ApiResponse Api::handle(const ApiRequest& req) {
  try {
    return route(req);
  } catch (const json::exception&) {
    return fail(400, say("api.bad_input"));
  } catch (const std::exception&) {
    return fail(500, say("api.internal"));
  }
}

ApiResponse Api::mqttCommand(const std::string& topic, const std::string& payload) {
  ApiRequest req;
  req.via = "mqtt";
  std::string what;
  {
    std::lock_guard<std::recursive_mutex> l(robot_.mutex());
    if (!mqttCommands(robot_.mqtt())) return fail(403, say("mqtt.commands_off"));
    const std::string base = mqttBase(robot_.deviceId()) + "/";
    if (topic.rfind(base, 0) != 0 || topic.size() < base.size() + 5 || topic.compare(topic.size() - 4, 4, "/set") != 0)
      return fail(404, say("api.not_found"));
    what = topic.substr(base.size(), topic.size() - base.size() - 4);
    const Config& cfg = robot_.config();
    auto refused = [&](int status, const Msg& m) {
      robot_.logEvent("control", "warn", say("ev.mqtt.refused", {{"command", what}}), m);
      return fail(status, m);
    };
    if (what == "stop") {
      req.method = "POST";
      req.path = "/api/v1/stop";
    } else if (what == "recipe") {
      // Home Assistant sends the option's text: the recipe's name
      const RecipeCfg* r = nullptr;
      for (const auto& x : cfg.recipes)
        if (!r && (x.name == payload || x.id == payload)) r = &x;
      if (!r) return refused(422, say("mqtt.no_recipe"));
      req.method = "PUT";
      req.path = "/api/v1/active";
      req.body = json{{"recipe", r->id}}.dump();
    } else if (what == "ec_target" || what == "ph_target") {
      char* end = nullptr;
      const double v = std::strtod(payload.c_str(), &end);
      if (payload.empty() || end != payload.c_str() + payload.size() || !std::isfinite(v)) return refused(422, say("mqtt.number"));
      // The targets of the recipe in force; a plant's phase is changed in the app (SD-128)
      const WaterPlan w = waterPlan(cfg, robot_.now());
      if (w.source == "plant" && w.plant) return refused(409, say("plant.leads", {{"name", w.plant->name}}));
      if (!w.recipe) return refused(409, say("cycle.off.no_recipe"));
      json r;
      const json all = robot_.configJson();
      for (const auto& x : all["recipes"])
        if (x["id"] == w.recipe->id) r = x;
      if (what == "ec_target") {
        r["ecTarget"] = v;
      } else {
        // The new centre keeps the range's width, 0.4 without one
        const double width = isNum(w.recipe->phLow) && isNum(w.recipe->phHigh) ? w.recipe->phHigh - w.recipe->phLow : 0.4;
        r["ph"] = {std::round((v - width / 2) * 100) / 100, std::round((v + width / 2) * 100) / 100};
      }
      req.method = "POST";
      req.path = "/api/v1/recipes";
      req.body = r.dump();
    } else {
      return fail(404, say("api.not_found"));
    }
  }
  ApiResponse resp;
  try {
    resp = route(req, Role::Account);
  } catch (const std::exception&) {
    resp = fail(400, say("api.bad_input"));
  }
  const json body = json::parse(resp.body, nullptr, false);
  if (resp.status == 200) robot_.logEvent("control", "info", say("ev.mqtt.command", {{"command", what}}));
  else if (body.is_object() && body.contains("error") && body["error"].is_object())
    robot_.logEvent("control", "warn", say("ev.mqtt.refused", {{"command", what}}),
                    Msg{jstr(body["error"], "key"), jstr(body["error"], "text"), body["error"].value("args", json::object())});
  return resp;
}

ApiResponse Api::route(const ApiRequest& req, std::optional<Role> as) {
  // While a factory reset is under way and until the device has restarted nothing more is taken,
  // over MQTT neither (SD-139)
  if (robot_.halted()) return fail(503, say("system.restarting"));
  auto p = split(req.path);
  if (p.size() < 2 || p[0] != "api" || p[1] != "v1") return fail(404, say("api.not_found"));
  p.erase(p.begin(), p.begin() + 2);
  const std::string& m = req.method;
  auto is = [&](const std::string& method, std::initializer_list<const char*> parts) {
    if (method != m || parts.size() != p.size()) return false;
    size_t i = 0;
    for (const char* s : parts) {
      if (std::string(s) != "*" && p[i] != s) return false;
      ++i;
    }
    return true;
  };
  json body = json::object();
  if (!req.body.empty()) {
    body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return fail(400, say("api.json"));
  }

  // ---- public
  if (is("GET", {"info"})) return jsonResp(200, robot_.info());
  if (is("GET", {"auth", "session"})) {
    const auto role = access(req);
    std::lock_guard<std::recursive_mutex> l(robot_.mutex());
    ApiResponse r = jsonResp(200, {{"authenticated", role.has_value()},
                                   {"role", role ? json(roleName(*role)) : json(nullptr)},
                                   {"hasPassword", robot_.auth().hasPassword()}});
    // A page left open asks now and then: its cookie lasts as long as the session is in use.
    // Never an access token in a cookie.
    if (role == Role::Owner) r.headers.emplace_back("Set-Cookie", sessionCookie(req.token, kCookieMaxAgeS));
    return r;
  }
  if (is("POST", {"auth", "setup"})) {
    // A mandatory password at the first start; afterwards it changes only signed in.
    std::lock_guard<std::recursive_mutex> l(robot_.mutex());
    if (robot_.passwordKept()) return fail(423, say("auth.kept"));
    if (robot_.credentialsLost()) return fail(423, say("auth.lost"));
    const Auth before = robot_.auth();
    const Ms now = clock_.nowMs();
    Msg e = robot_.auth().setInitialPassword(jstr(body, "password"), jstr(body, "code"), now);
    const bool wrongCode = robot_.auth().wrongSetupCodes() > before.wrongSetupCodes();
    if (wrongCode && robot_.auth().logWrongSetupCode(now))
      robot_.logEvent("auth", "warn", say("ev.auth.code_wrong"), say("ev.auth.code_wrong.text", {{"count", robot_.auth().wrongSetupCodes()}}));
    if (!e.key.empty()) return fail(e.key == "auth.exists" ? 409 : wrongCode ? 403 : 422, e);
    // auth.json first, on disk; only then the lock against a second setup
    if (!robot_.saveAuth()) {
      robot_.auth() = before;
      return fail(500, say("store.failed"));
    }
    robot_.markPasswordSet();
    robot_.logEvent("auth", "info", say("ev.auth.password_set"), say("ev.auth.first_setup"));
    Msg err;
    auto tok = robot_.auth().login(jstr(body, "password"), now, err, req.client);
    ApiResponse r = jsonResp(200, {{"ok", true}});
    if (tok) r.headers.emplace_back("Set-Cookie", sessionCookie(*tok, kCookieMaxAgeS));
    return r;
  }
  if (is("POST", {"auth", "login"})) {
    std::lock_guard<std::recursive_mutex> l(robot_.mutex());
    Msg err;
    auto tok = robot_.auth().login(jstr(body, "password"), clock_.nowMs(), err, req.client);
    if (!tok) {
      // Only a counted failure is logged; a try refused during a lockout is not
      if (err.key == "auth.wrong") robot_.logEvent("auth", "warn", say("ev.auth.failed"), err);
      return fail(err.key == "auth.locked" ? 429 : 401, err);
    }
    robot_.logEvent("auth", "info", say("ev.auth.signed_in"));
    ApiResponse r = jsonResp(200, {{"ok", true}});
    r.headers.emplace_back("Set-Cookie", sessionCookie(*tok, kCookieMaxAgeS));
    return r;
  }
  // The leader asks to couple; the pairing code is the credential (SD-131)
  if (is("POST", {"coupling", "join"})) return fromResult(robot_.join(body));
  if (is("POST", {"auth", "logout"})) {
    std::lock_guard<std::recursive_mutex> l(robot_.mutex());
    robot_.auth().logout(req.token);
    ApiResponse r = jsonResp(200, {{"ok", true}});
    r.headers.emplace_back("Set-Cookie", sessionCookie("", 0));
    return r;
  }

  // ---- signed in only, within the role (SD-129)
  const auto role = as ? as : access(req);
  if (!role) return fail(401, say("auth.required"));
  if (!permitted(*role, m, p)) return fail(403, say("auth.role", {{"role", say(std::string("auth.role.") + roleName(*role))}}));

  if (is("PUT", {"auth", "password"})) {
    std::lock_guard<std::recursive_mutex> l(robot_.mutex());
    const Auth before = robot_.auth();
    Msg e = robot_.auth().changePassword(jstr(body, "old"), jstr(body, "new"), clock_.nowMs(), req.client);
    if (!e.key.empty()) {
      if (e.key == "auth.wrong_old") robot_.logEvent("auth", "warn", say("ev.auth.failed"), e);
      return fail(e.key == "auth.locked" ? 429 : 422, e);
    }
    if (!robot_.saveAuth()) {  // the old password and the sessions stay, in memory as on disk
      robot_.auth() = before;
      return fail(500, say("store.failed"));
    }
    robot_.logEvent("auth", "notice", say("ev.auth.password_changed"), say("ev.auth.password_changed.text"));
    return jsonResp(200, {{"ok", true}});
  }
  if (is("POST", {"system", "factory-reset"})) return fromResult(robot_.factoryResetByOwner(body, req.client));
  if (is("GET", {"tokens"})) return jsonResp(200, robot_.tokens());
  if (is("POST", {"tokens"})) return fromResult(robot_.createToken(body));
  if (is("DELETE", {"tokens", "*"})) return fromResult(robot_.revokeToken(p[1]));
  if (is("GET", {"state"})) return jsonResp(200, robot_.state());
  if (is("GET", {"config"})) return jsonResp(200, robot_.configJson());
  if (is("GET", {"config", "export"})) {
    ApiResponse r = jsonResp(200, robot_.configJson());
    const std::string day = utcText(robot_.now()).substr(0, 10);
    r.headers.emplace_back("Content-Disposition", "attachment; filename=\"doseph-backup-" + day + ".json\"");
    return r;
  }
  if (is("POST", {"config", "import"})) return fromResult(robot_.importConfig(body));
  if (is("GET", {"templates"})) return jsonResp(200, Robot::templates());
  if (is("GET", {"diagnostics"})) return jsonResp(200, robot_.diagnostics());
  if (is("GET", {"history"})) {
    const Epoch now = robot_.now();
    auto it = req.query.find("series");
    if (it == req.query.end() || it->second.empty()) return fail(422, say("history.series"));
    Epoch from = qnum(req, "from", now - 24 * 3600), to = qnum(req, "to", now);
    size_t points = static_cast<size_t>(std::max<Epoch>(10, std::min<Epoch>(qnum(req, "points", 600), 4000)));
    std::vector<std::string> series;
    if (auto e = parseSeries(it->second, series)) return fail(422, *e);
    json out = json::array();
    for (const auto& s : series) out.push_back(robot_.history(s, from, to, points));
    return jsonResp(200, {{"from", from}, {"to", to}, {"series", out}});
  }
  if (is("GET", {"export.csv"})) {
    // A week of all four series by default; the page's language decides the format (RAT-016)
    const Epoch now = robot_.now();
    auto it = req.query.find("series");
    std::vector<std::string> series;
    if (auto e = parseSeries(it == req.query.end() || it->second.empty() ? "ph,ec,temp,flow" : it->second, series)) return fail(422, *e);
    auto l = req.query.find("lang");
    const std::string lang = l != req.query.end() && l->second == "de" ? "de" : "en";
    ApiResponse r;
    r.contentType = "text/csv; charset=utf-8";
    r.body = robot_.historyCsv(series, qnum(req, "from", now - 7 * 24 * 3600), qnum(req, "to", now), lang);
    r.headers.emplace_back("Content-Disposition",
                           lang == "de" ? "attachment; filename=\"doseph-verlauf.csv\"" : "attachment; filename=\"doseph-history.csv\"");
    return r;
  }
  if (is("GET", {"events"})) {
    const Epoch now = robot_.now();
    auto t = req.query.find("type");
    return jsonResp(200, robot_.events(qnum(req, "from", 0), qnum(req, "to", now), t == req.query.end() ? "" : t->second,
                                       static_cast<size_t>(std::clamp<Epoch>(qnum(req, "limit", 100), 1, 1000))));
  }

  if (is("PUT", {"system"})) return fromResult(robot_.setSystem(body));
  if (is("PUT", {"notify"})) return fromResult(robot_.putNotify(body));
  if (is("GET", {"notifications"})) return jsonResp(200, robot_.notifications());
  if (is("GET", {"mqtt"})) return jsonResp(200, robot_.mqttSettings());
  if (is("PUT", {"mqtt"})) return fromResult(robot_.putMqtt(body));
  if (is("GET", {"push"})) return jsonResp(200, robot_.pushSettings());
  if (is("PUT", {"push"})) return fromResult(robot_.putPush(body));
  if (is("GET", {"coupling"})) return jsonResp(200, robot_.coupling());
  if (is("POST", {"coupling", "code"})) return fromResult(robot_.armPairingCode());
  if (is("POST", {"coupling", "pair"})) return fromResult(robot_.pair(body));
  if (is("DELETE", {"coupling"})) return fromResult(robot_.unpair());
  if (is("POST", {"coupling", "leave"})) return fromResult(robot_.left(req.token));
  if (is("POST", {"peer", "beat"})) return fromResult(robot_.peerBeat(req.token, body));
  if (is("POST", {"peer", "run"})) return fromResult(robot_.peerRun(req.token, body));
  if (is("POST", {"peer", "abort"})) return fromResult(robot_.peerAbort(req.token));
  if (is("GET", {"commissioning"})) return jsonResp(200, robot_.setup());
  if (is("PUT", {"commissioning", "*"})) return fromResult(robot_.setSetup(p[1], body));
  if (is("GET", {"plants"})) return jsonResp(200, robot_.plants());
  if (is("GET", {"calendar"})) return jsonResp(200, robot_.calendar(56));
  if (is("POST", {"plants"})) return fromResult(robot_.startPlant(body));
  if (is("PUT", {"plants", "*"})) return fromResult(robot_.putPlant(p[1], body));
  if (is("DELETE", {"plants", "*"})) return fromResult(robot_.deletePlant(p[1]));
  if (is("POST", {"plants", "*", "next"})) return fromResult(robot_.plantNext(p[1]));
  if (is("POST", {"plants", "*", "harvest"})) return fromResult(robot_.plantHarvest(p[1], body));
  if (is("POST", {"plants", "*", "complete"})) return fromResult(robot_.plantComplete(p[1]));
  if (is("POST", {"plants", "*", "lead"})) return fromResult(robot_.plantLead(p[1]));
  if (is("PUT", {"plant-recipes"})) return fromResult(robot_.putPlantRecipe(body));
  if (is("DELETE", {"plant-recipes", "*"})) return fromResult(robot_.deletePlantRecipe(p[1]));
  if (is("GET", {"service"})) return jsonResp(200, robot_.service());
  if (is("PUT", {"service"})) return fromResult(robot_.putService(body));
  if (is("POST", {"service", "*", "done"})) return fromResult(robot_.serviceDone(p[1]));
  if (is("POST", {"push", "test"})) return fromResult(robot_.testPush(jstr(body, "channel")));
  if (is("PUT", {"tank"})) return fromResult(robot_.putTank(body));
  if (is("POST", {"tank", "new"})) return fromResult(robot_.newTank());
  if (is("PUT", {"control"})) return fromResult(robot_.putControl(body));
  if (is("PUT", {"locks"})) return fromResult(robot_.putLocks(body));
  // The refill valve (SD-132): not for an account token or MQTT
  if (is("PUT", {"refill"})) return fromResult(robot_.putRefill(body));
  if (is("POST", {"refill", "start"})) return fromResult(robot_.startRefill());
  if (is("POST", {"refill", "stop"})) return fromResult(robot_.stopRefill());
  if (is("PUT", {"circulation"})) return fromResult(robot_.putCirculation(body));
  // Watering (SD-134)
  if (is("PUT", {"watering"})) return fromResult(robot_.putWatering(body));
  if (is("PUT", {"watering", "circuits"})) return fromResult(robot_.putCircuit(body));
  if (is("DELETE", {"watering", "circuits", "*"})) return fromResult(robot_.deleteCircuit(p[2]));
  if (is("POST", {"watering", "circuits", "*", "now"})) return fromResult(robot_.waterNow(p[2], body));
  if (is("POST", {"watering", "circuits", "*", "stop"})) return fromResult(robot_.stopWater(p[2]));
  if (is("POST", {"watering", "circuits", "*", "pause"})) return fromResult(robot_.pauseCircuit(p[2], body));
  if (is("PUT", {"lighting", "circuits"})) return fromResult(robot_.putLight(body));
  if (is("DELETE", {"lighting", "circuits", "*"})) return fromResult(robot_.deleteLight(p[2]));
  if (is("POST", {"lighting", "circuits", "*", "hand"})) return fromResult(robot_.lightHand(p[2], body));
  if (is("PUT", {"climate", "rooms"})) return fromResult(robot_.putRoom(body));
  if (is("DELETE", {"climate", "rooms", "*"})) return fromResult(robot_.deleteRoom(p[2]));
  // Network devices and their roles (SD-132)
  if (is("GET", {"devices"})) return jsonResp(200, robot_.devices());
  if (is("POST", {"devices"})) return fromResult(robot_.addDevice(body));
  if (is("DELETE", {"devices", "*"})) return fromResult(robot_.removeDevice(p[1]));
  if (is("PUT", {"devices", "*", "credentials"})) return fromResult(robot_.putDeviceCredentials(p[1], body));
  if (is("PUT", {"roles", "*"})) return fromResult(robot_.putRole(p[1], body));
  if (is("DELETE", {"roles", "*"})) return fromResult(robot_.deleteRole(p[1]));
  if (is("POST", {"canisters"})) return fromResult(robot_.putCanister(body));
  if (is("DELETE", {"canisters", "*"})) return fromResult(robot_.deleteCanister(p[1]));
  if (is("POST", {"canisters", "*", "stock"})) return fromResult(robot_.setStock(p[1], body));
  if (is("DELETE", {"catchup"})) return fromResult(robot_.endCatchUp());
  if (is("POST", {"recipes"})) return fromResult(robot_.putRecipe(body));
  if (is("DELETE", {"recipes", "*"})) return fromResult(robot_.deleteRecipe(p[1]));
  if (is("PUT", {"active"})) return fromResult(robot_.setActive(body));

  if (is("POST", {"channels", "*", "prime"})) return fromResult(robot_.prime(channelOf(p[1]), jnum(body, "seconds", 5)));
  if (is("POST", {"channels", "cancel"})) return fromResult(robot_.cancelChannelJob());
  // A pump calibration by the user (SD-143)
  if (is("POST", {"channels", "*", "pump-calibration"})) return fromResult(robot_.startPumpCalibration(channelOf(p[1]), body));
  if (is("POST", {"pump-calibration", "next"})) return fromResult(robot_.pumpCalibrationNext(body));
  if (is("POST", {"pump-calibration", "cancel"})) return fromResult(robot_.cancelPumpCalibration());
  if (is("POST", {"probe"})) return fromResult(robot_.probe(body));
  if (is("POST", {"latches", "*", "ack"})) return fromResult(robot_.ackLatch(p[1], body));
  if (is("POST", {"alarms", "ack"})) return fromResult(robot_.ackAlarm(""));
  if (is("POST", {"alarms", "*", "ack"})) return fromResult(robot_.ackAlarm(p[1]));
  if (is("POST", {"stop"})) return fromResult(robot_.stop(say(req.via == "mqtt" ? "stop.mqtt" : "stop.app")));
  if (is("POST", {"maintenance"})) return fromResult(robot_.maintenance(jnum(body, "minutes")));
  if (is("POST", {"resume"})) return fromResult(robot_.resume());
  return fail(404, say("api.not_found"));
}

}  // namespace doseph
