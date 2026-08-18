# Handoff — 2026-08-17 (evening): P4 shipped, the heap was broken, the toolchains now match

> **SUPERSEDED by `docs/AXL-Session-Handoff-2026-08-17-3.md`.** §7 below lists
> C2/C3/C5/C6 as the next work; all four shipped later the same day, along with
> `axl-c++ --no-eh-frame`. Start at the newer handoff — it carries the one
> outstanding task. Everything else here is still accurate history.

> Self-contained. Every number was measured on this tree on this date.
> **Supersedes `docs/AXL-Session-Handoff-2026-08-17.md`**, which was written
> before P4 and says "one phase remains".
>
> Working tree clean apart from Mike's untracked `SCRATCH.txt` and six
> `docs/AXL-*.md` drafts — **do not commit those, and never `git add -A`.**

---

## 0. Where to start

**The libc substrate is FINISHED.** All five phases plus every deferred
leftover except one ROADMAP item (debug SDK variant). Nothing in this document
is a pending task.

**The next work is the C++ layer: C2, C3, C5, C6** — see §7. Those are
*feature* work and were never part of the substrate.

Owning design docs: `docs/AXL-Libc-Substrate-Design.md` (P4-RESULT and
SBRK-RESULT are the sections written this session) and
`docs/AXL-Cxx-Design.md` §9 for the C-phase table.

---

## 1. What shipped

| commit | |
|---|---|
| `aad516cf` | **P4** — libstdc++ on every C++ link; `libaxl-cxx.a` deleted |
| `af38344c` | hardware crypto/hash instructions are detected and used by nothing (ROADMAP) |
| `3d4a77b8` | **the C heap is PLACED, not accepted** — `malloc` past 1 MiB works again |
| `81d18d91` | `mallinfo` tells the truth, `stat`/`rename` exist, the fd layer is tested |
| `8cf7e8d6` | the underscore forms are the implementations, the plain names are weak |
| *(toolchain)* | x64 toolchain rebuilt to match ARM's; `-axl3` published |

---

## 2. P4 — one C++ link shape

`libaxl-cxx.a` is gone: **all seven** sources, 1,696 lines, against the 765 the
design scoped. Every C++ link now carries the toolchain's libstdc++/libsupc++,
the four `axl-cxxrt-*.o` glue objects and the **exceptions linker script**.
`EH_LINK` collapsed into `CXX_LINK`; `-fexceptions` is compile-side only.

**Scope grew because two of the three reasons to keep the other five files
measured FALSE.** Keeping them saved ~3 KB of `.text` against a +47 KB budget,
and their AVX rationale (the distro libstdc++'s `hashtable_c++0x.o` carries 49
VEX instructions) died with the hermetic toolchain — `check-no-avx.py` over the
pinned `libstdc++.a`/`libsupc++.a` is clean across all 189 members.

**Cost measured, and 63% smaller than the doc accepted** (+126,400 predated the
terminate-handler shrink):

    containers.cpp x64   .text 33,984 -> 80,912 (+46,928)   .efi 58,758 -> 159,097
    an IOSTREAMS image   .text 734,512 x64 / 702,576 aa64
    hello.c              47,247 -> 47,247   BYTE-IDENTICAL

The EH linker script on every C++ link is **load-bearing**: libstdc++ is
compiled WITH exceptions whatever the caller passed, so `vector::at` really
throws in a `-fno-exceptions` image. With a registered frame table that reaches
AXL's terminate handler (type + `what()`); without one it arrives via
`_URC_FATAL_PHASE1_ERROR` and prints nothing.

**`axl::cout` / `axl::string` still earn their place, on a CHANGED argument.**
The old one ("`<iostream>` collides with `libaxl-cxx.a`") retired with the
archive. What survives is SIZE — three orders of magnitude — and, for
`axl::string`, recoverable OOM (design §9c).

---

## 3. The heap was BROKEN, and it is the biggest find of the session

Found by probing the parked `mallinfo` item, not by reading the plan.

**`malloc` failed for every request >= 1 MiB on both arches and wedged the heap
afterwards** — a 256 KiB request that had just succeeded returned NULL once a
large one was attempted. Since P4 that reaches `operator new`, so a
`std::vector` growing past ~1 MiB became `bad_alloc` -> terminate.

**Cause was ours.** `axl_alloc_pages` is `AllocateAnyPages`, which every UEFI
satisfies DOWNWARD from high memory:

    sbrk[0] = 0x1de1f000   sbrk[1] = 0x1dd1f000   sbrk[2] = 0x1dc1f000

A break that moves backwards is not a break; newlib sizes its top as
`brk + size - old_end`, which went negative and wrapped. dlmalloc was blameless.
The design doc claimed dlmalloc "handles a non-contiguous MORECORE" and that a
"multi-chunk assertion" proved it — **neither was true**, and the largest
allocation any fixture made was `realloc(16 -> 4096)`. Retracted in place.

**The fix is PLACEMENT, and that is the non-obvious half.** Growing in place
works (`AllocateAddress`) but only if the region has room above it, and a
firmware-chosen one does not: the pages above such a region were occupied 4/4
measured, because that is exactly the stripe the firmware carves its own
allocations from. So `sbrk` asks `axl_mem_largest_free_run` where the big
untouched run is, takes its **LOW end**, and grows upward into the rest.

| | before | after |
|---|---|---|
| `malloc(1 MiB)` / `malloc(32 MiB)` | NULL | ok |
| 256 KiB after a large request | NULL (wedged) | ok |
| ceiling | ~1 MiB | **415 MiB** measured |
| initial commit | — | 1 MiB, geometric growth to 16 MiB steps |
| cap | — | `AXL_LIBC_HEAP_MAX` (MiB); 8 -> 7 MiB, 40 -> 39 MiB verified |

**Two new public APIs:** `axl_alloc_pages_at` (exact-address pages) and
`axl_mem_largest_free_run` (largest `EfiConventionalMemory` run — the
classified region view cannot answer this, it maps `EfiBootServicesData` and
`EfiLoaderData` onto `AXL_MEM_REGION_RAM` too).

### Routes measured and REJECTED — do not re-run these

| route | x64 | aa64 |
|---|---|---|
| `AllocateMaxAddress`, low ceiling | works | **REFUSED** — ARM DRAM starts at `0x40000000`, no RAM below 256 MiB |
| PI GCD `EfiGcdAllocateAnySearchBottomUp` | `EFI_NOT_FOUND` | `EFI_NOT_FOUND` |

GCD looks purpose-built and is not: it is the **address-space** map, and the
DXE Core claims ALL of SystemMemory for itself at init (14 ranges / 511 MiB
x64, 1 range / 512 MiB aa64, every one owned). The bottom-up search is correct
and its domain is empty. `AllocateMemorySpace` is for claiming space nobody has
DESCRIBED yet (MMIO, hot-add). **The real gap is that `gBS->AllocatePages` has
no bottom-up option at all**, which is why reading the map and placing
explicitly is the supported answer rather than a workaround.

*Trap:* `EFI_GCD_MEMORY_TYPE` is `{NonExistent, Reserved, SystemMemory, ...}` —
SystemMemory is **2**, not 1. A first probe used 1 and proved nothing.

---

## 4. The two toolchains now match, and the reason they differed was not what I assumed

**ARM ships its build recipe** — `<prefix>/14.3.rel1-*-manifest.txt` — with the
exact configure line for every component. Read it before theorising.

**What actually differed:**

| | ours (was) | ARM's |
|---|---|---|
| newlib | 4.4.0 | 4.5.0 |
| six newlib flags | absent | present |
| `--disable-newlib-supplied-syscalls` | yes | **yes — we already matched** |

**`mallinfo` was a newlib 4.4.0 bug.** Confirmed in upstream `_mallocr.c`:
4.4.0 fills ten `int` fields, 4.5.0 ten `size_t`, and BOTH versions' `<malloc.h>`
declares `size_t` above a comment that the two "must match".

**`printf("%zu")` diverged, and that is the consumer-visible one.** Measured
before the fix: `%zu %zd %ju %td` emitted the literal text `zu` `zd` `ju` `td`
on x64 and worked on aa64. It is `--enable-newlib-io-c99-formats`, **not**
`io-long-long` (long-long already worked). `%zu` on a `size_t` is the most
common conversion in C and it failed *silently*.

**The `sys*.o` difference is NOT configurable.** newlib's `configure.host`
grants `syscall_dir=syscalls` to `aarch64*-*-*` and gives `x86_64` no entry at
all, so ARM's `libc.a` defines `open/read/write/lseek/sbrk/...` and ours defines
almost none. Verified by building x64 *without* the flag and observing no
change. Upstream per-target policy.

**`-axl3` is published and pinned.** newlib 4.5.0 + ARM's six flags;
`_WANT_IO_C99_FORMATS 1`, `mallinfo` struct 40 -> 80 bytes, `%zu` correct on
both arches. The `AXL_NEWLIB_MALLINFO_INT` displacement in
`axl-cxxrt-alloc.c` is gated on a build-time probe of the symbol's SIZE, so it
**disabled itself** when the toolchain was fixed — it is now dead code that can
be deleted at leisure.

---

## 5. The porting layer: underscore forms are the implementations

`_open`/`_read`/`_write`/`_sbrk`/... are the definitions; `open`/`read`/... are
**weak aliases**. Exactly one definition reaches the image whichever side
supplies it:

    aa64   newlib's strong open -> _open_r -> _open   (AXL)
    x64    nothing else defines open, so AXL's weak one is used

Verified against ARM's actual objects: `libc_a-writer.o` needs `_write`,
`readr.o` `_read`, `openr.o` `_open`, `sbrkr.o` `_sbrk`. A `MUST_WIN` comment
claimed newlib's `write()` calls `_write_r` which calls `write()` — a cycle —
and that was **wrong for this newlib**.

`_sbrk` is the implementation rather than a wrapper CALLING `sbrk`, and that is
load-bearing: as a wrapper, newlib winning would have made
`_sbrk -> sbrk -> _sbrk_r -> _sbrk`, a stack overflow at the first malloc
rather than a link error.

**`rename` stays STRONG** — the documented exception. newlib's falls back to
`link()` + `unlink()` and AXL provides no `link()`.

---

## 6. What else got closed

- **`stat()` / `rename()` implemented.** `stat` fills only `st_mode` and
  `st_size` from `axl_file_info` (FAT has no owner/link-count/permissions).
  `rename` uses `axl_file_move`, NOT `axl_file_rename` — the latter refuses
  cross-directory, POSIX does not.
- **The raw fd layer got its own fixture** — `libc-fd-selftest.c`, 27
  assertions, both arches. Neither newlib nor the toolchain ships tests for it
  (newlib's DejaGnu suite is source-tree only and tests *newlib*, not the layer
  beneath it that is ours). Two sabotages caught: dropping the slot release in
  `close()` fails 2, turning `SEEK_END` into `SEEK_SET` fails 4.
- **`test-cmake-package.sh` had been skipping ENTIRELY** since the
  `out/`->`stage/` rename (v4.1.0) — it hard-coded `STAGE=$PROJECT_DIR/out`
  while every sibling uses `sdk-prefix.sh`, and reported "PASS 0s". Fixed;
  14/14, including the C++ and exceptions cases.
- **`check-no-avx` repointed** at the runtime archives (libstdc++/libsupc++ via
  `$(CXX)`, libc/libm/libgcc via `$(CC)`) — 7 images where it covered 3. Its
  old subject was `libaxl-cxx.a`, so P4 would have left it watching nothing.

### Still open

- **Debug SDK variant** (ROADMAP): staged `libaxl.a` is `-DNDEBUG`, so SDK
  consumers have no instrumented allocator.
- **`PRIu64` and friends do not work on EITHER arch** — a uniform gap, not a
  divergence. `<inttypes.h>` gates them on `__int64_t_defined`. Not chased.
- Hardware crypto/hash instructions detected and used by nothing (`af38344c`).
- The `AXL_NEWLIB_MALLINFO_INT` block is now dead and can be removed.

---

## 7. NEXT WORK — the C++ layer, C2/C3/C5/C6

From `docs/AXL-Cxx-Design.md` §9. These are *feature* work; the substrate does
not block any of them, and the standard library now works fully (containers,
iostreams, fstream, sstream) on both arches.

| phase | what |
|---|---|
| **C2** | `std::string` — WORKS as-is. A **seam to/from `const char *` and `AxlString *`** is the only thing left to write |
| **C3** | `std::vector` — WORKS as-is; `axl::arena_allocator` shipped with C1. Remaining: **`axl::c_array_ref`** for the 9 AGT sites holding an `AxlArray *` |
| **C5** | **Domain wrappers**: `AxlNTree`, `AxlRadixTree`, AGT's draw-context guard migrates |
| **C6** | **C++ JSON API** — RAII container scopes, templated `add`, range-for over the reader. Four faces, so `w.splice(r["items"])` falls out of `axl_json_write_token` |

C4 and C7 are DONE. §4.x of that doc records which skins were measured GO/NO-GO
and why — read it before designing, because several obvious-looking wrappers
were rejected on measurement.

---

## 8. Traps this session paid for

1. **A doc's ANALYSIS goes stale where its status table does not.** Three
   instances: §4b's premise ("`libc.a` is never linked, on any path" — P3 put it
   on every link and deleted the two files it named), §4d's heading still
   reading "NOT STARTED" above five ticks, and a test comment claiming coverage
   a fixture cannot provide. **When a phase lands, grep the doc for the FACTS it
   invalidated, not just the row to tick.**
2. **A test can pass against the broken build.** The first `mallinfo`
   regression test asserted `arena > 0`, `uordblks >= requested`,
   `arena >= used + free`, `fordblks != canary` — **all four green on garbage**,
   because 32-bit fields read as 64-bit give large numbers satisfying every
   inequality. The discriminator (`arena < 2^32`) had to be MEASURED.
3. **Run both arches before believing a fix.** `AllocateMaxAddress` and the
   first `mallinfo` layout were each x64-correct and aa64-wrong.
4. **`sabotage.sh` restores the SOURCE, not artifacts STAGED from it.** A
   post-sabotage run showed two failures that were the sabotaged `lseek` still
   in `stage/`. `make && install.sh` before believing the verdict.
5. **`verify.sh` runs NO integration test**, and does not run `check-no-avx`,
   `check-nx-compat`, `check-bss-clear` or `check-reloc-coverage`.
6. **A waiter and the thing waited-for must never share a command line.** An
   `until ! pgrep -f 'verify[.]sh'` loop whose own argv contained
   `./scripts/verify.sh` waited on itself for 17 minutes; `verify.sh` never ran.
   The bracket trick protects the PATTERN, not the argv. Then the "fix" —
   `nohup &` inside a foreground call — removed the harness notification, so a
   finished suite sat unnoticed for 13 minutes. **Use `run_in_background` and
   the task notification; check the ARTIFACT (log size/mtime), not `ps`.**
7. **Read the vendor's own build manifest before theorising** about why two
   toolchains differ. ARM ships one. Two of my explanations were wrong and it
   settled both.
