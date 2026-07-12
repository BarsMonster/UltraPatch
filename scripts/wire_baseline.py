#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Parser for the canonical combined size/wire/golden baseline."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re


SHA256_RE = re.compile(r"[0-9a-f]{64}")
NAME_RE = re.compile(r"[A-Za-z0-9_.-]+")


class BaselineError(ValueError):
    pass


@dataclass(frozen=True)
class PairBaseline:
    size: int | None
    digest: str


@dataclass(frozen=True)
class WireBaseline:
    home: dict[tuple[str, str], PairBaseline]
    foreign: dict[tuple[str, str], PairBaseline]
    golden: dict[str, PairBaseline]
    order: tuple[tuple[str, str, str], ...]


def parse_wire_baseline(path: str | Path) -> WireBaseline:
    source_path = Path(path)
    home: dict[tuple[str, str], PairBaseline] = {}
    foreign: dict[tuple[str, str], PairBaseline] = {}
    golden: dict[str, PairBaseline] = {}
    order: list[tuple[str, str, str]] = []
    try:
        lines = source_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BaselineError(f"cannot read {source_path}: {exc}") from exc
    for lineno, raw in enumerate(lines, 1):
        fields = raw.split()
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) != 5:
            raise BaselineError(f"{source_path}:{lineno}: expected five fields")
        kind, first, second, size_text, digest = fields
        if kind not in ("C", "F", "G") or not SHA256_RE.fullmatch(digest):
            raise BaselineError(f"{source_path}:{lineno}: invalid kind or SHA-256")
        if not NAME_RE.fullmatch(first) or not NAME_RE.fullmatch(second):
            raise BaselineError(f"{source_path}:{lineno}: invalid identifier")
        if size_text == "-":
            size = None
        elif size_text.isdigit():
            size = int(size_text)
        else:
            raise BaselineError(f"{source_path}:{lineno}: invalid size")
        if kind == "G":
            if second != "-" or size is None or first in golden:
                raise BaselineError(f"{source_path}:{lineno}: invalid golden row")
            golden[first] = PairBaseline(size, digest)
            order.append((kind, first, second))
            continue
        if kind == "C" and size is None:
            raise BaselineError(f"{source_path}:{lineno}: home row requires a size")
        target = home if kind == "C" else foreign
        key = (first, second)
        if key in target:
            raise BaselineError(f"{source_path}:{lineno}: duplicate pair")
        target[key] = PairBaseline(size, digest)
        order.append((kind, first, second))
    return WireBaseline(home, foreign, golden, tuple(order))
