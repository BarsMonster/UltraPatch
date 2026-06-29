# Test Bench

External firmware worktrees used for benchmarking live here.

`Sensor-Watch/` is ignored by the ultrapatcher repository so local firmware edits and upstream history stay separate from the patcher code.

`images/` and `fixtures/` are tracked binary corpora used by the A1 verification
checks. The gate uses 16 matrix images plus the `v0_base` and `v1_one_face`
fixtures; the root `Makefile` reads them directly by default.

`corpus.sha256` is tracked and pins every `watch.bin` and `watch.elf` required by
the release gate. Run this after restoring or regenerating the binary corpus:

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
