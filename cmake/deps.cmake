# SPDX-License-Identifier: AGPL-3.0-or-later
# Third-party libraries as single headers, version and SHA-256 pinned.
# They are downloaded to build/_deps at configure time and not checked in.
# Licenses: THIRD_PARTY_NOTICES.md (nlohmann/json also contains CC0 and
# Apache-2.0 parts).
#
# Offline: set DP_DEPS_DIR to a folder with the three files.

set(DP_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps/include" CACHE PATH "Folder of the header dependencies")

function(dp_fetch_header name url sha256 dest_rel)
  set(dest "${DP_DEPS_DIR}/${dest_rel}")
  if(EXISTS "${dest}")
    file(SHA256 "${dest}" have)
    if(have STREQUAL "${sha256}")
      return()
    endif()
    message(STATUS "${name}: checksum does not match, downloading again")
  endif()
  message(STATUS "Downloading ${name}")
  file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS st)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name} could not be downloaded: ${st}")
  endif()
endfunction()

dp_fetch_header(nlohmann_json
  "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
  9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
  "nlohmann/json.hpp")

dp_fetch_header(cpp_httplib
  "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.54.1/httplib.h"
  5933c14b2d0f45212925ed18ca579841f5fce717f431fc20cec712423e905b10
  "httplib.h")

dp_fetch_header(doctest
  "https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h"
  44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb
  "doctest/doctest.h")
