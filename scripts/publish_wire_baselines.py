#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

"""Validate and transactionally publish canonical wire baselines and their Make pins.

Measurement is deliberately outside this program.  ``publish`` accepts only completed
candidate artifacts plus the metrics emitted by check_corpus.sh, takes the publication lock,
revalidates them against the then-current canonical files, and only then starts a durable
transaction.  An interrupted uncommitted transaction is always rolled back; a transaction
whose committed marker reached disk is finalized.
"""

import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

from corpus_topology import TopologyError, parse_inventory as parse_corpus_inventory


MANIFEST_FILES = ("golden.sha256", "home-size-baseline.tsv", "corpus-wire.sha256")
FILES = MANIFEST_FILES + ("Makefile",)
LOCK_NAME = ".wire-baseline-update.lock"
STATE_NAME = ".wire-baseline-update.transaction"
PREPARE_PREFIX = ".wire-baseline-update.prepare."
RECOVER_PREFIX = ".wire-baseline-update.recover."
DONE_PREFIX = ".wire-baseline-update.done."
SHA256_RE = re.compile(r"[0-9a-f]{64}")
ID_RE = re.compile(r"[A-Za-z0-9_.-]+")
PIN_NAMES = ("BASE_FULL_TOTAL", "BASE_FOREIGN_TOTAL",
             "BASE_ONEFACE_GROW", "BASE_ONEFACE_REVERT")


class UpdateError(Exception):
    """A policy, structure, or recovery error safe to show without a traceback."""


class InjectedFailure(UpdateError):
    """Test-only publication failure, handled like a real rename failure."""


class FaultInjector:
    """Named, sentinel-gated test faults; production Make never enables this path."""

    def __init__(self, args, root):
        specification = getattr(args, "test_fault", "")
        hold = getattr(args, "test_lock_hold_ms", 0)
        if specification or hold:
            sentinel = root / ".wire-baseline-update-test-root"
            if not sentinel.is_file():
                fail("test fault controls require an explicit synthetic-root sentinel")
        self.mode = ""
        self.point = ""
        self.triggered = False
        if specification:
            try:
                self.mode, self.point = specification.split("@", 1)
            except ValueError:
                fail("test fault must have MODE@POINT syntax")
            if self.mode not in ("fail", "crash") or not self.point:
                fail("test fault must use fail@POINT or crash@POINT")
        self.hold_ms = hold

    def hit(self, point):
        if self.triggered or self.point != point:
            return
        self.triggered = True
        if self.mode == "crash":
            os._exit(86)  # Intentionally bypass Python cleanup to model SIGKILL/power loss.
        raise InjectedFailure("injected failure at %s" % point)


def fail(message):
    raise UpdateError(message)


def regular_file(path, label):
    try:
        mode = path.stat(follow_symlinks=False).st_mode
    except OSError as exc:
        fail("%s is unavailable: %s" % (label, exc))
    if not stat.S_ISREG(mode) or path.is_symlink():
        fail("%s must be a regular non-symlink file: %s" % (label, path))


def canonical_path(root, name):
    return root.parent / "Makefile" if name == "Makefile" else root / name


def rows(path, fields):
    regular_file(path, "manifest")
    result = []
    try:
        with path.open(encoding="utf-8") as source:
            for lineno, raw in enumerate(source, 1):
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                values = line.split()
                if len(values) != fields:
                    fail("%s:%d: expected %d fields" % (path, lineno, fields))
                result.append((lineno, values))
    except UnicodeError as exc:
        fail("%s is not UTF-8 text: %s" % (path, exc))
    return result


def load_topology(path):
    regular_file(path, "release inventory")
    try:
        return parse_corpus_inventory(path)
    except (OSError, UnicodeError, TopologyError) as exc:
        fail(str(exc))


def parse_home_sizes(path):
    result = {}
    for lineno, values in rows(path, 3):
        source, target, value = values
        key = (source, target)
        if key in result or not value.isdigit():
            fail("%s:%d: invalid or duplicate home-size row" % (path, lineno))
        result[key] = int(value)
    return result


def parse_wire(path):
    result = {}
    for lineno, values in rows(path, 4):
        tag, source, target, digest = values
        key = (tag, source, target)
        if tag not in ("C", "F") or not SHA256_RE.fullmatch(digest) or key in result:
            fail("%s:%d: invalid or duplicate wire row" % (path, lineno))
        result[key] = digest
    return result


def parse_golden(path):
    result = {}
    for lineno, values in rows(path, 3):
        digest, value, name = values
        if (not SHA256_RE.fullmatch(digest) or not value.isdigit() or
                not ID_RE.fullmatch(name) or name in result):
            fail("%s:%d: invalid or duplicate golden row" % (path, lineno))
        result[name] = (digest, int(value))
    return result


def parse_metrics(path):
    regular_file(path, "candidate metrics")
    wanted = {
        "matrix_ok", "full_total", "home_size_better", "home_size_worse",
        "home_size_equal", "foreign_ok", "foreign_total", "wire_identity",
        "max_journal", "max_amplified", "max_maxpageerase", "max_inversions",
        "max_unaligned", "max_oob_page_writes", "max_canary_corrupt",
        "measurement_release_profile", "measurement_host_tool_sha256",
        "measurement_preimage_golden_sha256", "measurement_preimage_home_sha256",
        "measurement_preimage_wire_sha256", "measurement_preimage_makefile_sha256",
    }
    values = {}
    with path.open(encoding="utf-8") as source:
        for raw in source:
            line = raw.rstrip("\n")
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            if key in wanted:
                if key in values:
                    fail("candidate metrics contain duplicate %s" % key)
                values[key] = value
    missing = sorted(wanted - set(values))
    if missing:
        fail("candidate metrics are missing %s" % ", ".join(missing))
    for key in ("full_total", "foreign_total", "max_journal", "max_amplified",
                "max_maxpageerase", "max_inversions", "max_unaligned",
                "max_oob_page_writes", "max_canary_corrupt"):
        if not values[key].isdigit():
            fail("candidate metric %s is not an unsigned integer" % key)
    if not SHA256_RE.fullmatch(values["measurement_release_profile"]):
        fail("candidate release-profile evidence is malformed")
    for key in ("measurement_host_tool_sha256", "measurement_preimage_golden_sha256",
                "measurement_preimage_home_sha256", "measurement_preimage_wire_sha256",
                "measurement_preimage_makefile_sha256"):
        if not SHA256_RE.fullmatch(values[key]):
            fail("candidate hash evidence %s is malformed" % key)
    return values


def row_order(path, fields, key_indexes):
    return [tuple(values[index] for index in key_indexes)
            for _, values in rows(path, fields)]


def parse_and_rewrite_makefile(path, replacements=None):
    regular_file(path, "canonical Makefile")
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeError as exc:
        fail("canonical Makefile is not UTF-8 text: %s" % exc)
    found = {}
    output = []
    patterns = {
        name: re.compile(r"^([ \t]*" + re.escape(name) +
                         r"[ \t]*\?=[ \t]*)([0-9]+)([ \t]*(?:#.*)?(?:\r?\n)?)$")
        for name in PIN_NAMES
    }
    for line in text.splitlines(keepends=True):
        matched = False
        for name, pattern in patterns.items():
            match = pattern.fullmatch(line)
            if match:
                if name in found:
                    fail("canonical Makefile defines %s more than once" % name)
                found[name] = int(match.group(2))
                value = found[name] if replacements is None else replacements[name]
                output.append(match.group(1) + str(value) + match.group(3))
                matched = True
                break
        if not matched:
            output.append(line)
    missing = sorted(set(PIN_NAMES) - set(found))
    if missing:
        fail("canonical Makefile lacks unique numeric assignments for %s" % ", ".join(missing))
    return found, "".join(output).encode("utf-8")


def exact_keys(label, actual, expected):
    actual = set(actual)
    expected = set(expected)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing or extra:
        fail("%s key mismatch (missing=%r extra=%r)" % (label, missing, extra))


def expected_sets(topology):
    home_pairs = set(topology.home_pairs)
    foreign_pairs = set(topology.foreign_pairs)
    wire = {("C", source, target) for source, target in home_pairs}
    wire.update(("F", source, target) for source, target in foreign_pairs)
    return home_pairs, foreign_pairs, wire


def check_golden_links(label, golden, home_sizes, wire, home_ids):
    numbers = {}
    for ident in home_ids:
        match = re.fullmatch(r"img_(\d+)_.*", ident)
        if match:
            numbers[match.group(1)] = ident
    for name, (digest, size) in golden.items():
        match = re.fullmatch(r"img(\d+)_to_img(\d+)\.blob", name)
        if not match:
            continue
        source = numbers.get(match.group(1))
        target = numbers.get(match.group(2))
        if source is None or target is None:
            fail("%s golden pair %s is absent from the inventory" % (label, name))
        if home_sizes[(source, target)] != size:
            fail("%s golden/home size mismatch for %s" % (label, name))
        if wire[("C", source, target)] != digest:
            fail("%s golden/wire hash mismatch for %s" % (label, name))


def load_snapshot(label, paths, topology, golden_keys=None):
    home_pairs, foreign_pairs, wire_keys = expected_sets(topology)
    home = parse_home_sizes(paths["home-size-baseline.tsv"])
    wire = parse_wire(paths["corpus-wire.sha256"])
    golden = parse_golden(paths["golden.sha256"])
    exact_keys(label + " home sizes", home, home_pairs)
    exact_keys(label + " wire manifest", wire, wire_keys)
    if golden_keys is not None:
        exact_keys(label + " golden manifest", golden, golden_keys)
    for required in ("oneface_grow.blob", "oneface_revert.blob"):
        if required not in golden:
            fail("%s golden manifest lacks %s" % (label, required))
    check_golden_links(label, golden, home, wire, topology.home)
    return {
        "home": home,
        "wire": wire,
        "golden": golden,
        "home_pairs": len(home_pairs),
        "foreign_pairs": len(foreign_pairs),
    }


def uint_limit(value, label):
    try:
        parsed = int(value, 10)
    except (TypeError, ValueError):
        fail("%s is not an unsigned integer" % label)
    if parsed < 0:
        fail("%s is not an unsigned integer" % label)
    return parsed


def validate_policy(args, root):
    topology = load_topology(Path(args.inventory))
    canonical_paths = {name: canonical_path(root, name) for name in MANIFEST_FILES}
    candidate_paths = {
        "golden.sha256": Path(args.candidate_golden),
        "home-size-baseline.tsv": Path(args.candidate_home_sizes),
        "corpus-wire.sha256": Path(args.candidate_wire),
    }
    for name in MANIFEST_FILES:
        regular_file(canonical_paths[name], "canonical " + name)
        regular_file(candidate_paths[name], "candidate " + name)
        try:
            if os.path.samefile(canonical_paths[name], candidate_paths[name]):
                fail("candidate aliases canonical %s" % name)
        except OSError as exc:
            fail("cannot check candidate alias for %s: %s" % (name, exc))
    regular_file(canonical_path(root, "Makefile"), "canonical Makefile")
    canonical_bytes = {name: canonical_path(root, name).read_bytes() for name in FILES}
    candidate_bytes = {name: path.read_bytes() for name, path in candidate_paths.items()}

    for name, fields, indexes in (
            ("golden.sha256", 3, (2,)),
            ("home-size-baseline.tsv", 3, (0, 1)),
            ("corpus-wire.sha256", 4, (0, 1, 2))):
        if row_order(canonical_paths[name], fields, indexes) != \
                row_order(candidate_paths[name], fields, indexes):
            fail("candidate %s row order differs from the canonical manifest" % name)

    old = load_snapshot("canonical", canonical_paths, topology)
    new = load_snapshot("candidate", candidate_paths, topology, old["golden"].keys())
    metrics = parse_metrics(Path(args.metrics))
    preimage_keys = {
        "golden.sha256": "measurement_preimage_golden_sha256",
        "home-size-baseline.tsv": "measurement_preimage_home_sha256",
        "corpus-wire.sha256": "measurement_preimage_wire_sha256",
        "Makefile": "measurement_preimage_makefile_sha256",
    }
    for name, key in preimage_keys.items():
        actual = hashlib.sha256(canonical_bytes[name]).hexdigest()
        if metrics[key] != actual:
            fail("canonical %s changed since candidate measurement began" % name)
    expected_matrix = "%d/%d" % (new["home_pairs"], new["home_pairs"])
    expected_foreign = "%d/%d" % (new["foreign_pairs"], new["foreign_pairs"])
    if metrics["matrix_ok"] != expected_matrix or metrics["foreign_ok"] != expected_foreign:
        fail("candidate metrics do not prove all release round-trips (%s, %s)" %
             (metrics["matrix_ok"], metrics["foreign_ok"]))
    if (metrics["home_size_better"], metrics["home_size_worse"],
            metrics["home_size_equal"]) != ("NA", "NA", "NA"):
        fail("candidate corpus run was not an unpinned measurement")
    expected_wire = "NA/%d" % (new["home_pairs"] + new["foreign_pairs"])
    if metrics["wire_identity"] != expected_wire:
        fail("candidate corpus run did not bypass the old wire manifest")
    safety_exact = {
        "max_amplified": "0", "max_inversions": "0", "max_unaligned": "0",
        "max_oob_page_writes": "0", "max_canary_corrupt": "0",
    }
    for key, expected in safety_exact.items():
        if metrics[key] != expected:
            fail("candidate safety metric %s=%s, expected %s" %
                 (key, metrics[key], expected))
    if int(metrics["max_maxpageerase"]) > 1:
        fail("candidate max_maxpageerase exceeds one")

    expected_profile = verify_release_profile(Path(args.release_profile_lock))
    if metrics["measurement_release_profile"] != expected_profile:
        fail("candidate was measured under a different release profile")
    host_tool = Path(args.host_tool)
    regular_file(host_tool, "measured host tool")
    measured_tool_hash = metrics["measurement_host_tool_sha256"]
    if digest(host_tool) != measured_tool_hash:
        fail("candidate host tool changed after measurement")

    home_limit = uint_limit(args.home_limit, "home total limit")
    foreign_limit = uint_limit(args.foreign_limit, "foreign total limit")
    grow_limit = uint_limit(args.oneface_grow_limit, "one-face grow limit")
    revert_limit = uint_limit(args.oneface_revert_limit, "one-face revert limit")
    make_pins, _ = parse_and_rewrite_makefile(canonical_path(root, "Makefile"))
    expected_pins = {
        "BASE_FULL_TOTAL": home_limit,
        "BASE_FOREIGN_TOTAL": foreign_limit,
        "BASE_ONEFACE_GROW": grow_limit,
        "BASE_ONEFACE_REVERT": revert_limit,
    }
    if make_pins != expected_pins:
        fail("canonical Makefile policy pins do not match the protected update inputs")
    old_home_total = sum(old["home"].values())
    new_home_total = sum(new["home"].values())
    measured_home = int(metrics["full_total"])
    measured_foreign = int(metrics["foreign_total"])
    old_grow = old["golden"]["oneface_grow.blob"][1]
    old_revert = old["golden"]["oneface_revert.blob"][1]
    new_grow = new["golden"]["oneface_grow.blob"][1]
    new_revert = new["golden"]["oneface_revert.blob"][1]

    if old_home_total != home_limit:
        fail("canonical home total %d does not match policy pin %d" %
             (old_home_total, home_limit))
    if old_grow != grow_limit or old_revert != revert_limit:
        fail("canonical one-face sizes %d/%d do not match policy pins %d/%d" %
             (old_grow, old_revert, grow_limit, revert_limit))
    if measured_home != new_home_total:
        fail("measured home total %d disagrees with candidate per-pair sum %d" %
             (measured_home, new_home_total))

    better = worse = equal = 0
    regressed = []
    for pair in sorted(old["home"]):
        before = old["home"][pair]
        after = new["home"][pair]
        if after < before:
            better += 1
        elif after > before:
            worse += 1
            regressed.append("%s->%s:%d>%d" % (pair[0], pair[1], after, before))
        else:
            equal += 1
    if regressed:
        fail("candidate regresses %d home pair(s): %s" %
             (len(regressed), ", ".join(regressed[:8])))
    if new_home_total > old_home_total or new_home_total > home_limit:
        fail("candidate home total regresses: %d > %d" % (new_home_total, old_home_total))
    if measured_foreign > foreign_limit:
        fail("candidate foreign total regresses: %d > %d" %
             (measured_foreign, foreign_limit))
    if new_grow > old_grow or new_revert > old_revert:
        fail("candidate one-face sizes regress: %d/%d > %d/%d" %
             (new_grow, new_revert, old_grow, old_revert))

    new_pins = {
        "BASE_FULL_TOTAL": new_home_total,
        "BASE_FOREIGN_TOTAL": measured_foreign,
        "BASE_ONEFACE_GROW": new_grow,
        "BASE_ONEFACE_REVERT": new_revert,
    }
    _, candidate_makefile = parse_and_rewrite_makefile(
        canonical_path(root, "Makefile"), new_pins)
    # Freeze the validated candidates in memory before preparing the durable transaction and
    # prove neither the candidates nor canonical policy files moved while they were parsed.
    if any(path.read_bytes() != candidate_bytes[name]
           for name, path in candidate_paths.items()):
        fail("candidate artifacts changed during policy validation")
    if any(canonical_path(root, name).read_bytes() != canonical_bytes[name]
           for name in FILES):
        fail("canonical artifacts changed during policy validation; retry the update")
    candidates = dict(candidate_bytes)
    candidates["Makefile"] = candidate_makefile
    return candidates, canonical_bytes, {
        "better": better,
        "worse": worse,
        "equal": equal,
        "old_home": old_home_total,
        "new_home": new_home_total,
        "old_foreign": foreign_limit,
        "new_foreign": measured_foreign,
        "old_grow": old_grow,
        "new_grow": new_grow,
        "old_revert": old_revert,
        "new_revert": new_revert,
        "host_tool": host_tool,
        "host_tool_hash": measured_tool_hash,
    }


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            result.update(chunk)
    return result.hexdigest()


def verify_release_profile(lock_path):
    helper = Path(__file__).resolve().with_name("build_profile.py")
    try:
        result = subprocess.run(
            [sys.executable, str(helper), "verify-release", str(lock_path)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace")
    except OSError as exc:
        fail("cannot recheck release profile: %s" % exc)
    output = result.stdout.strip()
    if result.returncode != 0:
        detail = result.stderr.strip() or output
        fail("release profile recheck failed: %s" % (detail.splitlines()[0] if detail else ""))
    if not output.startswith("release_profile=") or "\n" in output:
        fail("release profile recheck returned malformed evidence")
    value = output[len("release_profile="):]
    if not SHA256_RE.fullmatch(value):
        fail("release profile recheck returned a malformed hash")
    return value


def fsync_dir(path):
    fd = os.open(str(path), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_marker(state, marker, fault=None, point_prefix="marker"):
    temporary = state / ("marker.tmp.%d.%s" % (os.getpid(), uuid.uuid4().hex))
    data = (json.dumps(marker, sort_keys=True, separators=(",", ":")) + "\n").encode()
    fd = os.open(str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb", closefd=False) as target:
            target.write(data)
            target.flush()
            os.fsync(target.fileno())
    finally:
        os.close(fd)
    if fault is not None:
        fault.hit(point_prefix + "-temp-fsync")
    os.replace(temporary, state / "marker.json")
    if fault is not None:
        fault.hit(point_prefix + "-rename")
    fsync_dir(state)
    if fault is not None:
        fault.hit(point_prefix + "-fsync")


def read_marker(state):
    try:
        with (state / "marker.json").open(encoding="utf-8") as source:
            marker = json.load(source)
    except (OSError, ValueError) as exc:
        fail("cannot read durable wire-update marker: %s" % exc)
    files = marker.get("files")
    if (marker.get("version") != 1 or marker.get("phase") not in ("prepared", "committed") or
            not isinstance(files, list) or not files or
            files != [name for name in FILES if name in files] or len(set(files)) != len(files)):
        fail("durable wire-update marker has an unsupported shape")
    for side in ("old", "new"):
        values = marker.get(side)
        if not isinstance(values, dict) or set(values) != set(files):
            fail("durable wire-update marker lacks %s hashes" % side)
        if any(not isinstance(value, str) or not SHA256_RE.fullmatch(value)
               for value in values.values()):
            fail("durable wire-update marker has invalid %s hashes" % side)
    return marker


def cleanup_stale_files(root):
    for directory in (root, root.parent):
        changed = False
        try:
            entries = list(directory.iterdir())
        except OSError:
            continue
        for entry in entries:
            try:
                if directory == root and (entry.name.startswith(PREPARE_PREFIX) or
                                          entry.name.startswith(DONE_PREFIX)):
                    if entry.is_dir() and not entry.is_symlink():
                        shutil.rmtree(entry)
                    else:
                        entry.unlink()
                    changed = True
                elif entry.name.startswith(RECOVER_PREFIX) and entry.is_file():
                    entry.unlink()
                    changed = True
            except OSError:
                # These names are inactive garbage.  Never turn a durable OLD/NEW generation
                # choice into a reported update failure merely because cleanup must be retried.
                continue
        if changed:
            try:
                fsync_dir(directory)
            except OSError:
                pass


def remove_state(root, state, fault):
    garbage = root / (DONE_PREFIX + uuid.uuid4().hex)
    os.rename(state, garbage)
    fault.hit("cleanup-state-rename")
    fsync_dir(root)
    fault.hit("cleanup-state-fsync")
    try:
        shutil.rmtree(garbage)
    except OSError:
        # State retirement above is the durable decision point.  The ignored garbage directory
        # is intentionally left for best-effort cleanup by a later publisher invocation.
        pass


def install_file(root, source_path, name, mode, fault, index, point_prefix):
    destination = canonical_path(root, name)
    fd, temporary_name = tempfile.mkstemp(prefix=RECOVER_PREFIX,
                                           dir=str(destination.parent))
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as target, source_path.open("rb") as source:
            os.fchmod(target.fileno(), mode)
            shutil.copyfileobj(source, target)
            target.flush()
            os.fsync(target.fileno())
        fault.hit("%s-%d-temp-fsync" % (point_prefix, index))
        os.replace(temporary, destination)
        fault.hit("%s-%d-rename" % (point_prefix, index))
        fsync_dir(destination.parent)
        fault.hit("%s-%d-fsync" % (point_prefix, index))
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def validate_transaction_material(root, state, marker):
    """Validate the entire recovery decision without mutating a canonical byte."""
    for name in marker["files"]:
        for generation in ("old", "new"):
            staged = state / generation / name
            regular_file(staged, "wire-update %s material" % generation)
            if digest(staged) != marker[generation][name]:
                fail("wire-update %s material is corrupt for %s; retaining recovery evidence" %
                     (generation, name))
    for name in marker["files"]:
        canonical = canonical_path(root, name)
        regular_file(canonical, "canonical recovery target")
        current = digest(canonical)
        if current not in (marker["old"][name], marker["new"][name]):
            fail("canonical %s has an unknown hash; retaining recovery evidence" % name)


def recover_locked(root, fault):
    state = root / STATE_NAME
    if not state.exists():
        cleanup_stale_files(root)
        return "clean"
    if not state.is_dir() or state.is_symlink():
        fail("wire-update transaction path is not a directory")
    marker = read_marker(state)
    validate_transaction_material(root, state, marker)
    if marker["phase"] == "committed":
        new_dir = state / "new"
        for index, name in enumerate(marker["files"], 1):
            staged = new_dir / name
            regular_file(staged, "wire-update committed copy")
            mode = stat.S_IMODE(staged.stat().st_mode)
            install_file(root, staged, name, mode, fault, index, "finalize")
        for name in marker["files"]:
            if digest(canonical_path(root, name)) != marker["new"][name]:
                fail("committed wire-update transaction has a modified %s" % name)
        remove_state(root, state, fault)
        cleanup_stale_files(root)
        return "finalized"

    old_dir = state / "old"
    for index, name in enumerate(marker["files"], 1):
        backup = old_dir / name
        regular_file(backup, "wire-update backup")
        mode = stat.S_IMODE(backup.stat().st_mode)
        install_file(root, backup, name, mode, fault, index, "recover")
    for name in marker["files"]:
        if digest(canonical_path(root, name)) != marker["old"][name]:
            fail("wire-update rollback failed to restore %s" % name)
    remove_state(root, state, fault)
    cleanup_stale_files(root)
    return "rolled_back"


def prepare_transaction(root, candidates, expected_old, policy, fault):
    state = root / STATE_NAME
    if state.exists():
        fail("wire-update transaction was not recovered before prepare")
    prepare = Path(tempfile.mkdtemp(prefix=PREPARE_PREFIX, dir=str(root)))
    try:
        old_dir = prepare / "old"
        new_dir = prepare / "new"
        old_dir.mkdir()
        new_dir.mkdir()
        changed = [name for name in FILES if expected_old[name] != candidates[name]]
        if not changed:
            shutil.rmtree(prepare)
            return None
        marker = {"version": 1, "phase": "prepared", "files": changed,
                  "old": {}, "new": {}}
        for index, name in enumerate(changed, 1):
            canonical = canonical_path(root, name)
            mode = stat.S_IMODE(canonical.stat().st_mode)
            old = old_dir / name
            new = new_dir / name
            with old.open("xb") as target:
                os.fchmod(target.fileno(), mode)
                target.write(expected_old[name])
                target.flush()
                os.fsync(target.fileno())
            fault.hit("stage-%d-old-fsync" % index)
            with new.open("xb") as target:
                os.fchmod(target.fileno(), mode)
                target.write(candidates[name])
                target.flush()
                os.fsync(target.fileno())
            fault.hit("stage-%d-new-fsync" % index)
            marker["old"][name] = digest(old)
            marker["new"][name] = digest(new)
        fsync_dir(old_dir)
        fault.hit("prepare-old-dir-fsync")
        fsync_dir(new_dir)
        fault.hit("prepare-new-dir-fsync")
        write_marker(prepare, marker, fault, "prepare-marker")
        # Compare-and-swap boundary: do not make the durable transaction active if an editor or
        # another non-cooperating process changed any canonical byte after validation.  Recheck
        # the exact measured executable here as well.
        if any(canonical_path(root, name).read_bytes() != expected_old[name]
               for name in FILES):
            fail("canonical artifacts changed before transaction activation; retry")
        if digest(policy["host_tool"]) != policy["host_tool_hash"]:
            fail("measured host tool changed before transaction activation")
        os.rename(prepare, state)
        fault.hit("prepare-state-rename")
        fsync_dir(root)
        fault.hit("prepare-state-fsync")
        return marker
    except BaseException:
        if prepare.exists():
            shutil.rmtree(prepare, ignore_errors=True)
        raise


def publish_transaction(root, marker, fault):
    state = root / STATE_NAME
    # The marker, both rollback generations, and every current target must form one complete
    # transaction before the first canonical replacement.  A corrupt staged NEW must not cause a
    # partial publish, and a corrupt OLD must not make rollback impossible after publication.
    validate_transaction_material(root, state, marker)
    for index, name in enumerate(marker["files"], 1):
        staged = state / "new" / name
        mode = stat.S_IMODE(staged.stat().st_mode)
        install_file(root, staged, name, mode, fault, index, "publish")
    marker["phase"] = "committed"
    write_marker(state, marker, fault, "commit-marker")
    for name in marker["files"]:
        if digest(canonical_path(root, name)) != marker["new"][name]:
            fail("published %s does not match the staged transaction" % name)
    remove_state(root, state, fault)


def lock_root(root):
    root = root.resolve()
    if not root.is_dir() or root.is_symlink():
        fail("wire baseline root must be a real directory: %s" % root)
    if root.stat().st_dev != root.parent.stat().st_dev:
        fail("wire baseline root and Makefile must share one filesystem")
    flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_CLOEXEC", 0)
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        fd = os.open(str(root / LOCK_NAME), flags, 0o600)
    except OSError as exc:
        fail("cannot open wire-update lock: %s" % exc)
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        fail("wire-update lock is not a regular file")
    return root, fd


def command_recover(args):
    root, fd = lock_root(Path(args.root))
    fault = FaultInjector(args, root)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("another wire baseline update holds the publication lock")
        result = recover_locked(root, fault)
    finally:
        os.close(fd)
    print("wire_baseline_recovery=%s" % result)
    return 0


def command_assert_clean(args):
    """Reject readers while a durable transaction still owns generation choice."""
    root, fd = lock_root(Path(args.root))
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("a wire baseline publication is active; retry the release reader")
        if os.path.lexists(root / STATE_NAME):
            fail("wire baseline recovery is pending; run 'make golden-update' for recovery "
                 "before certification or packaging")
    finally:
        os.close(fd)
    print("wire_baseline_reader=clean")
    return 0


def command_publish(args):
    root, fd = lock_root(Path(args.root))
    fault = FaultInjector(args, root)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("another wire baseline update holds the publication lock")
        if fault.hold_ms:
            time.sleep(fault.hold_ms / 1000.0)
        recovered = recover_locked(root, fault)
        candidates, canonical_bytes, policy = validate_policy(args, root)
        try:
            marker = prepare_transaction(root, candidates, canonical_bytes, policy, fault)
            if marker is not None:
                publish_transaction(root, marker, fault)
        except BaseException:
            # Ordinary failures return with all canonical files restored.  SIGKILL and the
            # explicit hard-crash hooks leave the durable transaction for the next invocation.
            recover_locked(root, fault)
            raise
    finally:
        os.close(fd)
    print("wire_baseline_recovery=%s" % recovered)
    print("wire_baseline_home_pairs=%d/%d/%d (better/worse/equal)" %
          (policy["better"], policy["worse"], policy["equal"]))
    print("wire_baseline_home_total=%d->%d" %
          (policy["old_home"], policy["new_home"]))
    print("wire_baseline_foreign_total=%d->%d" %
          (policy["old_foreign"], policy["new_foreign"]))
    print("wire_baseline_oneface=%d/%d->%d/%d" %
          (policy["old_grow"], policy["old_revert"],
           policy["new_grow"], policy["new_revert"]))
    print("wire_baseline_transaction=%s" % ("NOOP" if marker is None else "COMMITTED"))
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    assert_clean = sub.add_parser(
        "assert-clean", help="fail if release readers could observe a pending transaction")
    assert_clean.add_argument("--root", required=True)
    assert_clean.set_defaults(func=command_assert_clean)

    recover = sub.add_parser("recover", help="recover/finalize an interrupted publication")
    recover.add_argument("--root", required=True)
    recover.add_argument("--test-fault", default="", help=argparse.SUPPRESS)
    recover.add_argument("--test-lock-hold-ms", type=int, default=0,
                         help=argparse.SUPPRESS)
    recover.set_defaults(func=command_recover)

    publish = sub.add_parser("publish", help="validate and publish completed candidates")
    publish.add_argument("--root", required=True)
    publish.add_argument("--inventory", required=True)
    publish.add_argument("--candidate-golden", required=True)
    publish.add_argument("--candidate-home-sizes", required=True)
    publish.add_argument("--candidate-wire", required=True)
    publish.add_argument("--metrics", required=True)
    publish.add_argument("--host-tool", required=True)
    publish.add_argument("--release-profile-lock", required=True)
    publish.add_argument("--home-limit", required=True)
    publish.add_argument("--foreign-limit", required=True)
    publish.add_argument("--oneface-grow-limit", required=True)
    publish.add_argument("--oneface-revert-limit", required=True)
    # Test-only fault/concurrency controls.  They require a sentinel in a synthetic root and the
    # Makefile never forwards them.
    publish.add_argument("--test-fault", default="", help=argparse.SUPPRESS)
    publish.add_argument("--test-lock-hold-ms", type=int, default=0,
                         help=argparse.SUPPRESS)
    publish.set_defaults(func=command_publish)
    return parser


def main():
    args = build_parser().parse_args()
    try:
        return args.func(args)
    except (OSError, UpdateError) as exc:
        print("publish_wire_baselines.py: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
