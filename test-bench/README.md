# Test Bench

This directory contains the tracked corpus sources used by verification
checks. The gate uses 16 matrix ELFs (`images/`) plus the `v0_base` and
`v1_one_face` fixture ELFs, and a second, unrelated Cortex-M0+ binary lineage under
`foreign/` (18 CircuitPython feather_m0_express release images, 34 pair-
directions — see `../docs/foreign-firmware-study.md`). Before a test runs, the
profile-pinned Arm `objcopy` derives the exact home and fixture binaries under
`.build/<profile-id>/corpus`, beside copies of their ELF sidecars.

`corpus-inventory.tsv` is the canonical ordered topology and hash inventory. A
fixture or home row records its expected derived-binary SHA-256 and tracked ELF
SHA-256; a foreign row records its tracked binary SHA-256 and `-` for the absent
ELF. The file also lists the 17 ordered undirected foreign edges explicitly;
each edge schedules its listed direction followed by the reverse direction.
`make check-release-inventory` proves that `wire-baseline.tsv` describes that
topology and agrees with the Makefile cardinality, home-total, one-face, and
golden-count pins. `make check-corpus` gates the live foreign total because
foreign baseline rows carry hashes but no per-pair sizes.

The tracked inputs and derived binaries are verified by:

```sh
make check-assets
```

`make check-malformed` uses the pinned one-face fixture to verify deterministic
rejection of malformed patch envelopes and truncated blobs without requiring
extra corpus assets.
