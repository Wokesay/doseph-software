#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Every check CI runs, in order: architecture rules, the core and the
# simulator (release build, then with AddressSanitizer and UBSan), the tool
# tests (text keys, licenses, provenance, SBOMs, workflow pins), the
# licenses of the web app's npm packages, the web app (registry signatures,
# types, build, size). E2E=1 also runs the end-to-end tests against the
# simulator (needs a Chromium for Playwright). Checking the packages'
# signatures needs the npm registry and Sigstore's TUF repository
# (tuf-repo-cdn.sigstore.dev). Before this, CI runs `reuse lint` and the
# provenance check of the npm packages (.github/workflows/ci.yml).
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

step "Tool tests (text keys, licenses, provenance, SBOMs, workflow pins)"
node --test tools/*.test.mjs

step "Licenses of the web app's npm packages"
node tools/check_licenses.mjs

# The installed packages' registry signatures and provenance attestations
# are checked right after they are installed, before any of them runs.
step "Web app (registry signatures, types, build, size)"
echo "Node $(node -v), npm $(npm -v) check the signatures"
(cd web && npm ci --ignore-scripts --no-audit --no-fund && npm audit signatures && npm run typecheck && npm run build)

if [ "${E2E:-0}" = 1 ]; then
  step "End-to-end"
  (cd web && CI=1 npx playwright test)
fi
echo
echo "All checks passed."
