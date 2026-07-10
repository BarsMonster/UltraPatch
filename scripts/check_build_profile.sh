#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Host-profile isolation regression. Optional arguments override MAKE and the Make target that
# prints the selected HOST_TOOL; the equivalent environment knobs are MAKE/HOST_TOOL_TARGET.
set -eu

if [ "$#" -gt 2 ]; then
  echo "usage: $0 [make-command [host-tool-target]]" >&2
  exit 2
fi
make_command="${1:-${MAKE:-make}}"
host_tool_target="${2:-${HOST_TOOL_TARGET:-host-tool-path}}"
read -r -a make_argv <<<"$make_command"
if [ "${#make_argv[@]}" -eq 0 ] || [ -z "$host_tool_target" ]; then
  echo "check_build_profile.sh: empty MAKE or host-tool target" >&2
  exit 2
fi

. "$(dirname "$0")/tempdir.sh"
build_root="$tmp/profiles"
release_lock="${RELEASE_PROFILE_LOCK:-toolchains/release-profile.json}"

container=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["container"])' \
  "$release_lock")
if ! grep -Fqx "      image: $container" .github/workflows/gate.yml; then
  echo "check_build_profile.sh: workflow container does not match $release_lock" >&2
  exit 1
fi

run_make() {
  "${make_argv[@]}" --no-print-directory "$@"
}

tool_path() {
  local output
  output=$(run_make -s BUILD_ROOT="$build_root" "$@" "$host_tool_target")
  if [ "$(printf '%s\n' "$output" | sed '/^$/d' | wc -l)" -ne 1 ]; then
    echo "check_build_profile.sh: $host_tool_target did not print one HOST_TOOL path" >&2
    printf '%s\n' "$output" >&2
    return 1
  fi
  printf '%s\n' "$output"
}

gcc_tool=$(tool_path CC=gcc)
clang_tool=$(tool_path CC=clang)
alternate_tool=$(tool_path CC=gcc CFLAGS_EXTRA=-DUP_BUILD_PROFILE_ALTERNATE=1)

if [ "$gcc_tool" = "$clang_tool" ] || [ "$gcc_tool" = "$alternate_tool" ] || \
   [ "$clang_tool" = "$alternate_tool" ]; then
  echo "check_build_profile.sh: distinct profiles selected the same HOST_TOOL" >&2
  exit 1
fi

run_make BUILD_ROOT="$build_root" CC=gcc ultrapatch >"$tmp/gcc.log" 2>&1 &
gcc_pid=$!
run_make BUILD_ROOT="$build_root" CC=clang ultrapatch >"$tmp/clang.log" 2>&1 &
clang_pid=$!
run_make BUILD_ROOT="$build_root" CC=gcc CFLAGS_EXTRA=-DUP_BUILD_PROFILE_ALTERNATE=1 \
  ultrapatch >"$tmp/alternate.log" 2>&1 &
alternate_pid=$!
rc=0
wait "$gcc_pid" || rc=1
wait "$clang_pid" || rc=1
wait "$alternate_pid" || rc=1
if [ "$rc" -ne 0 ]; then
  echo "check_build_profile.sh: concurrent profile build failed" >&2
  for log in "$tmp/gcc.log" "$tmp/clang.log" "$tmp/alternate.log"; do
    [ -s "$log" ] && { echo "--- $log" >&2; cat "$log" >&2; }
  done
  exit 1
fi

for tool in "$gcc_tool" "$clang_tool" "$alternate_tool"; do
  [ -x "$tool" ] || { echo "check_build_profile.sh: missing HOST_TOOL $tool" >&2; exit 1; }
  [ -f "$(dirname "$tool")/profile.json" ] || {
    echo "check_build_profile.sh: missing profile manifest for $tool" >&2
    exit 1
  }
  "$tool" --help >/dev/null
done

# Same-profile top-level builds may start together. They use unique sibling temporaries and
# atomically publish an equivalent executable; neither process may expose or leave a partial file.
same_root="$tmp/same-profile"
run_make -B BUILD_ROOT="$same_root" CC=gcc ultrapatch >"$tmp/same-a.log" 2>&1 &
same_a=$!
run_make -B BUILD_ROOT="$same_root" CC=gcc ultrapatch >"$tmp/same-b.log" 2>&1 &
same_b=$!
rc=0
wait "$same_a" || rc=1
wait "$same_b" || rc=1
if [ "$rc" -ne 0 ]; then
  echo "check_build_profile.sh: concurrent same-profile build failed" >&2
  cat "$tmp/same-a.log" "$tmp/same-b.log" >&2
  exit 1
fi
same_tool=$(run_make -s BUILD_ROOT="$same_root" CC=gcc "$host_tool_target")
[ -x "$same_tool" ] || { echo "check_build_profile.sh: same-profile tool missing" >&2; exit 1; }
"$same_tool" --help >/dev/null
if find "$(dirname "$same_tool")" -maxdepth 1 -name '*.tmp' -print -quit | grep -q .; then
  echo "check_build_profile.sh: same-profile build left a temporary file" >&2
  exit 1
fi

header="$tmp/patch_apply_single.h"
run_make DECODER_SINGLE_HDR="$header" decoder-header-internal >"$tmp/header-a.log" 2>&1 &
header_a=$!
run_make DECODER_SINGLE_HDR="$header" decoder-header-internal >"$tmp/header-b.log" 2>&1 &
header_b=$!
rc=0
wait "$header_a" || rc=1
wait "$header_b" || rc=1
if [ "$rc" -ne 0 ] || [ ! -s "$header" ]; then
  echo "check_build_profile.sh: concurrent single-header generation failed" >&2
  cat "$tmp/header-a.log" "$tmp/header-b.log" >&2
  exit 1
fi
grep -q '^/\* ===== begin src/patch_apply.h ===== \*/$' "$header"
if find "$tmp" -maxdepth 1 -name '.patch_apply_single.h.*.tmp' -print -quit | grep -q .; then
  echo "check_build_profile.sh: single-header generation left a temporary file" >&2
  exit 1
fi

shared="$tmp/shared-build"
run_make BUILD_DIR="$shared" CC=gcc ultrapatch >"$tmp/shared-gcc.log" 2>&1
shared_tool=$(run_make -s BUILD_DIR="$shared" CC=gcc "$host_tool_target")
shared_before=$(sha256sum "$shared_tool")
shared_before=${shared_before%% *}
status=0
run_make -j2 -B BUILD_DIR="$shared" CC=clang ultrapatch \
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
shared_after=$(sha256sum "$shared_tool")
shared_after=${shared_after%% *}
if [ "$shared_before" != "$shared_after" ]; then
  echo "check_build_profile.sh: profile mismatch overwrote the existing host tool" >&2
  exit 1
fi

echo "build_profiles=OK (gcc/clang/alternate isolated; same-profile artifacts atomic; shared BUILD_DIR mismatch rejected)"
