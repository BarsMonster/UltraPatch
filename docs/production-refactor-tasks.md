# Production Refactor Tasks

This is the Codex-tracked task list for the July 2026 high-impact UltraPatch
cleanup review. Each task keeps its review ID stable so follow-up discussion can
refer to the same number.

Acceptance rule for every code task:

- Keep the finished result on `main`.
- Commit each accepted fix separately.
- Accept only if it makes the code smaller/better by removing duplication, dead
  state, unnecessary storage, or real drift risk.
- Reject and revert if it degrades compression, regresses the real one-face
  grow/revert patch, fails round-trip/golden verification, or increases code
  size without a justified production benefit.
- For compression-affecting changes, report the per-pair better/worse/equal
  split, corpus totals, and the real one-face grow/revert sizes.
- Use `scripts/ab_matrix.sh <baseline-encoder> <candidate-encoder>
  <candidate-decoder> [jobs]` for explicit A/B compression experiments before
  trusting a size win.
- Baseline at review time: `make gate` and `make check-analyze` pass; ARM
  `text/data/bss = 6073/0/10852`, stack bound `408 B`, corpus `4151558`,
  foreign `1333407`, matrix `256/256`, real one-face grow/revert `574/287`.

## Tasks

- [ ] 1. Unify and harden all uLEB readers.
  Replace the duplicated envelope, dval, and content uLEB readers with one
  checked uLEB32 core that rejects fifth-byte overflow and exposes explicit
  policies for canonical envelope fields, the size-delta direction marker,
  content gaps, and dval escapes.

- [ ] 2. Stream op corrections instead of storing `op_corr[OPC_CAP]`.
  Change the wire so corrections are emitted and decoded in apply order,
  including grow/reverse order and `M_dval` history, keeping only the next
  correction offset/value in decoder state.

- [ ] 3. Rework shift-map storage against the journal/apply arena.
  Overlay shift-map entries with journal-reserved/apply arena memory where
  possible and make the host encoder account for the effective journal budget
  before emitting a blob.

- [ ] 4. Collapse the duplicated LZ parser paths.
  Remove the bootstrap/simple parser split by configuring the full priced parser
  with bootstrap prices, and finalize/reuse immutable candidate ordering instead
  of re-sorting it on every parse pass.

- [ ] 5. Fix and simplify LZ candidate handling.
  Correct out-match candidate retention, remove duplicated new/old source
  scanning, and make candidate eligibility use shared wire constants.

- [ ] 6. Make the wire grammar shared, not mirrored by convention.
  Single-source the body prologue, model-reset order, delta MTF transitions,
  token flag/history advance, content token state, and literal cursor read-ahead
  rules; add a small wire/profile discriminator before the next intentional wire
  change.

- [ ] 7. Create one encoder op-event iterator for field-aware output.
  Replace repeated field-aware walks in field-delta merging, preserve/correction
  simulation, and content emission with one decoder-order iterator that yields
  copy bytes, packed BL/EX bytes, suppressed BL bytes, and extras.

- [ ] 8. Own `OpWalkEnt` once per pipeline phase.
  Pass the already-built op walk through shift-map building, preserve budgeting,
  and feasibility checks or fold feasibility into PC construction, removing
  repeated allocations and full op passes.

- [ ] 9. Replace ELF raw byte-search with validated file-backed ranges.
  Make `Ranges` carry validated VM and file offsets, use symbol `st_shndx`, and
  reuse shared little-endian and ARM recognizer helpers instead of reconstructing
  data offsets via raw byte search.

- [ ] 10. Consolidate host IO and host decode validation.
  Share host file helpers, enforce decoded `from_size == image_size` in CLI
  decode, check writeback/flush/truncate/close failures, and add checked array
  allocation helpers for repeated `count * sizeof(T)` allocation patterns.
  Host decode/writeback validation subset completed: CLI decode now rejects
  `from_size` mismatches before opening the image for write, checks file commit
  failures, and treats output `fclose` failure as fatal. Checked array
  allocation remains.

- [ ] 11. Refactor gate metrics into one owner.
  Make edge fixture setup failures fatal, pin expected edge accepted/refused
  outcomes, single-source one-face metrics, and align gate comments/reporting
  with where per-pair better/worse/equal checks actually run.

- [x] 12. Decide whether DATA/CODE block-mask passes are product-dead.
  The DATA/CODE masking passes were measured neutral on current home/foreign
  corpora and save a small amount of host text when removed, but those streams
  are real for pointer-heavy firmware. Remove them only if product scope says
  this path is genuinely dead beyond the current corpus.
  Completed by removing only the DATA/CODE mask-only passes; A/B versus
  `/tmp/ultrapatch.baseline-db1faa3` was bit-identical on the 256-pair corpus
  (`better/worse/equal = 0/0/256`, `4151558 -> 4151558`) and unchanged for the
  real one-face grow/revert patches (`574/287 -> 574/287`).
