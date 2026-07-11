#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Publish the canonical host-tool build recipe without disturbing no-op mtimes."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import sys
import tempfile


def words(name: str) -> list[str]:
    value = os.environ.get(name)
    if value is None:
        raise SystemExit(f"write_build_recipe.py: required environment variable {name} is not set")
    try:
        return shlex.split(value, posix=True)
    except ValueError as exc:
        raise SystemExit(f"write_build_recipe.py: cannot parse {name}: {exc}") from exc


def text(name: str) -> str:
    value = os.environ.get(name)
    if value is None or not value:
        raise SystemExit(f"write_build_recipe.py: required environment variable {name} is empty")
    return value


def recipe() -> dict[str, object]:
    encoder_sources = words("UP_BUILD_ENCODER_SOURCES")
    backend_sources = words("UP_BUILD_BACKEND_SOURCES")
    objects = words("UP_BUILD_OBJECTS")
    if not encoder_sources or not backend_sources or not objects:
        raise SystemExit("write_build_recipe.py: source and object lists must not be empty")
    return {
        "schema": 1,
        "recipe": text("UP_BUILD_RECIPE_TAG"),
        "compiler": words("UP_BUILD_CC"),
        "encoder": {
            "sources": encoder_sources,
            "flags": words("UP_BUILD_ENCODER_FLAGS"),
        },
        "decoder_backend": {
            "sources": backend_sources,
            "flags": words("UP_BUILD_BACKEND_FLAGS"),
        },
        "link": {
            "objects": objects,
            "driver_flags": words("UP_BUILD_LINK_FLAGS"),
            "ldflags": words("UP_BUILD_LDFLAGS"),
        },
    }


def publish(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if path.read_bytes() == content:
            return
    except FileNotFoundError:
        pass
    except OSError as exc:
        raise SystemExit(f"write_build_recipe.py: cannot read {path}: {exc}") from exc

    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: write_build_recipe.py OUTPUT", file=sys.stderr)
        return 2
    content = (json.dumps(recipe(), sort_keys=True, indent=2) + "\n").encode("ascii")
    publish(Path(sys.argv[1]), content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
