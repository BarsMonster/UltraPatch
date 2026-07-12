#!/usr/bin/python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
"""Fast negative contract for the canonical release driver."""

from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile

import release_gate


def expect_release_error(function, label):
    try:
        function()
    except release_gate.ReleaseError:
        return
    raise RuntimeError(f"release driver accepted {label}")


def git(root: Path, *arguments: str) -> None:
    result = subprocess.run(
        ("/usr/bin/git", *arguments), cwd=root, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"git {' '.join(arguments)} failed: {result.stderr.strip()}"
        )


def main() -> int:
    workflow = (release_gate.ROOT / ".github/workflows/gate.yml").read_text(
        encoding="utf-8"
    )
    if "          ref: ${{ github.sha }}\n" not in workflow:
        raise RuntimeError("CI checkout is not pinned to github.sha")
    if '          git switch -C main "$GITHUB_SHA"\n' not in workflow:
        raise RuntimeError("CI does not create local main from the pinned push SHA")
    if "ref: main" in workflow:
        raise RuntimeError("CI release checkout resolves the moving main ref")
    if "run: /usr/bin/python3 -I -S scripts/release_gate.py\n" not in workflow:
        raise RuntimeError("CI does not invoke the release authority with isolated Python")
    expect_release_error(
        release_gate.validate_python_runtime,
        "a non-isolated Python authority process",
    )
    isolated = subprocess.run(
        (
            "/usr/bin/python3",
            "-I",
            "-S",
            "-c",
            "import sys; raise SystemExit(not (sys.flags.isolated and sys.flags.no_site))",
        ),
        check=False,
    )
    if isolated.returncode != 0:
        raise RuntimeError("/usr/bin/python3 -I -S did not create an isolated runtime")

    for name, value in (
        ("MAKEFLAGS", "-i"),
        ("MAKEFLAGS", "--eval=BASE_FULL_TOTAL=999999"),
        ("GNUMAKEFLAGS", "-i"),
        ("MAKEFILES", "/tmp/injected.mk"),
        ("PYTHONHOME", "/tmp/python-home"),
        ("PYTHONPATH", "/tmp/python-path"),
        ("PYTHONPATH", ""),
    ):
        expect_release_error(
            lambda name=name, value=value: release_gate.validate_launch_environment(
                {name: value}
            ),
            f"{name}={value}",
        )

    environment = release_gate.canonical_environment({
        "HOME": "/tmp/release-home",
        "XDG_CONFIG_HOME": "/tmp/release-xdg",
        "PATH": "/tmp/poison",
        "CPATH": "/tmp/headers",
        "COMPILER_PATH": "/tmp/tools",
        "LIBRARY_PATH": "/tmp/libraries",
        "LD_PRELOAD": "/tmp/preload.so",
    })
    if environment != {
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_CONFIG_NOSYSTEM": "1",
        "HOME": release_gate.CANONICAL_HOME,
        "LANG": "C",
        "LANGUAGE": "C",
        "LC_ALL": "C",
        "PATH": release_gate.CANONICAL_PATH,
        "TZ": "UTC",
        "XDG_CONFIG_HOME": release_gate.CANONICAL_XDG_HOME,
    }:
        raise RuntimeError("release driver did not construct the canonical environment")
    if release_gate.RELEASE_COMMANDS != (
        release_gate.ReleaseCommand(
            ("/usr/bin/make", "--no-print-directory", "check-build-profile"),
            "build_profiles=OK",
        ),
        release_gate.ReleaseCommand(
            ("/usr/bin/make", "--no-print-directory", "gate"),
            "RESULT                   : ALL GATES PASS",
        ),
        release_gate.ReleaseCommand(
            ("/usr/bin/make", "--no-print-directory", "check-decoder-sanitize"),
            "decoder_sanitizers=OK",
        ),
        release_gate.ReleaseCommand(
            ("/usr/bin/make", "--no-print-directory", "check-clang"),
            "clang_contract=OK",
        ),
    ):
        raise RuntimeError("release command sequence drifted")
    safe_directory = f"safe.directory={release_gate.ROOT}"
    if release_gate.GIT_COMMAND[0] != "/usr/bin/git" or (
        release_gate.GIT_COMMAND.count(safe_directory) != 1
        or any(
            value.startswith("safe.directory=") and value != safe_directory
            for value in release_gate.GIT_COMMAND
        )
        or "core.excludesFile=/dev/null" not in release_gate.GIT_COMMAND
        or "core.fsmonitor=false" not in release_gate.GIT_COMMAND
        or "core.untrackedCache=false" not in release_gate.GIT_COMMAND
    ):
        raise RuntimeError(
            "release git command lacks the exact root safety binding or cache controls"
        )

    silent = release_gate.ReleaseCommand(("/bin/true",), "test_evidence=OK")
    expect_release_error(
        lambda: release_gate.run_evidenced_command(Path.cwd(), environment, silent),
        "a silent successful child command",
    )
    duplicate = release_gate.ReleaseCommand(
        ("/bin/sh", "-c", "printf '%s\\n' test_evidence=OK test_evidence=OK"),
        "test_evidence=OK",
    )
    with contextlib.redirect_stdout(io.StringIO()):
        expect_release_error(
            lambda: release_gate.run_evidenced_command(Path.cwd(), environment, duplicate),
            "duplicate child evidence",
        )
    witnessed = release_gate.ReleaseCommand(
        ("/bin/sh", "-c", "printf '%s\\n' test_evidence=OK"),
        "test_evidence=OK",
    )
    streamed = io.StringIO()
    with contextlib.redirect_stdout(streamed):
        captured = release_gate.run_evidenced_command(Path.cwd(), environment, witnessed)
    if captured != "test_evidence=OK\n" or streamed.getvalue() != captured:
        raise RuntimeError("release child output was not both streamed and captured")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        git(root, "init", "-q", "-b", "main")
        tracked = root / "tracked.txt"
        tracked.write_text("initial\n", encoding="utf-8")
        git(root, "add", "tracked.txt")
        git(root, "-c", "user.name=release-test", "-c",
            "user.email=release-test@example.invalid", "commit", "-q", "-m", "initial")
        clean_environment = release_gate.canonical_environment(os.environ)
        initial = release_gate.repository_snapshot(root, clean_environment)
        different_owner_environment = dict(clean_environment)
        different_owner_environment["GIT_TEST_ASSUME_DIFFERENT_OWNER"] = "1"
        expect_release_error(
            lambda: release_gate.repository_snapshot(
                root, different_owner_environment
            ),
            "a forced-owner-mismatch repository outside the exact release root",
        )
        # In a normal checkout/worktree, the sole safe.directory exception must make the exact
        # release root readable under the same forced mismatch. A git-archive release tree has no
        # .git entry by design, so there is no repository index to probe there.
        if (release_gate.ROOT / ".git").exists():
            release_gate.require_normal_index(
                release_gate.ROOT, different_owner_environment
            )
        release_gate.require_expected_commit({"GITHUB_SHA": initial.commit}, initial)
        expect_release_error(
            lambda: release_gate.require_expected_commit(
                {"GITHUB_SHA": "0" * 40}, initial
            ),
            "a GITHUB_SHA different from snapshot HEAD",
        )
        expect_release_error(
            lambda: release_gate.require_expected_commit(
                {"GITHUB_SHA": "not-a-commit"}, initial
            ),
            "a malformed GITHUB_SHA",
        )

        stale_build = root / ".build"
        stale_build.mkdir()
        (stale_build / "stale-object.o").write_bytes(b"stale\n")
        with tempfile.TemporaryDirectory() as export_temporary:
            export_root = Path(export_temporary) / "source"
            release_gate.export_commit(
                root, clean_environment, initial.commit, export_root
            )
            if not (export_root / "tracked.txt").is_file() or (
                export_root / ".build"
            ).exists():
                raise RuntimeError("git archive export included stale build state")
        (stale_build / "stale-object.o").unlink()
        stale_build.rmdir()

        git(root, "update-index", "--assume-unchanged", "tracked.txt")
        expect_release_error(
            lambda: release_gate.repository_snapshot(root, clean_environment),
            "an assume-unchanged index entry",
        )
        git(root, "update-index", "--no-assume-unchanged", "tracked.txt")
        git(root, "update-index", "--skip-worktree", "tracked.txt")
        expect_release_error(
            lambda: release_gate.repository_snapshot(root, clean_environment),
            "a skip-worktree index entry",
        )
        git(root, "update-index", "--no-skip-worktree", "tracked.txt")

        with tempfile.TemporaryDirectory() as hostile_temporary:
            hostile_home = Path(hostile_temporary)
            excludes = hostile_home / "global-excludes"
            excludes.write_text("hidden-by-global\n", encoding="utf-8")
            (hostile_home / ".gitconfig").write_text(
                f"[core]\n\texcludesFile = {excludes}\n", encoding="utf-8"
            )
            hostile_environment = release_gate.canonical_environment(
                {"HOME": str(hostile_home), "XDG_CONFIG_HOME": str(hostile_home)}
            )
            untracked = root / "hidden-by-global"
            untracked.write_text("dirty\n", encoding="utf-8")
            expect_release_error(
                lambda: release_gate.repository_snapshot(root, hostile_environment),
                "an untracked file hidden by hostile global excludes",
            )
            untracked.unlink()

        git(root, "switch", "-q", "-c", "feature")
        expect_release_error(
            lambda: release_gate.repository_snapshot(root, clean_environment),
            "a non-main branch",
        )
        git(root, "switch", "-q", "main")

        tracked.write_text("next\n", encoding="utf-8")
        git(root, "add", "tracked.txt")
        git(root, "-c", "user.name=release-test", "-c",
            "user.email=release-test@example.invalid", "commit", "-q", "-m", "next")
        expect_release_error(
            lambda: release_gate.require_unchanged(root, clean_environment, initial),
            "HEAD changing during verification",
        )

    print(
        "release_driver_contract=OK (clean commit archive + normal index; absolute "
        "make/git + per-command evidence; config scrubbed + CI SHA pinned)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
