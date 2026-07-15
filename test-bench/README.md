# Test Bench

The release corpus is a frozen set of static raw binaries: 16 Sensor Watch home images, two real
one-face fixtures, and 18 CircuitPython `feather_m0_express` foreign images. Every input is committed
as `watch.bin` under `images`, `fixtures`, or `foreign`, and the gate reads those files in place. It
does not generate corpus inputs, copy them into `BUILD_DIR`, or use metadata sidecars.

`make gate` runs all 256 ordered home pairs, both directions of the 17 adjacent version-sorted
foreign pairs, and the real one-face grow and revert directions.

Each encoder call self-applies its emitted patch through `patch_apply.h`, checks the exact target and
NVM write safety, and refuses to write an invalid patch. The corpus runner therefore records only
the resulting patch size. One aggregate ceiling in `check_corpus.sh` covers all 290 home and foreign
patches; the real one-face patches retain their own product limits. Exact corpus provenance is the
Git commit. Replacing any binary is an explicit corpus and ratchet change.

The gate also exercises the shipped host `--decode` workflow on a nontrivial page-boundary patch. It
requires an exact in-place target result, then truncates the same patch and verifies that rejection
leaves the host image file unchanged.
