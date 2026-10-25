#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# The core tests on every core : each test case runs in a process of its
# own, with a temporary folder of its own, as many at once as there are cores,
# the cycle scenarios first since they take longest. A failing test case's output is
# shown after all have ended, and the run then ends with exit 1. A passing run
# prints one summary line and the slowest cases. build/doseph_tests alone still runs
# everything in one process.
#   tools/core_tests.sh [binary]      JOBS=<n> sets how many run at once
# With sanitizers a test case takes up to about 0.8 GB of memory.
set -euo pipefail
DP_BIN=${1:-build/doseph_tests}
case "$DP_BIN" in */*) ;; *) DP_BIN=./$DP_BIN ;; esac
[ -x "$DP_BIN" ] || { echo "Core tests: $DP_BIN is not there or cannot be run"; exit 1; }
parallel=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
case "$parallel" in '' | *[!0-9]*) parallel=bad ;; esac
[ "$parallel" != bad ] && [ "$parallel" -ge 1 ] || { echo "Core tests: JOBS must be a whole number of at least 1, not '${JOBS:-}'"; exit 1; }
DP_OUT=$(mktemp -d)
export DP_BIN DP_OUT

# Two parts by source file; each test case is then picked by its place in its
# part (doctest's --first/--last count the cases that pass the filters, from 1)
DP_SCENARIOS='*test_cycle.cpp'
export DP_SCENARIOS
filter() { if [ "$1" = s ]; then echo "--source-file=$DP_SCENARIOS"; else echo "--source-file-exclude=$DP_SCENARIOS"; fi; }
count() { "$DP_BIN" --no-version --count "$(filter "$1")" | sed -n 's/.*passing the current filters: *\([0-9][0-9]*\).*/\1/p'; }
ns=$(count s)
no=$(count o)
if [ -z "$ns" ] || [ -z "$no" ] || [ $((ns + no)) -eq 0 ]; then
  echo "Core tests: $DP_BIN lists no test cases"
  rm -rf "$DP_OUT"
  exit 1
fi
places() { [ "$2" -gt 0 ] && seq 1 "$2" | sed "s/^/$1 /"; true; }
again() { echo "$DP_BIN '$(filter "$1")' --first=$2 --last=$2"; }

# A case passed if it ended with exit 0 after running exactly one test case
# (doctest prints one "<seconds> s: <name>" line per case it ran)
ran() { grep -c '^[0-9][0-9]*\.[0-9]\{6\} s: ' "$1" 2> /dev/null || true; }
show() {
  local log="$DP_OUT/$1$2" n rc
  n=$(ran "$log.log")
  n=${n:-0}
  rc=$(cat "$log.rc" 2>/dev/null || echo none)
  if [ "$rc" = 0 ] && [ "$n" = 1 ]; then return 0; fi
  if [ "$rc" = none ]; then echo "=== $1$2: did not end ==="
  elif [ "$rc" != 0 ] && [ "$n" = 0 ]; then echo "=== $1$2: ended early (exit $rc) ==="
  elif [ "$rc" != 0 ]; then echo "=== $1$2: failed (exit $rc) ==="
  else echo "=== $1$2: ran $n test cases instead of one ==="; fi
  echo "Again: $(again "$1" "$2")"
  cat "$log.log" 2> /dev/null || true
  return 1
}
# Stopped from outside (a CI time limit, Ctrl-C): name the cases that had not
# ended, with what they printed so far, before the logs are removed
stopped() {
  trap - INT TERM
  echo "Core tests: stopped; these had not ended:"
  for f in "$DP_OUT"/*.log; do
    [ -e "${f%.log}.rc" ] || { echo "=== $(basename "$f" .log) ==="; tail -n 20 "$f"; }
  done
  exit 1
}
trap 'rm -rf "$DP_OUT"' EXIT
trap stopped INT TERM

run() {
  local log="$DP_OUT/$1$2"
  mkdir "$log.tmp"
  TMPDIR="$log.tmp" "$DP_BIN" --no-version --duration=true "$(filter "$1")" --first="$2" --last="$2" > "$log.log" 2>&1
  echo $? > "$log.rc"
  rm -rf "$log.tmp"
}
export -f run filter
start=$SECONDS
{ places s "$ns"; places o "$no"; } | xargs -P "$parallel" -n 2 bash -c 'run "$1" "$2"' _

failed=0
asserts=0
for part in s o; do
  n=$no; [ "$part" = s ] && n=$ns
  [ "$n" -gt 0 ] || continue
  for k in $(seq 1 "$n"); do
    if show "$part" "$k"; then
      a=$(sed -n 's/^\[doctest\] assertions: *\([0-9][0-9]*\) .*/\1/p' "$DP_OUT/$part$k.log")
      asserts=$((asserts + ${a:-0}))
    else
      failed=$((failed + 1))
    fi
  done
done
total=$((ns + no))
took=$((SECONDS - start))
if [ "$failed" -gt 0 ]; then
  echo "Core tests: $failed of $total test cases failed ($parallel at once, ${took} s)"
  exit 1
fi
echo "Core tests: $total test cases, $asserts assertions, all passed ($parallel at once, ${took} s)"
echo "Slowest:"
grep -h '^[0-9][0-9]*\.[0-9]\{6\} s: ' "$DP_OUT"/*.log | sort -rn | sed -n '1,5s/^/  /p'
