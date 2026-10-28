// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 11: REST API /api/v1, independent of the transport. The platform
// (simulator: cpp-httplib; device: its own server) only translates request
// and response. The web app uses only this API.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "doseph/robot.hpp"

namespace doseph {

struct ApiRequest {
  std::string method, path;
  std::map<std::string, std::string> query;
  std::string body;
  std::string token;   // the session or access token, from sessionToken()
  std::string client;  // the peer's address as clientKey() gives it, never from a header; "" when unknown
  std::string via;     // "mqtt" for a command over MQTT; "" over HTTP
};

// Origin checks for the transport, without platform headers.
// DNS rebinding: the Host header must be an IP address, `localhost`, a name
// of the home network (.local, .lan, .home.arpa, .internal, .fritz.box) or `extraHost`.
bool hostAllowed(const std::string& hostHeader, const std::string& extraHost = "");
// CSRF: writing requests of a browser only from the same origin
// (Sec-Fetch-Site, else Origin against Host). Without both headers: no browser.
bool writeAllowed(const std::string& method, const std::string& secFetchSite, const std::string& origin,
                  const std::string& hostHeader);
// The session token of a request: a header "Authorization: Bearer <token>"
// wins; else the cookie named exactly dp_session, "" when there is none or
// more than one.
std::string sessionToken(const std::string& authorization, const std::string& cookie);
// What a role may call (SD-129). The owner may call everything. An app token may call
// everything but the password and the access tokens. An account token may read, stop, acknowledge
// alarms, and change recipes, the recipe in force and plants; never limits, releases, calibration,
// single pumps or settings. A peer token may read.
bool permitted(Role role, const std::string& method, const std::vector<std::string>& path);

struct ApiResponse {
  int status = 200;
  std::string body;
  std::string contentType = "application/json";
  std::vector<std::pair<std::string, std::string>> headers;
};

class Api {
 public:
  Api(Robot& robot, const IClock& clock) : robot_(robot), clock_(clock) {}
  // Catches every exception: wrong input → 400, anything else → 500. The
  // server never crashes on a request.
  ApiResponse handle(const ApiRequest& req);
  // Who the request comes from (SD-129): the owner with a session, else an access token's role
  std::optional<Role> access(const ApiRequest& req);
  bool authorized(const ApiRequest& req) { return access(req).has_value(); }
  // A command over MQTT (SD-130) on one of mqttCommandTopics(): a call of the account role
  // (SD-129) with the same checks as one from the app, taken only while commands are on. A known
  // command is logged with its result; the platform sends the body back on <base>/result.
  ApiResponse mqttCommand(const std::string& topic, const std::string& payload);

 private:
  // `as`: the role of a call the device makes for itself (MQTT); otherwise the request's own
  ApiResponse route(const ApiRequest& req, std::optional<Role> as = std::nullopt);
  Robot& robot_;
  const IClock& clock_;
};

}  // namespace doseph
