#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

"""Fast synthetic policy, transaction, interruption, and concurrency regression."""

import argparse
import fcntl
import hashlib
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path


SCRIPT = Path(__file__).resolve().with_name("publish_wire_baselines.py")
FILES = ("golden.sha256", "home-size-baseline.tsv", "corpus-wire.sha256")
PROFILE_RE = re.compile(r"release_profile=([0-9a-f]{64})\n?$")


def die(message):
    raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def hash_char(value):
    return value * 64


def make_repo(parent):
    repo = parent / "repo"
    root = repo / "test-bench"
    root.mkdir(parents=True)
    write(root / ".wire-baseline-update-test-root", "synthetic only\n")
    write(repo / "Makefile", """# synthetic update-policy fixture
BASE_FULL_TOTAL ?= 40
BASE_FOREIGN_TOTAL ?= 20
BASE_ONEFACE_GROW ?= 10
BASE_ONEFACE_REVERT ?= 9
unrelated := byte-identical
""")
    write(root / "release-inventory.tsv", """fixture v0_base
fixture v1_one_face
home img_00_a
home img_01_b
foreign 1.0
foreign 2.0
""")
    home = [("img_00_a", "img_00_a"), ("img_00_a", "img_01_b"),
            ("img_01_b", "img_00_a"), ("img_01_b", "img_01_b")]
    write(root / "home-size-baseline.tsv",
          "".join("%s\t%s\t10\n" % pair for pair in home))
    wire = [("C", "img_00_a", "img_00_a"),
            ("C", "img_00_a", "img_01_b"),
            ("C", "img_01_b", "img_00_a"),
            ("C", "img_01_b", "img_01_b"),
            ("F", "1.0", "2.0"), ("F", "2.0", "1.0")]
    write(root / "corpus-wire.sha256",
          "".join("%s\t%s\t%s\t%s\n" % (tag, source, target, hash_char("a"))
                  for tag, source, target in wire))
    write(root / "golden.sha256", """%s 10 img00_to_img01.blob
%s 10 oneface_grow.blob
%s 9 oneface_revert.blob
%s 7 synth_pin.blob
""" % (hash_char("a"), hash_char("b"), hash_char("c"), hash_char("d")))
    return repo, root


def make_candidate(repo, name, profile, tool_hash, sizes=None, foreign=20,
                   grow=10, revert=9, digest_char="a", metrics_override=None,
                   reverse_home=False):
    candidate = repo / name
    candidate.mkdir()
    pairs = [("img_00_a", "img_00_a"), ("img_00_a", "img_01_b"),
             ("img_01_b", "img_00_a"), ("img_01_b", "img_01_b")]
    values = list(sizes if sizes is not None else (10, 10, 10, 10))
    home_rows = list(zip(pairs, values))
    if reverse_home:
        home_rows.reverse()
    write(candidate / "home-size-baseline.tsv",
          "".join("%s\t%s\t%d\n" % (pair[0], pair[1], value)
                  for pair, value in home_rows))
    wire = [("C", "img_00_a", "img_00_a"),
            ("C", "img_00_a", "img_01_b"),
            ("C", "img_01_b", "img_00_a"),
            ("C", "img_01_b", "img_01_b"),
            ("F", "1.0", "2.0"), ("F", "2.0", "1.0")]
    digest_value = hash_char(digest_char)
    write(candidate / "corpus-wire.sha256",
          "".join("%s\t%s\t%s\t%s\n" % (tag, source, target, digest_value)
                  for tag, source, target in wire))
    write(candidate / "golden.sha256", """%s %d img00_to_img01.blob
%s %d oneface_grow.blob
%s %d oneface_revert.blob
%s 7 synth_pin.blob
""" % (digest_value, values[1], hash_char("e") if digest_char != "a" else hash_char("b"),
         grow, hash_char("f") if digest_char != "a" else hash_char("c"), revert,
         hash_char("1") if digest_char != "a" else hash_char("d")))
    metrics = {
        "matrix_ok": "4/4", "full_total": str(sum(values)),
        "home_size_better": "NA", "home_size_worse": "NA", "home_size_equal": "NA",
        "foreign_ok": "2/2", "foreign_total": str(foreign), "wire_identity": "NA/6",
        "max_journal": "3", "max_amplified": "0", "max_maxpageerase": "1",
        "max_inversions": "0", "max_unaligned": "0", "max_oob_page_writes": "0",
        "max_canary_corrupt": "0", "measurement_release_profile": profile,
        "measurement_host_tool_sha256": tool_hash,
        "measurement_preimage_golden_sha256": sha(repo / "test-bench/golden.sha256"),
        "measurement_preimage_home_sha256": sha(repo / "test-bench/home-size-baseline.tsv"),
        "measurement_preimage_wire_sha256": sha(repo / "test-bench/corpus-wire.sha256"),
        "measurement_preimage_makefile_sha256": sha(repo / "Makefile"),
    }
    if metrics_override:
        metrics.update(metrics_override)
    write(candidate / "metrics.txt",
          "".join("%s=%s\n" % item for item in metrics.items()))
    return candidate


def snapshot(repo, root):
    paths = [root / name for name in FILES] + [repo / "Makefile"]
    return {str(path): path.read_bytes() for path in paths}


def mtimes(repo, root):
    paths = [root / name for name in FILES] + [repo / "Makefile"]
    return {str(path): path.stat().st_mtime_ns for path in paths}


def command(root, candidate, host_tool, profile_lock, limits=(40, 20, 10, 9), extra=None):
    argv = [sys.executable, str(SCRIPT), "publish", "--root", str(root),
            "--inventory", str(root / "release-inventory.tsv"),
            "--candidate-golden", str(candidate / "golden.sha256"),
            "--candidate-home-sizes", str(candidate / "home-size-baseline.tsv"),
            "--candidate-wire", str(candidate / "corpus-wire.sha256"),
            "--metrics", str(candidate / "metrics.txt"),
            "--host-tool", str(host_tool), "--release-profile-lock", str(profile_lock),
            "--home-limit", str(limits[0]), "--foreign-limit", str(limits[1]),
            "--oneface-grow-limit", str(limits[2]),
            "--oneface-revert-limit", str(limits[3])]
    if extra:
        argv.extend(extra)
    return argv


def run(argv, success=True):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding="utf-8", errors="replace")
    if success and result.returncode != 0:
        die("command failed (%d): %s" % (result.returncode, result.stderr.strip()))
    if not success and result.returncode == 0:
        die("command unexpectedly succeeded: %s" % " ".join(argv))
    return result


def assert_clean(root):
    if (root / ".wire-baseline-update.transaction").exists():
        die("transaction state leaked")
    leaked = [entry for entry in root.iterdir()
              if entry.name.startswith((".wire-baseline-update.prepare.",
                                        ".wire-baseline-update.done.",
                                        ".wire-baseline-update.recover."))]
    if leaked:
        die("transaction temporary paths leaked: %r" % leaked)


def recover(root, extra=None, success=True):
    argv = [sys.executable, str(SCRIPT), "recover", "--root", str(root)]
    if extra:
        argv.extend(extra)
    return run(argv, success=success)


def assert_reader(root, success=True):
    return run([sys.executable, str(SCRIPT), "assert-clean", "--root", str(root)],
               success=success)


def fresh(base, label):
    directory = base / label
    directory.mkdir()
    return make_repo(directory)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-tool", required=True, type=Path)
    parser.add_argument("--release-profile-lock", required=True, type=Path)
    args = parser.parse_args()
    host_tool = args.host_tool.resolve()
    profile_lock = args.release_profile_lock.resolve()
    if not host_tool.is_file() or not profile_lock.is_file():
        die("test requires the built host tool and release profile lock")
    profile_result = subprocess.run(
        [sys.executable, str(SCRIPT.with_name("build_profile.py")),
         "verify-release", str(profile_lock)], check=True, stdout=subprocess.PIPE,
        text=True, encoding="utf-8")
    match = PROFILE_RE.fullmatch(profile_result.stdout)
    if not match:
        die("release profile helper returned malformed evidence")
    profile = match.group(1)
    tool_hash = sha(host_tool)

    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)

        # Exercise the publisher's streaming digest across more than one chunk and compare it to
        # the independent hashlib implementation used for measurement evidence.
        spec = importlib.util.spec_from_file_location("wire_publisher_under_test", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        digest_probe = base / "digest-probe"
        digest_probe.write_bytes((b"0123456789abcdef" * 70000) + b"tail")
        if module.digest(digest_probe) != hashlib.sha256(digest_probe.read_bytes()).hexdigest():
            die("publisher streaming digest disagrees with hashlib")

        cleanup_root = base / "cleanup" / "test-bench"
        cleanup_root.mkdir(parents=True)
        active = cleanup_root / module.STATE_NAME
        active.mkdir()
        real_rmtree = module.shutil.rmtree

        class NoFault:
            @staticmethod
            def hit(_point):
                pass

        def reject_garbage_delete(path, *args, **kwargs):
            if Path(path).name.startswith(module.DONE_PREFIX):
                raise OSError("injected garbage cleanup failure")
            return real_rmtree(path, *args, **kwargs)

        module.shutil.rmtree = reject_garbage_delete
        try:
            module.remove_state(cleanup_root, active, NoFault())
        finally:
            module.shutil.rmtree = real_rmtree
        if active.exists() or not any(
                entry.name.startswith(module.DONE_PREFIX) for entry in cleanup_root.iterdir()):
            die("post-decision garbage failure did not retain retired state harmlessly")
        module.cleanup_stale_files(cleanup_root)

        # An exact measurement is a true no-op: no canonical content or mtime changes.
        repo, root = fresh(base, "noop")
        candidate = make_candidate(repo, "candidate", profile, tool_hash)
        before = snapshot(repo, root)
        before_times = mtimes(repo, root)
        result = run(command(root, candidate, host_tool, profile_lock))
        if "wire_baseline_transaction=NOOP" not in result.stdout:
            die("identical publication did not report NOOP")
        if snapshot(repo, root) != before or mtimes(repo, root) != before_times:
            die("identical publication changed canonical bytes or mtimes")
        assert_clean(root)

        # A real improvement publishes manifests and all four ratchets coherently.
        repo, root = fresh(base, "success")
        os.chmod(root / "golden.sha256", 0o444)
        candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                   sizes=(9, 9, 9, 9), foreign=19,
                                   grow=9, revert=8, digest_char="2")
        result = run(command(root, candidate, host_tool, profile_lock))
        if "wire_baseline_transaction=COMMITTED" not in result.stdout:
            die("improved publication did not commit")
        for name in FILES:
            if (root / name).read_bytes() != (candidate / name).read_bytes():
                die("published %s differs from its candidate" % name)
        if (root / "golden.sha256").stat().st_mode & 0o777 != 0o444:
            die("publication did not preserve canonical file mode")
        make_text = (repo / "Makefile").read_text(encoding="utf-8")
        for line in ("BASE_FULL_TOTAL ?= 36", "BASE_FOREIGN_TOTAL ?= 19",
                     "BASE_ONEFACE_GROW ?= 9", "BASE_ONEFACE_REVERT ?= 8"):
            if line not in make_text:
                die("updated Makefile lacks %s" % line)
        assert_clean(root)

        # Independent policy boundaries all reject before publication.
        rejection_cases = [
            ("pair", dict(sizes=(9, 11, 9, 9))),
            ("foreign", dict(foreign=21)),
            ("oneface", dict(grow=11)),
            ("order", dict(reverse_home=True)),
            ("safety", dict(metrics_override={"max_amplified": "1"})),
            ("profile", dict(metrics_override={"measurement_release_profile": hash_char("0")})),
            ("tool", dict(metrics_override={"measurement_host_tool_sha256": hash_char("0")})),
        ]
        for label, options in rejection_cases:
            repo, root = fresh(base, "reject-" + label)
            candidate = make_candidate(repo, "candidate", profile, tool_hash, **options)
            before = snapshot(repo, root)
            run(command(root, candidate, host_tool, profile_lock), success=False)
            if snapshot(repo, root) != before:
                die("%s policy failure modified canonical files" % label)
            assert_clean(root)

        # Every one of the four canonical publication replaces is faulted both immediately after
        # rename and after its directory fsync. Ordinary errors roll back before returning; hard
        # interruptions leave a durable marker that a fresh recovery rolls back deterministically.
        for index in range(1, 5):
            for suffix in ("temp-fsync", "rename", "fsync"):
                point = "publish-%d-%s" % (index, suffix)
                repo, root = fresh(base, "fail-" + point)
                candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                           sizes=(9, 9, 9, 9), foreign=19,
                                           grow=9, revert=8, digest_char="3")
                before = snapshot(repo, root)
                run(command(root, candidate, host_tool, profile_lock,
                            extra=["--test-fault", "fail@" + point]), success=False)
                if snapshot(repo, root) != before:
                    die("ordinary %s failure did not roll back" % point)
                assert_clean(root)

                repo, root = fresh(base, "crash-" + point)
                candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                           sizes=(9, 9, 9, 9), foreign=19,
                                           grow=9, revert=8, digest_char="4")
                before = snapshot(repo, root)
                crashed = run(command(root, candidate, host_tool, profile_lock,
                                      extra=["--test-fault", "crash@" + point]),
                              success=False)
                if crashed.returncode != 86:
                    die("hard %s fault returned %d" % (point, crashed.returncode))
                recover(root)
                if snapshot(repo, root) != before:
                    die("recovery after %s did not restore the old generation" % point)
                assert_clean(root)

        # Prepare-side interruptions, recovery itself being interrupted, and cleanup after the
        # commit point are all idempotent on the next invocation.
        prepare_points = ["stage-%d-%s-fsync" % (index, side)
                          for index in range(1, 5) for side in ("old", "new")]
        prepare_points += ["prepare-old-dir-fsync", "prepare-new-dir-fsync",
                           "prepare-marker-temp-fsync", "prepare-marker-rename",
                           "prepare-marker-fsync", "prepare-state-rename",
                           "prepare-state-fsync"]
        for point in prepare_points:
            repo, root = fresh(base, "prepare-" + point)
            candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                       sizes=(9, 9, 9, 9), foreign=19,
                                       grow=9, revert=8, digest_char="5")
            before = snapshot(repo, root)
            run(command(root, candidate, host_tool, profile_lock,
                        extra=["--test-fault", "crash@" + point]), success=False)
            recover(root)
            if snapshot(repo, root) != before:
                die("prepare recovery failed at %s" % point)
            assert_clean(root)

        # Failure before the committed-marker rename retains the prepared decision and therefore
        # rolls the fully installed-but-uncommitted generation back to OLD.
        repo, root = fresh(base, "commit-before-marker")
        candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                   sizes=(9, 9, 9, 9), foreign=19,
                                   grow=9, revert=8, digest_char="a")
        before = snapshot(repo, root)
        interrupted = run(command(
            root, candidate, host_tool, profile_lock,
            extra=["--test-fault", "crash@commit-marker-temp-fsync"]), success=False)
        if interrupted.returncode != 86:
            die("pre-commit reader fixture did not reach its hard-crash point")
        for name in FILES:
            if (root / name).read_bytes() != (candidate / name).read_bytes():
                die("pre-commit reader fixture is not a fully-new manifest generation")
        if "BASE_FULL_TOTAL ?= 36" not in (repo / "Makefile").read_text(encoding="utf-8"):
            die("pre-commit reader fixture is not a fully-new Makefile generation")
        pending = snapshot(repo, root)
        rejected = assert_reader(root, success=False)
        if ("run 'make golden-update' for recovery" not in rejected.stderr or
                snapshot(repo, root) != pending):
            die("release reader did not reject the fully-new prepared generation")
        recover(root)
        if snapshot(repo, root) != before:
            die("pre-commit marker interruption did not select the old generation")
        clean = assert_reader(root)
        if "wire_baseline_reader=clean" not in clean.stdout:
            die("release reader did not accept the recovered old generation")
        assert_clean(root)

        for index in range(1, 5):
            for suffix in ("temp-fsync", "rename", "fsync"):
                point = "recover-%d-%s" % (index, suffix)
                repo, root = fresh(base, "recovery-" + point)
                candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                           sizes=(9, 9, 9, 9), foreign=19,
                                           grow=9, revert=8, digest_char="6")
                before = snapshot(repo, root)
                run(command(root, candidate, host_tool, profile_lock,
                            extra=["--test-fault", "crash@publish-1-rename"]),
                    success=False)
                interrupted = recover(root, ["--test-fault", "crash@" + point], success=False)
                if interrupted.returncode != 86:
                    die("recovery fault %s returned %d" % (point, interrupted.returncode))
                recover(root)
                if snapshot(repo, root) != before:
                    die("second recovery failed after %s" % point)
                assert_clean(root)

        for phase, crash_point in (("prepared", "publish-1-rename"),
                                   ("committed", "commit-marker-rename")):
            for generation in ("old", "new"):
                label = "corrupt-%s-%s" % (phase, generation)
                repo, root = fresh(base, label)
                candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                           sizes=(9, 9, 9, 9), foreign=19,
                                           grow=9, revert=8, digest_char="c")
                run(command(root, candidate, host_tool, profile_lock,
                            extra=["--test-fault", "crash@" + crash_point]),
                    success=False)
                state = root / ".wire-baseline-update.transaction"
                write(state / generation / "Makefile", "corrupt staged payload\n")
                before_recovery = snapshot(repo, root)
                rejected = recover(root, success=False)
                if ("material is corrupt" not in rejected.stderr or
                        snapshot(repo, root) != before_recovery):
                    die("%s recovery mutated canonicals before rejecting corruption" % label)
                if not state.is_dir():
                    die("%s recovery discarded its durable transaction" % label)

        repo, root = fresh(base, "unknown-recovery-hash")
        candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                   sizes=(9, 9, 9, 9), foreign=19,
                                   grow=9, revert=8, digest_char="c")
        run(command(root, candidate, host_tool, profile_lock,
                    extra=["--test-fault", "crash@publish-1-rename"]), success=False)
        # Makefile is the last target: validation must inspect all canonical hashes before an
        # attempted rollback could rewrite any earlier manifest.
        write(repo / "Makefile", "external edit must not be overwritten\n")
        after_edit = snapshot(repo, root)
        rejected = recover(root, success=False)
        if "unknown hash" not in rejected.stderr or snapshot(repo, root) != after_edit:
            die("recovery did not retain evidence for an unknown canonical hash")
        if not (root / ".wire-baseline-update.transaction").is_dir():
            die("unknown-hash recovery discarded its durable transaction")

        for point in ("commit-marker-rename", "commit-marker-fsync",
                      "cleanup-state-rename", "cleanup-state-fsync"):
            repo, root = fresh(base, "commit-" + point)
            candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                       sizes=(9, 9, 9, 9), foreign=19,
                                       grow=9, revert=8, digest_char="7")
            interrupted = run(command(root, candidate, host_tool, profile_lock,
                                      extra=["--test-fault", "crash@" + point]),
                              success=False)
            if interrupted.returncode != 86:
                die("post-commit fault %s returned %d" % (point, interrupted.returncode))
            recover(root)
            for name in FILES:
                if (root / name).read_bytes() != (candidate / name).read_bytes():
                    die("committed generation was lost after %s" % point)
            assert_clean(root)

        for index in range(1, 5):
            for suffix in ("temp-fsync", "rename", "fsync"):
                point = "finalize-%d-%s" % (index, suffix)
                repo, root = fresh(base, "finalize-" + point)
                candidate = make_candidate(repo, "candidate", profile, tool_hash,
                                           sizes=(9, 9, 9, 9), foreign=19,
                                           grow=9, revert=8, digest_char="b")
                run(command(root, candidate, host_tool, profile_lock,
                            extra=["--test-fault", "crash@commit-marker-rename"]),
                    success=False)
                interrupted = recover(root, ["--test-fault", "crash@" + point],
                                      success=False)
                if interrupted.returncode != 86:
                    die("committed recovery fault %s returned %d" %
                        (point, interrupted.returncode))
                recover(root)
                for name in FILES:
                    if (root / name).read_bytes() != (candidate / name).read_bytes():
                        die("committed recovery did not converge after %s" % point)
                assert_clean(root)

        # Publication lock is fail-fast under contention, is released by process death, and a
        # stale measurement is rejected even after the lock becomes free.
        repo, root = fresh(base, "concurrent")
        first = make_candidate(repo, "first", profile, tool_hash, digest_char="8")
        stale = make_candidate(repo, "stale", profile, tool_hash, digest_char="9")
        argv = command(root, first, host_tool, profile_lock,
                       extra=["--test-lock-hold-ms", "400"])
        process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8")
        lock_path = root / ".wire-baseline-update.lock"
        deadline = time.monotonic() + 2.0
        observed = False
        while time.monotonic() < deadline:
            if lock_path.exists():
                fd = os.open(lock_path, os.O_RDWR)
                try:
                    try:
                        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        fcntl.flock(fd, fcntl.LOCK_UN)
                    except BlockingIOError:
                        observed = True
                        break
                finally:
                    os.close(fd)
            time.sleep(0.01)
        if not observed:
            process.kill()
            die("did not observe the first updater holding its lock")
        shared_fd = os.open(lock_path, os.O_RDWR)
        try:
            try:
                fcntl.flock(shared_fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
                die("shared release reader entered during exclusive publication")
            except BlockingIOError:
                pass
        finally:
            os.close(shared_fd)
        started = time.monotonic()
        contender = run(command(root, stale, host_tool, profile_lock), success=False)
        if time.monotonic() - started > 1.0 or "holds the publication lock" not in contender.stderr:
            die("concurrent updater did not fail fast with the lock diagnostic")
        stdout, stderr = process.communicate(timeout=3)
        if process.returncode != 0:
            die("lock holder failed: %s" % stderr.strip())
        committed = snapshot(repo, root)
        run(command(root, stale, host_tool, profile_lock), success=False)
        if snapshot(repo, root) != committed:
            die("stale concurrent measurement changed the committed generation")
        second = make_candidate(repo, "second", profile, tool_hash,
                                sizes=(9, 9, 9, 9), foreign=19,
                                grow=9, revert=8, digest_char="2")
        run(command(root, second, host_tool, profile_lock))
        assert_clean(root)

    print("wire_baseline_update_contract=OK (policy + no-op + 4-file transaction + recovery + concurrency)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("check_wire_baseline_update.py: %s" % exc, file=sys.stderr)
        sys.exit(1)
