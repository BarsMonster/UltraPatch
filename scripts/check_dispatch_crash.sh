#!/usr/bin/env bash
# Regression test for the malformed/edge command-status dispatchers.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [ -n "${CRASH_DISPATCH_MODE:-}" ]; then
  if [ "$CRASH_DISPATCH_MODE" = decode ] && [ "${1:-}" = --decode ]; then
    ulimit -c 0
    kill -s SEGV "$$"
  fi
  if [ "$CRASH_DISPATCH_MODE" = encode ] && [ "${1:-}" != --decode ] &&
     [ ! -e "$CRASH_DISPATCH_MARKER" ]; then
    : > "$CRASH_DISPATCH_MARKER"
    ulimit -c 0
    kill -s SEGV "$$"
  fi
  exec "$REAL_ULTRAPATCH" "$@"
fi

mode="${1:-}"
if [ "$mode" != malformed ] && [ "$mode" != edge ]; then
  echo "usage: $0 malformed|edge" >&2
  exit 2
fi

. "$script_dir/tempdir.sh"

real_ultrapatch="$PWD/ultrapatch"
if [ ! -x "$real_ultrapatch" ]; then
  echo "dispatch regression infrastructure failure: $real_ultrapatch is missing or not executable" >&2
  exit 2
fi

if [ "$mode" = malformed ]; then
  if CRASH_DISPATCH_MODE=decode REAL_ULTRAPATCH="$real_ultrapatch" ULTRAPATCH="$script_dir/check_dispatch_crash.sh" \
       FIXTURES="${FIXTURES:-test-bench/fixtures}" "$script_dir/check_malformed.sh" \
       >"$tmp/out" 2>"$tmp/err"; then
    echo "dispatch regression failure: malformed checker accepted a crashed decoder" >&2
    exit 1
  fi
  grep -Fq 'malformed case dispatcher failure: empty (status=139)' "$tmp/err"
  if grep -q '^malformed_rejects=' "$tmp/out"; then
    echo "dispatch regression failure: crashed decoder incremented malformed_rejects" >&2
    exit 1
  fi
else
  if CRASH_DISPATCH_MODE=encode CRASH_DISPATCH_MARKER="$tmp/crashed" \
       REAL_ULTRAPATCH="$real_ultrapatch" ULTRAPATCH="$script_dir/check_dispatch_crash.sh" \
       "$script_dir/check_edge.sh" >"$tmp/out" 2>"$tmp/err"; then
    echo "dispatch regression failure: edge checker accepted a crashed encoder" >&2
    exit 1
  fi
  grep -Fq 'edge FAILURE: empty_to_small encoder dispatcher failure (status=139)' "$tmp/err"
  grep -q '^edge_refusals=1$' "$tmp/out"
fi
