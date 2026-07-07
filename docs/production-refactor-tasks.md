# Production Refactor Tasks

This is the Codex-tracked task list for the high-impact UltraPatch cleanup
review. Each task keeps its review ID stable so follow-up discussion can refer
to the same number.

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

## Tasks

- [x] 1. Harden decoder error flow and unify checked readers.
  Replace EOF zero-fill and post-error write-through behavior with checked
  byte, ULEB, Rice, and content readers that stop side effects immediately.

- [ ] 2. Stream op corrections instead of storing `op_corr[OPC_CAP]`.
  Change the wire so corrections are emitted and decoded in apply order, keeping
  only the next correction offset/value in decoder state.

- [ ] 3. Rework shift-map storage against the journal/apply arena.
  Store shift-map entries in journal-reserved memory where possible and make the
  host encoder account for the effective journal budget before emitting a blob.

- [ ] 4. Collapse the duplicated LZ parser paths.
  Remove the bootstrap/simple parser split by configuring the full priced parser
  with bootstrap prices, and route LZ field widths through shared constants.

- [ ] 5. Fix and simplify LZ candidate handling.
  Correct out-match candidate retention, remove duplicated new/old source
  scanning, and make candidate eligibility use shared wire constants.

- [ ] 6. Make the wire grammar shared, not mirrored by convention.
  Single-source the body prologue, delta MTF transitions, content token state,
  and add a small wire/profile discriminator before the next intentional wire
  change.

- [ ] 7. Remove per-op literal/temp emission plumbing.
  Stream each op's literals/extras directly into the full content stream instead
  of building per-op literal vectors and temporary buffers.

- [ ] 8. Deduplicate field discovery/classification walks.
  Use one encoder field-walk helper for BL/LDR discovery, preservation, and
  degradation decisions while preserving current suppression behavior.

- [ ] 9. Fold ARM/ELF scanner code into the encoder utilities.
  Replace scanner-local containers, allocation handling, sorting, and fake error
  returns with existing encoder vector, sort, and error helpers.

- [ ] 10. Replace ELF data byte-search with validated range objects.
  Carry VM and file offsets in one file-backed loadable range object and remove
  raw byte-search/reconstruction logic.

- [x] 11. Consolidate host file/NVM helpers and envelope preflight.
  Share file loading and NVM statistics helpers, and reject oversized or invalid
  image/envelope inputs before narrowing sizes or starting analysis.

- [x] 12. Make verification metrics single-source and stricter.
  Centralize real one-face metric collection, reject A/B baseline encode
  failures, assert expected case counts, and keep published stack/size contracts
  generated from the gate.
