# Handoff — v3.2.0 release prep, T2 phase 1, and the libc-substrate direction

> **Status: `main` = `8d3e684d`, clean, pushed, BUT THE RELEASE GATE IS RED.**
> Two integration failures; one is fixed-but-uncommitted, one is a REAL LEAK
> that blocks the tag. Version decided: **v3.2.0** (minor, Mike's call —
> semver would say major, see §5).
>
> Self-contained. Every number was measured on this tree this session.

---

## 1. STOP HERE FIRST — the release is blocked on one real leak

`./test/integration/run-integration.sh -j$(nproc)` → **143 passed, 2 failed**.

### 1a. `test-jose-qemu.sh` — REAL LEAK, blocks the release

103 assertions pass, then the leak gate fires. The sites, from the RAW log:

```
[0]   8 bytes  src/net/axl-mbedtls-platform.c:32   (axl_mbedtls_calloc)
[1]  32 bytes  src/net/axl-mbedtls-platform.c:32
[2]  32 bytes  src/net/axl-mbedtls-platform.c:32
[3] 256 bytes  src/net/axl-mbedtls-platform.c:32
[4]  24 bytes  src/net/axl-pk-verify.c:349         (key_alloc -> AxlPkKey)
```

**Hypothesis, untested: ONE leaked `AxlPkKey` explains all five.** `key_alloc()`
does `axl_malloc(sizeof(AxlPkKey))` then `mbedtls_pk_init(&k->pk)`; if that key
is never freed, its mbedtls context leaks too and that is the other four. Start
by finding which JOSE path allocates a key without a matching free — likely a
test, possibly the library.

**Getting the allocation sites is the trap.** `test-jose-qemu.sh:47` pipes
through `grep -iE "jws|jwt|jwk|jose|Results:|EXCEPTION|leak report"`, and the
per-allocation lines (`  [N] bytes at 0x… - file:line`) match NONE of those
patterns. The captured stdout therefore shows a leak report header and footer
with nothing between, which reads like a counter/list divergence and is not.
Run the binary directly for the real list:

```sh
./scripts/run-qemu.sh --timeout 170 out/native-x64-tls/AxlTestJose.efi
```

### 1b. `test-json-corpus-qemu.sh` — FIXED, uncommitted

`run-integration.sh:153` invokes EVERY test as `bash "$t" --arch "$ARCH"`, and
this test's own arg parser rejected `--arch`, so it exited 2 in 3s and could
never pass under the runner. Fixed in the working tree: `--arch` accepted, and
the `ARCH -> ARCH_SUFFIX` normalisation moved AFTER the parse loop so the flag
is honoured. Verified: now exits 0 with `SKIP: no corpora` (the corpora are not
fetched on this box, which is correct behaviour).

## 2. THE BIG PROCESS FINDING: `verify.sh` never covered TLS

`verify.sh` builds WITHOUT `AXL_TLS=1`, so **all 16 crypto/JOSE test groups are
skipped every run**. I reported "ALL GREEN, both arches, 10384" perhaps a dozen
times this session; it was true and much narrower than it sounded. The jose leak
had been sitting there the whole time and only the release gate — which builds
`AXL_TLS=1` — exercises it.

The skip lines say `(needs AXL_TLS=1)` right in the x64 log. Read them.

## 3. What shipped this session (all pushed, all green at the time)

| commit | what |
|---|---|
| `b852be15` | retired the hand-written exception runtime: removed `deps/libunwind`, `src/cxxabi/`, `check-cxxabi-oracle` (21,436 lines) |
| `48695da5` | one manifest for toolchain paths (`scripts/axl-toolchains.conf`) + `check-toolchain-conf` |
| `ef28ee5e` | `$(CC)`/`$(CXX)` in the rebuild signature |
| `597c52a9` | flaky loop test fixed; `check-dogfood` widened to `tools/` |
| `5c9b4562` | **apps can no longer `#include <uefi/…>`** (breaking) |
| `a0234245` | `libaxl-cxxrt.a` — T2 phase 1 |
| `d9ea53d2`, `0a49cd49`, `2e8f58c1` | the libc-substrate direction |
| `8d3e684d` | the `noexcept` CHANGELOG entry |

## 4. T2 — where it stands

**Phase 1 DONE (`a0234245`).** `libaxl-cxxrt.a`, nine symbols, built by the
BARE-METAL compiler, in the rebuild signature. Verified end to end: a
containers+exceptions image links it and runs **7/7 with zero leaks on both
arches**.

Findings that cost time, so they are not re-derived:

- **The handoff's "four required newlib stubs" are required by NOTHING.** A full
  newlib link supplies `getenv`, `strtoul`, `_impure_ptr`, `__xpg_strerror_r`.
  That list belonged to the spike, which linked libsupc++ WITHOUT libc. And
  `_impure_ptr` must NOT be defined — its member is pulled regardless, so
  defining it is a multiple-definition error rather than an override.
- **`sbrk` needs BOTH spellings.** x64 libnosys defines `sbrk`, ARM's defines
  `_sbrk`, both reference the undefined linker symbol `end`. Shipping one
  covered exactly one arch, in each direction.
- **`memalign`, not `posix_memalign`.** Measured on both toolchains: libsupc++
  references `memalign` (from `operator new(size_t, align_val_t)`) and
  `posix_memalign` nowhere.
- **TWO archive members, not one.** Every C++ link pulls the allocator half, and
  merged it would drag `axl-cxxrt-eh.o`'s `__eh_frame_start` reference —
  `--no-undefined` fires before `--gc-sections` can collect it, so nothing
  in-tree could link the archive at all.
- **`__deregister_frame` on an unregistered table TRAPS** (`ud2` x64 / `abort`
  aa64), and `#UD` + `CR2=0` under UEFI reads exactly like the AVX fault this
  tree has lore about. `axl_cxxrt_fini` is guarded and idempotent.

**Phase 2 REMAINING:** the `--exceptions` link path (separate linker script with
`KEEP`, `.eh_frame` + `.gcc_except_table` in the `objcopy -j` list, **omitting
`.eh_frame_hdr`** — it fails on x64 with "overlapping FDEs" and
`__register_frame` does not read it); the 7/7 demo as a committed both-arch
integration test; `scripts/lint.sh` cross-toolchain `-isystem` paths (clang-tidy
cannot infer them and the lint gate goes red); `install.sh` does not ship
`libaxl-cxxrt.a`; then flip x64's default compiler.

**Working recipes are in the session scratchpad** (`build-eh.sh`,
`build-eh-x64.sh`) and already compile against the committed sources.

## 5. Direction agreed: `axl_*` on a libc substrate

`docs/AXL-Libc-Substrate-Design.md` (new, committed). AXL sits on newlib the way
GLib sits on libc, with **the allocator inverted rather than replaced**: AXL's
allocator keeps its implementation and takes the STANDARD NAMES
(`malloc` -> `axl_malloc_impl(size, "<libc>", 0)`), so newlib never needs a heap
and `_sbrk` leaves the picture. That is what unblocks the whole idea — the
investigation doc had called `_sbrk` the blocker.

Also settled there: why this IS the GLib model (glibc's malloc is the libc's own
implementation over the platform primitive; `AllocatePool` is ours; newlib's
dlmalloc is a fallback for targets that only have `sbrk`); that link order must
be made a non-issue by STRIPPING newlib's eight allocator members rather than
documenting an ordering; and the no-regression constraint with per-function
gates.

**Open measurement that decides how deep the substrate goes:** does newlib's
`printf` reintroduce the Log -> Data cycle `AxlFormat` exists to break?

## 6. `AXL_CB_NOEXCEPT` — keep it, and it is NOT removable later

Asked and answered this session. Removing it needs `libaxl.a`'s C built with
`-fexceptions` (row 3 of the measured table: `__attribute__((cleanup))` alone
emits no landing pad), which collides with §U2's byte-identity constraint, AND
an exception-safety audit of **440 allocation sites, 59% of which transfer
ownership out of the frame**. The toolchain work does not touch any of that —
it makes throwing possible, which makes the boundary LIVE rather than
theoretical.

**Verified comparison:** glibmm has the identical limitation and solves it with
a runtime trampoline in generated code. Measured here: `libglib-2.0`,
`libgobject`, `libgio`, `libgmodule` all have `.eh_frame` but NO
`.gcc_except_table` — built without `-fexceptions`, so their `g_autoptr`
cleanups would not run either. (Control: `libstdc++` and `libc` both DO have it,
so the discriminator works.) AXL enforces at compile time because it has no
generated wrapper layer and a worse failure mode: a leaked `RaiseTPL` wedges
the machine, silently on x64.

## 7. Consumers — all verified on this host

| consumer | links via | language | vs HEAD |
|---|---|---|---|
| softbmc | source checkout | C | builds clean |
| delldiags/**axl-utils** | **pinned `.axl-sdk-version` = 3.1.0** | C | unreachable by a tag |
| **agt** | source checkout | **C++** | **was broken; FIXED, uncommitted** |
| axl-webfs | source checkout | C | builds clean |

**`agt` is fixed in its working tree and builds rc=0** — 45 files, `noexcept`
only. NOT committed; review and commit it there. `devkit.conf` in that repo is
Mike's own uncommitted work from Jul 27, untouched.

**Find consumers by grepping `#include <axl`, over `~/projects ~/work ~/dell`.**
`axl-utils` lives at `~/work/dell/delldiags/source/src/axl-utils` — SIX levels
deep, outside `aximcode/`. I used `find -maxdepth 5` and missed it, which is the
second time that class of mistake has happened.

## 8. Release procedure (docs/RELEASING.md)

The gate is LOCAL, not CI:

```sh
make ARCH=x64 AXL_TLS=1 all tests tools axl-busybox   # green
./test/integration/run-integration.sh -j"$(nproc)"    # 143/2 -- MUST be 145/0
scripts/lint.sh                                        # clean
gh workflow run ci.yml --ref main                      # §4b fresh-OS backstop
scripts/cut-release.sh 3.2.0                           # NOT RUN YET
```

Build and lint are green as of `8d3e684d`. Only the suite is red.

## 9. Do this next, in order

1. Fix the jose leak (§1a). Start at `key_alloc` / `AxlPkKey` ownership.
2. Commit the `test-json-corpus` fix (§1b) — it is already in the tree.
3. Re-run the full suite; require **145 passed, 0 failed**.
4. `gh workflow run ci.yml --ref main`, watch green.
5. `scripts/cut-release.sh 3.2.0`.
6. Then T2 phase 2 (§4).

## 10. Related

- `docs/AXL-Cxx-Toolchain-Handoff.md` — the toolchain, its four build traps
- `docs/AXL-Cxx-Design.md` §6a-PLAN — T1-T5, and why T2 is a LINK change
- `docs/AXL-Libc-Substrate-Design.md` — the substrate direction
- `docs/RELEASING.md` — the cut procedure
