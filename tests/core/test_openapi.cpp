// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "doseph/api.hpp"

using namespace doseph;

// The OpenAPI file names who may call each operation (`x-roles`); the device decides it with
// permitted(). Both must say the same (SD-129). That every route is in the file is checked by
// tools/openapi.test.mjs.
TEST_CASE("API: the OpenAPI file names for each operation exactly the roles the device lets call it (SD-129)") {
  std::ifstream f(std::string(DP_SOURCE_DIR) + "/docs/openapi.json");
  REQUIRE(f.good());
  const json spec = json::parse(f);
  int operations = 0;
  for (const auto& [path, item] : spec.at("paths").items())
    for (const auto& [method, op] : item.items()) {
      if (op.contains("security") && op["security"].empty()) continue;  // public
      std::vector<std::string> parts;  // a path parameter stands for any id
      size_t pos = 1;
      while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        parts.push_back(!part.empty() && part.front() == '{' ? "1" : part);
        if (slash == std::string::npos) break;
        pos = slash + 1;
      }
      std::string m = method;
      std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      const json roles = op.value("x-roles", json::array());
      for (Role r : {Role::Owner, Role::App, Role::Account, Role::Peer}) {
        const bool listed = std::find(roles.begin(), roles.end(), roleName(r)) != roles.end();
        CHECK_MESSAGE(permitted(r, m, parts) == listed, m << " " << path << " as " << roleName(r));
      }
      ++operations;
    }
  CHECK(operations > 50);
}
