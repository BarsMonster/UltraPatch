# Test Bench

This directory contains the tracked binary corpora used by the A1 verification
checks. The gate uses 16 matrix images (`images/`) plus the `v0_base` and
`v1_one_face` fixtures, and a second, unrelated Cortex-M0+ lineage under
`foreign/` (18 CircuitPython feather_m0_express release images, 34 pair-
directions — see `../docs/foreign-firmware-study.md`); the root `Makefile` reads
them directly by default.

`corpus.sha256` pins every home `watch.bin`/`watch.elf`; `foreign.sha256` pins
every `foreign/<ver>/watch.bin`. Both are verified by:

```sh
make check-assets
```

Create a deterministic standalone bundle, if needed, with:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```

`make check-malformed` uses the pinned one-face fixture to verify deterministic
rejection of malformed patch envelopes and truncated blobs without requiring
extra corpus assets.
