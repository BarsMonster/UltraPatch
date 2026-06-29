# Test Bench

External firmware worktrees used for benchmarking live here.

`Sensor-Watch/` is ignored by the ultrapatcher repository so local firmware edits and upstream history stay separate from the patcher code.

`images/` and `fixtures/` are local binary corpora used by the A1 verification checks. They are ignored to keep the repository small; the root `Makefile` reads them directly by default.

`corpus.sha256` is tracked and pins every `watch.bin` and `watch.elf` required by
the release gate. Run this after restoring or regenerating the binary corpus:

```sh
make check-assets
```

Create a deterministic ignored bundle for CI/cache seeding with:

```sh
scripts/pack_corpus.sh artifacts/a1-corpus.tar.gz
```
