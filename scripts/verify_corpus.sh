#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# Author: Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -eu

manifest="${1:-test-bench/corpus.sha256}"

if [ ! -f "$manifest" ]; then
  echo "missing corpus manifest: $manifest" >&2
  echo "restore the A1 corpus bundle, or pass CORPUS_MANIFEST=/path/to/manifest" >&2
  exit 3
fi

missing=0
files=0
while read -r sum path rest; do
  [ -n "${sum:-}" ] || continue
  files=$((files + 1))
  if [ -n "${rest:-}" ]; then
    echo "bad manifest line for $path" >&2
    exit 3
  fi
  if [ ! -f "$path" ]; then
    echo "missing corpus asset: $path" >&2
    missing=1
  fi
done < "$manifest"

if [ "$missing" -ne 0 ]; then
  exit 3
fi

sha256sum -c "$manifest" >/dev/null
printf 'corpus_assets=verified %s files via %s\n' "$files" "$manifest"
