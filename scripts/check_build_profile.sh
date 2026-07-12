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
read -r -a make_argv <<<"$make_command"
if [ "${#make_argv[@]}" -eq 0 ] || [ -z "$host_tool_target" ] || [ -z "$clang_command" ]; then
  echo "check_build_profile.sh: empty MAKE, CLANG, or host-tool target" >&2
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

if [ "$gcc_tool" = "$clang_tool" ] || [ "$gcc_tool" = "$alternate_tool" ] || \
   [ "$clang_tool" = "$alternate_tool" ]; then
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
gcc_recipe="$gcc_dir/host-build.recipe.json"
gcc_encoder_obj="$gcc_dir/obj/src/enc_util.o"
gcc_backend_obj="$gcc_dir/obj/src/patch_host_backend.o"
gcc_vendor_obj="$gcc_dir/obj/vendor/libdivsufsort/divsufsort.o"
for artifact in "$gcc_recipe" "$gcc_encoder_obj" "${gcc_encoder_obj%.o}.d" \
                "$gcc_backend_obj" "${gcc_backend_obj%.o}.d" \
                "$gcc_vendor_obj" "${gcc_vendor_obj%.o}.d"; do
  [ -f "$artifact" ] || {
    echo "check_build_profile.sh: missing per-TU artifact $artifact" >&2
    exit 1
  }
done

# The pinned profile and local recipe must describe the exact role split: encoder/vendor TUs use
# CFLAGS, while only the backend embedding patch_apply.h receives decoder integration defines.
python3 - "$gcc_dir/profile.json" "$gcc_recipe" <<'PY'
import json
from pathlib import Path
import sys

profile = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
recipe = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
flags = profile["flags"]
encoder = recipe["encoder"]["flags"]
backend = recipe["decoder_backend"]["flags"]
assert encoder == flags["encoder_cflags"]
assert backend == flags["backend_cflags"]
assert "-DPATCH_IMAGE_BASE=0u" not in encoder
assert "-D_POSIX_C_SOURCE=200809L" not in encoder
assert "-DPATCH_IMAGE_BASE=0u" in backend
assert "-D_POSIX_C_SOURCE=200809L" in backend
assert recipe["link"]["driver_flags"] == flags["link_driver_flags"]
assert recipe["link"]["ldflags"] == flags["ldflags"]
assert recipe["encoder"]["sources"][-1] == "vendor/libdivsufsort/divsufsort.c"
assert recipe["decoder_backend"]["sources"] == ["src/patch_host_backend.c"]
PY

# A no-op invocation refreshes the content check for the recipe signature, but does not disturb
# any object, dependency, recipe, or executable timestamp/inode.
noop_tool_before=$(file_state "$gcc_tool")
noop_recipe_before=$(file_state "$gcc_recipe")
noop_encoder_before=$(file_state "$gcc_encoder_obj")
noop_backend_before=$(file_state "$gcc_backend_obj")
noop_vendor_before=$(file_state "$gcc_vendor_obj")
run_make BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null
require_unchanged "$noop_tool_before" "$(file_state "$gcc_tool")" "no-op host tool"
require_unchanged "$noop_recipe_before" "$(file_state "$gcc_recipe")" "no-op recipe signature"
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
require_changed "$decoder_tool_before" "$(file_state "$gcc_tool")" "decoder-header host tool"
require_unchanged "$decoder_encoder_before" "$(file_state "$gcc_encoder_obj")" \
  "encoder object after decoder-header touch"
require_unchanged "$decoder_vendor_before" "$(file_state "$gcc_vendor_obj")" \
  "vendored object after decoder-header touch"

# Source ordering and explicit recipe revisions are not part of the compiler profile, so the
# content-stable build signature must invalidate objects/linking inside the same profile directory.
default_recipe_hash=$(sha256sum "$gcc_recipe")
default_recipe_hash=${default_recipe_hash%% *}
source_obj_before=$(file_state "$gcc_encoder_obj")
source_tool_before=$(file_state "$gcc_tool")
reordered_modules='src/enc_plan.c src/enc_util.c src/enc_elf.c src/enc_bsdiff.c src/enc_field.c src/enc_rc.c src/enc_lz.c src/enc_emit.c'
run_make BUILD_ROOT="$build_root" CC=gcc ENC_MODULE_SRCS="$reordered_modules" ultrapatch >/dev/null
reordered_hash=$(sha256sum "$gcc_recipe")
reordered_hash=${reordered_hash%% *}
require_changed "$default_recipe_hash" "$reordered_hash" "encoder source-list recipe"
require_changed "$source_obj_before" "$(file_state "$gcc_encoder_obj")" \
  "encoder object after source-list change"
require_changed "$source_tool_before" "$(file_state "$gcc_tool")" \
  "host tool after source-list change"
"$gcc_tool" --help >/dev/null

run_make BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null
restored_recipe_hash=$(sha256sum "$gcc_recipe")
restored_recipe_hash=${restored_recipe_hash%% *}
require_unchanged "$default_recipe_hash" "$restored_recipe_hash" "restored source-list recipe"
tag_obj_before=$(file_state "$gcc_encoder_obj")
tag_tool_before=$(file_state "$gcc_tool")
run_make BUILD_ROOT="$build_root" CC=gcc HOST_BUILD_RECIPE_TAG=per-tu-regression ultrapatch >/dev/null
tagged_hash=$(sha256sum "$gcc_recipe")
tagged_hash=${tagged_hash%% *}
require_changed "$default_recipe_hash" "$tagged_hash" "build-recipe tag signature"
require_changed "$tag_obj_before" "$(file_state "$gcc_encoder_obj")" \
  "encoder object after recipe change"
require_changed "$tag_tool_before" "$(file_state "$gcc_tool")" \
  "host tool after recipe change"
run_make BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null

# The Makefile itself is a direct object/link prerequisite. Simulate touching it and prove every
# role is recompiled even though the content-stable recipe JSON correctly keeps its old mtime.
makefile_recipe_before=$(file_state "$gcc_recipe")
makefile_encoder_before=$(file_state "$gcc_encoder_obj")
makefile_backend_before=$(file_state "$gcc_backend_obj")
makefile_vendor_before=$(file_state "$gcc_vendor_obj")
makefile_tool_before=$(file_state "$gcc_tool")
run_make -W Makefile BUILD_ROOT="$build_root" CC=gcc ultrapatch >/dev/null
require_unchanged "$makefile_recipe_before" "$(file_state "$gcc_recipe")" \
  "content-stable recipe after Makefile touch"
require_changed "$makefile_encoder_before" "$(file_state "$gcc_encoder_obj")" \
  "encoder object after Makefile touch"
require_changed "$makefile_backend_before" "$(file_state "$gcc_backend_obj")" \
  "backend object after Makefile touch"
require_changed "$makefile_vendor_before" "$(file_state "$gcc_vendor_obj")" \
  "vendored object after Makefile touch"
require_changed "$makefile_tool_before" "$(file_state "$gcc_tool")" \
  "host tool after Makefile touch"

# run_gate.sh assumes the already-published tool with make -o. Prove that contract skips the
# complete compile/link subtree even when a changed recipe tag would otherwise force a rebuild.
old_tool_before=$(file_state "$gcc_tool")
old_recipe_before=$(file_state "$gcc_recipe")
old_encoder_before=$(file_state "$gcc_encoder_obj")
old_backend_before=$(file_state "$gcc_backend_obj")
run_make -o "$gcc_tool" BUILD_ROOT="$build_root" CC=gcc \
  HOST_BUILD_RECIPE_TAG=old-target-must-not-rebuild ultrapatch >/dev/null
require_unchanged "$old_tool_before" "$(file_state "$gcc_tool")" "-o host tool"
require_unchanged "$old_recipe_before" "$(file_state "$gcc_recipe")" "-o recipe signature"
require_unchanged "$old_encoder_before" "$(file_state "$gcc_encoder_obj")" "-o encoder object"
require_unchanged "$old_backend_before" "$(file_state "$gcc_backend_obj")" "-o backend object"

# Decoder flag changes receive their own exact profile and appear only in the backend recipe.
decoder_probe_flags='-DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u -DUP_BACKEND_PROFILE_PROBE=1'
decoder_tool=$(tool_path CC=gcc DECODER_CONFIG_FLAGS="$decoder_probe_flags")
[ "$decoder_tool" != "$gcc_tool" ] || {
  echo "check_build_profile.sh: decoder flags did not select a distinct profile" >&2
  exit 1
}
run_make BUILD_ROOT="$build_root" CC=gcc DECODER_CONFIG_FLAGS="$decoder_probe_flags" \
  ultrapatch >/dev/null
python3 - "$(dirname "$decoder_tool")/host-build.recipe.json" <<'PY'
import json
from pathlib import Path
import sys

recipe = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
probe = "-DUP_BACKEND_PROFILE_PROBE=1"
assert probe in recipe["decoder_backend"]["flags"]
assert probe not in recipe["encoder"]["flags"]
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

# The distributor is a content-stable atomic publisher, not just a concatenation recipe.  Exercise
# its filesystem contract through Make's DECODER_PUBLIC_HDRS-driven hook so this regression never
# grows a second hard-coded public-header list.
header_dir="$tmp/single-header"
header="$header_dir/patch_apply_single.h"
mkdir -p "$header_dir"

# A first publication has a fixed public-readable mode even under a restrictive caller umask.
( umask 0077; run_make DECODER_SINGLE_HDR="$header" decoder-header-internal ) \
  >"$tmp/header-first.log" 2>&1
if [ ! -s "$header" ] || [ "$(stat -c '%a' "$header")" != 644 ]; then
  echo "check_build_profile.sh: first single-header publication was not nonempty mode 0644" >&2
  cat "$tmp/header-first.log" >&2
  exit 1
fi
grep -q '^/\* ===== begin src/patch_apply.h ===== \*/$' "$header"
header_hash=$(sha256sum "$header")
header_hash=${header_hash%% *}
header_state=$(stat -c '%i:%s:%y:%a' "$header")

# Identical bytes are a genuine no-op, including inode, timestamp, and permissions.
run_make DECODER_SINGLE_HDR="$header" decoder-header-internal >"$tmp/header-noop.log" 2>&1
require_unchanged "$header_state" "$(stat -c '%i:%s:%y:%a' "$header")" \
  "content-stable single header"
noop_hash=$(sha256sum "$header")
noop_hash=${noop_hash%% *}
require_unchanged "$header_hash" "$noop_hash" "content-stable single-header bytes"

# Replacing stale regular artifacts preserves their exact readable permission mode.
printf '\n/* stale */\n' >> "$header"
chmod 0440 "$header"
run_make DECODER_SINGLE_HDR="$header" decoder-header-internal >"$tmp/header-0440.log" 2>&1
mode=$(stat -c '%a' "$header")
restored_hash=$(sha256sum "$header")
restored_hash=${restored_hash%% *}
if [ "$mode" != 440 ] || [ "$restored_hash" != "$header_hash" ]; then
  echo "check_build_profile.sh: single-header overwrite did not preserve mode 0440" >&2
  exit 1
fi

chmod 0640 "$header"
printf '\n/* stale again */\n' >> "$header"
run_make DECODER_SINGLE_HDR="$header" decoder-header-internal >"$tmp/header-0640.log" 2>&1
mode=$(stat -c '%a' "$header")
restored_hash=$(sha256sum "$header")
restored_hash=${restored_hash%% *}
if [ "$mode" != 640 ] || [ "$restored_hash" != "$header_hash" ]; then
  echo "check_build_profile.sh: single-header overwrite did not preserve mode 0640" >&2
  exit 1
fi

# All source reads precede destination handling.  A missing dependency must preserve the
# published bytes, inode/timestamp, and permission mode and must leave no sibling temporary.
missing_state=$(stat -c '%i:%s:%y:%a' "$header")
missing_hash=$(sha256sum "$header")
missing_hash=${missing_hash%% *}
if python3 scripts/gen_single_header.py "$header" "$tmp/missing-public-header.h" \
    >"$tmp/header-missing.log" 2>&1; then
  echo "check_build_profile.sh: single-header generation accepted a missing dependency" >&2
  exit 1
fi
require_unchanged "$missing_state" "$(stat -c '%i:%s:%y:%a' "$header")" \
  "single header after missing dependency"
after_missing_hash=$(sha256sum "$header")
after_missing_hash=${after_missing_hash%% *}
require_unchanged "$missing_hash" "$after_missing_hash" \
  "single-header bytes after missing dependency"

# Same-destination publishers use private sibling temporaries and converge on canonical bytes.
printf '\n/* concurrent stale */\n' >> "$header"
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
concurrent_hash=$(sha256sum "$header")
concurrent_hash=${concurrent_hash%% *}
if [ "$concurrent_hash" != "$header_hash" ] || [ "$(stat -c '%a' "$header")" != 640 ]; then
  echo "check_build_profile.sh: concurrent single-header publication was not canonical mode 0640" >&2
  exit 1
fi
if find "$header_dir" -maxdepth 1 -name '.patch_apply_single.h.*.tmp' -print -quit | grep -q .; then
  echo "check_build_profile.sh: single-header generation left a temporary file" >&2
  exit 1
fi

# Compile as an external integrator would: the generated header and consumer are isolated in a
# private directory, and the compiler receives no repository or src include search path.
cat > "$header_dir/consumer.c" <<'EOF'
#include <stdint.h>
#define CORTEX_M0 1
#define PATCH_IMAGE_BASE 0u
#define PATCH_IMAGE_CAPACITY 67108864u
#include "patch_apply_single.h"

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
int consume_single_header(void){
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

echo "build_profiles=OK (exact per-TU flags/deps; incremental rebuilds; gcc/clang/config isolated; same-profile artifacts atomic; single-header publish atomic; shared BUILD_DIR mismatch immutable)"
