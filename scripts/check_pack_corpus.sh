#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

# Determinism, exact-content, and failure-publication contract for pack_corpus.sh.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/tempdir.sh"
cd "$ROOT"
umask 0022

out="$tmp/publish/a1-corpus.tar.gz"
sum="$out.sha256"
mkdir -p "$(dirname "$out")"

scripts/pack_corpus.sh "$out" >/dev/null
sha256sum -c "$sum" >/dev/null
gzip -t "$out"
[ "$(stat -c '%a' "$out")" = 644 ]
[ "$(stat -c '%a' "$sum")" = 644 ]
first=$(sha256sum "$out")
first=${first%% *}
chmod 444 "$out" "$sum"
scripts/pack_corpus.sh "$out" >/dev/null
second=$(sha256sum "$out")
second=${second%% *}
[ "$(stat -c '%a' "$out")" = 444 ]
[ "$(stat -c '%a' "$sum")" = 444 ]
[ "$first" = "$second" ] || {
  echo "corpus package is not deterministic" >&2
  exit 1
}
awk 'NF && $1 !~ /^#/ { print $2 }' \
  test-bench/corpus.sha256 test-bench/foreign.sha256 > "$tmp/expected-list"
printf '%s\n' test-bench/release-inventory.tsv test-bench/corpus.sha256 \
  test-bench/foreign.sha256 test-bench/home-size-baseline.tsv \
  test-bench/corpus-wire.sha256 test-bench/golden.sha256 >> "$tmp/expected-list"
LC_ALL=C sort -u -o "$tmp/expected-list" "$tmp/expected-list"
tar -tzf "$out" | LC_ALL=C sort > "$tmp/archive-list"
cmp "$tmp/expected-list" "$tmp/archive-list"

seed_pair() {
  chmod u+w "$out" "$sum" 2>/dev/null || :
  printf 'previous corpus archive\n' > "$out"
  sha256sum "$out" > "$sum"
  cp "$out" "$tmp/archive.before"
  cp "$sum" "$tmp/checksum.before"
}

no_stages() {
  if find "$(dirname "$out")" -maxdepth 1 \
      \( -name ".$(basename "$out").tmp.*" -o \
         -name ".$(basename "$sum").tmp.*" \) -print -quit | grep -q .; then
    echo "corpus packer leaked a publication stage" >&2
    exit 1
  fi
}

expect_preserved_failure() {
  local name=$1 wrapper=$2
  seed_pair
  if PATH="$wrapper:$PATH" scripts/pack_corpus.sh "$out" \
      >"$tmp/$name.out" 2>"$tmp/$name.err"; then
    echo "corpus packer accepted injected $name failure" >&2
    exit 1
  fi
  cmp "$out" "$tmp/archive.before"
  cmp "$sum" "$tmp/checksum.before"
  no_stages
}

for tool in tar gzip mv; do
  wrapper="$tmp/fail-$tool"
  mkdir "$wrapper"
  printf '%s\n' '#!/bin/sh' 'exit 77' > "$wrapper/$tool"
  chmod +x "$wrapper/$tool"
  expect_preserved_failure "$tool" "$wrapper"
done

# If archive publication succeeds but checksum publication fails, the command fails and the old
# checksum detects the mixed pair. This is the strongest useful contract for two filesystem names.
seed_pair
wrapper="$tmp/fail-second-mv"
mkdir "$wrapper"
cat > "$wrapper/mv" <<'EOF'
#!/bin/sh
n=0
[ ! -f "$PUBLISH_STATE" ] || n=$(cat "$PUBLISH_STATE")
n=$((n + 1))
printf '%s\n' "$n" > "$PUBLISH_STATE"
[ "$n" -ne 2 ] || exit 78
exec "$REAL_MV" "$@"
EOF
chmod +x "$wrapper/mv"
if PUBLISH_STATE="$tmp/mv-count" REAL_MV="$(command -v mv)" PATH="$wrapper:$PATH" \
    scripts/pack_corpus.sh "$out" >"$tmp/second-mv.out" 2>"$tmp/second-mv.err"; then
  echo "corpus packer accepted checksum publication failure" >&2
  exit 1
fi
if sha256sum -c "$sum" >/dev/null 2>&1; then
  echo "mixed corpus archive/checksum pair was not detectable" >&2
  exit 1
fi
no_stages

echo "corpus_package=OK (deterministic exact archive; generation/publication failures detected)"
