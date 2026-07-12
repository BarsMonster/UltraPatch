# Install Requirements

Packages needed to build and inspect the final A1 C encoder/decoder on a
Debian/Ubuntu system:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  gcc \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
  git \
  libnewlib-arm-none-eabi \
  make \
  python3 \
  clang
```

The release workflow runs on an `ubuntu-24.04` GitHub-hosted amd64 runner, but
executes the job inside the official Ubuntu 26.04 image pinned in
`.github/workflows/gate.yml` by digest. The container runs as root, so its
package-install step intentionally does not use `sudo`.

The host `gcc` builds the `ultrapatch` encoder CLI and the host decoder wrapper
(the Makefile uses `$(CROSS_COMPILE)gcc`, i.e. plain `gcc`, when `CROSS_COMPILE`
is empty); `python3` drives the build/profile checks and stack-bound analysis
that several `make gate` legs run. The GNU Arm compiler, binutils, and
Newlib packages provide the Cortex-M0+ compile, link, size, divide-policy, and
stack-analysis tools. Clang is the required second host compiler for the release
checks.

The persistent host executable is scoped by a build-profile identifier and is
written to `.build/<profile-id>/ultrapatch`, not to the repository root. The
identifier changes with the host compiler identity, corpus `objcopy` identity,
and every effective build flag, so builds with different settings cannot
silently reuse one executable or generated corpus.
To obtain the exact path selected by the current Make arguments, run:

```sh
make host-tool-path
```

The Makefile exports that exact path to its test scripts. Do not invoke a cached
`./ultrapatch` from an earlier build or hard-code a `.build` subdirectory in an
external test wrapper.

Among persistent build directories, `make clean` removes only the selected
profile under the canonical `.build` tree; `make clean-all` removes that whole
canonical tree. Both also remove any legacy root CLI. Make deliberately refuses
to recursively clean custom or external `BUILD_DIR` paths.

`make check-build-profile` is a separate regression for this isolation. It
builds colliding configurations and verifies that they resolve to distinct,
correct host tools:

```sh
make check-build-profile
```

Release verification has a stricter provenance contract. The selected host GCC
driver and its `cc1`, `collect2`, assembler, and linker; required Clang; host
`nm`; selected GNU Arm GCC driver and its `cc1`, `collect2`, assembler, and
linker; the other named Arm binutils, including the `objcopy` used to derive
the profile-local corpus binaries; `libc.a`/`libgcc.a` content hashes; clean
compiler environment; and effective compile/link flags are pinned in
`toolchains/release-profile.json`. The full schema-3 lock also records the
immutable OCI digest configured for authoritative push CI. `make gate` validates
those inputs before forking any verification leg and prints the validated
`release_profile` in its consolidated summary. A mismatch is a failed release
gate, even when the local tools happen to compile the sources successfully.

Refresh the complete lock with the repository workflow:

```sh
/usr/bin/make release-profile-json   # inspect the complete candidate wrapper
/usr/bin/make release-profile-update # atomic, mode-preserving publication
git diff -- toolchains/release-profile.json
make check-release-profile
```

The updater takes the exclusive release-input lock, validates and preserves the
existing immutable `container` value;
changing that digest is a separate deliberate edit that must also update the CI
workflow. A second identical refresh is an exact inode/mtime/mode-preserving
no-op. Review and land the new lock with the resulting footprint, wire, corpus,
and full release evidence. Both authority targets force `PATH=/usr/bin:/bin`,
the C locale, isolated `/usr/bin/python3`, and absolute Make/coreutils launchers;
they reject Make launch controls and runtime tool/flag overrides at parse time.

This profile deliberately covers the selected compiler drivers, named GCC
subtools, named binutils, runtime archives, environment policy, and flags. It is
not a recursive dynamic-library closure and does not claim byte-for-byte binary
reproducibility; the behavioral gates are part of the release evidence.

Optional but useful for local inspection:

```sh
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  file \
  ripgrep
```

Verification commands, run from the repository root:

```sh
make
make host-tool-path
make check-build-profile
make check
make check-assets
make check-arm
make gate
```

For a local release preflight, run `/usr/bin/python3 scripts/release_gate.py`
from a clean `main` checkout. The driver holds the release-input lock, exports
the captured commit into a fresh temporary tree, runs with a small fixed child
environment, and requires explicit evidence from the build-profile, gate,
sanitizer, and Clang commands. It verifies that the selected local inputs match
the lock, but it does not attest that the local process is running in the
recorded OCI image. The successful push workflow at the exact `github.sha`,
inside the pinned container digest, is the authoritative release run.

`make check-arm` cross-builds the Cortex-M0+ decoder object and prints and
gates both its relocatable and no-startup linked ARM `text`/`data`/`bss` sizes
against the Makefile pins. The link uses an explicit FLASH/RAM layout, minimal
platform stubs, and only the runtime-library members pulled by the decoder; it
does not include CRT/startup or board code. A decoder compile needs
`-DCORTEX_M0`, an aligned `PATCH_IMAGE_BASE`, and a positive, page-aligned
`PATCH_IMAGE_CAPACITY` covering the complete physical patch partition from that
base. Repository host/ARM harnesses explicitly use
`-DPATCH_IMAGE_BASE=0u -DPATCH_IMAGE_CAPACITY=67108864u`. Missing, negative,
misaligned, or uint32-overflowing partition geometry is a compile-time error.
This check also runs as one leg of `make gate`.
