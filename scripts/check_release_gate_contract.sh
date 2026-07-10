#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Fast regression for the release-only gate contract. Individual checks intentionally remain
# configurable; only the aggregate release gate rejects runtime substitutions and missing proof.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
read -r -a make_argv <<<"${MAKE:-make}"
[ "${#make_argv[@]}" -gt 0 ] || {
  echo "check_release_gate_contract.sh: empty MAKE command" >&2
  exit 2
}

. "$ROOT/scripts/tempdir.sh"

run_make() {
  # run_gate.sh owns these canonical values for its evidence comparisons and therefore exports
  # them to every concurrent leg. They are not caller overrides of this nested probe.
  env -u BASE_FULL_TOTAL -u BASE_FOREIGN_TOTAL \
      -u BASE_ONEFACE_GROW -u BASE_ONEFACE_REVERT \
      -u BASE_ARM_TEXT -u BASE_ARM_DATA -u BASE_ARM_BSS \
      -u BASE_ARM_LINKED_TEXT -u BASE_ARM_LINKED_DATA -u BASE_ARM_LINKED_BSS \
      -u BASE_ARM_SOFT_DIV -u BASE_STACK_STATIC_CEIL_O2 -u BASE_STACK_GENERIC_CEIL_O2 \
      "${make_argv[@]}" --no-print-directory "$@"
}

expect_reject() {
  local label=$1
  shift
  if run_make "$@" >"$tmp/$label.out" 2>&1; then
    echo "release gate accepted runtime override: $label" >&2
    exit 1
  fi
  if ! grep -Eq '^release gate (rejects runtime override|requires unset mode): ' \
      "$tmp/$label.out"; then
    echo "release gate rejected $label without its contract diagnostic" >&2
    cat "$tmp/$label.out" >&2
    exit 1
  fi
}

cd "$ROOT"
run_make release-gate-origin-probe-internal

# One representative for each independently meaningful release input class. The probe is
# parse/build-free, so this catches regressions without multiplying full gate runs.
expect_reject fixtures FIXTURES="$tmp/fixtures" release-gate-origin-probe-internal
expect_reject corpus_manifest CORPUS_MANIFEST="$tmp/corpus.sha256" \
  release-gate-origin-probe-internal
expect_reject size_baseline CORPUS_SIZE_BASELINE= release-gate-origin-probe-internal
expect_reject compression_cap BASE_ONEFACE_GROW=999999 release-gate-origin-probe-internal
expect_reject arm_ratchet BASE_ARM_TEXT=999999 release-gate-origin-probe-internal
expect_reject stack_ratchet BASE_STACK_STATIC_CEIL_O2=999999 \
  release-gate-origin-probe-internal
expect_reject release_lock RELEASE_PROFILE_LOCK="$tmp/release.json" \
  release-gate-origin-probe-internal
expect_reject build_dir BUILD_DIR="$tmp/build" release-gate-origin-probe-internal
expect_reject source_set TOOL_SRCS=src/patch_generate.c release-gate-origin-probe-internal
expect_reject decoder_headers DECODER_PUBLIC_HDRS=src/patch_apply.h \
  release-gate-origin-probe-internal
expect_reject arm_harness ARM_APPLY_HARNESS=: release-gate-origin-probe-internal
expect_reject decoder_mode DECODER_API_REGULAR=0 release-gate-origin-probe-internal
expect_reject golden_manifest GOLDEN_MANIFEST="$tmp/golden.sha256" \
  release-gate-origin-probe-internal
expect_reject gate_timeout GATE_TIMEOUT=81 release-gate-origin-probe-internal

# Confirm that the restriction is gate-local: a direct, non-certifying target still accepts
# custom fixture and build-root inputs used by measurements and profile isolation tests.
run_make -s FIXTURES="$tmp/fixtures" BUILD_ROOT="$tmp/builds" host-tool-path >/dev/null

# A child launcher that exits successfully without running any leg must never be enough for the
# aggregate gate to print PASS. This directly regression-tests the evidence/cardinality boundary.
fake_make="$tmp/fake-make"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_make"
chmod +x "$fake_make"
profile="release_profile=$(printf '0%.0s' {1..64})"
if MAKE="$fake_make" HOST_TOOL=/bin/true RELEASE_PROFILE="$profile" JOBS=1 \
    BASE_FULL_TOTAL=4151373 BASE_FOREIGN_TOTAL=1333327 \
    BASE_ONEFACE_GROW=573 BASE_ONEFACE_REVERT=287 \
    BASE_ARM_TEXT=6073 BASE_ARM_DATA=0 BASE_ARM_BSS=10296 \
    BASE_ARM_LINKED_TEXT=6653 BASE_ARM_LINKED_DATA=0 BASE_ARM_LINKED_BSS=10296 \
    BASE_ARM_SOFT_DIV=0 BASE_STACK_STATIC_CEIL_O2=480 BASE_STACK_GENERIC_CEIL_O2=480 \
    "$ROOT/scripts/run_gate.sh" >"$tmp/evidence.out" 2>"$tmp/evidence.err"; then
  echo "gate accepted successful child statuses without release evidence" >&2
  exit 1
fi
grep -q '^missing or duplicate gate evidence: ' "$tmp/evidence.err"
if grep -q '^RESULT[[:space:]]*: ALL GATES PASS$' "$tmp/evidence.out"; then
  echo "gate printed PASS without release evidence" >&2
  exit 1
fi

echo "release_gate_contract=OK (runtime overrides rejected; complete evidence required)"
