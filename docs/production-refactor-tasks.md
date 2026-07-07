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

Issue 3 is intentionally not tracked below as a code split. The current product
decision is that `ultrapatch` remains a unified host CLI with encode and decode
modes; the device decoder artifact remains the header-only `patch_apply.h`
header set.

## Tasks

- [x] 1. Compact `A1UGGamma` storage.
  Store only reachable gamma mantissa probabilities and share the accessor with
  encoder/pricing so wire behavior stays bit-exact.

- [ ] 2. Overlay shift-map storage with the journal arena.
  Store shift-map entries in journal-reserved memory and make the host encoder
  account for the effective journal budget before emitting a valid blob.

- [x] 4. Single-source relocation and shift-map wire helpers.
  Move duplicated BL, LDR-literal, and shift-map prediction semantics into shared
  helpers, including the decoder/encoder LDR scan-bound drift fix.

- [ ] 5. Make decoder helper sharing reduce ARM text.
  Remove stale gamma init flexibility, route duplicated header zigzag decoding
  through shared helpers, and experiment with non-forced-inlined literal cursor
  helpers.

- [ ] 6. Shrink encoder `PriceTab`.
  Remove the unused `go` member and narrow literal price tables if guarded by
  range checks/static assertions.

- [ ] 7. Stream per-op content emission without heap vectors.
  Replace per-op literal vectors/temp buffers with a counted directional cursor
  and fixed uLEB scratch while preserving injection cursor timing.

- [ ] 8. Collapse injection and shift-map preparation copies.
  Avoid copying `Inj` data into a second `FieldRef` vector and build shared
  shift-map preparation data once for both scorers.

- [ ] 9. Simplify ARM relocation scanner containers.
  Transfer finalized scanner maps directly to `m4_stream_t` and replace the
  literal-pool hash set with smaller host-only storage.

- [ ] 10. Flatten SequenceMatcher and block ownership.
  Replace nested `B2J` vectors with a flat stable sorted index and make block
  value ownership explicit without duplicate heap copies.

- [ ] 11. Stream ELF range reduction.
  Compute the best code/data ranges without materializing temporary range
  vectors, preserving the current heuristic exactly.

- [ ] 12. Refactor verification around model/wire changes.
  Add the missing model-check coverage, wire the A/B matrix into the documented
  refactor workflow, harden edge fixture generation, and reduce duplicated gate
  shell plumbing where it materially improves maintainability.
