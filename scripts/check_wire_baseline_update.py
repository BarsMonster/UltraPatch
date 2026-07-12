#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Small policy regression for the intentional wire-baseline publisher."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile


PUBLISHER = Path(__file__).resolve().with_name("publish_wire_baselines.py")
PROFILE = re.compile(r"release_profile=([0-9a-f]{64})\n?")


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def h(character: str) -> str:
    return character * 64


def baseline(sizes=(10, 10, 10, 10), foreign_hash="a", grow=10,
             revert=9, golden_hashes=("b", "c", "d", "e")) -> str:
    pairs = (("h0", "h0"), ("h0", "h1"), ("h1", "h0"), ("h1", "h1"))
    rows = [f"C\t{a}\t{b}\t{size}\t{h('a')}\n"
            for (a, b), size in zip(pairs, sizes)]
    rows += [f"F\tf0\tf1\t-\t{h(foreign_hash)}\n",
             f"F\tf1\tf0\t-\t{h(foreign_hash)}\n"]
    names = ("oneface_grow.blob", "oneface_revert.blob",
             "synth_journal_degrade.blob", "synth_unnatural_dir.blob")
    golden_sizes = (grow, revert, 7, 8)
    rows += [f"G\t{name}\t-\t{size}\t{h(hash_char)}\n"
             for name, size, hash_char in zip(names, golden_sizes, golden_hashes)]
    return "".join(rows)


def make_fixture(parent: Path) -> tuple[Path, Path]:
    repo = parent / "repo"
    root = repo / "test-bench"
    write(repo / "Makefile", """BASE_FULL_TOTAL ?= 40
BASE_FOREIGN_TOTAL ?= 20
BASE_ONEFACE_GROW ?= 10
BASE_ONEFACE_REVERT ?= 9
unrelated := preserved
""")
    asset_hash = h("a")
    write(root / "corpus-inventory.tsv", f"""fixture base {asset_hash} {asset_hash}
home h0 {asset_hash} {asset_hash}
home h1 {asset_hash} {asset_hash}
foreign f0 {asset_hash} -
foreign f1 {asset_hash} -
foreign-edge f0 f1
""")
    write(root / "wire-baseline.tsv", baseline())
    return repo, root


def candidate(repo: Path, profile: str, tool_hash: str, *,
              sizes=(10, 10, 10, 10), foreign=20, grow=10, revert=9,
              changed=False, metric_overrides=None) -> tuple[Path, Path]:
    path = repo / "candidate.tsv"
    write(path, baseline(sizes, "f" if changed else "a", grow, revert,
                         ("6", "7", "8", "9") if changed else
                         ("b", "c", "d", "e")))
    metrics = {
        "matrix_ok": "4/4", "full_total": str(sum(sizes)),
        "home_size_better": "NA", "home_size_worse": "NA",
        "home_size_equal": "NA", "foreign_ok": "2/2",
        "foreign_total": str(foreign), "wire_identity": "NA/6",
        "max_journal": "3", "max_amplified": "0",
        "max_maxpageerase": "1", "max_inversions": "0",
        "max_unaligned": "0", "max_oob_page_writes": "0",
        "max_canary_corrupt": "0",
        "measurement_release_profile": profile,
        "measurement_host_tool_sha256": tool_hash,
        "measurement_preimage_baseline_sha256": digest(repo / "test-bench/wire-baseline.tsv"),
        "measurement_preimage_makefile_sha256": digest(repo / "Makefile"),
    }
    if metric_overrides:
        metrics.update(metric_overrides)
    metrics_path = repo / "metrics.txt"
    write(metrics_path, "".join(f"{key}={value}\n" for key, value in metrics.items()))
    return path, metrics_path


def command(root: Path, candidate_path: Path, metrics: Path, host: Path,
            lock: Path) -> list[str]:
    return [sys.executable, str(PUBLISHER), "--root", str(root),
            "--inventory", str(root / "corpus-inventory.tsv"),
            "--candidate-baseline", str(candidate_path), "--metrics", str(metrics),
            "--host-tool", str(host), "--release-profile-lock", str(lock),
            "--home-limit", "40", "--foreign-limit", "20",
            "--oneface-grow-limit", "10", "--oneface-revert-limit", "9"]


def run(argv: list[str], success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding="utf-8", errors="replace")
    if (result.returncode == 0) != success:
        raise RuntimeError("unexpected publisher result: " +
                           (result.stderr.strip() or result.stdout.strip()))
    return result


def unchanged(repo: Path, before: tuple[bytes, bytes]) -> None:
    after = ((repo / "test-bench/wire-baseline.tsv").read_bytes(),
             (repo / "Makefile").read_bytes())
    if after != before:
        raise RuntimeError("rejected publication modified canonical files")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-tool", required=True, type=Path)
    parser.add_argument("--release-profile-lock", required=True, type=Path)
    args = parser.parse_args()
    host = args.host_tool.resolve()
    lock = args.release_profile_lock.resolve()
    verified = subprocess.run(
        [sys.executable, str(PUBLISHER.with_name("build_profile.py")),
         "verify-release", str(lock)], check=True, stdout=subprocess.PIPE,
        text=True, encoding="utf-8")
    match = PROFILE.fullmatch(verified.stdout)
    if not match:
        raise RuntimeError("release profile helper returned malformed evidence")
    profile, tool_hash = match.group(1), digest(host)

    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)

        repo, root = make_fixture(base / "noop")
        cand, metrics = candidate(repo, profile, tool_hash)
        before = (root.joinpath("wire-baseline.tsv").read_bytes(),
                  repo.joinpath("Makefile").read_bytes())
        mtimes = (root.joinpath("wire-baseline.tsv").stat().st_mtime_ns,
                  repo.joinpath("Makefile").stat().st_mtime_ns)
        result = run(command(root, cand, metrics, host, lock), True)
        unchanged(repo, before)
        if "wire_baseline_update=NOOP" not in result.stdout or mtimes != (
                root.joinpath("wire-baseline.tsv").stat().st_mtime_ns,
                repo.joinpath("Makefile").stat().st_mtime_ns):
            raise RuntimeError("identical publication was not a byte-and-mtime no-op")

        repo, root = make_fixture(base / "success")
        cand, metrics = candidate(repo, profile, tool_hash, sizes=(9, 9, 9, 9),
                                  foreign=19, grow=9, revert=8, changed=True)
        result = run(command(root, cand, metrics, host, lock), True)
        if "wire_baseline_update=COMMITTED" not in result.stdout or \
                root.joinpath("wire-baseline.tsv").read_bytes() != cand.read_bytes():
            raise RuntimeError("valid improvement was not published")
        makefile = repo.joinpath("Makefile").read_text(encoding="utf-8")
        for pin in ("BASE_FULL_TOTAL ?= 36", "BASE_FOREIGN_TOTAL ?= 19",
                    "BASE_ONEFACE_GROW ?= 9", "BASE_ONEFACE_REVERT ?= 8"):
            if pin not in makefile:
                raise RuntimeError(f"publisher did not update {pin}")

        cases = (
            ("per-pair regression", {"sizes": (8, 11, 8, 8), "changed": True}, {}),
            ("one-face regression", {"grow": 11, "changed": True}, {}),
            ("stale measurement", {}, {"measurement_preimage_baseline_sha256": h("0")}),
        )
        for index, (_, options, overrides) in enumerate(cases):
            repo, root = make_fixture(base / f"reject-{index}")
            cand, metrics = candidate(repo, profile, tool_hash,
                                      metric_overrides=overrides, **options)
            before = (root.joinpath("wire-baseline.tsv").read_bytes(),
                      repo.joinpath("Makefile").read_bytes())
            run(command(root, cand, metrics, host, lock), False)
            unchanged(repo, before)

        # A crash between the two ordinary replaces needs no private recovery protocol:
        # the canonical baseline/Makefile consistency check rejects the mixed tree.
        repo, root = make_fixture(base / "mixed")
        write(root / "wire-baseline.tsv", baseline((9, 9, 9, 9), "f", 9, 8,
                                                    ("6", "7", "8", "9")))
        cand, metrics = candidate(repo, profile, tool_hash, sizes=(9, 9, 9, 9),
                                  foreign=19, grow=9, revert=8, changed=True)
        run(command(root, cand, metrics, host, lock), False)

    print("wire_baseline_update_contract=OK (noop + publish + policy + mixed-state rejection)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"check_wire_baseline_update.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
