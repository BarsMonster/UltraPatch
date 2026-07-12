#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

"""Validate that every committed release manifest describes one inventory."""

import argparse
import re
import sys

from corpus_topology import TopologyError, parse_inventory
from wire_baseline import BaselineError, parse_wire_baseline


SHA256_RE = re.compile(r"[0-9a-f]{64}")


def fail(message):
    raise ValueError(message)


def hashes(path):
    result = {}
    with open(path, encoding="utf-8") as source:
        for lineno, raw in enumerate(source, 1):
            values = raw.split()
            if not values or values[0].startswith("#"):
                continue
            if len(values) != 2 or not SHA256_RE.fullmatch(values[0]):
                fail("%s:%d: invalid asset hash row" % (path, lineno))
            digest, name = values
            if name in result:
                fail("%s:%d: duplicate path %r" % (path, lineno, name))
            result[name] = digest
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
    parser.add_argument("--wire-baseline", required=True)
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

        baseline = parse_wire_baseline(args.wire_baseline)
        expected_home_pairs = set(topology.home_pairs)
        exact_keys("home baseline", baseline.home, expected_home_pairs)
        measured_total = sum(row.size for row in baseline.home.values()
                             if row.size is not None)
        if measured_total != args.home_total:
            fail("home size baseline totals %d, Makefile pin is %d" %
                 (measured_total, args.home_total))

        expected_foreign_pairs = set(topology.foreign_pairs)
        exact_keys("foreign baseline", baseline.foreign, expected_foreign_pairs)

        pins = baseline.golden
        if len(pins) != args.golden_blobs:
            fail("wire baseline has %d golden blobs, expected %d" %
                 (len(pins), args.golden_blobs))
        for name, expected in (("oneface_grow.blob", args.oneface_grow),
                               ("oneface_revert.blob", args.oneface_revert)):
            if name not in pins or pins[name].size != expected:
                fail("%s size does not match Makefile pin %d" % (name, expected))
    except (OSError, UnicodeError, TopologyError, BaselineError, ValueError) as exc:
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
    print("release_wire_pairs=%d" %
          (len(expected_home_pairs) + len(expected_foreign_pairs)))
    print("release_golden_blobs=%d" % len(pins))
    print("release_home_total=%d" % measured_total)
    print("release_oneface_grow=%d" % pins["oneface_grow.blob"].size)
    print("release_oneface_revert=%d" % pins["oneface_revert.blob"].size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
