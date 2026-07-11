#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# Direct callers need the same coherent four-file generation as `make gate`.  Keep this shared
# descriptor open for the whole body; nested gate/package readers may take another shared lock.
exec 8<>"$ROOT/test-bench/.wire-baseline-update.lock"
flock --shared 8
python3 "$ROOT/scripts/publish_wire_baselines.py" assert-clean \
  --root "$ROOT/test-bench" >/dev/null

out="${1:-artifacts/a1-corpus.tar.gz}"
sum_out="$out.sha256"

# The lock inode must stay persistent for flock to serialize every participant.  Replacing it
# through an output path (including a hardlink outside test-bench) creates a split-brain lock;
# transaction/temporary names likewise belong exclusively to the baseline publisher.
reject_reserved_output() {
  candidate=$1
  candidate_abs=$(realpath -m -- "$candidate")
  makefile_abs=$(realpath -m -- "$ROOT/Makefile")
  if [ "$candidate_abs" = "$makefile_abs" ] || \
     { [ -e "$candidate" ] && [ "$candidate" -ef "$ROOT/Makefile" ]; }; then
    echo "pack_corpus.sh: output aliases canonical Makefile: $candidate" >&2
    exit 1
  fi
  case "$candidate_abs" in
    */.wire-baseline-update.*|*/.wire-baseline-update.*/*)
      echo "pack_corpus.sh: output uses reserved wire-baseline state: $candidate" >&2
      exit 1
      ;;
  esac
  if [ -e "$candidate" ] && \
     find "$ROOT/test-bench" -xdev \
       \( -path "$ROOT/test-bench/.wire-baseline-update.*" -o \
          -path "$ROOT/test-bench/.wire-baseline-update.*/*" \) \
       -samefile "$candidate" -print -quit | grep -q .; then
    echo "pack_corpus.sh: output aliases reserved wire-baseline state: $candidate" >&2
    exit 1
  fi
}
reject_reserved_output "$out"
reject_reserved_output "$sum_out"

. "$(dirname "$0")/tempdir.sh"

"${MAKE:-make}" --no-print-directory check-release-inventory-internal >/dev/null
scripts/verify_corpus.sh test-bench/corpus.sha256 >/dev/null
scripts/verify_corpus.sh test-bench/foreign.sha256 foreign_assets >/dev/null
mkdir -p "$(dirname "$out")"

awk 'NF && $1 !~ /^#/ { print $2 }' \
  test-bench/corpus.sha256 test-bench/foreign.sha256 > "$tmp/files"
printf '%s\n' \
  test-bench/release-inventory.tsv \
  test-bench/corpus.sha256 \
  test-bench/foreign.sha256 \
  test-bench/home-size-baseline.tsv \
  test-bench/corpus-wire.sha256 \
  test-bench/golden.sha256 >> "$tmp/files"
LC_ALL=C sort -u -o "$tmp/files" "$tmp/files"
while IFS= read -r path; do
  [ -f "$path" ] || { echo "pack_corpus.sh: missing package input: $path" >&2; exit 1; }
  path_abs=$(realpath -m -- "$path")
  if [ "$path_abs" = "$(realpath -m -- "$out")" ] || \
     [ "$path_abs" = "$(realpath -m -- "$sum_out")" ] || \
     { [ -e "$out" ] && [ "$path" -ef "$out" ]; } || \
     { [ -e "$sum_out" ] && [ "$path" -ef "$sum_out" ]; }; then
    echo "pack_corpus.sh: output aliases package input: $path" >&2
    exit 1
  fi
done < "$tmp/files"

out_dir=$(dirname -- "$out")
out_base=$(basename -- "$out")
sum_base=$(basename -- "$sum_out")
stage_archive=
stage_sum=
cleanup() {
  rm -f -- "${stage_archive:-}" "${stage_sum:-}"
  rm -rf -- "$tmp"
}
trap 'cleanup' EXIT
trap 'cleanup; trap - TERM INT EXIT; kill -s TERM "$$"' TERM
trap 'cleanup; trap - TERM INT EXIT; kill -s INT "$$"' INT
stage_archive=$(mktemp "$out_dir/.${out_base}.tmp.XXXXXX")
stage_sum=$(mktemp "$out_dir/.${sum_base}.tmp.XXXXXX")
default_mode=$((0666 & ~8#$(umask)))
if [ -e "$out" ]; then archive_mode=$(stat -c '%a' -- "$out"); \
else printf -v archive_mode '%o' "$default_mode"; fi
if [ -e "$sum_out" ]; then sum_mode=$(stat -c '%a' -- "$sum_out"); \
else printf -v sum_mode '%o' "$default_mode"; fi

tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
  --no-recursion --verbatim-files-from -cf - -T "$tmp/files" | gzip -n > "$stage_archive"
gzip -t "$stage_archive"
tar -tzf "$stage_archive" | LC_ALL=C sort > "$tmp/archive-files"
cmp "$tmp/files" "$tmp/archive-files"

mkdir "$tmp/unpack"
tar -xzf "$stage_archive" -C "$tmp/unpack"
(cd "$tmp/unpack" && sha256sum -c test-bench/corpus.sha256 \
  test-bench/foreign.sha256 >/dev/null)

digest=$(sha256sum "$stage_archive")
digest=${digest%% *}
printf '%s  %s\n' "$digest" "$out" > "$stage_sum"

# The two names cannot be replaced as one filesystem object. Publish the verified archive first
# and its checksum second: any interruption between them leaves a checksum mismatch, not a
# silently valid-looking pair. Both individual renames stay within the destination directory.
chmod "$archive_mode" "$stage_archive"
chmod "$sum_mode" "$stage_sum"
mv -f -- "$stage_archive" "$out"
stage_archive=
mv -f -- "$stage_sum" "$sum_out"
stage_sum=
published=$(sha256sum "$out")
published=${published%% *}
[ "$published" = "$digest" ]

printf 'wrote %s and %s\n' "$out" "$sum_out"
