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
#   * both directions of the 17 adjacent sorted foreign versions, with the cross-major pair first;
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

: "${IMAGES:?check_corpus.sh: IMAGES not set to the build-local home corpus}"
: "${FIXTURES:?check_corpus.sh: FIXTURES not set to the build-local fixtures}"
: "${ULTRAPATCH:?check_corpus.sh: ULTRAPATCH not set to the host tool}"

IMG="$IMAGES"
FGN="${FOREIGN:-test-bench/foreign}"
FIX="$FIXTURES"
UP="$ULTRAPATCH"

# One aggregate ratchet covers the complete home and foreign corpus. The real one-face update
# remains independently visible because it is the product release patch.
CORPUS_LIMIT=5416247
ONEFACE_GROW_LIMIT=571
ONEFACE_REVERT_LIMIT=288

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

append_foreign_pair() {
  local from=$1 to=$2 from_id to_id
  from_id=${from##*/}; to_id=${to##*/}
  printf 'P\t%s\t%s\t%s\t%s\n' "$from_id" "$to_id" \
    "$from/watch.bin" "$to/watch.bin" >> "$tmp/jobs.txt"
  printf 'P\t%s\t%s\t%s\t%s\n' "$to_id" "$from_id" \
    "$to/watch.bin" "$from/watch.bin" >> "$tmp/jobs.txt"
}

# Start the slow cross-major foreign edge first, then the remaining adjacent edges.  This keeps
# the longest job overlapped with the full home matrix without a separate scheduler.
cross=-1
for ((i=1; i<${#FOREIGN_DIRS[@]}; i++)); do
  prev=${FOREIGN_DIRS[i-1]##*/}; cur=${FOREIGN_DIRS[i]##*/}
  pmaj=${prev%%.*}; cmaj=${cur%%.*}
  if { [ "$pmaj" -le 3 ] && [ "$cmaj" -ge 10 ]; } || \
     { [ "$pmaj" -ge 10 ] && [ "$cmaj" -le 3 ]; }; then
    cross=$i
    break
  fi
done
if [ "$cross" -lt 1 ]; then
  echo "check_corpus.sh: foreign corpus has no cross-major adjacent edge" >&2
  exit 3
fi

: > "$tmp/jobs.txt"
append_foreign_pair "${FOREIGN_DIRS[cross-1]}" "${FOREIGN_DIRS[cross]}"
for ((i=1; i<${#FOREIGN_DIRS[@]}; i++)); do
  [ "$i" -eq "$cross" ] && continue
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

read corpus_count corpus_total oneface_count grow revert <<EOF
$(awk '$1=="P"{count++; total+=$4}
       $1=="G"{oneface++}
       $1=="G" && $2=="oneface_grow"{grow=$4}
       $1=="G" && $2=="oneface_revert"{revert=$4}
       END{print count+0, total+0, oneface+0, grow+0, revert+0}' "$tmp/pairs.txt")
EOF

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
