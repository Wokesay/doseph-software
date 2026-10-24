# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DP_VERSION)
string(STRIP "${DP_VERSION}" DP_VERSION)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" DP_VERSION_NUM "${DP_VERSION}")

project(doseph VERSION ${DP_VERSION_NUM} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DP_BUILD_SIM "Build the simulator with its host server" ON)
option(DP_BUILD_TESTS "Build the tests" ON)
option(DP_SANITIZE "AddressSanitizer and UBSan in tests and simulator" OFF)
option(DP_WERROR "Warnings as errors" ON)

if(MSVC)
  add_compile_options(/utf-8 /bigobj /EHsc /permissive- /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

include(cmake/deps.cmake)

# Version and recipe templates are embedded into the core as data.
set(DP_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DP_GEN_DIR}/doseph")
add_custom_command(
  OUTPUT "${DP_GEN_DIR}/doseph/embedded.cpp"
  COMMAND ${CMAKE_COMMAND}
          -DTEMPLATES=${CMAKE_CURRENT_SOURCE_DIR}/core/data/templates.json
          -DVERSION=${DP_VERSION}
          -DOUT=${DP_GEN_DIR}/doseph/embedded.cpp
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS core/data/templates.json VERSION cmake/embed.cmake
  COMMENT "Embedding version and recipe templates")

if(MSVC)
  set(DP_WARN /W3)
  if(DP_WERROR)
    list(APPEND DP_WARN /WX)
  endif()
else()
  set(DP_WARN -Wall -Wextra -Wpedantic -Wshadow)
  if(DP_WERROR)
    list(APPEND DP_WARN -Werror)
  endif()
endif()

add_library(doseph_core
  core/src/common.cpp
  core/src/hardware.cpp
  core/src/clock.cpp
  core/src/config.cpp
  core/src/truth.cpp
  core/src/history.cpp
  core/src/events.cpp
  core/src/sha256.cpp
  core/src/auth.cpp
  core/src/plan.cpp
  core/src/journal.cpp
  core/src/gateway.cpp
  core/src/cycle.cpp
  core/src/watchdog.cpp
  core/src/robot.cpp
  core/src/api.cpp
  core/src/messages.cpp
  "${DP_GEN_DIR}/doseph/embedded.cpp")
target_include_directories(doseph_core PUBLIC core/include "${DP_DEPS_DIR}" "${DP_GEN_DIR}")
target_compile_options(doseph_core PRIVATE ${DP_WARN})

if(DP_SANITIZE)
  set(DP_SAN -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_compile_options(doseph_core PUBLIC ${DP_SAN})
  target_link_options(doseph_core PUBLIC ${DP_SAN})
endif()

if(DP_BUILD_SIM)
  find_package(Threads REQUIRED)
  add_library(doseph_sim STATIC sim/world.cpp sim/simhw.cpp sim/simulation.cpp)
  target_include_directories(doseph_sim PUBLIC sim)
  target_link_libraries(doseph_sim PUBLIC doseph_core)
  target_compile_options(doseph_sim PRIVATE ${DP_WARN})

  add_executable(doseph_sim_server sim/main.cpp)
  target_link_libraries(doseph_sim_server PRIVATE doseph_sim Threads::Threads)
  # httplib.h warns a lot with -Wshadow; relaxed for main.cpp only
  if(NOT MSVC)
    target_compile_options(doseph_sim_server PRIVATE -Wall -Wextra)
  endif()
  if(WIN32)
    target_link_libraries(doseph_sim_server PRIVATE ws2_32)
  endif()
endif()

if(DP_BUILD_TESTS AND DP_BUILD_SIM)
  enable_testing()
  add_executable(doseph_tests
    tests/core/main.cpp
    tests/core/test_auth.cpp
    tests/core/test_history.cpp
    tests/core/test_messages.cpp
    tests/core/test_config.cpp
    tests/core/test_truth.cpp
    tests/core/test_plan.cpp
    tests/core/test_journal.cpp
    tests/core/test_watchdog.cpp
    tests/core/test_cycle.cpp
    tests/core/test_api.cpp)
  target_link_libraries(doseph_tests PRIVATE doseph_sim)
  if(NOT MSVC)
    target_compile_options(doseph_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME core COMMAND doseph_tests)
endif()
