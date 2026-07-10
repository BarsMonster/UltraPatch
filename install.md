# Install Requirements

Packages needed to build and inspect the final A1 C encoder/decoder on a
Debian/Ubuntu system:

```sh
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  gcc \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
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
is empty); `python3` drives the single-header generator and the stack-bound
analysis that several `make gate` legs run. The GNU Arm compiler, binutils, and
Newlib packages provide the Cortex-M0+ compile, link, size, divide-policy, and
stack-analysis tools. Clang is the required second host compiler for the release
checks.

The persistent host executable is scoped by a build-profile identifier and is
written to `.build/<profile-id>/ultrapatch`, not to the repository root. The
identifier changes with the host compiler identity and every effective build
flag, so builds with different settings cannot silently reuse one executable.
To obtain the exact path selected by the current Make arguments, run:

```sh
make host-tool-path
```

The Makefile exports that exact path to its test scripts. Do not invoke a cached
`./ultrapatch` from an earlier build or hard-code a `.build` subdirectory in an
external test wrapper.

Among persistent build directories, `make clean` removes only the selected
profile under the canonical `.build` tree; `make clean-all` removes that whole
canonical tree. Both also remove the default generated single header and any
legacy root CLI. Make deliberately refuses to recursively clean custom or
external `BUILD_DIR` paths.

`make check-build-profile` is a separate regression for this isolation. It
builds colliding configurations and verifies that they resolve to distinct,
correct host tools:

```sh
make check-build-profile
```

Release verification has a stricter provenance contract. The default GCC,
required Clang, GNU Arm GCC/binutils, `libc.a`/`libgcc.a` content hashes, and
effective release flags are pinned in `toolchains/release-profile.json`.
`make gate` validates those exact inputs before forking any verification leg and
prints the validated `release_profile` in its consolidated summary. A mismatch
is a failed release gate, even when the local tools happen to compile the
sources successfully. Updating an accepted tool identity, archive, or flag is a
deliberate release-profile change: update the descriptor, review the resulting
code, footprint, wire, and corpus evidence, and land those changes together.
This contract establishes traceable inputs; it does not claim that compiler
executables or output binaries are byte-for-byte reproducible across machines.

Optional but useful for local inspection:

```sh
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  file \
  git \
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
