#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Packs the simulator as a download and tests the package:
#   1. a start with arguments: the API and the built-in web app answer;
#   2. a plain start without arguments, as by a double-click: it finds a
#      port, names the address and the demo password, and answers there.
# Run from the repository's root after the build in build-pkg/:
#   tools/package.sh <program name> <platform>
# In GitHub Actions PKG_DIR and PKG_ZIP go to $GITHUB_ENV.
set -Eeuo pipefail  # -E: the ERR trap shows the logs also when a function fails
bin="$1"
name="$2"
cd "$(dirname "$0")/.."

v=$(tr -d '\r\n' < VERSION)
d="doseph-simulator-$v-$name"
rm -rf "dist/$d"
mkdir -p "dist/$d"
if [ -f build-pkg/Release/doseph_sim_server.exe ]; then src=build-pkg/Release/doseph_sim_server.exe; else src=build-pkg/doseph_sim_server; fi
cp "$src" "dist/$d/$bin"
cp sim/README.txt "dist/$d/"
cp LICENSE "dist/$d/LICENSE.txt"
node tools/third_party.mjs "dist/$d/THIRD_PARTY_LICENSES.txt"

# On Windows (Git Bash) taskkill ends the program reliably
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) windows=1 ;; *) windows=0 ;; esac
stop() {
  if [ "$windows" = 1 ]; then taskkill //F //IM "$bin" > /dev/null 2>&1 || true; else kill "$1" 2> /dev/null || true; fi
  wait "$1" 2> /dev/null || true
}
logs() {
  for f in args plain; do echo "--- $f.log"; cat "build-pkg/$f.log" 2> /dev/null || true; done
}
# On a failure: the logs, and no simulator left running
trap 'logs; kill "${pid:-}" 2> /dev/null || true' ERR

# 1. With arguments
"./dist/$d/$bin" --port 18080 --scenario new --prefill 0 > build-pkg/args.log 2>&1 &
pid=$!
for _ in $(seq 1 60); do curl -sf http://127.0.0.1:18080/api/v1/info > /dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:18080/api/v1/info
echo
curl -sf --compressed http://127.0.0.1:18080/ | grep -q '<div id="app">'
curl -sf --compressed http://127.0.0.1:18080/history | grep -q '<div id="app">'  # a page's address falls back to the app
# The first password needs the code on the device label, printed on the console (SD-149)
grep -q "Code on the device label: [2-9A-Z]\{4\}-" build-pkg/args.log
stop "$pid"

# 2. A plain start in a folder of its own; no browser to open on a runner
p=build-pkg/plain
rm -rf "$p"
mkdir -p "$p"
cp "dist/$d/$bin" "$p/"
(cd "$p" && BROWSER=true DISPLAY= exec "./$bin") > build-pkg/plain.log 2>&1 &
pid=$!
url=""
for _ in $(seq 1 60); do
  url=$(grep -o 'http://127\.0\.0\.1:[0-9]*' build-pkg/plain.log | head -n 1 || true)
  [ -n "$url" ] && curl -sf "$url/api/v1/info" > /dev/null && break
  sleep 1
done
curl -sf "$url/api/v1/info" > /dev/null
grep -q "Demo password" build-pkg/plain.log
stop "$pid"
logs

echo "Package dist/$d checked"
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "PKG_DIR=dist/$d" >> "$GITHUB_ENV"
  echo "PKG_ZIP=$d.zip" >> "$GITHUB_ENV"
fi
