# Toolchains

Bare-metal cross toolchains AXL builds for itself.

**Deliberately separate from the SDK build.** Nothing here is wired into
the Makefile, and it is expected that this directory may become its own
project — a toolchain has its own release cadence, host matrix and
reproducibility concerns, none of which belong in a library's build.

## Why AXL builds one at all

| arch | toolchain | source |
|---|---|---|
| aa64 | `aarch64-none-elf` | **ARM publishes it** — `scripts/install-arm-toolchain.sh` fetches it |
| x64 | `x86_64-elf` | **nobody publishes one** — `x86_64-elf/build-toolchain.sh` builds it |

That asymmetry is the whole reason this exists. x64 has been borrowing
the **host's glibc-targeted g++**, and measurement showed that is the
root of three separate problems rather than a stylistic preference:

- **libsupc++ keeps `__cxa_eh_globals` in `__thread` storage**, read
  through `%fs`. UEFI sets up no thread pointer, so the first `throw`
  dereferences a garbage pointer and jumps — before it ever reaches the
  unwinder. Measured: 1 TLS symbol in the hosted `eh_globals.o`.
- **18 further `%fs:0x28` accesses across 11 objects** — glibc's stack
  canary, which UEFI likewise never sets up. AXL already compiles its own
  code with `-mstack-protector-guard=global` for exactly this reason; the
  distro archive was not built that way.
- **libstdc++'s headers are configured for glibc**, which is why
  `axl-c++ --hosted` exists at all. It is a workaround for borrowing
  them, not a feature.

A `--with-newlib --disable-threads --disable-tls` build has none of
those. The ARM toolchain's `libsupc++` measures **0 TLS symbols and 0
thread-pointer reads across all 65 objects**; this directory produces the
x86_64 counterpart so both arches are built the same way.

## Why not an existing package

| candidate | verdict |
|---|---|
| Homebrew `x86_64-elf-gcc` (16.2.0, has Linux bottles) | **bare compiler, no libc** — no newlib formula, no newlib dependency, so no `libstdc++`/`libsupc++` |
| bootlin toolchains | x86-64 targets are linux-gnu / musl only, not bare-metal elf |
| distro packages | none; `crosstool-ng` is not packaged either |
| `libstdc++-static` (RHEL) | wrong build anyway, and lives in **CRB**, which is not enabled by default — a hard `Requires:` would make `dnf install` fail outright |

## Building

```sh
./x86_64-elf/build-toolchain.sh          # ~1 hour on 8 cores
PREFIX=/opt/x86_64-elf JOBS=16 ./x86_64-elf/build-toolchain.sh
```

Four stages, each skipped if already present: binutils, GCC stage 1
(compiler + libgcc), newlib, then GCC stage 2 — which is the step that
produces the thing x64 has never had, a `libstdc++`/`libsupc++`
configured for a freestanding target.

The script **verifies the property it exists for** and fails the build if
the result still carries TLS symbols or `%fs` accesses in `libsupc++`.
That check is the point of the exercise, not a nicety.

## Distribution

For now the toolchain is built locally. If it ships, it should ship the
way ARM's does — a tarball fetched by a script, mirroring
`scripts/install-arm-toolchain.sh` — and that is the point at which this
directory probably wants to become its own repository.

## Related

- [`docs/AXL-Cxx-Design.md`](../docs/AXL-Cxx-Design.md) §6a and §6a-PLAN —
  freestanding vs hosted, and the plan to retire the distinction
- [`docs/AXL-Cxx-Unwinder-Design.md`](../docs/AXL-Cxx-Unwinder-Design.md)
  §U1 — the exception-runtime work that surfaced all of this
- [`docs/AXL-Newlib-Investigation.md`](../docs/AXL-Newlib-Investigation.md)
  — newlib as a substrate under AXL, a separate and larger question
