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
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any


SCHEMA = 3
CANONICAL_RELEASE_PATH = "/usr/bin:/bin"
IMMUTABLE_OCI_RE = re.compile(r"^[^\s@]+@sha256:[0-9a-f]{64}$")
ABSOLUTE_PATH_RE = re.compile(r"(?:^|[\s=,:;(])/(?!/)[^\s,;)]*")
COMPILER_ENV_UNSET = (
    "COMPILER_PATH",
    "CPATH",
    "CPLUS_INCLUDE_PATH",
    "C_INCLUDE_PATH",
    "DEPENDENCIES_OUTPUT",
    "GCC_COMPARE_DEBUG",
    "GCC_EXEC_PREFIX",
    "GCC_SPECS",
    "LD_LIBRARY_PATH",
    "LD_PRELOAD",
    "LIBRARY_PATH",
    "OBJC_INCLUDE_PATH",
    "SOURCE_DATE_EPOCH",
    "SUNPRO_DEPENDENCIES",
    "ZERO_AR_DATE",
)


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


def compiler_environment() -> dict[str, str]:
    environment = dict(os.environ)
    for name in COMPILER_ENV_UNSET:
        environment.pop(name, None)
    environment.update({"LANG": "C", "LC_ALL": "C", "LANGUAGE": "C"})
    return environment


def canonicalize_release_environment() -> None:
    os.environ.update(
        {
            "LANG": "C",
            "LANGUAGE": "C",
            "LC_ALL": "C",
            "PATH": CANONICAL_RELEASE_PATH,
        }
    )


def environment_policy() -> dict[str, Any]:
    declared = tuple(env_words("UP_PROFILE_ENV_UNSET", reject_paths=False))
    if declared != COMPILER_ENV_UNSET:
        raise ProfileError(
            "UP_PROFILE_ENV_UNSET differs from build_profile.py compiler environment policy"
        )
    return {
        "locale": {"LANG": "C", "LANGUAGE": "C", "LC_ALL": "C"},
        "unset": list(COMPILER_ENV_UNSET),
    }


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
            env=compiler_environment(),
        )
    except OSError as exc:
        raise ProfileError(f"cannot run {purpose}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stdout.strip().splitlines()
        suffix = f": {detail[0]}" if detail else ""
        raise ProfileError(f"{purpose} exited with status {result.returncode}{suffix}")
    return result.stdout


def resolve_executable(command: str, label: str) -> Path:
    resolved = shutil.which(command, path=compiler_environment().get("PATH"))
    if resolved is None:
        raise ProfileError(f"cannot resolve {label} executable: {command}")
    try:
        path = Path(resolved).resolve(strict=True)
    except OSError as exc:
        raise ProfileError(f"cannot resolve {label} executable: {exc}") from exc
    if not path.is_file():
        raise ProfileError(f"{label} executable is not a regular file: {path}")
    return path


def command_identity(actual: list[str], display: list[str], label: str) -> dict[str, Any]:
    identity = executable_identity(actual, display, label)
    output = run(actual + ["--version"], f"{label} --version")
    lines = output.splitlines()
    if not lines or not lines[0].strip():
        raise ProfileError(f"{label} --version produced no version line")
    version = lines[0].strip()
    reject_absolute_paths([version], f"{label} version line")
    return {**identity, "version": version}


def executable_identity(
    actual: list[str], display: list[str], label: str
) -> dict[str, Any]:
    executable = resolve_executable(actual[0], label)
    return {
        "command": display,
        "executable_sha256": sha256_file(executable, f"{label} executable"),
    }


def tool_identity(name: str) -> dict[str, Any]:
    actual, display = command_words(name)
    return command_identity(actual, display, name)


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
    return {"schema": SCHEMA, "environment": environment_policy(), **host_payload()}


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


def gcc_program_identity(
    cc: list[str], scope: str, program: str, *, with_version: bool = True
) -> dict[str, Any]:
    option = f"-print-prog-name={program}"
    output = run(cc + [option], f"{scope} compiler {option}")
    lines = output.splitlines()
    if len(lines) != 1 or not lines[0].strip():
        raise ProfileError(f"{scope} compiler {option} did not return one program")
    command = lines[0].strip()
    label = f"{scope} {program}"
    if with_version:
        return command_identity([command], [Path(command).name], label)
    return executable_identity([command], [Path(command).name], label)


def release_descriptor() -> dict[str, Any]:
    host_cc, _ = command_words("UP_PROFILE_CC")
    arm_cc, _ = command_words("UP_PROFILE_ARM_CC")
    arm_source_flags = env_words("UP_PROFILE_ARM_SOURCE_FLAGS")
    return {
        "schema": SCHEMA,
        "environment": environment_policy(),
        "host": {
            **host_payload(),
            "assembler": gcc_program_identity(host_cc, "Host", "as"),
            "cc1": gcc_program_identity(host_cc, "Host", "cc1", with_version=False),
            "collect2": gcc_program_identity(host_cc, "Host", "collect2"),
            "linker": gcc_program_identity(host_cc, "Host", "ld"),
            "nm": tool_identity("UP_PROFILE_NM"),
        },
        "clang": tool_identity("UP_PROFILE_CLANG"),
        "arm": {
            "assembler": gcc_program_identity(arm_cc, "Arm", "as"),
            "cc": tool_identity("UP_PROFILE_ARM_CC"),
            "cc1": gcc_program_identity(arm_cc, "Arm", "cc1", with_version=False),
            "collect2": gcc_program_identity(arm_cc, "Arm", "collect2"),
            "compile_flags": {
                "single_header": env_words("UP_PROFILE_ARM_SINGLE_FLAGS"),
                "source_headers": arm_source_flags,
            },
            "link": {
                "flags": env_words("UP_PROFILE_ARM_LINK_FLAGS"),
                "libraries": env_words("UP_PROFILE_ARM_LINK_LIBS"),
            },
            "linker": gcc_program_identity(arm_cc, "Arm", "ld"),
            "optimization": {
                "object": env_words("UP_PROFILE_ARM_OBJECT_OPT"),
                "stack": env_words("UP_PROFILE_ARM_STACK_OPT"),
            },
            "libraries": {
                "libc.a": {
                    "sha256": arm_library_hash(
                        arm_cc, arm_source_flags, "-print-file-name=libc.a", "libc.a"
                    )
                },
                "libgcc.a": {
                    "sha256": arm_library_hash(
                        arm_cc, arm_source_flags, "-print-libgcc-file-name", "libgcc.a"
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


def read_regular_file(path: Path, label: str) -> tuple[bytes, os.stat_result]:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise ProfileError(f"cannot open {label} {path}: {exc}") from exc
    try:
        metadata = os.fstat(fd)
        if not stat.S_ISREG(metadata.st_mode):
            raise ProfileError(f"{label} must be a regular, non-symlink file: {path}")
        try:
            current = path.lstat()
        except OSError as exc:
            raise ProfileError(f"cannot inspect {label} {path}: {exc}") from exc
        if stat.S_ISLNK(current.st_mode) or (
            current.st_dev,
            current.st_ino,
        ) != (metadata.st_dev, metadata.st_ino):
            raise ProfileError(f"{label} must be a regular, non-symlink file: {path}")
        with os.fdopen(fd, "rb") as stream:
            fd = -1
            raw = stream.read()
    except OSError as exc:
        raise ProfileError(f"cannot read {label} {path}: {exc}") from exc
    finally:
        if fd >= 0:
            os.close(fd)
    return raw, metadata


def parse_lock(raw: bytes, path: Path) -> dict[str, Any]:
    try:
        value = json.loads(raw.decode("utf-8"))
    except UnicodeDecodeError as exc:
        raise ProfileError(f"release profile lock {path} is not UTF-8: {exc}") from exc
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


def load_lock(path: Path) -> dict[str, Any]:
    raw, _ = read_regular_file(path, "release profile lock")
    return parse_lock(raw, path)


def release_lock_candidate(path: Path) -> dict[str, Any]:
    lock = load_lock(path)
    return {
        "schema": SCHEMA,
        "container": lock["container"],
        "profile": release_descriptor(),
    }


def same_file_state(left: os.stat_result, right: os.stat_result) -> bool:
    return (
        left.st_dev,
        left.st_ino,
        left.st_mode,
        left.st_size,
        left.st_mtime_ns,
    ) == (
        right.st_dev,
        right.st_ino,
        right.st_mode,
        right.st_size,
        right.st_mtime_ns,
    )


def refresh_release_lock(path: Path) -> tuple[dict[str, Any], bool]:
    raw, original = read_regular_file(path, "release profile lock")
    lock = parse_lock(raw, path)
    candidate = {
        "schema": SCHEMA,
        "container": lock["container"],
        "profile": release_descriptor(),
    }
    expected = display_text(candidate).encode("ascii")
    if raw == expected:
        return candidate, False

    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            # Set the preserved mode while the temporary inode is still open, then include that
            # metadata in the same fsync that makes its contents durable before publication.
            os.fchmod(stream.fileno(), stat.S_IMODE(original.st_mode))
            stream.write(expected)
            stream.flush()
            os.fsync(stream.fileno())

        try:
            current = path.lstat()
        except OSError as exc:
            raise ProfileError(f"cannot recheck release profile lock {path}: {exc}") from exc
        if stat.S_ISLNK(current.st_mode) or not same_file_state(original, current):
            raise ProfileError(f"release profile lock changed during refresh: {path}")

        try:
            os.replace(temporary, path)
            temporary = ""
            directory_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
            directory_fd = os.open(path.parent, directory_flags)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        except OSError as exc:
            raise ProfileError(f"cannot publish release profile lock {path}: {exc}") from exc
    finally:
        if temporary:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
    return candidate, True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("host-id", help="print the current canonical host profile SHA-256")
    ensure = sub.add_parser("ensure-host", help="atomically create or compare a host manifest")
    ensure.add_argument("path", type=Path)
    sub.add_parser("release-json", help="print the current canonical release descriptor")
    lock_json = sub.add_parser(
        "release-lock-json",
        help="print a full release lock while preserving its immutable container",
    )
    lock_json.add_argument("lock", type=Path)
    refresh = sub.add_parser(
        "refresh-release", help="atomically refresh a full release profile lock"
    )
    refresh.add_argument("lock", type=Path)
    verify = sub.add_parser("verify-release", help="verify an immutable release profile lock")
    verify.add_argument("lock", type=Path)
    args = parser.parse_args()

    try:
        if args.command in {
            "release-json",
            "release-lock-json",
            "refresh-release",
            "verify-release",
        }:
            canonicalize_release_environment()
        if args.command == "host-id":
            print(profile_id(host_descriptor()))
        elif args.command == "ensure-host":
            descriptor = host_descriptor()
            ensure_host(args.path, descriptor)
            print(f"host_profile={profile_id(descriptor)}")
        elif args.command == "release-json":
            print(display_text(release_descriptor()), end="")
        elif args.command == "release-lock-json":
            print(display_text(release_lock_candidate(args.lock)), end="")
        elif args.command == "refresh-release":
            lock, changed = refresh_release_lock(args.lock)
            state = "updated" if changed else "unchanged"
            print(f"release_profile_update={state} (full schema-{SCHEMA} lock)")
            print(f"release_profile={profile_id(lock)}")
        elif args.command == "verify-release":
            expected = release_descriptor()
            lock = load_lock(args.lock)
            if lock["profile"] != expected:
                detail = diff_values(
                    lock["profile"], expected, str(args.lock), "current release profile"
                )
                raise ProfileError(f"release profile mismatch\n{detail.rstrip()}")
            print(f"release_profile={profile_id(lock)}")
    except ProfileError as exc:
        print(f"build_profile.py: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
