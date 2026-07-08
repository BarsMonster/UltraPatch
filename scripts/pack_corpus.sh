#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

out="${1:-artifacts/a1-corpus.tar.gz}"
. "$(dirname "$0")/tempdir.sh"

scripts/verify_corpus.sh test-bench/corpus.sha256 >/dev/null
scripts/verify_corpus.sh test-bench/foreign.sha256 foreign_assets >/dev/null
mkdir -p "$(dirname "$out")"

find test-bench/fixtures test-bench/images test-bench/foreign -type f | sort > "$tmp/files"
printf '%s\n' test-bench/corpus.sha256 test-bench/foreign.sha256 >> "$tmp/files"

tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner -cf - -T "$tmp/files" \
  | gzip -n > "$out"
sha256sum "$out" > "$out.sha256"

printf 'wrote %s and %s\n' "$out" "$out.sha256"
