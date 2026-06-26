# Ultrapatcher

This is the local repository for the ultrapatcher experiments and A1 decoder work.

Primary code lives under `v3/nvm/hybrid12k`. The legacy `detools-dev` checkout is intentionally outside this repository boundary; keep it as an external dependency only when a research script still needs it.

`test-bench/Sensor-Watch` is also an external worktree. It is used for firmware build and patch-size experiments, but it keeps its own Git history and is ignored here.

The benchmark image and fixture corpora live locally under `test-bench/images` and `test-bench/fixtures`; they are ignored binary test data. The source tree keeps lightweight symlinks at `v3/nvm/hybrid12k/images` and `v3/nvm/hybrid12k/fixtures` so existing tools can find them.
