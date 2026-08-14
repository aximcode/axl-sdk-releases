# Handoff — 2026-08-13 (second session): CI green, three spikes answered, the SDK stopped needing a system linker

> Self-contained. Everything here was measured on this tree on this date.
> `main` is at `311eea1c` and pushed; working tree clean apart from Mike's
> untracked notes.
>
> **Supersedes `docs/AXL-Session-Handoff-2026-08-13.md`** — every §6 item in
> that document is now closed. Read this one.

---

## 0. Where to start

**Next task: C++ design task T2.** It is the ONLY remaining host input
(§4 below), it is well-specified, and its premise was re-verified today
against the toolchain that replaced the one it was measured on.

Nothing is half-finished. Twelve commits landed, `verify.sh` was ALL GREEN
before each, and CI run `31710463776` is green on all four jobs.

---

## 1. What shipped

| | |
|---|---|
| `655ba9b2` | `verify.sh --only=LIST` — a docs-only change costs 1m54s, not ~10m |
| `1994e123` | CI runs `scripts/lint.sh` instead of a drifted second copy |
| `03359f3f` | the hermetic direction + inventory of what is still the host's |
| `119c8d76` | aa64 links with OUR binutils |
| `fa6f02d2` | package deps: aarch64 pair dropped, missing `g++` declared |
| `e53d3987` | status mapping needs no new code — it needs a test seam |
| `a3d2350c` | **queue step 5 DROPPED** — the re-arm window is ~4%, measured |
| `05bacdf7` | `$(CROSS)` joins the build-state signature |
| `52195bcd` | `test-pkg-deps-minimal.sh` — deps validated on a bare image |
| `756786c8` | **x64 links with OUR binutils; the SDK needs no system linker** |
| `311eea1c` | hermetic inventory down to one entry |
| **release** | `toolchain-x86_64-elf-14.3.0-axl` on `aximcode/axl-sdk-releases` |

## 2. The CI failure, and the class of bug behind it

Run `31672194387`'s clang-tidy job died on
`features-time64.h:20:10: error: 'bits/wordsize.h' file not found` in
`axl-compress.c` and `axl-compress-lzma.c` — the only two TUs that reach a libc
header (via vendored sdefl/sinfl and the LZMA SDK).

**`bear` records commands whose compiler is `$(CC)`, and the cross libc is
IMPLICIT in that binary.** Nothing on the command line names
`<toolchain>/x86_64-elf/include`. clang-tidy replays such a command, infers the
freestanding target from the compiler's NAME, has no cross libc to match it,
and falls back to the host's `/usr/include`. EL/Fedora keep `bits/` there so it
resolved locally; Ubuntu puts it in the multiarch dir clang adds only for a
linux-gnu target. **The failure was the lucky part** — every TU that DID
analyze was analyzed against glibc, for a freestanding target.

Fixed by `make print-cc-libc-include` (asks `$(CC)`, fails rather than printing
an empty string) + `-nostdlibinc -idirafter<dir>`. **NOT `-isystem`**, which
puts the libc ahead of clang's builtin headers where the build searches the
compiler's own first — measured: it flips `limits.h`/`stdint.h`/`stdatomic.h`/
`tgmath.h` and defines `PATH_MAX` where the gcc build leaves it undefined.

`check-toolchain-conf` was ALSO red at HEAD, from CI-only commits validated by
dispatching CI — which does not run that gate.

## 3. Three questions answered

### 3a. §7 TCP spike — OVMF takes 32 concurrent Transmits, in order

`test/integration/test-tcp-multi-transmit-qemu.sh`. 4 x 64 KB (and 32 x 64 KB
= 2 MB with `MTX_TOKENS=32`): all accepted, all outstanding at once, retired in
submission order, no interleaving on the wire.

**But `EFI_TCP4.Transmit` lands in EDK2's `SockSend`, and that function IS the
same two-list queue** (`SndTokenList` / `ProcessingSndTokenList`, chosen on
`SndBuffer.LowWater`). So AXL's queue DUPLICATES the protocol's own
implementation at the source level. Two spec facts keep it anyway
(UEFI 2.11 §28.1.10):

- `EFI_NOT_READY` — "the transmit queue is full" — is a documented return.
  TcpDxe never returns it; a conforming stack may. An N-token path would have
  to defer the refused token, which IS a queue. **Not removable, only
  re-depthable.**
- Completion ORDER of multiple outstanding tokens is unspecified. TCP is a byte
  stream, so a stack that serialises two the wrong way corrupts it silently.

Design §2a carries the full reasoning. **`EFI_NOT_READY` is a live gap** —
`tcp_send_arm_chunk`'s caller treats every `EFI_ERROR` as fatal (§6 step 7).

### 3b. picolibc vs AxlFormat — a 35-byte tie

`scripts/measure-printf-size.sh` (committed; §4.1's rig never was and is not
recoverable). All rows one rig, `-Os`, same entry point:

    axl_vsnprintf             8,185 B image   no allocator
    newlib vsniprintf (int)  16,801 B         ALLOCATOR
    newlib vsnprintf         53,045 B         ALLOCATOR
    picolibc integer-only     2,624 B         no allocator
    picolibc float-capable    8,150 B         no allocator

**§4.1's blocking objection does not apply to picolibc** — tinystdio pulls four
members, no FILE/`_impure_ptr`/malloc, and even the float build is clean
(`dtoa_ryu` allocates nothing). **The `__thread` errno wall is a meson flag**,
verified: `-Dthread-local-storage=false -Dnewlib-global-errno=true` gives no
TLS symbols and no TLS segment.

**DECISION: keep `AxlFormat`.** 35 bytes apart at equal functionality; a tie
does not buy an external dependency for every consumer. picolibc stays the
leading candidate for the FOREIGN-code compatibility surface, which is what
§4b is actually about.

### 3c. Queue step 5 — DROPPED, on two independent grounds

1. **No consumer can use a receive queue.** Every caller's next buffer depends
   on what the last read returned (`axl-http-conn.c` picks by connection PHASE;
   the 9P server depends on the OPPOSITE — one outstanding recv is what makes
   "is `rbuf` lent?" answerable). Design §6g.
2. **The window is ~4%, measured.** 1.5 MB over plain HTTP, 1094 receives:
   armed 84,524 us, unarmed 3,927 us. ~3.6 us of re-arm against ~77 us waiting.
   And 4% is the CEILING, not the saving — the firmware buffers across it.

**Step 6 was already moot**: `23c75ee0` deleted `TCP_SEND_HIGH_WATER`/
`_LOW_WATER`. The surviving question is §7's depth cap, a design decision.

## 4. The hermetic direction — ONE input left

Mike, this session: **nothing from the host — not headers, not libraries, not
compilers.** Recorded in `AXL-Libc-Substrate-Design.md` §4.1d.

| input | source |
|---|---|
| C compiler, both arches | ours |
| C++ compiler, aa64 | ours (ARM's) |
| `ld`/`ar`/`objcopy`, both arches | **ours (this session)** |
| libc headers | ours (newlib) |
| package deps | **`g++` alone** |
| **C++ compiler, x64** | **host `g++` — T2, the last one** |

**Binutils was the unlisted gap**, with an explicit carve-out ("they consume
objects, not headers") the direction supersedes.

**x64 was blocked and is not any more.** A `--target=x86_64-elf` binutils
carries no PE target, so `objcopy --output-target=pei-x86-64` — the `.so` ->
`.efi` step — could not run. One command shows it:

    x86_64-elf-objcopy --info | grep pei

`build-toolchain.sh` gained `--enable-targets=x86_64-pep`; the toolchain was
rebuilt and published as **`toolchain-x86_64-elf-14.3.0-axl`** (GPL sources
attached, byte-identical to the 14.3.0 release's, verified against its
SHA256SUMS; three-year written offer in the notes).

`-axl` marks AximCode's BUILD of upstream 14.3.0, not a new upstream release —
the distinction ARM draws with `14.3.rel1`. `build-toolchain.sh` carries it as
`AXL_REV`, and `check-toolchain-conf` compares the manifest against
`GCC_VER + AXL_REV` because those are different facts. **The gate caught this
immediately** when only the manifest moved.

`install-toolchain.sh x64` was exercised end to end for the first time —
download, sha256 verify, extract beside an existing install.

## 5. NEXT: task T2, and why it is not a one-line change

`AXL-Cxx-Design.md` §6a-PLAN. **Premise re-verified 2026-08-13 against the
`-axl` toolchain** (a rebuild could have moved it; it did not):

    libaxl-cxx.a defines         61 symbols
    collide with libstdc++/libsupc++  51
    unique                       10  -> abort (libc.a), ceil (libm.a),
                                        __libc_single_threaded (unreferenced),
                                        6 anonymous-namespace internals,
                                        2 .localalias

So on the bare-metal path `libaxl-cxx.a` is a **multiple-definition ERROR**.
T2 must change the LINK — toolchain libs in, `libaxl-cxx.a` out — and supply
the newlib glue the spike carried in `ehenv.c`: `malloc`/`free`/`realloc`/
`calloc`/`posix_memalign` onto `axl_*` (which is also what keeps allocation
tracking working), four newlib stubs (`getenv`, `strtoul`, `_impure_ptr`,
`__xpg_strerror_r`), an AXL-owned `sbrk` (newlib's references the linker symbol
`end`, which AXL's scripts do not define), and `__register_frame` /
`__freeres` + `__deregister_frame` (`AXL-Cxx-Unwinder-Design.md` §U3).

**I attempted the default flip alone and reverted it.** `scripts/axl-cc:536`
says "Host g++, deliberately" AT THE CALL SITE and names T2. Do not repeat it.

Validation T2 needs: `test-cxx-hosted-qemu.sh` (120), `test-cxx-streams-qemu.sh`
(78), the 7/7 exception fixture, both arches. T3 (delete `--hosted`), T4, T5
follow it.

## 6. Also queued

- **Fault-injection seam** for §6 step 7. `EFI_NOT_READY` and
  `EFI_ACCESS_DENIED` are BOTH unreachable from a test today (TcpDxe never
  returns the first; `sock->closed` refuses before the firmware can return the
  second), so writing the mapping without a seam ships untested branches.
  **No new status code is needed** — `AXL_BUSY` (-10) and `AXL_DENIED` (-6)
  already exist and fit.
- **Toolchain `.deb`/`.rpm` packages**, so `axl-sdk` depends on them instead of
  the user running `install-toolchain.sh`. Then `test-pkg-deps-minimal.sh`'s
  mounts become `apt-get install` lines and it gets stronger.
- The old `/opt/x86_64-elf-gcc-14.3.0` tree is unreferenced and can be deleted.

## 7. Traps hit this session

- **`build-docs.sh` runs INSIDE `verify.sh`.** Running both pays for Sphinx
  twice. Use `./scripts/verify.sh --only=docs` (1m54s) for a prose change; a
  filtered run prints `NOT RUN: ...` so it can never be mistaken for a full one.
- **NEVER edit the Makefile while `verify.sh` is running.** `CC`/`CXX`/`CROSS`
  are in the build-state signature, so an edit wipes objects mid-build. Cost a
  wasted 10-minute run.
- **`gcc` has no `-nostdlibinc`** (that is clang's). For gcc use
  `-nostdinc -isystem $(gcc -print-file-name=include) -isystem <libc>`.
- **`local a=1 b=$a` does not work under `set -u`** — bash expands every word of
  a `local` before assigning. Two statements.
- **`grep -q` under `set -o pipefail` inverts a result**: it exits at the first
  match, the upstream `grep` dies of SIGPIPE, and the pipeline reports failure
  on a HIT. Collect into a variable, then test.
- **A `pkill -f` pattern can match your own shell** (exit 144).

## 8. Repo state

- `main` at `311eea1c`, pushed. Working tree clean apart from `SCRATCH.txt` and
  several untracked `docs/AXL-*.md` drafts.
- Local gates green at HEAD: `verify.sh` ALL GREEN both arches (10393),
  `test-pkg-deps-minimal.sh` PASS with `g++` alone, CI `31710463776` all four
  jobs green.
- Toolchains: `/opt/x86_64-elf-gcc-14.3.0-axl` and
  `/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf`. A fresh machine
  runs `./scripts/install-toolchain.sh all` — both are downloads.
- 147 integration tests (was 145): `test-tcp-multi-transmit-qemu.sh` and
  `test-pkg-deps-minimal.sh` joined.
