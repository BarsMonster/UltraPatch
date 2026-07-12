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
clang_command="${CLANG:-clang}"
: "${DECODER_PUBLIC_HDRS:?check_build_profile.sh: DECODER_PUBLIC_HDRS not set by make}"
objcopy_command="${ARM_OBJCOPY:-arm-none-eabi-objcopy}"
read -r -a make_argv <<<"$make_command"
if [ "${#make_argv[@]}" -eq 0 ] || [ -z "$host_tool_target" ] || \
   [ -z "$clang_command" ] || [ -z "$objcopy_command" ]; then
  echo "check_build_profile.sh: empty MAKE, CLANG, ARM_OBJCOPY, or host-tool target" >&2
  exit 2
fi

. "$(dirname "$0")/tempdir.sh"
build_root="$tmp/profiles"
release_lock="${RELEASE_PROFILE_LOCK:-toolchains/release-profile.json}"
vendor_header=vendor/libdivsufsort/divsufsort.c.inc.h
decoder_header=src/patch_apply.h

container=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["container"])' \
  "$release_lock")
if ! grep -Fqx "      image: $container" .github/workflows/gate.yml; then
  echo "check_build_profile.sh: workflow container does not match $release_lock" >&2
  exit 1
fi

run_make() {
  "${make_argv[@]}" --no-print-directory "$@"
}

file_state() {
  stat -c '%i:%s:%y' "$1"
}

require_unchanged() {
  if [ "$1" != "$2" ]; then
    echo "check_build_profile.sh: $3 changed unexpectedly" >&2
    exit 1
  fi
}

require_changed() {
  if [ "$1" = "$2" ]; then
    echo "check_build_profile.sh: $3 was not rebuilt" >&2
    exit 1
  fi
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
clang_tool=$(tool_path CC="$clang_command")
alternate_tool=$(tool_path CC=gcc CFLAGS_EXTRA=-DUP_BUILD_PROFILE_ALTERNATE=1)
objcopy_proxy="$tmp/objcopy-profile-proxy"
ln -s "$(command -v "$objcopy_command")" "$objcopy_proxy"
objcopy_tool=$(tool_path CC=gcc ARM_OBJCOPY="$objcopy_proxy")

if [ "$gcc_tool" = "$clang_tool" ] || [ "$gcc_tool" = "$alternate_tool" ] || \
   [ "$clang_tool" = "$alternate_tool" ] || [ "$objcopy_tool" = "$gcc_tool" ]; then
  echo "check_build_profile.sh: distinct profiles selected the same HOST_TOOL" >&2
  exit 1
fi

run_make BUILD_ROOT="$build_root" CC=gcc ultrapatch >"$tmp/gcc.log" 2>&1 &
gcc_pid=$!
run_make BUILD_ROOT="$build_root" CC="$clang_command" ultrapatch >"$tmp/clang.log" 2>&1 &
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

# Ambient compiler search paths are deliberately removed at the Make boundary.  A poison
# standard header must neither select another host profile nor reach a real compilation.
poison="$tmp/compiler-search-poison"
mkdir "$poison"
printf '%s\n' '#error "CPATH reached the compiler"' >"$poison/stdint.h"
poison_tool=$(tool_path CC=gcc CPATH="$poison")
require_unchanged "$gcc_tool" "$poison_tool" "CPATH host profile"
run_make -B BUILD_ROOT="$build_root" CC=gcc CPATH="$poison" all-internal \
  >"$tmp/cpath.log" 2>&1 || {
    echo "check_build_profile.sh: sanitized CPATH affected compilation" >&2
    cat "$tmp/cpath.log" >&2
    exit 1
  }

gcc_dir=$(dirname "$gcc_tool")
gcc_encoder_obj="$gcc_dir/obj/src/enc_util.o"
gcc_backend_obj="$gcc_dir/obj/src/patch_host_backend.o"
gcc_vendor_obj="$gcc_dir/obj/vendor/libdivsufsort/divsufsort.o"
for artifact in "$gcc_encoder_obj" "${gcc_encoder_obj%.o}.d" \
                "$gcc_backend_obj" "${gcc_backend_obj%.o}.d" \
                "$gcc_vendor_obj" "${gcc_vendor_obj%.o}.d"; do
  [ -f "$artifact" ] || {
    echo "check_build_profile.sh: missing per-TU artifact $artifact" >&2
    exit 1
  }
done

# The profile describes the exact role split and link order: encoder/vendor TUs use CFLAGS, while
# only the backend embedding patch_apply.h receives decoder integration defines.
python3 - "$gcc_dir/profile.json" <<'PY'
import json
from pathlib import Path
import sys

profile = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
flags = profile["flags"]
build = profile["build"]
encoder = build["source_roles"]["encoder"]
backend = build["source_roles"]["decoder_backend"]
assert build["recipe_revision"] == "per-tu-v1"
assert encoder[-1] == "vendor/libdivsufsort/divsufsort.c"
assert backend == ["src/patch_host_backend.c"]
assert build["link_objects"] == ["obj/" + source.removesuffix(".c") + ".o"
                                  for source in encoder + backend]
assert "-DPATCH_IMAGE_BASE=0u" not in flags["encoder_cflags"]
assert "-D_POSIX_C_SOURCE=200809L" not in flags["encoder_cflags"]
assert "-DPATCH_IMAGE_BASE=0u" in flags["backend_cflags"]
assert "-D_POSIX_C_SOURCE=200809L" in flags["backend_cflags"]
PY

# A no-op invocation does not disturb any object, dependency, or executable timestamp/inode.
noop_tool_before=$(file_state "$gcc_tool")
noop_encoder_before=$(file_state "$gcc_encoder_obj")
noop_backend_before=$(file_state "$gcc_backend_obj")
noop_vendor_before=$(file_state "$gcc_vendor_obj")
run_make BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null
require_unchanged "$noop_tool_before" "$(file_state "$gcc_tool")" "no-op host tool"
require_unchanged "$noop_encoder_before" "$(file_state "$gcc_encoder_obj")" "no-op encoder object"
require_unchanged "$noop_backend_before" "$(file_state "$gcc_backend_obj")" "no-op backend object"
require_unchanged "$noop_vendor_before" "$(file_state "$gcc_vendor_obj")" "no-op vendored object"

# GCC-generated dependency files must carry transitive vendored includes. Touching a nested
# implementation header rebuilds only libdivsufsort and the final link.
grep -Fq "$vendor_header" "${gcc_vendor_obj%.o}.d"
vendor_before=$(file_state "$gcc_vendor_obj")
vendor_encoder_before=$(file_state "$gcc_encoder_obj")
vendor_backend_before=$(file_state "$gcc_backend_obj")
vendor_tool_before=$(file_state "$gcc_tool")
# GNU make's -W is a deterministic, non-mutating simulation of touching the dependency now.
run_make -W "$vendor_header" BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null
require_changed "$vendor_before" "$(file_state "$gcc_vendor_obj")" "transitive vendored object"
require_changed "$vendor_tool_before" "$(file_state "$gcc_tool")" "vendored-header host tool"
require_unchanged "$vendor_encoder_before" "$(file_state "$gcc_encoder_obj")" \
  "encoder object after vendored-header touch"
require_unchanged "$vendor_backend_before" "$(file_state "$gcc_backend_obj")" \
  "backend object after vendored-header touch"

# Decoder public headers are dependencies of the backend object only; an edit must not recompile
# encoder or vendored translation units.
grep -Fq "$decoder_header" "${gcc_backend_obj%.o}.d"
decoder_before=$(file_state "$gcc_backend_obj")
decoder_encoder_before=$(file_state "$gcc_encoder_obj")
decoder_vendor_before=$(file_state "$gcc_vendor_obj")
decoder_tool_before=$(file_state "$gcc_tool")
run_make -W "$decoder_header" BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null
require_changed "$decoder_before" "$(file_state "$gcc_backend_obj")" "decoder backend object"
require_changed "$decoder_tool_before" "$(file_state "$gcc_tool")" "decoder public-header host tool"
require_unchanged "$decoder_encoder_before" "$(file_state "$gcc_encoder_obj")" \
  "encoder object after decoder public-header touch"
require_unchanged "$decoder_vendor_before" "$(file_state "$gcc_vendor_obj")" \
  "vendored object after decoder public-header touch"

# Source ordering and explicit recipe revisions participate in the profile ID and select isolated
# build directories without disturbing the default profile.
default_tool_before=$(file_state "$gcc_tool")
reordered_modules='src/enc_plan.c src/enc_util.c src/enc_elf.c src/enc_bsdiff.c src/enc_field.c src/enc_rc.c src/enc_lz.c src/enc_emit.c'
reordered_tool=$(tool_path CC=gcc ENC_MODULE_SRCS="$reordered_modules")
revision_tool=$(tool_path CC=gcc HOST_BUILD_RECIPE_TAG=per-tu-regression)
if [ "$reordered_tool" = "$gcc_tool" ] || [ "$revision_tool" = "$gcc_tool" ] || \
   [ "$reordered_tool" = "$revision_tool" ]; then
  echo "check_build_profile.sh: build recipe identities selected the same HOST_TOOL" >&2
  exit 1
fi
run_make BUILD_ROOT="$build_root" CC=gcc ENC_MODULE_SRCS="$reordered_modules" ultrapatch >/dev/null
run_make BUILD_ROOT="$build_root" CC=gcc HOST_BUILD_RECIPE_TAG=per-tu-regression ultrapatch >/dev/null
"$reordered_tool" --help >/dev/null
"$revision_tool" --help >/dev/null
require_unchanged "$default_tool_before" "$(file_state "$gcc_tool")" \
  "default host tool after alternate build recipes"

# The Makefile itself is a direct object/link prerequisite. Simulate touching it and prove every
# role is recompiled within the selected profile.
makefile_encoder_before=$(file_state "$gcc_encoder_obj")
makefile_backend_before=$(file_state "$gcc_backend_obj")
makefile_vendor_before=$(file_state "$gcc_vendor_obj")
makefile_tool_before=$(file_state "$gcc_tool")
run_make -W Makefile BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null
require_changed "$makefile_encoder_before" "$(file_state "$gcc_encoder_obj")" \
  "encoder object after Makefile touch"
require_changed "$makefile_backend_before" "$(file_state "$gcc_backend_obj")" \
  "backend object after Makefile touch"
require_changed "$makefile_vendor_before" "$(file_state "$gcc_vendor_obj")" \
  "vendored object after Makefile touch"
require_changed "$makefile_tool_before" "$(file_state "$gcc_tool")" \
  "host tool after Makefile touch"

# Decoder flag changes receive their own exact profile and appear only in the backend flags.
decoder_probe_flags='-DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u -DUP_BACKEND_PROFILE_PROBE=1'
decoder_tool=$(tool_path CC=gcc DECODER_CONFIG_FLAGS="$decoder_probe_flags")
[ "$decoder_tool" != "$gcc_tool" ] || {
  echo "check_build_profile.sh: decoder flags did not select a distinct profile" >&2
  exit 1
}
run_make BUILD_ROOT="$build_root" CC=gcc DECODER_CONFIG_FLAGS="$decoder_probe_flags" \
  ultrapatch >/dev/null
python3 - "$(dirname "$decoder_tool")/profile.json" <<'PY'
import json
from pathlib import Path
import sys

profile = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
probe = "-DUP_BACKEND_PROFILE_PROBE=1"
assert probe in profile["flags"]["backend_cflags"]
assert probe not in profile["flags"]["encoder_cflags"]
PY
"$decoder_tool" --help >/dev/null

# Same-profile top-level rebuilds may start together. A live reader continuously executes the
# previous tool while unique sibling object/dependency/link temporaries are atomically published.
same_root="$tmp/same-profile"
run_make BUILD_ROOT="$same_root" CC=gcc ultrapatch >"$tmp/same-initial.log" 2>&1
same_tool=$(run_make -s BUILD_ROOT="$same_root" CC=gcc "$host_tool_target")
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
run_make BUILD_ROOT="$same_root" CC=gcc ultrapatch >/dev/null

# Exercise the public packaging contract without a repository include path. The integration TU
# includes only patch_apply.h; its two transitive support headers must be installed beside it.
header_dir="$tmp/public-headers"
mkdir -p "$header_dir"
cp $DECODER_PUBLIC_HDRS "$header_dir/"
cat > "$header_dir/consumer.c" <<'EOF'
#include <stdint.h>
#define CORTEX_M0 1
#define PATCH_IMAGE_BASE 0u
#define PATCH_IMAGE_CAPACITY 67108864u
#include "patch_apply.h"

uint8_t flash_read(uint32_t absolute_addr){
    (void)absolute_addr;
    return 0xffu;
}
void flash_write_page(uint32_t absolute_page_addr, const uint8_t page[OUTROW]){
    (void)absolute_page_addr;
    (void)page;
}
static int pull_end(void *ctx, uint8_t *out){
    (void)ctx;
    (void)out;
    return PATCH_PULL_END;
}
int consume_decoder(void){
    PatchApply state;
    return (int)patch_apply_run(&state, pull_end, 0);
}
EOF
( cd "$header_dir" && gcc -std=c11 -Wall -Wextra -Werror -c consumer.c -o consumer.o )

shared="$tmp/shared-build"
run_make BUILD_DIR="$shared" CC=gcc ultrapatch >"$tmp/shared-gcc.log" 2>&1
shared_tool=$(run_make -s BUILD_DIR="$shared" CC=gcc "$host_tool_target")
shared_before=$(find "$shared" -type f -printf '%P\t%i\t%s\t%T@\n' | LC_ALL=C sort | sha256sum)
shared_before=${shared_before%% *}
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
shared_after=$(find "$shared" -type f -printf '%P\t%i\t%s\t%T@\n' | LC_ALL=C sort | sha256sum)
shared_after=${shared_after%% *}
if [ "$shared_before" != "$shared_after" ]; then
  echo "check_build_profile.sh: profile mismatch mutated the existing build tree" >&2
  exit 1
fi

echo "build_profiles=OK (exact per-TU flags/deps; incremental rebuilds; gcc/clang/config isolated; same-profile artifacts atomic; isolated public-header integration; shared BUILD_DIR mismatch immutable)"
