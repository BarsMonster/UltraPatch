#!/usr/bin/python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Run the canonical, commit-bound A1 release verification."""

from __future__ import annotations

from dataclasses import dataclass
import fcntl
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile
from typing import Mapping, Sequence


ROOT = Path(__file__).resolve().parent.parent
CANONICAL_PATH = "/usr/bin:/bin"
CANONICAL_HOME = "/nonexistent/ultrapatch-release-home"
CANONICAL_XDG_HOME = "/nonexistent/ultrapatch-release-xdg"
LAUNCH_CONTROL_VARS = (
    "MAKEFLAGS",
    "GNUMAKEFLAGS",
    "MAKEFILES",
    "PYTHONHOME",
    "PYTHONPATH",
)
GIT_COMMAND = (
    "/usr/bin/git",
    "-c",
    f"safe.directory={ROOT}",
    "-c",
    "core.excludesFile=/dev/null",
    "-c",
    "core.fsmonitor=false",
    "-c",
    "core.untrackedCache=false",
)
COMMIT_RE = re.compile(r"[0-9a-f]{40}")


@dataclass(frozen=True)
class ReleaseCommand:
    argv: tuple[str, ...]
    evidence: str


RELEASE_COMMANDS = (
    ReleaseCommand(
        ("/usr/bin/make", "--no-print-directory", "check-build-profile"),
        "build_profiles=OK",
    ),
    ReleaseCommand(
        ("/usr/bin/make", "--no-print-directory", "gate"),
        "RESULT                   : ALL GATES PASS",
    ),
    ReleaseCommand(
        ("/usr/bin/make", "--no-print-directory", "check-decoder-sanitize"),
        "decoder_sanitizers=OK",
    ),
    ReleaseCommand(
        ("/usr/bin/make", "--no-print-directory", "check-clang"),
        "clang_contract=OK",
    ),
)


class ReleaseError(RuntimeError):
    pass


@dataclass(frozen=True)
class RepositorySnapshot:
    branch: str
    commit: str


def validate_launch_environment(source: Mapping[str, str]) -> None:
    for name in LAUNCH_CONTROL_VARS:
        if name in source:
            raise ReleaseError(f"release driver rejects inherited {name}")


def validate_python_runtime() -> None:
    if not sys.flags.isolated or not sys.flags.no_site:
        raise ReleaseError(
            "release authority requires /usr/bin/python3 -I -S scripts/release_gate.py"
        )


def canonical_environment(source: Mapping[str, str]) -> dict[str, str]:
    return {
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_CONFIG_NOSYSTEM": "1",
        "HOME": CANONICAL_HOME,
        "LANG": "C",
        "LANGUAGE": "C",
        "LC_ALL": "C",
        "PATH": CANONICAL_PATH,
        "TZ": "UTC",
        "XDG_CONFIG_HOME": CANONICAL_XDG_HOME,
    }


def command_output(root: Path, environment: Mapping[str, str], argv: Sequence[str]) -> str:
    try:
        result = subprocess.run(
            argv,
            cwd=root,
            env=dict(environment),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise ReleaseError(f"cannot run {' '.join(argv)}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        suffix = f": {detail.splitlines()[0]}" if detail else ""
        raise ReleaseError(f"{' '.join(argv)} exited with status {result.returncode}{suffix}")
    return result.stdout.strip()


def require_normal_index(root: Path, environment: Mapping[str, str]) -> None:
    argv = (*GIT_COMMAND, "ls-files", "-v", "-z", "--")
    try:
        result = subprocess.run(
            argv,
            cwd=root,
            env=dict(environment),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise ReleaseError(f"cannot run {' '.join(argv)}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        suffix = f": {detail.splitlines()[0]}" if detail else ""
        raise ReleaseError(f"{' '.join(argv)} exited with status {result.returncode}{suffix}")
    for record in result.stdout.split(b"\0"):
        if not record:
            continue
        if not record.startswith(b"H "):
            flag = record[:1].decode("ascii", "replace")
            name = record[2:].decode("utf-8", "replace")
            raise ReleaseError(
                f"release rejects non-normal git index flag {flag!r} for {name!r}"
            )


def repository_snapshot(root: Path, environment: Mapping[str, str]) -> RepositorySnapshot:
    branch = command_output(root, environment, (*GIT_COMMAND, "branch", "--show-current"))
    if branch != "main":
        raise ReleaseError(f"release requires branch main, found {branch or 'detached HEAD'}")
    status = command_output(
        root,
        environment,
        (*GIT_COMMAND, "status", "--porcelain", "--untracked-files=all"),
    )
    if status:
        raise ReleaseError("release requires a clean working tree")
    require_normal_index(root, environment)
    commit = command_output(root, environment, (*GIT_COMMAND, "rev-parse", "HEAD"))
    if not COMMIT_RE.fullmatch(commit):
        raise ReleaseError("git returned a malformed release commit")
    return RepositorySnapshot(branch=branch, commit=commit)


def export_commit(
    root: Path,
    environment: Mapping[str, str],
    commit: str,
    destination: Path,
) -> None:
    destination.mkdir(mode=0o700)
    argv = (*GIT_COMMAND, "archive", "--format=tar", commit)
    with tempfile.TemporaryFile() as archive_file:
        try:
            result = subprocess.run(
                argv,
                cwd=root,
                env=dict(environment),
                check=False,
                stdout=archive_file,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            raise ReleaseError(f"cannot run {' '.join(argv)}: {exc}") from exc
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", "replace").strip()
            suffix = f": {detail.splitlines()[0]}" if detail else ""
            raise ReleaseError(
                f"{' '.join(argv)} exited with status {result.returncode}{suffix}"
            )
        archive_file.seek(0)
        try:
            with tarfile.open(fileobj=archive_file, mode="r:") as archive:
                members = archive.getmembers()
                for member in members:
                    name = PurePosixPath(member.name)
                    if name.is_absolute() or ".." in name.parts:
                        raise ReleaseError(
                            f"git archive contains an unsafe path: {member.name!r}"
                        )
                    if not (member.isfile() or member.isdir()):
                        raise ReleaseError(
                            f"git archive contains a non-file entry: {member.name!r}"
                        )
                archive.extractall(destination, members=members, filter="data")
        except (OSError, tarfile.TarError) as exc:
            raise ReleaseError(f"cannot extract release git archive: {exc}") from exc
    if (destination / ".git").exists() or (destination / ".build").exists():
        raise ReleaseError("release git archive contains repository metadata or build cache")


def require_expected_commit(
    source: Mapping[str, str], snapshot: RepositorySnapshot
) -> None:
    if "GITHUB_SHA" not in source:
        return
    expected = source["GITHUB_SHA"]
    if not COMMIT_RE.fullmatch(expected):
        raise ReleaseError("GITHUB_SHA is not a lowercase 40-hex commit")
    if expected != snapshot.commit:
        raise ReleaseError(
            f"GITHUB_SHA does not match release HEAD: {expected} != {snapshot.commit}"
        )


def require_unchanged(
    root: Path, environment: Mapping[str, str], expected: RepositorySnapshot
) -> None:
    actual = repository_snapshot(root, environment)
    if actual != expected:
        raise ReleaseError(
            f"release repository changed during verification: {expected.commit} -> {actual.commit}"
        )


def evidence_count(output: str, marker: str) -> int:
    return sum(
        line == marker or line.startswith(f"{marker} ")
        for line in output.splitlines()
    )


def run_evidenced_command(
    root: Path, environment: Mapping[str, str], command: ReleaseCommand
) -> str:
    try:
        process = subprocess.Popen(
            command.argv,
            cwd=root,
            env=dict(environment),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
    except OSError as exc:
        raise ReleaseError(f"cannot run {' '.join(command.argv)}: {exc}") from exc
    assert process.stdout is not None
    captured: list[str] = []
    for line in process.stdout:
        captured.append(line)
        print(line, end="", flush=True)
    returncode = process.wait()
    output = "".join(captured)
    if returncode != 0:
        raise ReleaseError(
            f"{' '.join(command.argv)} exited with status {returncode}"
        )
    count = evidence_count(output, command.evidence)
    if count != 1:
        raise ReleaseError(
            f"{' '.join(command.argv)} produced {count} copies of required evidence "
            f"{command.evidence!r}"
        )
    return output


def run_release(source_environment: Mapping[str, str]) -> RepositorySnapshot:
    validate_launch_environment(source_environment)
    environment = canonical_environment(source_environment)
    lock_path = ROOT / "test-bench/.wire-baseline-update.lock"
    try:
        lock = lock_path.open("a+b")
    except OSError as exc:
        raise ReleaseError(f"cannot open release lock {lock_path}: {exc}") from exc
    with lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_SH)
        expected = repository_snapshot(ROOT, environment)
        require_expected_commit(source_environment, expected)
        print(f"release_commit={expected.commit}", flush=True)
        with tempfile.TemporaryDirectory(prefix="ultrapatch-release-") as temporary:
            export_root = Path(temporary) / "source"
            export_commit(ROOT, environment, expected.commit, export_root)
            print(f"release_source=git-archive {expected.commit}", flush=True)
            for command in RELEASE_COMMANDS:
                print(f"release_command={' '.join(command.argv)}", flush=True)
                run_evidenced_command(export_root, environment, command)
                require_unchanged(ROOT, environment, expected)
            print(
                f"release_preflight=OK (clean main archive at {expected.commit}; selected "
                "profile + gate + sanitizers + clang evidence)",
                flush=True,
            )
        return expected


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments:
        print("usage: scripts/release_gate.py", file=sys.stderr)
        return 2
    try:
        validate_python_runtime()
        run_release(os.environ)
    except ReleaseError as exc:
        print(f"release_gate.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
