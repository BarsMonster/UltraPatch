#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

out="${1:-artifacts/a1-corpus.tar.gz}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

scripts/verify_corpus.sh test-bench/corpus.sha256 >/dev/null
mkdir -p "$(dirname "$out")"

find test-bench/fixtures test-bench/images -type f | sort > "$tmp/files"
printf '%s\n' test-bench/corpus.sha256 >> "$tmp/files"

tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner -cf - -T "$tmp/files" \
  | gzip -n > "$out"
sha256sum "$out" > "$out.sha256"

printf 'wrote %s and %s\n' "$out" "$out.sha256"
