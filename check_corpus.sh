#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Size-only release regression for all shipped firmware pairs.
#
# Every encoder invocation applies its finished patch with the real decoder backend and checks
# the exact target plus NVM write safety before writing the blob.  This script therefore only
# schedules those self-verifying encodes and records their sizes; it does not decode or hash the
# blobs a second time.
#
# The release set is deliberately fixed and small enough to describe without another inventory:
#   * all 256 ordered pairs over the 16 sorted home image directories;
#   * both directions of the 17 adjacent sorted foreign versions;
#   * the real one-face fixture in both directions.
#
# Every parallel worker owns a private temporary directory.  Worker output is a single line, so
# completion order cannot corrupt another worker's result.
#
# Usage: check_corpus.sh [jobs]      (jobs defaults to nproc)
# Exit 3 on a malformed/missing corpus.
# Exit 4 when an encode fails or a size ratchet regresses.
set -u
export LC_ALL=C

JOBS="${1:-$(nproc 2>/dev/null || echo 4)}"
case "$JOBS" in
  ''|*[!0-9]*|0) echo "check_corpus.sh: jobs must be a positive integer" >&2; exit 3 ;;
esac

: "${ULTRAPATCH:?check_corpus.sh: ULTRAPATCH not set to the host tool}"

IMG=test-bench/images
FGN=test-bench/foreign
FIX=test-bench/fixtures
UP="$ULTRAPATCH"

# One aggregate ratchet covers the complete home and foreign corpus. The real one-face update
# remains independently visible because it is the product release patch.
CORPUS_LIMIT=5388359
ONEFACE_GROW_LIMIT=573
ONEFACE_REVERT_LIMIT=290

[ -x "$UP" ] || { echo "check_corpus.sh: encoder is missing or not executable: $UP" >&2; exit 3; }

mapfile -t HOME_DIRS < <(
  for path in "$IMG"/img_*; do [ -d "$path" ] && printf '%s\n' "$path"; done | sort
)
mapfile -t FOREIGN_DIRS < <(
  for path in "$FGN"/*; do [ -d "$path" ] && printf '%s\n' "$path"; done | sort -V
)

if [ "${#HOME_DIRS[@]}" -ne 16 ] || [ "${#FOREIGN_DIRS[@]}" -ne 18 ]; then
  echo "check_corpus.sh: release corpus must contain 16 home and 18 foreign images (found ${#HOME_DIRS[@]}/${#FOREIGN_DIRS[@]})" >&2
  exit 3
fi

for path in "${HOME_DIRS[@]}" "${FOREIGN_DIRS[@]}" \
            "$FIX/v0_base" "$FIX/v1_one_face"; do
  [ -f "$path/watch.bin" ] || {
    echo "check_corpus.sh: missing firmware image: $path/watch.bin" >&2
    exit 3
  }
done

tmp=$(mktemp -d) || { echo "check_corpus.sh: cannot create temporary directory" >&2; exit 3; }
trap 'rm -rf "$tmp"' EXIT

# Exercise the shortest valid range stream. Its counted body is empty after the encoder drops
# the leading cache byte; the production decoder must supply the omitted zero bootstrap bytes.
: > "$tmp/empty.bin"
if ! "$UP" "$tmp/empty.bin" "$tmp/empty.bin" "$tmp/empty.patch" \
     >"$tmp/empty.stdout" 2>"$tmp/empty.stderr"; then
  echo "check_corpus.sh: empty-image self-verifying encode failed" >&2
  sed -n '1,20p' "$tmp/empty.stderr" >&2
  exit 4
fi
echo "short_body_regression=OK"

# Exercise cache startup, page transitions, and the final partial-page commit around the
# decoder's 256-byte OUTROW boundary. Same-size and shrink cases change the target's last
# shared byte, guaranteeing a dirty final physical page rather than a read-only seam. Encoder
# self-verification snapshots [to_size,page_round_up(max(from_size,to_size))) before apply;
# a successful encode therefore also proves that old physical-page content beyond the new
# logical image was preserved. Shrink cases protect real old-image bytes in that range, while
# grow cases protect the deterministic pre-existing flash padding after the target.
page_boundary_case() {
  local id=$1 from_n=$2 to_n=$3 d="$tmp/page-$1" common_n
  mkdir -p "$d" || return 1
  head -c "$from_n" "$FIX/v0_base/watch.bin" > "$d/from.bin" || return 1
  head -c "$to_n" "$FIX/v1_one_face/watch.bin" > "$d/to.bin" || return 1
  if [ "$from_n" -gt 0 ]; then
    printf '\132' | dd of="$d/from.bin" bs=1 seek=0 conv=notrunc status=none || return 1
    printf '\074' | dd of="$d/from.bin" bs=1 seek=$((from_n-1)) conv=notrunc status=none || return 1
  fi
  if [ "$to_n" -gt 0 ]; then
    printf '\245' | dd of="$d/to.bin" bs=1 seek=0 conv=notrunc status=none || return 1
    printf '\303' | dd of="$d/to.bin" bs=1 seek=$((to_n-1)) conv=notrunc status=none || return 1
  fi
  common_n=$((from_n < to_n ? from_n : to_n))
  if [ "$common_n" -gt 0 ]; then
    printf '\074' | dd of="$d/from.bin" bs=1 seek=$((common_n-1)) conv=notrunc status=none || return 1
    printf '\303' | dd of="$d/to.bin" bs=1 seek=$((common_n-1)) conv=notrunc status=none || return 1
  fi
  if ! "$UP" "$d/from.bin" "$d/to.bin" "$d/patch.blob" \
       >"$d/stdout" 2>"$d/stderr"; then
    echo "check_corpus.sh: page-boundary self-verification failed: $id ($from_n -> $to_n)" >&2
    sed -n '1,20p' "$d/stderr" >&2
    return 1
  fi
}

page_cases=0
while read -r id from_n to_n; do
  [ -n "$id" ] || continue
  if ! page_boundary_case "$id" "$from_n" "$to_n"; then
    exit 4
  fi
  page_cases=$((page_cases+1))
done <<'EOF'
empty-grow       0   1
empty-shrink     1   0
one-page         1   1
below-page     255 255
grow-to-page   255 256
shrink-in-page 256 255
grow-past-page 256 257
shrink-to-page 257 256
past-page      257 257
grow-to-2page  511 512
shrink-in-2page 512 511
grow-past-2page 512 513
shrink-to-2page 513 512
past-2page     513 513
EOF
printf 'page_boundary_regression=%d/14\n' "$page_cases"

# Exercise the shipped mutating CLI wrapper, not only the encoder's direct selfcheck call. Reuse
# the nontrivial two-page grow case above: successful decode must atomically replace a source copy
# with the exact target, while a truncated patch must fail without changing its input file.
decode_cli_regression() {
  local d=$1 patch_n
  cp "$d/from.bin" "$d/applied.bin" || return 1
  if ! "$UP" --decode "$d/applied.bin" "$d/patch.blob" \
       >"$d/decode.stdout" 2>"$d/decode.stderr"; then
    echo "check_corpus.sh: --decode regression failed on valid patch" >&2
    sed -n '1,20p' "$d/decode.stderr" >&2
    return 1
  fi
  if ! cmp -s "$d/applied.bin" "$d/to.bin"; then
    echo "check_corpus.sh: --decode result differs from target" >&2
    return 1
  fi

  patch_n=$(wc -c < "$d/patch.blob") || return 1
  if [ "$patch_n" -le 1 ]; then
    echo "check_corpus.sh: --decode regression patch is unexpectedly short" >&2
    return 1
  fi
  head -c $((patch_n-1)) "$d/patch.blob" > "$d/truncated.blob" || return 1
  cp "$d/from.bin" "$d/rejected.bin" || return 1
  if "$UP" --decode "$d/rejected.bin" "$d/truncated.blob" \
       >"$d/reject.stdout" 2>"$d/reject.stderr"; then
    echo "check_corpus.sh: --decode accepted a truncated patch" >&2
    return 1
  fi
  if ! cmp -s "$d/rejected.bin" "$d/from.bin"; then
    echo "check_corpus.sh: rejected --decode modified its input file" >&2
    return 1
  fi
}

if ! decode_cli_regression "$tmp/page-grow-past-page"; then
  exit 4
fi
echo "decode_cli_regression=OK"

append_foreign_pair() {
  local from=$1 to=$2 from_id to_id
  from_id=${from##*/}; to_id=${to##*/}
  printf 'P\t%s\t%s\t%s\t%s\n' "$from_id" "$to_id" \
    "$from/watch.bin" "$to/watch.bin" >> "$tmp/jobs.txt"
  printf 'P\t%s\t%s\t%s\t%s\n' "$to_id" "$from_id" \
    "$to/watch.bin" "$from/watch.bin" >> "$tmp/jobs.txt"
}

# Schedule both directions of each adjacent foreign edge. All foreign jobs precede the home matrix
# in jobs.txt, so xargs dispatches them in the first waves and their tails overlap the home run.
: > "$tmp/jobs.txt"
for ((i=1; i<${#FOREIGN_DIRS[@]}; i++)); do
  append_foreign_pair "${FOREIGN_DIRS[i-1]}" "${FOREIGN_DIRS[i]}"
done

for from in "${HOME_DIRS[@]}"; do
  for to in "${HOME_DIRS[@]}"; do
    printf 'P\t%s\t%s\t%s\t%s\n' "${from##*/}" "${to##*/}" \
      "$from/watch.bin" "$to/watch.bin" >> "$tmp/jobs.txt"
  done
done
printf 'G\toneface_grow\tv0_base_to_v1_one_face\t%s\t%s\n' \
  "$FIX/v0_base/watch.bin" "$FIX/v1_one_face/watch.bin" >> "$tmp/jobs.txt"
printf 'G\toneface_revert\tv1_one_face_to_v0_base\t%s\t%s\n' \
  "$FIX/v1_one_face/watch.bin" "$FIX/v0_base/watch.bin" >> "$tmp/jobs.txt"

scheduled=$(wc -l < "$tmp/jobs.txt")
if [ "$scheduled" -ne 292 ]; then
  echo "check_corpus.sh: internal job-count error: $scheduled (expected 292)" >&2
  exit 3
fi

cm_work() {
  local tag=$1 from_id=$2 to_id=$3 from=$4 to=$5 d sz
  d=$(mktemp -d "${TMPDIR:-/tmp}/ultrapatch-corpus-worker.XXXXXX") || return 1
  trap 'rm -rf "$d"' EXIT
  if ! "$UP" "$from" "$to" "$d/patch.blob" >"$d/stdout" 2>"$d/stderr"; then
    printf 'check_corpus.sh: encode failed: %s -> %s\n' "$from_id" "$to_id" >&2
    sed -n '1,20p' "$d/stderr" >&2
    return 1
  fi
  sz=$(wc -c < "$d/patch.blob") || return 1
  printf '%s\t%s\t%s\t%s\n' "$tag" "$from_id" "$to_id" "$sz"
  rm -rf "$d"
  trap - EXIT
}
export UP
export -f cm_work

if ! xargs -P "$JOBS" -n 5 bash -c 'cm_work "$@"' _ \
       < "$tmp/jobs.txt" > "$tmp/pairs.txt"; then
  echo "check_corpus.sh: at least one self-verifying encode failed" >&2
  exit 4
fi

if ! summary=$(awk '$1=="P"{count++; total+=$4}
                    $1=="G"{oneface++}
                    $1=="G" && $2=="oneface_grow"{grow=$4}
                    $1=="G" && $2=="oneface_revert"{revert=$4}
                    END{print count+0, total+0, oneface+0, grow+0, revert+0}' \
                   "$tmp/pairs.txt"); then
  echo "check_corpus.sh: cannot summarize corpus results" >&2
  exit 3
fi
summary_re='^([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)$'
if [[ ! "$summary" =~ $summary_re ]]; then
  echo "check_corpus.sh: malformed corpus summary" >&2
  exit 3
fi
corpus_count=${BASH_REMATCH[1]}
corpus_total=${BASH_REMATCH[2]}
oneface_count=${BASH_REMATCH[3]}
grow=${BASH_REMATCH[4]}
revert=${BASH_REMATCH[5]}

if [ "$corpus_count" -ne 290 ] || [ "$oneface_count" -ne 2 ] || \
   [ "$grow" -le 0 ] || [ "$revert" -le 0 ]; then
  echo "check_corpus.sh: structural error (corpus=$corpus_count/290 oneface=$oneface_count/2)" >&2
  exit 3
fi

printf 'corpus_ok=%d/290\ncorpus_total=%d\noneface_ok=%d/2\noneface_grow=%d\noneface_revert=%d\n' \
  "$corpus_count" "$corpus_total" "$oneface_count" "$grow" "$revert"

if [ "$corpus_total" -gt "$CORPUS_LIMIT" ]; then
  echo "check_corpus.sh: corpus size regression ($corpus_total > $CORPUS_LIMIT)" >&2
  exit 4
fi
if [ "$grow" -gt "$ONEFACE_GROW_LIMIT" ] || [ "$revert" -gt "$ONEFACE_REVERT_LIMIT" ]; then
  echo "check_corpus.sh: one-face size regression (grow=$grow/$ONEFACE_GROW_LIMIT revert=$revert/$ONEFACE_REVERT_LIMIT)" >&2
  exit 4
fi
echo "corpus_regression=OK"
