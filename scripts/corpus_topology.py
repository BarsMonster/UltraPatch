#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

"""Shared release-corpus membership and foreign-edge parser."""

import argparse
import re
import sys
from pathlib import Path


ID_RE = re.compile(r"[A-Za-z0-9_.-]+")
ROLES = ("fixture", "home", "foreign")


class TopologyError(ValueError):
    pass


class CorpusTopology:
    def __init__(self, members, edges):
        self.fixtures = tuple(members["fixture"])
        self.home = tuple(members["home"])
        self.foreign = tuple(members["foreign"])
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
    member_ids = set()
    raw_edges = []
    for lineno, raw in enumerate(lines, 1):
        fields = raw.split()
        if not fields or fields[0].startswith("#"):
            continue
        role = fields[0]
        if role in ROLES:
            if (len(fields) != 2 or not ID_RE.fullmatch(fields[1]) or
                    fields[1] in member_ids):
                raise TopologyError("%s:%d: invalid or duplicate %s member" %
                                    (path, lineno, role))
            member_ids.add(fields[1])
            members[role].append(fields[1])
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
    return CorpusTopology(members, edges)


def parse_inventory(path):
    path = Path(path)
    return _parse(path.read_text(encoding="utf-8").splitlines(), path)


def selftest():
    text = """home h0
foreign f0
foreign f1
foreign f2
foreign-edge f1 f2
foreign-edge f0 f2
foreign-edge f0 f1
"""
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
    parser.add_argument("command", choices=("counts", "jobs", "test"))
    parser.add_argument("--inventory")
    parser.add_argument("--images-root", default="test-bench/images")
    parser.add_argument("--foreign-root", default="test-bench/foreign")
    args = parser.parse_args()
    try:
        if args.command == "test":
            selftest()
            return 0
        if not args.inventory:
            parser.error("counts/jobs require --inventory")
        topology = parse_inventory(args.inventory)
        if args.command == "jobs":
            for job in topology.jobs(args.images_root, args.foreign_root):
                print("%s\t%s\t%s" % job)
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
