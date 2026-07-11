#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Canonical build identities and release-toolchain verification."""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
from typing import Any


SCHEMA = 2
IMMUTABLE_OCI_RE = re.compile(r"^[^\s@]+@sha256:[0-9a-f]{64}$")
ABSOLUTE_PATH_RE = re.compile(r"(?:^|[\s=,:;(])/(?!/)[^\s,;)]*")


class ProfileError(RuntimeError):
    pass


def env_words(name: str, *, reject_paths: bool = True) -> list[str]:
    if name not in os.environ:
        raise ProfileError(f"required environment variable {name} is not set")
    try:
        words = shlex.split(os.environ[name], posix=True)
    except ValueError as exc:
        raise ProfileError(f"cannot parse {name}: {exc}") from exc
    if reject_paths:
        reject_absolute_paths(words, name)
    return words


def reject_absolute_paths(words: list[str], source: str) -> None:
    for word in words:
        candidates = [word]
        for prefix in ("-I", "-L", "-B"):
            if word.startswith(prefix) and len(word) > len(prefix):
                candidates.append(word[len(prefix) :])
        if "=" in word:
            candidates.append(word.split("=", 1)[1])
        if ",/" in word:
            candidates.append(word[word.index(",") + 1 :])
        if any(os.path.isabs(candidate) for candidate in candidates) or ABSOLUTE_PATH_RE.search(word):
            raise ProfileError(f"{source} contains an absolute path: {word!r}")


def command_words(name: str) -> tuple[list[str], list[str]]:
    actual = env_words(name, reject_paths=False)
    if not actual:
        raise ProfileError(f"{name} is empty")
    reject_absolute_paths(actual[1:], name)
    display = list(actual)
    display[0] = Path(display[0]).name
    if not display[0]:
        raise ProfileError(f"{name} has an invalid executable")
    return actual, display


def run(argv: list[str], purpose: str) -> str:
    try:
        result = subprocess.run(
            argv,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            env={**os.environ, "LC_ALL": "C"},
        )
    except OSError as exc:
        raise ProfileError(f"cannot run {purpose}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stdout.strip().splitlines()
        suffix = f": {detail[0]}" if detail else ""
        raise ProfileError(f"{purpose} exited with status {result.returncode}{suffix}")
    return result.stdout


def tool_identity(name: str) -> dict[str, Any]:
    actual, display = command_words(name)
    output = run(actual + ["--version"], f"{name} --version")
    lines = output.splitlines()
    if not lines or not lines[0].strip():
        raise ProfileError(f"{name} --version produced no version line")
    version = lines[0].strip()
    reject_absolute_paths([version], f"{name} version line")
    return {"command": display, "version": version}


def host_payload() -> dict[str, Any]:
    return {
        "compiler": tool_identity("UP_PROFILE_CC"),
        "flags": {
            "encoder_cflags": env_words("UP_PROFILE_ENCODER_CFLAGS"),
            "backend_cflags": env_words("UP_PROFILE_BACKEND_CFLAGS"),
            "link_driver_flags": env_words("UP_PROFILE_LINK_CFLAGS"),
            "decoder_config": env_words("UP_PROFILE_DECODER_FLAGS"),
            "ldflags": env_words("UP_PROFILE_LDFLAGS"),
            "wire_config": env_words("UP_PROFILE_WIRE_FLAGS"),
        },
    }


def host_descriptor() -> dict[str, Any]:
    return {"schema": SCHEMA, **host_payload()}


def sha256_file(path: Path, label: str) -> str:
    if not path.is_file():
        raise ProfileError(f"{label} was not found: {path}")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ProfileError(f"cannot hash {label}: {exc}") from exc
    return digest.hexdigest()


def arm_library_hash(cc: list[str], flags: list[str], option: str, label: str) -> str:
    output = run(cc + flags + [option], f"UP_PROFILE_ARM_CC {option}")
    lines = output.splitlines()
    if len(lines) != 1 or not lines[0].strip():
        raise ProfileError(f"UP_PROFILE_ARM_CC {option} did not return one library path")
    value = lines[0].strip()
    if value == label:
        raise ProfileError(f"UP_PROFILE_ARM_CC could not resolve {label}")
    return sha256_file(Path(value), label)


def release_descriptor() -> dict[str, Any]:
    arm_cc, _ = command_words("UP_PROFILE_ARM_CC")
    arm_flags = env_words("UP_PROFILE_ARM_FLAGS")
    return {
        "schema": SCHEMA,
        "host": host_payload(),
        "clang": tool_identity("UP_PROFILE_CLANG"),
        "arm": {
            "cc": tool_identity("UP_PROFILE_ARM_CC"),
            "flags": arm_flags,
            "optimization": {
                "object": env_words("UP_PROFILE_ARM_OBJECT_OPT"),
                "stack": env_words("UP_PROFILE_ARM_STACK_OPT"),
            },
            "libraries": {
                "libc.a": {
                    "sha256": arm_library_hash(
                        arm_cc, arm_flags, "-print-file-name=libc.a", "libc.a"
                    )
                },
                "libgcc.a": {
                    "sha256": arm_library_hash(
                        arm_cc, arm_flags, "-print-libgcc-file-name", "libgcc.a"
                    )
                },
            },
            "nm": tool_identity("UP_PROFILE_ARM_NM"),
            "objdump": tool_identity("UP_PROFILE_ARM_OBJDUMP"),
            "size": tool_identity("UP_PROFILE_ARM_SIZE"),
        },
    }


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def display_text(value: Any) -> str:
    return json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n"


def profile_id(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def diff_values(old: Any, new: Any, old_name: str, new_name: str) -> str:
    return "".join(
        difflib.unified_diff(
            display_text(old).splitlines(keepends=True),
            display_text(new).splitlines(keepends=True),
            fromfile=old_name,
            tofile=new_name,
        )
    )


def ensure_host(path: Path, descriptor: dict[str, Any]) -> None:
    expected = display_text(descriptor).encode("ascii")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(expected)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        try:
            os.link(temporary, path)
        except FileExistsError:
            try:
                actual = path.read_bytes()
            except OSError as exc:
                raise ProfileError(f"cannot read existing host profile {path}: {exc}") from exc
            if actual != expected:
                try:
                    parsed = json.loads(actual)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    parsed = {"invalid_manifest_text": actual.decode("utf-8", "replace")}
                detail = diff_values(parsed, descriptor, str(path), "current host profile")
                raise ProfileError(f"host profile mismatch for {path}\n{detail.rstrip()}")
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def load_lock(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ProfileError(f"cannot read release profile lock {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ProfileError(f"invalid JSON in release profile lock {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ProfileError("release profile lock must be a JSON object")
    if set(value) != {"schema", "container", "profile"} or value["schema"] != SCHEMA:
        raise ProfileError(
            f"release profile lock must contain exactly schema={SCHEMA}, container, and profile"
        )
    container = value["container"]
    if not isinstance(container, str) or not IMMUTABLE_OCI_RE.fullmatch(container):
        raise ProfileError("release profile container must use an immutable @sha256:<64hex> ref")
    if not isinstance(value["profile"], dict):
        raise ProfileError("release profile lock 'profile' must be a JSON object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("host-id", help="print the current canonical host profile SHA-256")
    ensure = sub.add_parser("ensure-host", help="atomically create or compare a host manifest")
    ensure.add_argument("path", type=Path)
    sub.add_parser("release-json", help="print the current canonical release descriptor")
    verify = sub.add_parser("verify-release", help="verify an immutable release profile lock")
    verify.add_argument("lock", type=Path)
    args = parser.parse_args()

    try:
        if args.command == "host-id":
            print(profile_id(host_descriptor()))
        elif args.command == "ensure-host":
            descriptor = host_descriptor()
            ensure_host(args.path, descriptor)
            print(f"host_profile={profile_id(descriptor)}")
        elif args.command == "release-json":
            print(display_text(release_descriptor()), end="")
        elif args.command == "verify-release":
            expected = release_descriptor()
            lock = load_lock(args.lock)
            if lock["profile"] != expected:
                detail = diff_values(
                    lock["profile"], expected, str(args.lock), "current release profile"
                )
                raise ProfileError(f"release profile mismatch\n{detail.rstrip()}")
            print(f"release_profile={profile_id(expected)}")
    except ProfileError as exc:
        print(f"build_profile.py: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
