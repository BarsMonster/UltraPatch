# Foreign corpus provenance

The foreign corpus is a second, unrelated Cortex-M0+ firmware lineage used to
check that encoder planning generalizes beyond the Sensor Watch home corpus.
It contains official CircuitPython `feather_m0_express` release images.

CircuitPython source is published at
<https://github.com/adafruit/circuitpython> under the MIT license. The release
artifacts were obtained from
<https://adafruit-circuit-python.s3.amazonaws.com/bin/feather_m0_express/en_US/>.

The committed raw-bin releases are:

`2.2.0`, `2.2.1`, `2.2.2`, `2.2.3`, `2.2.4`, `2.3.0`, `2.3.1`, `3.0.0`,
`3.0.1`, `3.0.2`, and `3.0.3`.

The committed UF2-derived releases are:

`10.0.0`, `10.0.1`, `10.0.2`, `10.0.3`, `10.1.1`, `10.1.2`, and `10.1.3`.
They were unpacked at application base `0x2000` to match the raw-bin layout.

[`test-bench/corpus-inventory.tsv`](../test-bench/corpus-inventory.tsv) is the
authority for the committed bytes, SHA-256 hashes, release order, and 17
ordered undirected edges. Both directions of every edge run in the common
corpus worker pool, giving 34 foreign cases. See
[`test-bench/README.md`](../test-bench/README.md) for the verification contract.

Live patch-size ratchets are defined only in the
[`Makefile`](../Makefile); pinned wire hashes and sizes are defined only in
[`test-bench/wire-baseline.tsv`](../test-bench/wire-baseline.tsv). Do not copy
those changing measurements into this provenance note.

Whole-relink cases can require much more literal data than adjacent releases.
The host encoder handles them by degrading compression while keeping every
plan inside the decoder's fixed journal and correction caps. This fallback is
encoder-side; it does not enlarge or complicate the device decoder.
