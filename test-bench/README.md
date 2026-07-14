# Test Bench

The release corpus contains 16 Sensor Watch home images, two real one-face fixtures, and 18
CircuitPython `feather_m0_express` foreign images. Home and fixture binaries are derived from their
tracked ELFs into the selected `BUILD_DIR`; the ELF sidecars remain beside them so the encoder uses
the same relocation information as production. Foreign images are tracked raw binaries.

`regressions/opc-extra` is a non-product raw pair that exercises correction-cap splitting inside
an extra-heavy operation. It is self-verified by the release gate but is not part of the corpus
compression total.

`make gate` runs all 256 ordered home pairs, both directions of the 17 adjacent version-sorted
foreign pairs, and the real one-face grow and revert directions.

Each encoder call self-applies its emitted patch through `patch_apply.h`, checks the exact target and
NVM write safety, and refuses to write an invalid patch. The corpus runner therefore records only
the resulting patch size. One aggregate ceiling in `check_corpus.sh` covers all 290 home and foreign
patches; the real one-face patches retain their own product limits. Exact corpus provenance is the
Git commit.
