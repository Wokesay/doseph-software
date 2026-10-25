#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Every check CI runs, in order: architecture rules, the core and the
# simulator (release build, then with AddressSanitizer and UBSan), the text
# keys, the web app (types, build, size). E2E=1 also runs the end-to-end
# tests against the simulator (needs a Chromium for Playwright).
#   tools/ci.sh            from the repository's root
set -euo pipefail
cd "$(dirname "$0")/.."
step() { printf '\n== %s\n' "$*"; }

step "Architecture rules"
tools/arch_check.sh

step "Core and simulator"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
tools/core_tests.sh build/doseph_tests

step "Core with sanitizers"
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDP_SANITIZE=ON
cmake --build build-san
tools/core_tests.sh build-san/doseph_tests

step "Text keys"
node --test tools/*.test.mjs

step "Web app"
(cd web && npm ci --no-audit --no-fund && npm run typecheck && npm run build)

if [ "${E2E:-0}" = 1 ]; then
  step "End-to-end"
  (cd web && CI=1 npx playwright test)
fi
echo
echo "All checks passed."
