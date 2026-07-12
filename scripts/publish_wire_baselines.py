#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Validate and publish an intentional combined wire-baseline update."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

from corpus_topology import TopologyError, parse_inventory
from wire_baseline import BaselineError, WireBaseline, parse_wire_baseline


PIN_NAMES = ("BASE_FULL_TOTAL", "BASE_FOREIGN_TOTAL",
             "BASE_ONEFACE_GROW", "BASE_ONEFACE_REVERT")
SHA256_RE = re.compile(r"[0-9a-f]{64}")


class UpdateError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise UpdateError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def regular(path: Path, label: str) -> None:
    try:
        mode = path.stat(follow_symlinks=False).st_mode
    except OSError as exc:
        fail(f"{label} is unavailable: {exc}")
    if not stat.S_ISREG(mode) or path.is_symlink():
        fail(f"{label} must be a regular non-symlink file: {path}")


def verify_release_profile(path: Path) -> str:
    helper = Path(__file__).resolve().with_name("build_profile.py")
    result = subprocess.run(
        [sys.executable, str(helper), "verify-release", str(path)], check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        encoding="utf-8", errors="replace"
    )
    if result.returncode:
        fail("release profile recheck failed: " +
             (result.stderr.strip() or result.stdout.strip()))
    marker = result.stdout.strip()
    if not marker.startswith("release_profile="):
        fail("release profile recheck returned malformed evidence")
    value = marker.removeprefix("release_profile=")
    if not SHA256_RE.fullmatch(value):
        fail("release profile recheck returned a malformed hash")
    return value


def parse_metrics(path: Path) -> dict[str, str]:
    wanted = {
        "matrix_ok", "full_total", "home_size_better", "home_size_worse",
        "home_size_equal", "foreign_ok", "foreign_total", "wire_identity",
        "max_journal", "max_amplified", "max_maxpageerase", "max_inversions",
        "max_unaligned", "max_oob_page_writes", "max_canary_corrupt",
        "measurement_release_profile", "measurement_host_tool_sha256",
        "measurement_preimage_baseline_sha256", "measurement_preimage_makefile_sha256",
    }
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if "=" not in raw:
            continue
        key, value = raw.split("=", 1)
        if key in wanted:
            if key in values:
                fail(f"candidate metrics contain duplicate {key}")
            values[key] = value
    missing = sorted(wanted - values.keys())
    if missing:
        fail("candidate metrics are missing " + ", ".join(missing))
    return values


def rewrite_makefile(path: Path, replacements: dict[str, int] | None = None
                     ) -> tuple[dict[str, int], bytes]:
    text = path.read_text(encoding="utf-8")
    found: dict[str, int] = {}
    output: list[str] = []
    patterns = {
        name: re.compile(r"^([ \t]*" + re.escape(name) +
                         r"[ \t]*\?=[ \t]*)([0-9]+)([ \t]*(?:#.*)?(?:\r?\n)?)$")
        for name in PIN_NAMES
    }
    for line in text.splitlines(keepends=True):
        for name, pattern in patterns.items():
            match = pattern.fullmatch(line)
            if match:
                if name in found:
                    fail(f"Makefile defines {name} more than once")
                found[name] = int(match.group(2))
                value = found[name] if replacements is None else replacements[name]
                output.append(match.group(1) + str(value) + match.group(3))
                break
        else:
            output.append(line)
    if set(found) != set(PIN_NAMES):
        fail("Makefile lacks the four canonical BASE_* pins")
    return found, "".join(output).encode("utf-8")


def validate_shape(label: str, baseline: WireBaseline, topology,
                   golden_names: set[str]) -> None:
    home = set(topology.home_pairs)
    foreign = set(topology.foreign_pairs)
    if set(baseline.home) != home:
        fail(f"{label} home-pair set differs from the release inventory")
    if set(baseline.foreign) != foreign:
        fail(f"{label} foreign-pair set differs from the release inventory")
    if set(baseline.golden) != golden_names:
        fail(f"{label} golden set differs from the canonical baseline")


def unsigned(value: str, label: str) -> int:
    if not value.isdigit():
        fail(f"{label} is not an unsigned integer")
    return int(value)


def validate(args, root: Path):
    baseline_path = root / "wire-baseline.tsv"
    makefile_path = root.parent / "Makefile"
    candidate_path = Path(args.candidate_baseline)
    for path, label in ((baseline_path, "canonical baseline"),
                        (makefile_path, "canonical Makefile"),
                        (candidate_path, "candidate baseline"),
                        (Path(args.metrics), "candidate metrics"),
                        (Path(args.host_tool), "measured host tool")):
        regular(path, label)
    if os.path.samefile(baseline_path, candidate_path):
        fail("candidate baseline aliases the canonical baseline")
    try:
        topology = parse_inventory(args.inventory)
        old = parse_wire_baseline(baseline_path)
        new = parse_wire_baseline(candidate_path)
    except (OSError, UnicodeError, TopologyError, BaselineError) as exc:
        fail(str(exc))
    validate_shape("canonical", old, topology, set(old.golden))
    validate_shape("candidate", new, topology, set(old.golden))
    if old.order != new.order:
        fail("candidate row order differs from the canonical baseline")

    baseline_bytes = baseline_path.read_bytes()
    makefile_bytes = makefile_path.read_bytes()
    candidate_bytes = candidate_path.read_bytes()
    metrics = parse_metrics(Path(args.metrics))
    if metrics["measurement_preimage_baseline_sha256"] != hashlib.sha256(baseline_bytes).hexdigest():
        fail("canonical baseline changed since measurement began")
    if metrics["measurement_preimage_makefile_sha256"] != hashlib.sha256(makefile_bytes).hexdigest():
        fail("canonical Makefile changed since measurement began")
    if metrics["matrix_ok"] != f"{len(old.home)}/{len(old.home)}" or \
            metrics["foreign_ok"] != f"{len(old.foreign)}/{len(old.foreign)}":
        fail("candidate metrics do not prove every release round-trip")
    if (metrics["home_size_better"], metrics["home_size_worse"],
            metrics["home_size_equal"]) != ("NA", "NA", "NA"):
        fail("candidate corpus run was not an unpinned measurement")
    if metrics["wire_identity"] != f"NA/{len(old.home) + len(old.foreign)}":
        fail("candidate corpus run did not bypass the old baseline")
    for key in ("max_amplified", "max_inversions", "max_unaligned",
                "max_oob_page_writes", "max_canary_corrupt"):
        if metrics[key] != "0":
            fail(f"candidate safety metric {key} is nonzero")
    if unsigned(metrics["max_maxpageerase"], "max_maxpageerase") > 1:
        fail("candidate max erases per page exceeds one")

    profile = verify_release_profile(Path(args.release_profile_lock))
    if metrics["measurement_release_profile"] != profile:
        fail("candidate was measured under a different release profile")
    host_tool = Path(args.host_tool)
    if metrics["measurement_host_tool_sha256"] != digest(host_tool):
        fail("measured host tool changed after measurement")

    pins, _ = rewrite_makefile(makefile_path)
    limits = {
        "BASE_FULL_TOTAL": unsigned(args.home_limit, "home total limit"),
        "BASE_FOREIGN_TOTAL": unsigned(args.foreign_limit, "foreign total limit"),
        "BASE_ONEFACE_GROW": unsigned(args.oneface_grow_limit, "one-face grow limit"),
        "BASE_ONEFACE_REVERT": unsigned(args.oneface_revert_limit, "one-face revert limit"),
    }
    if pins != limits:
        fail("canonical Makefile pins do not match protected update inputs")
    old_home = sum(value.size or 0 for value in old.home.values())
    new_home = sum(value.size or 0 for value in new.home.values())
    if old_home != limits["BASE_FULL_TOTAL"]:
        fail("canonical home total does not match its Makefile pin")
    grow = new.golden["oneface_grow.blob"].size
    revert = new.golden["oneface_revert.blob"].size
    old_grow = old.golden["oneface_grow.blob"].size
    old_revert = old.golden["oneface_revert.blob"].size
    assert grow is not None and revert is not None and old_grow is not None and old_revert is not None
    if old_grow != limits["BASE_ONEFACE_GROW"] or old_revert != limits["BASE_ONEFACE_REVERT"]:
        fail("canonical one-face sizes do not match their Makefile pins")
    regressed = [pair for pair in old.home if
                 (new.home[pair].size or 0) > (old.home[pair].size or 0)]
    if regressed:
        fail(f"candidate regresses {len(regressed)} home pair(s)")
    measured_home = unsigned(metrics["full_total"], "measured home total")
    measured_foreign = unsigned(metrics["foreign_total"], "measured foreign total")
    if measured_home != new_home or new_home > old_home:
        fail("candidate home total disagrees with rows or regresses")
    if measured_foreign > limits["BASE_FOREIGN_TOTAL"]:
        fail("candidate foreign total regresses")
    if grow > old_grow or revert > old_revert:
        fail("candidate one-face size regresses")
    new_pins = {
        "BASE_FULL_TOTAL": new_home,
        "BASE_FOREIGN_TOTAL": measured_foreign,
        "BASE_ONEFACE_GROW": grow,
        "BASE_ONEFACE_REVERT": revert,
    }
    _, new_makefile = rewrite_makefile(makefile_path, new_pins)
    if candidate_path.read_bytes() != candidate_bytes:
        fail("candidate baseline changed during validation")
    if baseline_path.read_bytes() != baseline_bytes or makefile_path.read_bytes() != makefile_bytes:
        fail("canonical inputs changed during validation")
    return baseline_path, makefile_path, candidate_bytes, new_makefile, old_home, new_home, \
        limits["BASE_FOREIGN_TOTAL"], measured_foreign, old_grow, grow, old_revert, revert


def replace(path: Path, content: bytes) -> None:
    mode = stat.S_IMODE(path.stat().st_mode)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as target:
            target.write(content)
            target.flush()
            os.fchmod(target.fileno(), mode)
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def publish(args) -> int:
    root = Path(args.root).resolve()
    lock_path = root / ".wire-baseline-update.lock"
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        values = validate(args, root)
        baseline, makefile, baseline_bytes, makefile_bytes = values[:4]
        changed = baseline.read_bytes() != baseline_bytes or makefile.read_bytes() != makefile_bytes
        if changed:
            replace(baseline, baseline_bytes)
            replace(makefile, makefile_bytes)
    old_home, new_home, old_foreign, new_foreign, old_grow, new_grow, old_revert, new_revert = values[4:]
    print(f"wire_baseline_home_total={old_home}->{new_home}")
    print(f"wire_baseline_foreign_total={old_foreign}->{new_foreign}")
    print(f"wire_baseline_oneface={old_grow}/{old_revert}->{new_grow}/{new_revert}")
    print("wire_baseline_update=" + ("COMMITTED" if changed else "NOOP"))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--inventory", required=True)
    parser.add_argument("--candidate-baseline", required=True)
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--host-tool", required=True)
    parser.add_argument("--release-profile-lock", required=True)
    parser.add_argument("--home-limit", required=True)
    parser.add_argument("--foreign-limit", required=True)
    parser.add_argument("--oneface-grow-limit", required=True)
    parser.add_argument("--oneface-revert-limit", required=True)
    args = parser.parse_args()
    try:
        return publish(args)
    except (OSError, UnicodeError, UpdateError) as exc:
        print(f"publish_wire_baselines.py: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
