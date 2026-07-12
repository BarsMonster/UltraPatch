#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Fast regression for the release-only gate contract. Individual checks intentionally remain
# configurable; only the aggregate release gate rejects runtime substitutions and missing proof.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
release_lock="${RELEASE_PROFILE_LOCK:-toolchains/release-profile.json}"
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
      -u BASE_RELEASE_FIXTURES -u BASE_RELEASE_HOME_IMAGES \
      -u BASE_RELEASE_FOREIGN_IMAGES -u BASE_RELEASE_FOREIGN_EDGES \
      -u BASE_RELEASE_GOLDEN_BLOBS \
      -u BASE_ARM_TEXT -u BASE_ARM_DATA -u BASE_ARM_BSS \
      -u BASE_ARM_LINKED_TEXT -u BASE_ARM_LINKED_DATA -u BASE_ARM_LINKED_BSS \
      -u BASE_ARM_SOFT_DIV -u BASE_STACK_STATIC_CEIL_O2 -u BASE_STACK_GENERIC_CEIL_O2 \
      -u RELEASE_PROFILE_LOCK \
      "${make_argv[@]}" --no-print-directory "$@"
}

run_authority_make() {
  # The documented authority starts the system Make directly. The parent Make exports MAKE for
  # ordinary recursive probes; remove that bookkeeping value so the public parse guard sees the
  # same clean entry environment as a user invoking /usr/bin/make.
  env -u BASE_FULL_TOTAL -u BASE_FOREIGN_TOTAL \
      -u BASE_ONEFACE_GROW -u BASE_ONEFACE_REVERT \
      -u BASE_RELEASE_FIXTURES -u BASE_RELEASE_HOME_IMAGES \
      -u BASE_RELEASE_FOREIGN_IMAGES -u BASE_RELEASE_FOREIGN_EDGES \
      -u BASE_RELEASE_GOLDEN_BLOBS \
      -u BASE_ARM_TEXT -u BASE_ARM_DATA -u BASE_ARM_BSS \
      -u BASE_ARM_LINKED_TEXT -u BASE_ARM_LINKED_DATA -u BASE_ARM_LINKED_BSS \
      -u BASE_ARM_SOFT_DIV -u BASE_STACK_STATIC_CEIL_O2 -u BASE_STACK_GENERIC_CEIL_O2 \
      -u RELEASE_PROFILE_LOCK -u MAKE \
      /usr/bin/make --no-print-directory "$@"
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

expect_update_reject() {
  local label=$1
  shift
  if run_make "$@" >"$tmp/update-$label.out" 2>&1; then
    echo "golden update accepted runtime override: $label" >&2
    exit 1
  fi
  if ! grep -Eq '^golden update (rejects runtime override|requires unset mode): ' \
      "$tmp/update-$label.out"; then
    echo "golden update rejected $label without its contract diagnostic" >&2
    cat "$tmp/update-$label.out" >&2
    exit 1
  fi
}

expect_profile_update_reject() {
  local label=$1
  shift
  if run_make "$@" >"$tmp/profile-update-$label.out" 2>&1; then
    echo "release profile update accepted runtime override: $label" >&2
    exit 1
  fi
  if ! grep -Eq '^release gate (rejects runtime override|requires unset mode): ' \
      "$tmp/profile-update-$label.out"; then
    echo "release profile update rejected $label without its contract diagnostic" >&2
    cat "$tmp/profile-update-$label.out" >&2
    exit 1
  fi
}

expect_public_update_env_reject() {
  local label=$1 control=$2 value=$3 diagnostic=$4
  local candidate="$tmp/public-$label.json"
  cp "$tmp/stale-release-lock.json" "$candidate"
  if (
    export "$control=$value"
    run_authority_make RELEASE_PROFILE_LOCK="$candidate" \
      WIRE_BASELINE_LOCK="$tmp/public-update.lock" release-profile-update
  ) >"$tmp/public-$label.out" 2>&1; then
    echo "release profile update accepted launch control: $label" >&2
    exit 1
  fi
  grep -Fq "release-profile-update rejects launch control: $diagnostic" \
    "$tmp/public-$label.out"
  cmp "$candidate" "$tmp/stale-release-lock.json"
}

expect_public_update_arg_reject() {
  local label=$1 diagnostic=$2
  shift 2
  local candidate="$tmp/public-$label.json"
  cp "$tmp/stale-release-lock.json" "$candidate"
  if run_authority_make "$@" RELEASE_PROFILE_LOCK="$candidate" \
      WIRE_BASELINE_LOCK="$tmp/public-update.lock" release-profile-update \
      >"$tmp/public-$label.out" 2>&1; then
    echo "release profile update accepted launch control: $label" >&2
    exit 1
  fi
  grep -Fq "release-profile-update rejects launch control: $diagnostic" \
    "$tmp/public-$label.out"
  cmp "$candidate" "$tmp/stale-release-lock.json"
}

cd "$ROOT"
run_make release-gate-origin-probe-internal
run_make golden-update-origin-probe-internal
run_make release-gate-inputs-internal >"$tmp/release-inputs.out"
grep -Fxq 'wire_baseline_reader=clean' "$tmp/release-inputs.out"
run_make golden-update-validate-canonical-internal >"$tmp/update-inputs.out"
grep -q '^release_inventory=OK ' "$tmp/update-inputs.out"
grep -q '^corpus_assets=verified ' "$tmp/update-inputs.out"
grep -q '^foreign_assets=verified ' "$tmp/update-inputs.out"
grep -Fxq 'golden_update_canonical_inputs=OK (inventory + corpus/foreign asset hashes)' \
  "$tmp/update-inputs.out"

# The public release identity binds the immutable OCI image as well as the inner tool profile.
# Changing only the container digest must therefore produce a distinct certificate hash.
canonical_profile=$(python3 scripts/build_profile.py verify-release "$release_lock")
python3 - "$release_lock" "$tmp/container-only.json" <<'PY'
import json
from pathlib import Path
import sys

source = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
digest = source["container"][-64:]
replacement = ("0" if digest[0] != "0" else "1") + digest[1:]
source["container"] = source["container"][:-64] + replacement
Path(sys.argv[2]).write_text(json.dumps(source, sort_keys=True, indent=2) + "\n",
                             encoding="utf-8")
PY
container_profile=$(python3 scripts/build_profile.py verify-release "$tmp/container-only.json")
if [ "$canonical_profile" = "$container_profile" ]; then
  echo "release profile identity ignored the OCI container digest" >&2
  exit 1
fi

# Required tools and ARM link policy are descriptor inputs, not merely Make gate variables.
# The proxy reports the exact locked version and retains the same display name; only its bytes
# differ. Acceptance would prove the profile compared labels rather than executable content.
clang_version=$(python3 - "$release_lock" <<'PY'
import json
from pathlib import Path
import sys
print(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))["profile"]["clang"]["version"])
PY
)
clang_proxy="$tmp/clang"
printf '%s\n' '#!/bin/sh' 'printf "%s\n" "$LOCKED_CLANG_VERSION"' >"$clang_proxy"
chmod +x "$clang_proxy"
if LOCKED_CLANG_VERSION="$clang_version" run_make CLANG="$clang_proxy" \
    check-release-profile-internal >"$tmp/clang-profile.out" \
    2>"$tmp/clang-profile.err"; then
  echo "release profile accepted a byte-different Clang with the locked version" >&2
  exit 1
fi
grep -Fq 'executable_sha256' "$tmp/clang-profile.err"
if run_make ARM_LINK_FLAGS='-mcpu=cortex-m0plus -mthumb -nostdlib' \
    check-release-profile-internal >"$tmp/arm-link-profile.out" \
    2>"$tmp/arm-link-profile.err"; then
  echo "release profile accepted substituted ARM link flags" >&2
  exit 1
fi
grep -Fq 'release profile mismatch' "$tmp/arm-link-profile.err"

# One representative for each independently meaningful release input class. The probe is
# parse/build-free, so this catches regressions without multiplying full gate runs.
expect_reject fixtures FIXTURES="$tmp/fixtures" release-gate-origin-probe-internal
expect_reject corpus_manifest CORPUS_MANIFEST="$tmp/corpus.sha256" \
  release-gate-origin-probe-internal
expect_reject corpus_inventory CORPUS_INVENTORY="$tmp/inventory.tsv" \
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
expect_reject integration_tu DECODER_INTEGRATION_TU=/dev/null \
  release-gate-origin-probe-internal
expect_reject decoder_mode DECODER_API_REGULAR=0 release-gate-origin-probe-internal
expect_reject golden_manifest GOLDEN_MANIFEST="$tmp/golden.sha256" \
  release-gate-origin-probe-internal
expect_reject gate_timeout GATE_TIMEOUT=81 release-gate-origin-probe-internal

# Canonical wire publication has the same protected release identity.  It may vary only JOBS,
# which changes scheduling but neither measured bytes nor policy.
expect_update_reject fixtures FIXTURES="$tmp/fixtures" golden-update-origin-probe-internal
expect_update_reject size_pin BASE_FULL_TOTAL=999999 golden-update-origin-probe-internal
expect_update_reject wire_config WIRE_CONFIG_FLAGS=-DCORTEX_M0 \
  golden-update-origin-probe-internal
expect_update_reject bypass_dump CORPUS_WIRE_DUMP="$tmp/wire.tsv" \
  golden-update-origin-probe-internal
expect_update_reject timeout GATE_TIMEOUT=81 golden-update-origin-probe-internal
run_make JOBS=1 golden-update-origin-probe-internal

# A canonical lock refresh is just as sensitive as verification: runtime compiler, tool, flag,
# archive, or policy substitutions must fail before the atomic updater can publish anything.
cp "$release_lock" "$tmp/release-lock-before.json"
expect_profile_update_reject clang_tool CLANG=false release-profile-update-internal
expect_profile_update_reject arm_link ARM_LINK_FLAGS=-mthumb \
  release-profile-update-internal
cmp "$release_lock" "$tmp/release-lock-before.json"

# Make launch controls act before recipes, so recipe failures cannot guard a mutation: -i can
# ignore them, -n/-t can false-success, and injected makefiles/shells can neutralize them. Use a
# stale private lock and private flock path to prove each public invocation exits nonzero without
# changing one byte. These parse-time failures never acquire the gate's canonical shared lock.
python3 - "$release_lock" "$tmp/stale-release-lock.json" <<'PY'
import json
from pathlib import Path
import sys

lock = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
lock["profile"] = {"stale": True}
Path(sys.argv[2]).write_text(json.dumps(lock, sort_keys=True, indent=2) + "\n",
                             encoding="utf-8")
PY
expect_public_update_env_reject makeflags_i MAKEFLAGS -i MAKEFLAGS
expect_public_update_env_reject makeflags_n MAKEFLAGS -n MAKEFLAGS
expect_public_update_env_reject makeflags_t MAKEFLAGS -t MAKEFLAGS
expect_public_update_env_reject gnumakeflags GNUMAKEFLAGS -i GNUMAKEFLAGS
printf '%s\n' '.IGNORE: release-gate-origin-probe-internal' >"$tmp/injected.mk"
expect_public_update_env_reject makefiles MAKEFILES "$tmp/injected.mk" MAKEFILES
expect_public_update_arg_reject make MAKE MAKE=/bin/true
expect_public_update_arg_reject shell SHELL SHELL=/bin/true
expect_public_update_arg_reject shellflags .SHELLFLAGS .SHELLFLAGS=-c

# Candidate inspection and publication share the same early canonical PATH. PATH proxies for all
# selected drivers and launch tools must stay untouched, and the full candidate must equal the
# checked lock byte-for-byte.
mkdir "$tmp/poison-path"
for tool in gcc clang arm-none-eabi-gcc nm arm-none-eabi-nm arm-none-eabi-objdump \
            arm-none-eabi-size python3 timeout flock; do
  printf '%s\n' '#!/bin/sh' 'printf "%s\n" poison >>"$PROXY_MARKER"' 'exit 97' \
    >"$tmp/poison-path/$tool"
  chmod +x "$tmp/poison-path/$tool"
done
PATH="$tmp/poison-path:/usr/bin:/bin" PROXY_MARKER="$tmp/proxy-executed" \
  run_authority_make release-profile-json >"$tmp/canonical-candidate.json"
cmp "$release_lock" "$tmp/canonical-candidate.json"
test ! -e "$tmp/proxy-executed"

# A direct authority Make under the release driver's exact env -i policy gives GNU Make's
# built-in /bin/sh the `default` origin (recursive Make commonly reports `file`). Both canonical
# origins must produce the identical full candidate without admitting any caller-controlled one.
env -i GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 \
  HOME=/nonexistent/ultrapatch-release-home \
  XDG_CONFIG_HOME=/nonexistent/ultrapatch-release-xdg \
  LANG=C LANGUAGE=C LC_ALL=C PATH=/usr/bin:/bin TZ=UTC \
  /usr/bin/make --no-print-directory release-profile-json \
  >"$tmp/minimal-environment-candidate.json"
cmp "$release_lock" "$tmp/minimal-environment-candidate.json"

# The outer public gate takes the canonical shared lock before starting an inner Make, so the
# inner invocation reparses a coherent Makefile/manifests generation.  MAKE=true prevents GNU
# make's -n recursive-command exception from launching the real nested gate during inspection;
# a real runtime MAKE substitution is separately rejected below.
run_make -n MAKE=true gate >"$tmp/gate-lock.out"
grep -Fq 'flock --shared "test-bench/.wire-baseline-update.lock"' "$tmp/gate-lock.out"
grep -Fq 'true --no-print-directory gate-internal' "$tmp/gate-lock.out"
if run_make MAKE=true gate >"$tmp/gate-make.out" 2>&1; then
  echo "release gate accepted runtime MAKE substitution" >&2
  exit 1
fi
grep -Fq 'release gate rejects runtime override: MAKE' "$tmp/gate-make.out"

# Confirm that the restriction is gate-local: a direct, non-certifying target still accepts
# custom fixture and build-root inputs used by measurements and profile isolation tests.
run_make -s FIXTURES="$tmp/fixtures" BUILD_ROOT="$tmp/builds" host-tool-path >/dev/null

# A child launcher that exits successfully without running any leg must never be enough for the
# aggregate gate to print PASS. This directly regression-tests the evidence/cardinality boundary.
fake_make="$tmp/fake-make"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_make"
chmod +x "$fake_make"
profile="release_profile=$(printf '0%.0s' {1..64})"
if MAKE="$fake_make" HOST_TOOL=/bin/true DECODER_CANONICAL_HDR=/dev/null \
    RELEASE_PROFILE="$profile" \
    RELEASE_CORPUS_INVENTORY=test-bench/release-inventory.tsv JOBS=1 \
    BASE_RELEASE_FIXTURES=2 BASE_RELEASE_HOME_IMAGES=16 \
    BASE_RELEASE_FOREIGN_IMAGES=18 BASE_RELEASE_FOREIGN_EDGES=17 \
    BASE_RELEASE_GOLDEN_BLOBS=8 \
    BASE_FULL_TOTAL=4151373 BASE_FOREIGN_TOTAL=1333390 \
    BASE_ONEFACE_GROW=573 BASE_ONEFACE_REVERT=287 \
    BASE_ARM_TEXT=6329 BASE_ARM_DATA=0 BASE_ARM_BSS=9752 \
    BASE_ARM_LINKED_TEXT=6909 BASE_ARM_LINKED_DATA=0 BASE_ARM_LINKED_BSS=9752 \
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

echo "release_gate_contract=OK (parse-time updater controls + canonical PATH; gate overrides rejected; complete evidence required)"
