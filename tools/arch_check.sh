#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Architecture rules, checked by machine (docs/CONCEPT.md §4).
set -u
cd "$(dirname "$0")/.."
fail=0
err() { echo "ARCH: $*"; fail=1; }

# R0: the core is platform-neutral: no platform, network or thread headers.
if grep -rnE '#include <(esp_|freertos/|driver/|sys/|unistd|windows|httplib|filesystem|thread|fstream|iostream)' core/; then
  err "platform header in the core"
fi

# R1: only the actuator gateway (gateway.cpp) calls the hardware's actuator methods.
if grep -rnE '(hw_?|hardware)(\.|->)(startRun|setFeedPump|allowReturnPump|stopAllPumps)\(' core/src | grep -v '^core/src/gateway.cpp'; then
  err "actuator call outside the gateway"
fi

# R1 for network devices (SD-132): only the gateway switches an output or writes its settings.
if grep -rnE '"Switch\.(Set|SetConfig|Toggle)"|\.(rpc|rpcAt)\(' core/src | grep -v '^core/src/gateway.cpp'; then
  err "a network output switched outside the gateway"
fi

# R2: the watchdog sees only the read model and the configuration: no hardware, no gateway, no cycle (RAT-074).
if grep -nE '#include "doseph/(hardware|gateway|cycle|robot|truth|api|plan)\.hpp"' core/include/doseph/watchdog.hpp core/src/watchdog.cpp; then
  err "the watchdog has a path to actuators"
fi

# Logic reads capabilities, never a line or variant name (SD-108); only the
# profile's JSON names them.
if grep -rnE '\.(line|variant)\b' core/src | grep -v '^core/src/hardware.cpp'; then
  err "logic reads a line or variant name"
fi

# Phases provide parameters, never names (RAT-076): no phase name is written into the core.
if grep -rniE '"(seedling|clone|vegetative|veg|growth|flower|flowering|bloom|ripening|keimling|steckling|wachstum|bl.{1,2}te|reife)"' core/src core/include; then
  err "a phase name in the core"
fi

# R5: a missing value is never 0 (RAT-006).
if grep -rnE 'value_or\(0(\.0)?\)' core/; then
  err "a missing value becomes 0"
fi

# The lockout knows a client by the socket's peer address. A trusted proxy
# would let cpp-httplib take it from X-Forwarded-For instead.
if grep -rn 'set_trusted_proxies' sim/; then
  err "client address from a header (set_trusted_proxies)"
fi

# Session tokens, salts and the setup code take their random numbers from the
# operating system (sim::randomBytes: getentropy()). On Linux x86-64
# std::random_device tries RDSEED first, which AMD Zen 5 without AMD's fix
# answers with zeros (AMD-SB-7055). Our sources use neither it nor the RDSEED
# and RDRAND instructions, except for the one declaration right after
# `#ifdef _WIN32` in sim/simulation.cpp, where MSVC's calls rand_s.
rd=$(find core sim -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 2>/dev/null | sort -z |
  xargs -0 awk '
    FNR == 1 { prev = "" }
    { code = $0; sub(/\/\/.*/, "", code) }  # a comment may name it
    code ~ /random_device|rdseed|rdrand/ && !(FILENAME == "sim/simulation.cpp" && prev ~ /^#ifdef[ \t]+_WIN32[ \t\r]*$/) { print FILENAME ":" FNR ":" $0 }
    { prev = code }')
if [ -n "$rd" ]; then
  echo "$rd"
  err "std::random_device, RDSEED or RDRAND outside the Windows branch of sim::randomBytes"
fi

# Every own source file carries its SPDX header (SD-116).
missing=$(git ls-files -- '*.cpp' '*.hpp' '*.ts' '*.tsx' '*.mjs' '*.sh' '*.cmake' 'CMakeLists.txt' '*.yml' '*.css' 2>/dev/null |
  while read -r f; do head -n 3 "$f" | grep -q 'SPDX-License-Identifier' || echo "$f"; done)
if [ -n "$missing" ]; then
  echo "$missing"
  err "source file without an SPDX header"
fi

# Doseph stands alone (SD-117): no issue, decision or repository of the grow
# controller it started from is referred to. Its decisions were SD-001 to
# SD-104, its issues and product decisions had a "gc" prefix.
refs=$(git grep -nE 'gc[#][0-9]|\bgc (PD|SD|RAT)-[0-9]|\bSD-(0[0-9]{2}|10[0-4])\b|growcontroller' -- . ':!tools/arch_check.sh' 2>/dev/null)
if [ -n "$refs" ]; then
  echo "$refs"
  err "a reference to the predecessor (SD-117)"
fi

[ $fail -eq 0 ] && echo "Architecture rules: ok"
exit $fail
