// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 11: REST API /api/v1, independent of the transport. The platform
// (simulator: cpp-httplib; device: its own server) only translates request
// and response. The web app uses only this API.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doseph/robot.hpp"

namespace doseph {

struct ApiRequest {
  std::string method, path;
  std::map<std::string, std::string> query;
  std::string body;
  std::string token;   // the session token, from sessionToken()
  std::string client;  // the peer's address as clientKey() gives it, never from a header; "" when unknown
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
  bool authorized(const ApiRequest& req);

 private:
  ApiResponse route(const ApiRequest& req);
  Robot& robot_;
  const IClock& clock_;
};

}  // namespace doseph
