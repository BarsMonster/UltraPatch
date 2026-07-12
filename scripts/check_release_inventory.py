#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

"""Validate that every committed release manifest describes one inventory."""

import argparse
import re
import sys
from pathlib import Path

from corpus_topology import TopologyError, parse_inventory


SHA256_RE = re.compile(r"[0-9a-f]{64}")


def fail(message):
    raise ValueError(message)


def rows(path, fields):
    result = []
    with Path(path).open(encoding="utf-8") as source:
        for lineno, raw in enumerate(source, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            values = line.split()
            if len(values) != fields:
                fail("%s:%d: expected %d fields" % (path, lineno, fields))
            result.append((lineno, values))
    return result


def hashes(path):
    result = {}
    for lineno, values in rows(path, 2):
        digest, name = values
        if not SHA256_RE.fullmatch(digest):
            fail("%s:%d: invalid SHA-256" % (path, lineno))
        if name in result:
            fail("%s:%d: duplicate path %r" % (path, lineno, name))
        result[name] = digest
    return result


def sizes(path):
    result = {}
    for lineno, values in rows(path, 3):
        source, target, size = values
        if not size.isdigit():
            fail("%s:%d: invalid size %r" % (path, lineno, size))
        key = (source, target)
        if key in result:
            fail("%s:%d: duplicate pair %s -> %s" % (path, lineno, source, target))
        result[key] = int(size)
    return result


def wire_hashes(path):
    result = {}
    for lineno, values in rows(path, 4):
        tag, source, target, digest = values
        if tag not in ("C", "F") or not SHA256_RE.fullmatch(digest):
            fail("%s:%d: invalid wire entry" % (path, lineno))
        key = (tag, source, target)
        if key in result:
            fail("%s:%d: duplicate wire pair" % (path, lineno))
        result[key] = digest
    return result


def golden(path):
    result = {}
    for lineno, values in rows(path, 3):
        digest, size, name = values
        if not SHA256_RE.fullmatch(digest) or not size.isdigit():
            fail("%s:%d: invalid golden entry" % (path, lineno))
        if name in result:
            fail("%s:%d: duplicate golden blob %r" % (path, lineno, name))
        result[name] = (digest, int(size))
    return result


def exact_keys(label, actual, expected):
    actual = set(actual)
    expected = set(expected)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing or extra:
        fail("%s inventory mismatch (missing=%r extra=%r)" % (label, missing, extra))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--inventory", required=True)
    parser.add_argument("--corpus-assets", required=True)
    parser.add_argument("--foreign-assets", required=True)
    parser.add_argument("--home-sizes", required=True)
    parser.add_argument("--wire", required=True)
    parser.add_argument("--golden", required=True)
    parser.add_argument("--home-total", required=True, type=int)
    parser.add_argument("--oneface-grow", required=True, type=int)
    parser.add_argument("--oneface-revert", required=True, type=int)
    parser.add_argument("--fixtures", required=True, type=int)
    parser.add_argument("--home-images", required=True, type=int)
    parser.add_argument("--foreign-images", required=True, type=int)
    parser.add_argument("--foreign-edges", required=True, type=int)
    parser.add_argument("--golden-blobs", required=True, type=int)
    args = parser.parse_args()

    try:
        topology = parse_inventory(args.inventory)
        fixtures = topology.fixtures
        home = topology.home
        foreign = topology.foreign
        if (len(fixtures), len(home), len(foreign)) != (
                args.fixtures, args.home_images, args.foreign_images):
            fail("release inventory cardinality changed: fixtures=%d home=%d foreign=%d" %
                 (len(fixtures), len(home), len(foreign)))
        if fixtures != ("v0_base", "v1_one_face"):
            fail("release fixtures must remain v0_base then v1_one_face")
        if len(topology.foreign_edges) != args.foreign_edges:
            fail("release foreign-edge cardinality changed: %d" %
                 len(topology.foreign_edges))

        corpus_expected = []
        for ident in fixtures:
            corpus_expected.extend(("test-bench/fixtures/%s/watch.bin" % ident,
                                    "test-bench/fixtures/%s/watch.elf" % ident))
        for ident in home:
            corpus_expected.extend(("test-bench/images/%s/watch.bin" % ident,
                                    "test-bench/images/%s/watch.elf" % ident))
        foreign_expected = ["test-bench/foreign/%s/watch.bin" % ident for ident in foreign]
        exact_keys("corpus asset manifest", hashes(args.corpus_assets), corpus_expected)
        exact_keys("foreign asset manifest", hashes(args.foreign_assets), foreign_expected)

        expected_home_pairs = set(topology.home_pairs)
        home_sizes = sizes(args.home_sizes)
        exact_keys("home size baseline", home_sizes, expected_home_pairs)
        measured_total = sum(home_sizes.values())
        if measured_total != args.home_total:
            fail("home size baseline totals %d, Makefile pin is %d" %
                 (measured_total, args.home_total))

        expected_foreign_pairs = set(topology.foreign_pairs)
        expected_wire = {("C", source, target) for source, target in expected_home_pairs}
        expected_wire.update(("F", source, target) for source, target in expected_foreign_pairs)
        wire = wire_hashes(args.wire)
        exact_keys("corpus wire manifest", wire, expected_wire)

        pins = golden(args.golden)
        if len(pins) != args.golden_blobs:
            fail("golden manifest has %d blobs, expected %d" %
                 (len(pins), args.golden_blobs))
        for name, expected in (("oneface_grow.blob", args.oneface_grow),
                               ("oneface_revert.blob", args.oneface_revert)):
            if name not in pins or pins[name][1] != expected:
                fail("%s size does not match Makefile pin %d" % (name, expected))

        # Every home-pair golden pin must be the same artifact as the full wire manifest.
        home_number = {}
        for ident in home:
            match = re.fullmatch(r"img_(\d+)_.*", ident)
            if match:
                home_number[match.group(1)] = ident
        for name, (digest, _) in pins.items():
            match = re.fullmatch(r"img(\d+)_to_img(\d+)\.blob", name)
            if not match:
                continue
            source = home_number.get(match.group(1))
            target = home_number.get(match.group(2))
            if source is None or target is None:
                fail("golden pair %s is absent from the release inventory" % name)
            if wire[("C", source, target)] != digest:
                fail("golden and corpus wire hashes differ for %s" % name)
            if home_sizes[(source, target)] != pins[name][1]:
                fail("golden and home size baselines differ for %s" % name)
    except (OSError, UnicodeError, TopologyError, ValueError) as exc:
        print("check_release_inventory.py: %s" % exc, file=sys.stderr)
        return 1

    print("release_inventory=OK (fixtures=%d home=%d foreign=%d edges=%d)" %
          (len(fixtures), len(home), len(foreign), len(topology.foreign_edges)))
    print("release_fixture_count=%d" % len(fixtures))
    print("release_home_images=%d" % len(home))
    print("release_foreign_images=%d" % len(foreign))
    print("release_home_pairs=%d" % len(expected_home_pairs))
    print("release_foreign_edges=%d" % len(topology.foreign_edges))
    print("release_foreign_pairs=%d" % len(expected_foreign_pairs))
    print("release_wire_pairs=%d" % len(expected_wire))
    print("release_golden_blobs=%d" % len(pins))
    print("release_home_total=%d" % measured_total)
    print("release_oneface_grow=%d" % pins["oneface_grow.blob"][1])
    print("release_oneface_revert=%d" % pins["oneface_revert.blob"][1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
