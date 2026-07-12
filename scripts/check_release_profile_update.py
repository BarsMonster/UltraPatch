#!/usr/bin/python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Regression for full-lock refresh and selected GCC subtool identities."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import tempfile
from unittest import mock

import build_profile


ROOT = Path(__file__).resolve().parent.parent
PYTHON = "/usr/bin/python3"


def run_profile(
    *arguments: str,
    check: bool = True,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        (PYTHON, "-I", "-S", "scripts/build_profile.py", *arguments),
        cwd=ROOT,
        env=os.environ if environment is None else environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"build_profile.py {' '.join(arguments)} failed: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return result


def file_state(path: Path) -> tuple[int, int, int, int]:
    metadata = path.lstat()
    return (
        metadata.st_ino,
        metadata.st_mtime_ns,
        stat.S_IMODE(metadata.st_mode),
        metadata.st_size,
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compiler_program(driver_name: str, program: str) -> tuple[str, Path, str | None]:
    driver = shlex.split(os.environ[driver_name], posix=True)
    result = subprocess.run(
        (*driver, f"-print-prog-name={program}"),
        env=build_profile.compiler_environment(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    lines = result.stdout.splitlines()
    if result.returncode != 0 or len(lines) != 1 or not lines[0].strip():
        raise RuntimeError(f"cannot independently resolve {driver_name} {program}")
    command = lines[0].strip()
    resolved = shutil.which(command, path=build_profile.compiler_environment().get("PATH"))
    if resolved is None:
        raise RuntimeError(f"cannot independently locate {driver_name} {program}: {command}")
    executable = Path(resolved).resolve(strict=True)
    version_result = subprocess.run(
        (command, "--version"),
        env=build_profile.compiler_environment(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    version_lines = version_result.stdout.splitlines()
    version = version_lines[0].strip() if version_lines and version_lines[0].strip() else None
    return command, executable, version


def verify_subtools(lock: dict[str, object]) -> None:
    profile = lock["profile"]
    assert isinstance(profile, dict)
    scopes = (
        ("UP_PROFILE_CC", profile["host"]),
        ("UP_PROFILE_ARM_CC", profile["arm"]),
    )
    keys = (("as", "assembler"), ("cc1", "cc1"), ("collect2", "collect2"), ("ld", "linker"))
    for driver_name, descriptor in scopes:
        assert isinstance(descriptor, dict)
        for program, key in keys:
            command, executable, version = compiler_program(driver_name, program)
            identity = descriptor[key]
            assert isinstance(identity, dict)
            if identity.get("command") != [Path(command).name]:
                raise RuntimeError(f"{driver_name} {program} command is not profile-bound")
            if identity.get("executable_sha256") != sha256(executable):
                raise RuntimeError(f"{driver_name} {program} executable hash is not profile-bound")
            if program == "cc1":
                if "version" in identity:
                    raise RuntimeError(f"{driver_name} cc1 must be hash-only")
            elif version is None or identity.get("version") != version:
                raise RuntimeError(f"{driver_name} {program} version is not profile-bound")


def main() -> int:
    canonical_path = Path(os.environ.get(
        "RELEASE_PROFILE_LOCK", "toolchains/release-profile.json"
    ))
    if not canonical_path.is_absolute():
        canonical_path = ROOT / canonical_path
    canonical = build_profile.load_lock(canonical_path)

    emitted = run_profile("release-lock-json", str(canonical_path)).stdout
    if emitted != build_profile.display_text(canonical):
        raise RuntimeError("release-lock-json did not emit the full canonical lock")
    verify_subtools(canonical)

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        lock_path = directory / "release-profile.json"
        stale = {
            "schema": build_profile.SCHEMA,
            "container": canonical["container"],
            "profile": {"stale": True},
        }
        lock_path.write_text(build_profile.display_text(stale), encoding="ascii")
        lock_path.chmod(0o640)
        before = file_state(lock_path)

        refreshed = run_profile("refresh-release", str(lock_path))
        if "release_profile_update=updated (full schema-3 lock)\n" not in refreshed.stdout:
            raise RuntimeError("refresh did not report a full-lock update")
        after = file_state(lock_path)
        updated = json.loads(lock_path.read_text(encoding="utf-8"))
        if updated["container"] != canonical["container"]:
            raise RuntimeError("refresh changed the immutable container wrapper")
        if updated["profile"] != build_profile.release_descriptor():
            raise RuntimeError("refresh did not publish the current release descriptor")
        if before[0] == after[0] or after[2] != before[2]:
            raise RuntimeError("refresh was not an atomic, mode-preserving replacement")

        durable_path = directory / "durable-mode.json"
        durable_path.write_text(build_profile.display_text(stale), encoding="ascii")
        durable_path.chmod(0o751)
        regular_modes_at_fsync: list[int] = []
        real_fsync = os.fsync

        def record_fsync(fd: int) -> None:
            metadata = os.fstat(fd)
            if stat.S_ISREG(metadata.st_mode):
                regular_modes_at_fsync.append(stat.S_IMODE(metadata.st_mode))
            real_fsync(fd)

        with mock.patch.object(build_profile.os, "fsync", side_effect=record_fsync):
            _, changed = build_profile.refresh_release_lock(durable_path)
        if not changed or regular_modes_at_fsync != [0o751]:
            raise RuntimeError("preserved lock mode was not set before the file fsync")

        stable_before = file_state(lock_path)
        unchanged = run_profile("refresh-release", str(lock_path))
        stable_after = file_state(lock_path)
        if "release_profile_update=unchanged (full schema-3 lock)\n" not in unchanged.stdout:
            raise RuntimeError("second refresh did not report an exact no-op")
        if stable_after != stable_before:
            raise RuntimeError("exact no-op changed lock inode, mtime, mode, or size")

        invalid_path = directory / "invalid.json"
        invalid = dict(stale)
        invalid["container"] = "ubuntu:26.04"
        invalid_path.write_text(build_profile.display_text(invalid), encoding="ascii")
        invalid_before = invalid_path.read_bytes()
        rejected = run_profile("refresh-release", str(invalid_path), check=False)
        if rejected.returncode == 0 or invalid_path.read_bytes() != invalid_before:
            raise RuntimeError("refresh accepted or changed a mutable container wrapper")

        target_path = directory / "target.json"
        target_path.write_text(build_profile.display_text(stale), encoding="ascii")
        target_before = target_path.read_bytes()
        symlink_path = directory / "symlink.json"
        symlink_path.symlink_to(target_path)
        rejected = run_profile("refresh-release", str(symlink_path), check=False)
        if rejected.returncode == 0 or target_path.read_bytes() != target_before:
            raise RuntimeError("refresh followed a release-lock symlink")

        poison = directory / "poison-path"
        poison.mkdir()
        marker = directory / "poison-executed"
        for tool in (
            "arm-none-eabi-gcc",
            "arm-none-eabi-nm",
            "arm-none-eabi-objdump",
            "arm-none-eabi-size",
            "clang",
            "flock",
            "gcc",
            "nm",
            "python3",
            "timeout",
        ):
            proxy = poison / tool
            proxy.write_text(
                "#!/bin/sh\nprintf '%s\\n' poison >>\"$PROXY_MARKER\"\nexit 97\n",
                encoding="ascii",
            )
            proxy.chmod(0o755)
        poison_lock = directory / "poison-lock.json"
        poison_lock.write_text(build_profile.display_text(stale), encoding="ascii")
        poison_environment = dict(os.environ)
        poison_environment.update(
            {
                "PATH": f"{poison}:{build_profile.CANONICAL_RELEASE_PATH}",
                "PROXY_MARKER": str(marker),
            }
        )
        run_profile(
            "refresh-release",
            str(poison_lock),
            environment=poison_environment,
        )
        poison_updated = json.loads(poison_lock.read_text(encoding="utf-8"))
        if marker.exists() or poison_updated != canonical:
            raise RuntimeError("release refresh used a PATH proxy or published profile drift")

    print(
        "release_profile_update_contract=OK (full schema-3 lock + selected GCC "
        "subtools; canonical PATH; mode-before-fsync atomic refresh; exact no-op)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
