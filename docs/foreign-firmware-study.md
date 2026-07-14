# Foreign corpus provenance

The foreign corpus is a second, unrelated Cortex-M0+ firmware lineage used to
check that encoder planning generalizes beyond the Sensor Watch home corpus.
It contains official CircuitPython `feather_m0_express` release images.

CircuitPython source is published at
<https://github.com/adafruit/circuitpython> under the MIT license. The release
artifacts came from the official S3 listings for
[older raw binaries](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/OLD/)
and [current UF2 releases](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/).

The committed raw-bin releases are:

`2.2.0`, `2.2.1`, `2.2.2`, `2.2.3`, `2.2.4`, `2.3.0`, `2.3.1`, `3.0.0`,
`3.0.1`, `3.0.2`, and `3.0.3`.

The committed UF2-derived releases are:

`10.0.0`, `10.0.1`, `10.0.2`, `10.0.3`, `10.1.1`, `10.1.2`, and `10.1.3`.
They were unpacked at application base `0x2000` to match the raw-bin layout.

The gate reads these committed static binaries in place, sorts their 18 version directories, and
tests both directions of every adjacent pair, including the `3.0.3` to `10.0.0` cross-major
transition. This produces 34 foreign cases in the common corpus worker pool. Like the home and
one-face inputs, the foreign set is frozen; replacing a binary is an explicit corpus and ratchet
change. The exact Git commit is the corpus provenance record.

These cases contribute to the single complete-corpus size ratchet enforced by
[`check_corpus.sh`](../check_corpus.sh).

Whole-relink cases can require much more literal data than adjacent releases.
The host encoder literalizes unsafe read-after-write copies and folds target-byte
corrections into the normal content payload. A relocation field that cannot be
reconstructed exactly is literalized as one complete field. This work remains
encoder-side; wire v7 has no correction channel or decoder correction cap.
