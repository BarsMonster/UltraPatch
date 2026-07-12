#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Host-profile collision and publication regression. Optional arguments override MAKE and the
# Make target that prints the selected HOST_TOOL; equivalent environment knobs are
# MAKE/HOST_TOOL_TARGET.
set -eu

if [ "$#" -gt 2 ]; then
  echo "usage: $0 [make-command [host-tool-target]]" >&2
  exit 2
fi
make_command="${1:-${MAKE:-make}}"
host_tool_target="${2:-${HOST_TOOL_TARGET:-host-tool-path}}"
clang_command="${CLANG:-clang}"
objcopy_command="${ARM_OBJCOPY:-arm-none-eabi-objcopy}"
read -r -a make_argv <<<"$make_command"
if [ "${#make_argv[@]}" -eq 0 ] || [ -z "$host_tool_target" ] || \
   [ -z "$clang_command" ] || [ -z "$objcopy_command" ]; then
  echo "check_build_profile.sh: empty MAKE, CLANG, ARM_OBJCOPY, or host-tool target" >&2
  exit 2
fi

. "$(dirname "$0")/tempdir.sh"
build_root="$tmp/profiles"

run_make() {
  "${make_argv[@]}" --no-print-directory "$@"
}

tool_path() {
  local root=$1
  local output profile_dir profile_id
  shift
  output=$(run_make -s BUILD_ROOT="$root" "$@" "$host_tool_target")
  if [ "$(printf '%s\n' "$output" | sed '/^$/d' | wc -l)" -ne 1 ]; then
    echo "check_build_profile.sh: $host_tool_target did not print one HOST_TOOL path" >&2
    printf '%s\n' "$output" >&2
    return 1
  fi
  case "$output" in
    "$root"/*/ultrapatch) ;;
    *)
      echo "check_build_profile.sh: HOST_TOOL is not an absolute profile path under $root: $output" >&2
      return 1
      ;;
  esac
  profile_dir=${output%/ultrapatch}
  profile_id=${profile_dir##*/}
  if [[ ! "$profile_id" =~ ^[0-9a-f]{64}$ ]]; then
    echo "check_build_profile.sh: HOST_TOOL has a malformed profile id: $output" >&2
    return 1
  fi
  printf '%s\n' "$output"
}

decoder_probe_flags='-DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u -DUP_BACKEND_PROFILE_PROBE=1'
gcc_tool=$(tool_path "$build_root" CC=gcc)
clang_tool=$(tool_path "$build_root" CC="$clang_command")
alternate_tool=$(tool_path "$build_root" CC=gcc CFLAGS_EXTRA=-DUP_BUILD_PROFILE_ALTERNATE=1)
decoder_tool=$(tool_path "$build_root" CC=gcc DECODER_CONFIG_FLAGS="$decoder_probe_flags")
objcopy_proxy="$tmp/objcopy-profile-proxy"
ln -s "$(command -v "$objcopy_command")" "$objcopy_proxy"
objcopy_tool=$(tool_path "$build_root" CC=gcc ARM_OBJCOPY="$objcopy_proxy")

profile_tools=("$gcc_tool" "$clang_tool" "$alternate_tool" "$decoder_tool" "$objcopy_tool")
profile_names=(gcc clang cflags decoder objcopy)
for ((i = 0; i < ${#profile_tools[@]}; ++i)); do
  for ((j = i + 1; j < ${#profile_tools[@]}; ++j)); do
    if [ "${profile_tools[i]}" = "${profile_tools[j]}" ]; then
      echo "check_build_profile.sh: ${profile_names[i]} and ${profile_names[j]} selected the same profile" >&2
      exit 1
    fi
  done
done

# Build every compiler/configuration variant concurrently. The objcopy-only identity needs a
# manifest but no duplicate executable build.
run_make BUILD_ROOT="$build_root" CC=gcc ultrapatch >"$tmp/gcc.log" 2>&1 &
gcc_pid=$!
run_make BUILD_ROOT="$build_root" CC="$clang_command" ultrapatch >"$tmp/clang.log" 2>&1 &
clang_pid=$!
run_make BUILD_ROOT="$build_root" CC=gcc CFLAGS_EXTRA=-DUP_BUILD_PROFILE_ALTERNATE=1 \
  ultrapatch >"$tmp/alternate.log" 2>&1 &
alternate_pid=$!
run_make BUILD_ROOT="$build_root" CC=gcc DECODER_CONFIG_FLAGS="$decoder_probe_flags" \
  ultrapatch >"$tmp/decoder.log" 2>&1 &
decoder_pid=$!
rc=0
wait "$gcc_pid" || rc=1
wait "$clang_pid" || rc=1
wait "$alternate_pid" || rc=1
wait "$decoder_pid" || rc=1
if [ "$rc" -ne 0 ]; then
  echo "check_build_profile.sh: concurrent profile build failed" >&2
  for log in "$tmp/gcc.log" "$tmp/clang.log" "$tmp/alternate.log" "$tmp/decoder.log"; do
    [ -s "$log" ] && { echo "--- $log" >&2; cat "$log" >&2; }
  done
  exit 1
fi
run_make BUILD_ROOT="$build_root" CC=gcc ARM_OBJCOPY="$objcopy_proxy" profile-check \
  >"$tmp/objcopy.log" 2>&1

for tool in "${profile_tools[@]}"; do
  [ -f "$(dirname "$tool")/profile.json" ] || {
    echo "check_build_profile.sh: missing profile manifest beside $tool" >&2
    exit 1
  }
done
for tool in "$gcc_tool" "$clang_tool" "$alternate_tool" "$decoder_tool"; do
  [ -x "$tool" ] || { echo "check_build_profile.sh: missing HOST_TOOL $tool" >&2; exit 1; }
  "$tool" --help >/dev/null
done

# Decoder integration flags belong exclusively to the backend TU.
python3 - "$(dirname "$decoder_tool")/profile.json" <<'PY'
import json
from pathlib import Path
import sys

profile = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
probe = "-DUP_BACKEND_PROFILE_PROBE=1"
assert probe in profile["flags"]["backend_cflags"]
assert probe not in profile["flags"]["encoder_cflags"]
PY

# Ambient compiler search paths are removed at the Make boundary. A poison standard header must
# neither alter the selected profile nor reach a forced real compilation.
poison="$tmp/compiler-search-poison"
mkdir "$poison"
printf '%s\n' '#error "CPATH reached the compiler"' >"$poison/stdint.h"
poison_tool=$(tool_path "$build_root" CC=gcc CPATH="$poison")
if [ "$poison_tool" != "$gcc_tool" ]; then
  echo "check_build_profile.sh: CPATH changed the selected host profile" >&2
  exit 1
fi
run_make -B BUILD_ROOT="$build_root" CC=gcc CPATH="$poison" all-internal \
  >"$tmp/cpath.log" 2>&1 || {
    echo "check_build_profile.sh: sanitized CPATH affected compilation" >&2
    cat "$tmp/cpath.log" >&2
    exit 1
  }

# Same-profile top-level rebuilds may overlap while a live reader executes the previously
# published tool. Object, dependency, manifest, and link outputs must publish atomically.
same_root="$tmp/same-profile"
run_make BUILD_ROOT="$same_root" CC=gcc ultrapatch >"$tmp/same-initial.log" 2>&1
same_tool=$(tool_path "$same_root" CC=gcc)
( while [ ! -e "$tmp/same.done" ]; do "$same_tool" --help >/dev/null; done ) &
same_reader=$!
run_make -B BUILD_ROOT="$same_root" CC=gcc ultrapatch >"$tmp/same-a.log" 2>&1 &
same_a=$!
run_make -B BUILD_ROOT="$same_root" CC=gcc ultrapatch >"$tmp/same-b.log" 2>&1 &
same_b=$!
rc=0
wait "$same_a" || rc=1
wait "$same_b" || rc=1
touch "$tmp/same.done"
wait "$same_reader" || rc=1
if [ "$rc" -ne 0 ]; then
  echo "check_build_profile.sh: concurrent same-profile build failed" >&2
  cat "$tmp/same-a.log" "$tmp/same-b.log" >&2
  exit 1
fi
[ -x "$same_tool" ] || { echo "check_build_profile.sh: same-profile tool missing" >&2; exit 1; }
"$same_tool" --help >/dev/null
if find "$(dirname "$same_tool")" -type f -name '*.tmp' -print -quit | grep -q .; then
  echo "check_build_profile.sh: same-profile build left a temporary file" >&2
  exit 1
fi

tree_digest() {
  local root=$1 path relative digest
  while IFS= read -r -d '' path; do
    relative=${path#"$root"/}
    digest=$(sha256sum "$path")
    printf '%s\t%s\n' "$relative" "${digest%% *}"
  done < <(find "$root" -type f -print0 | LC_ALL=C sort -z) | sha256sum | awk '{print $1}'
}

# An explicitly shared BUILD_DIR is allowed for one profile only. A different compiler must fail
# with the diagnostic and leave every existing file byte-for-byte unchanged.
shared="$tmp/shared-build"
run_make BUILD_DIR="$shared" CC=gcc ultrapatch >"$tmp/shared-gcc.log" 2>&1
shared_tool=$(run_make -s BUILD_DIR="$shared" CC=gcc "$host_tool_target")
"$shared_tool" --help >/dev/null
shared_before=$(tree_digest "$shared")
status=0
run_make -j2 BUILD_DIR="$shared" CC="$clang_command" ultrapatch \
  >"$tmp/shared-clang.log" 2>&1 || status=$?
if [ "$status" -eq 0 ]; then
  echo "check_build_profile.sh: a shared BUILD_DIR accepted a different profile" >&2
  exit 1
fi
grep -Fq 'host profile mismatch' "$tmp/shared-clang.log" || {
  echo "check_build_profile.sh: shared BUILD_DIR failed without a profile mismatch" >&2
  cat "$tmp/shared-clang.log" >&2
  exit 1
}
shared_after=$(tree_digest "$shared")
if [ "$shared_before" != "$shared_after" ]; then
  echo "check_build_profile.sh: profile mismatch mutated the existing build tree" >&2
  exit 1
fi
"$shared_tool" --help >/dev/null

echo "build_profiles=OK (gcc/clang/flags/decoder/objcopy isolated; CPATH sanitized; same-profile publication atomic; shared BUILD_DIR mismatch immutable)"
