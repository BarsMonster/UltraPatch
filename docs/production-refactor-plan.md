# UltraPatch Production Refactor Plan

Status file for the production cleanup sequence requested on main. Each item is
committed separately after its verification passes.

## Rules

- Keep the device decoder header-only and free of heap/global static storage.
- Keep every intended change wire-neutral; a golden mismatch is a refactor bug.
- Commit only from a clean, reviewed diff and never include unrelated changes.
- Record the test command and commit hash for each completed item.

## Items

1. Tracked plan.
   - Status: complete.
   - Verification: `git diff --check`.
   - Commit: `e8aa361`.

2. Deduplicate the host decoder backend used by CLI decode and encoder selfcheck.
   - Status: complete.
   - Verification: `make gate`; `nm -S ultrapatch` showed one copy of decoder-local symbols.
   - Commit: this commit.

3. Remove the `model_diff` harness and raw-gamma test-only code.
   - Status: pending.
   - Verification: `make gate`; `make clean`.
   - Commit: pending.

4. Remove dead helper APIs (`join2`, `a1_m4_pack` facade).
   - Status: pending.
   - Verification: `make gate`.
   - Commit: pending.

5. Unify shift-map fitter prune/cap/merge mechanics.
   - Status: pending.
   - Verification: `make gate`.
   - Commit: pending.

6. Fuse preserve/correction planning and remove the redundant `chas` bitmap.
   - Status: pending.
   - Verification: `make gate`.
   - Commit: pending.

7. Single-source encoder delta stream transition logic.
   - Status: pending.
   - Verification: `make gate`.
   - Commit: pending.

8. Consolidate repeated host vector growth boilerplate.
   - Status: pending.
   - Verification: `make gate`; `make check-analyze`.
   - Commit: pending.

## Final Verification

- Status: pending.
- Verification: `make gate`; `make check-analyze`.
