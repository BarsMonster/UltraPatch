#!/usr/bin/python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Run the commit-bound UltraPatch release verification."""

from __future__ import annotations

from dataclasses import dataclass
import fcntl
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parent.parent
COMMIT_RE = re.compile(r"[0-9a-f]{40}")


@dataclass(frozen=True)
class ReleaseCommand:
    argv: tuple[str, ...]
    evidence: str


RELEASE_COMMANDS = (
    ReleaseCommand(("/usr/bin/make", "--no-print-directory", "check-build-profile"),
                   "build_profiles=OK"),
    ReleaseCommand(("/usr/bin/make", "--no-print-directory", "gate"),
                   "RESULT                   : ALL GATES PASS"),
    ReleaseCommand(("/usr/bin/make", "--no-print-directory", "check-decoder-sanitize"),
                   "decoder_sanitizers=OK"),
    ReleaseCommand(("/usr/bin/make", "--no-print-directory", "check-encoder-sanitize"),
                   "encoder_sanitizers=OK"),
    ReleaseCommand(("/usr/bin/make", "--no-print-directory", "check-clang"),
                   "clang_contract=OK"),
)


class ReleaseError(RuntimeError):
    pass


def release_environment() -> dict[str, str]:
    return {
        "GIT_CONFIG_COUNT": "1",
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_CONFIG_KEY_0": "safe.directory",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_VALUE_0": str(ROOT),
        "HOME": "/nonexistent/ultrapatch-release-home",
        "LANG": "C",
        "LANGUAGE": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "TZ": "UTC",
    }


def output(root: Path, env: dict[str, str], *argv: str) -> str:
    result = subprocess.run(
        argv, cwd=root, env=env, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace"
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ReleaseError(
            f"{' '.join(argv)} exited with status {result.returncode}"
            + (f": {detail.splitlines()[0]}" if detail else "")
        )
    return result.stdout.strip()


def snapshot(env: dict[str, str]) -> str:
    branch = output(ROOT, env, "/usr/bin/git", "branch", "--show-current")
    if branch != "main":
        raise ReleaseError(f"release requires branch main, found {branch or 'detached HEAD'}")
    if output(ROOT, env, "/usr/bin/git", "status", "--porcelain", "--untracked-files=all"):
        raise ReleaseError("release requires a clean working tree")
    commit = output(ROOT, env, "/usr/bin/git", "rev-parse", "HEAD")
    if not COMMIT_RE.fullmatch(commit):
        raise ReleaseError("git returned a malformed release commit")
    expected = os.environ.get("GITHUB_SHA")
    if expected is not None and expected != commit:
        raise ReleaseError(f"GITHUB_SHA does not match release HEAD: {expected} != {commit}")
    return commit


def export_commit(env: dict[str, str], commit: str, destination: Path) -> None:
    destination.mkdir(mode=0o700)
    with tempfile.TemporaryFile() as stream:
        result = subprocess.run(
            ("/usr/bin/git", "archive", "--format=tar", commit), cwd=ROOT,
            env=env, check=False, stdout=stream, stderr=subprocess.PIPE
        )
        if result.returncode:
            detail = result.stderr.decode("utf-8", "replace").strip()
            raise ReleaseError(f"git archive failed" + (f": {detail}" if detail else ""))
        stream.seek(0)
        try:
            with tarfile.open(fileobj=stream, mode="r:") as archive:
                archive.extractall(destination, filter="data")
        except (OSError, tarfile.TarError) as exc:
            raise ReleaseError(f"cannot extract release archive: {exc}") from exc


def run_command(root: Path, env: dict[str, str], command: ReleaseCommand) -> None:
    process = subprocess.Popen(
        command.argv, cwd=root, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        bufsize=1
    )
    assert process.stdout is not None
    lines = []
    for line in process.stdout:
        lines.append(line)
        print(line, end="", flush=True)
    if process.wait():
        raise ReleaseError(f"{' '.join(command.argv)} failed")
    if sum(line.rstrip("\n").startswith(command.evidence) for line in lines) != 1:
        raise ReleaseError(
            f"{' '.join(command.argv)} did not produce exactly one {command.evidence!r}"
        )


def run_release() -> str:
    env = release_environment()
    lock_path = ROOT / "test-bench/.wire-baseline-update.lock"
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_SH)
        commit = snapshot(env)
        print(f"release_commit={commit}", flush=True)
        with tempfile.TemporaryDirectory(prefix="ultrapatch-release-") as temporary:
            source = Path(temporary) / "source"
            export_commit(env, commit, source)
            print(f"release_source=git-archive {commit}", flush=True)
            for command in RELEASE_COMMANDS:
                print(f"release_command={' '.join(command.argv)}", flush=True)
                run_command(source, env, command)
        if snapshot(env) != commit:
            raise ReleaseError("release repository changed during verification")
        print(
            f"release_preflight=OK (clean main archive at {commit}; selected profile + "
            "gate + decoder/encoder sanitizers + clang evidence)", flush=True
        )
        return commit


def main() -> int:
    if len(sys.argv) != 1:
        print("usage: scripts/release_gate.py", file=sys.stderr)
        return 2
    try:
        run_release()
    except (OSError, ReleaseError) as exc:
        print(f"release_gate.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
