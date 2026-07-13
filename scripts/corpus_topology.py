#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

"""Parse, materialize, and verify the authoritative release corpus inventory."""

import argparse
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ID_RE = re.compile(r"[A-Za-z0-9_.-]+")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
ROLES = ("fixture", "home", "foreign")


class TopologyError(ValueError):
    pass


class CorpusTopology:
    def __init__(self, members, hashes, edges):
        self.fixtures = tuple(members["fixture"])
        self.home = tuple(members["home"])
        self.foreign = tuple(members["foreign"])
        self.hashes = dict(hashes)
        self.foreign_edges = tuple(edges)

    @property
    def home_pairs(self):
        return tuple((a, b) for a in self.home for b in self.home)

    @property
    def foreign_pairs(self):
        return tuple(pair for a, b in self.foreign_edges
                     for pair in ((a, b), (b, a)))

    def jobs(self, images, foreign):
        for a, b in self.foreign_pairs:
            yield "F", str(Path(foreign) / a), str(Path(foreign) / b)
        for a, b in self.home_pairs:
            yield "C", str(Path(images) / a), str(Path(images) / b)


def _parse(lines, path):
    members = {role: [] for role in ROLES}
    hashes = {}
    member_ids = set()
    raw_edges = []
    for lineno, raw in enumerate(lines, 1):
        fields = raw.split()
        if not fields or fields[0].startswith("#"):
            continue
        role = fields[0]
        if role in ROLES:
            if (len(fields) != 4 or not ID_RE.fullmatch(fields[1]) or
                    fields[1] in member_ids):
                raise TopologyError("%s:%d: invalid or duplicate %s member" %
                                    (path, lineno, role))
            bin_hash, elf_hash = fields[2:]
            if (not SHA256_RE.fullmatch(bin_hash) or
                    (role == "foreign" and elf_hash != "-") or
                    (role != "foreign" and not SHA256_RE.fullmatch(elf_hash))):
                raise TopologyError("%s:%d: invalid %s asset hashes" %
                                    (path, lineno, role))
            member_ids.add(fields[1])
            members[role].append(fields[1])
            hashes[(role, fields[1])] = (bin_hash, elf_hash)
        elif role == "foreign-edge":
            if (len(fields) != 3 or fields[1] == fields[2] or
                    not all(ID_RE.fullmatch(value) for value in fields[1:])):
                raise TopologyError("%s:%d: invalid foreign edge" % (path, lineno))
            raw_edges.append((lineno, fields[1], fields[2]))
        else:
            raise TopologyError("%s:%d: unknown role %r" % (path, lineno, role))

    foreign = set(members["foreign"])
    if not members["home"] or len(foreign) < 2 or not raw_edges:
        raise TopologyError("%s: inventory needs home members, foreign members, and edges" % path)
    edges, seen, covered = [], set(), set()
    for lineno, a, b in raw_edges:
        key = frozenset((a, b))
        if a not in foreign or b not in foreign or key in seen:
            raise TopologyError("%s:%d: unknown endpoint or duplicate undirected edge" %
                                (path, lineno))
        seen.add(key)
        covered.update((a, b))
        edges.append((a, b))
    if covered != foreign:
        raise TopologyError("%s: every foreign member must have an edge" % path)
    return CorpusTopology(members, hashes, edges)


def parse_inventory(path):
    path = Path(path)
    return _parse(path.read_text(encoding="utf-8").splitlines(), path)


def sha256_file(path):
    if not path.is_file() or path.is_symlink():
        raise TopologyError("corpus asset must be a regular non-symlink file: %s" % path)
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_asset(root, role, ident, suffix):
    directory = {"fixture": "fixtures", "home": "images", "foreign": "foreign"}[role]
    return root / directory / ident / ("watch." + suffix)


def output_asset(root, role, ident, suffix):
    directory = {"fixture": "fixtures", "home": "images"}[role]
    return root / directory / ident / ("watch." + suffix)


def verify_digest(path, expected, label):
    actual = sha256_file(path)
    if actual != expected:
        raise TopologyError("%s SHA-256 mismatch for %s: %s != %s" %
                            (label, path, actual, expected))


def verify_sources(topology, source_root):
    for role, members in (("fixture", topology.fixtures), ("home", topology.home)):
        for ident in members:
            _, elf_hash = topology.hashes[(role, ident)]
            verify_digest(source_asset(source_root, role, ident, "elf"), elf_hash,
                          "tracked ELF")
    for ident in topology.foreign:
        bin_hash, _ = topology.hashes[("foreign", ident)]
        verify_digest(source_asset(source_root, "foreign", ident, "bin"), bin_hash,
                      "tracked foreign binary")


def publish_copy(source, destination, expected):
    try:
        if sha256_file(destination) == expected:
            return
    except (OSError, TopologyError):
        pass
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=".%s." % destination.name,
                                           suffix=".tmp", dir=destination.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as target, source.open("rb") as input_file:
            shutil.copyfileobj(input_file, target)
            target.flush()
            os.fsync(target.fileno())
        os.chmod(temporary, 0o644)
        verify_digest(temporary, expected, "copied ELF")
        os.replace(temporary, destination)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def publish_binary(objcopy, source, destination, expected):
    try:
        if sha256_file(destination) == expected:
            return
    except (OSError, TopologyError):
        pass
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=".%s." % destination.name,
                                           suffix=".tmp", dir=destination.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        result = subprocess.run(objcopy + ["-O", "binary", str(source), str(temporary)],
                                check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                errors="replace")
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            raise TopologyError("objcopy failed for %s%s" %
                                (source, ": " + detail.splitlines()[0] if detail else ""))
        os.chmod(temporary, 0o644)
        verify_digest(temporary, expected, "derived binary")
        os.replace(temporary, destination)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def materialize(topology, source_root, output_root, objcopy):
    verify_sources(topology, source_root)
    derived = 0
    for role, members in (("fixture", topology.fixtures), ("home", topology.home)):
        for ident in members:
            bin_hash, elf_hash = topology.hashes[(role, ident)]
            source = source_asset(source_root, role, ident, "elf")
            publish_copy(source, output_asset(output_root, role, ident, "elf"), elf_hash)
            publish_binary(objcopy, source,
                           output_asset(output_root, role, ident, "bin"), bin_hash)
            derived += 1
    verify_outputs(topology, output_root)
    return derived


def verify_outputs(topology, output_root):
    files = 0
    for role, members in (("fixture", topology.fixtures), ("home", topology.home)):
        for ident in members:
            bin_hash, elf_hash = topology.hashes[(role, ident)]
            verify_digest(output_asset(output_root, role, ident, "bin"), bin_hash,
                          "materialized binary")
            verify_digest(output_asset(output_root, role, ident, "elf"), elf_hash,
                          "materialized ELF")
            files += 2
    return files


def selftest():
    a, b, c = "a" * 64, "b" * 64, "c" * 64
    text = """home h0 %s %s
foreign f0 %s -
foreign f1 %s -
foreign f2 %s -
foreign-edge f1 f2
foreign-edge f0 f2
foreign-edge f0 f1
""" % (a, b, c, a, b)
    topology = _parse(text.splitlines(), "synthetic")
    expected = (("f1", "f2"), ("f2", "f1"), ("f0", "f2"),
                ("f2", "f0"), ("f0", "f1"), ("f1", "f0"))
    if topology.foreign_pairs != expected:
        raise TopologyError("synthetic added-edge topology changed")
    try:
        _parse((text + "foreign-edge f2 f1\n").splitlines(), "synthetic")
    except TopologyError:
        pass
    else:
        raise TopologyError("synthetic reverse duplicate was accepted")
    print("corpus_topology_contract=OK (synthetic_added_edge_and_duplicate=OK)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("counts", "jobs", "materialize", "test", "verify"))
    parser.add_argument("--inventory")
    parser.add_argument("--images-root")
    parser.add_argument("--foreign-root", default="test-bench/foreign")
    parser.add_argument("--source-root", default="test-bench")
    parser.add_argument("--output-root")
    parser.add_argument("--objcopy")
    args = parser.parse_args()
    try:
        if args.command == "test":
            selftest()
            return 0
        if not args.inventory:
            parser.error("command requires --inventory")
        topology = parse_inventory(args.inventory)
        if args.command == "jobs":
            if not args.images_root:
                parser.error("jobs requires --images-root")
            for job in topology.jobs(args.images_root, args.foreign_root):
                print("%s\t%s\t%s" % job)
        elif args.command in ("materialize", "verify"):
            if not args.output_root:
                parser.error("%s requires --output-root" % args.command)
            source_root = Path(args.source_root)
            output_root = Path(args.output_root)
            if args.command == "materialize":
                if not args.objcopy:
                    parser.error("materialize requires --objcopy")
                objcopy = shlex.split(args.objcopy, posix=True)
                if not objcopy:
                    parser.error("--objcopy must not be empty")
                derived = materialize(topology, source_root, output_root, objcopy)
                print("corpus_materialized=%d binaries in %s" % (derived, output_root))
            else:
                verify_sources(topology, source_root)
                corpus_files = verify_outputs(topology, output_root)
                print("corpus_assets=verified %d files via %s" %
                      (corpus_files, args.inventory))
                print("foreign_assets=verified %d files via %s" %
                      (len(topology.foreign), args.inventory))
        else:
            counts = (len(topology.home_pairs), len(topology.foreign_edges),
                      len(topology.foreign_pairs))
            names = ("HOME_PAIRS", "FOREIGN_EDGES", "FOREIGN_PAIRS")
            for name, value in zip(names, counts):
                print("CORPUS_TOPOLOGY_%s=%d" % (name, value))
            print("CORPUS_TOPOLOGY_WIRE_PAIRS=%d" % (counts[0] + counts[2]))
        return 0
    except (OSError, UnicodeError, TopologyError) as exc:
        print("corpus_topology.py: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
