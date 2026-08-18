# Handoff — 2026-08-17: AXL is a C platform now

> Self-contained. Every number was measured on this tree on this date.
> **Supersedes `docs/AXL-Session-Handoff-2026-08-16.md`** — its §6
> (terminate-handler shrink) shipped, and its other open items are
> restated below where still live.
>
> `main` is at **`aad516cf`**, **29 commits ahead of origin and NOT
> PUSHED** (`0ae15e55..aad516cf`, counted against `origin/main` as of the
> 2026-08-16 fetch — the previous handoff's "19 commits" was counted from
> `c856ee01`, which is NOT an ancestor of `origin/main`, so ask git rather
> than carrying that number forward). Working tree clean apart from Mike's
> untracked `SCRATCH.txt` and six `docs/AXL-*.md` drafts — **do not
> commit those, and never `git add -A`.**
>
> Gates: `verify.sh` **ALL GREEN** both arches (10405 unit tests, 21 gate
> lines, lint clean, docs 0 warnings); `run-integration.sh` 154/156 with
> both failures triaged (one asserted a pre-P4 invariant and is fixed, one
> is a timing flake in a pure-C image P4 cannot reach);
> `test-cxx-iostreams-qemu.sh` 40/40 and `test-cxx-hosted-qemu.sh` 79/79
> per arch.

---

## 0. Where to start

**ALL FIVE PHASES ARE DONE.** P4 landed later the same day this was
written — see **P4-RESULT** in `docs/AXL-Libc-Substrate-Design.md`, which
supersedes §4 below. The rest of this document is the state BEFORE P4 and
is kept because its measurements and traps are still the reference.

What P4 changed, in one paragraph: `libaxl-cxx.a` is deleted — all seven
sources, 1,696 lines, not the two §4 scopes — and every C++ link now
carries the toolchain's libstdc++/libsupc++, the four `axl-cxxrt-*.o` glue
objects and the exceptions linker script. `<iostream>`, `<sstream>` and
`<fstream>` work on both arches. **The cost in §4 below is stale by 63%:**
the real figure is +46,928 `.text`, not +126,400, because that measurement
predated the terminate-handler shrink (`fa54572b`).

The owning design doc is **`docs/AXL-Libc-Substrate-Design.md`** — read
§2-DECISION, §4c, §4c.1 and §4d before touching anything. This handoff
does not repeat what lives there.

---

## 1. The headline

**A plain `.c` file built with `axl-cc` now gets `printf`, `malloc`,
`strcpy`, `fopen`.** Before today, a C link was `libaxl.a` and nothing
else; the C library arrived only on the `-fexceptions` C++ path, so the
majority of consumers had the whole C library as undefined references.

`hello.c` is **47,247 bytes before and after** — byte-identical. Archive
semantics plus `--gc-sections` mean a consumer pays only for what they
call, which the six-image spike predicted and the real path confirmed.

| phase | status | |
|---|---|---|
| **P1'** | ✅ | TWO allocators, split by namespace |
| **P2** | ✅ | real stdio — newlib's `FILE` running on `AxlStream` |
| **P3** | ✅ | one provider of the leaf C names; 195 lines deleted |
| **P5** | ✅ | measured — nothing to do (§5 below) |
| **P4** | ✅ | landed same day — 1,696 lines (not 765), iostreams, +46,928 `.text` (not +126 KB) |

## 2. The design decision, and that I argued the wrong side

Mike's call: **two allocators, split by NAME.**

    malloc / free / realloc / calloc / _malloc_r / ...   newlib's dlmalloc
                                                          -> AXL sbrk -> EFI pages
    axl_malloc / axl_free / ...                           AXL, AllocatePool

I repeatedly objected that two allocators corrupt, citing `strdup()`
allocating through one and `free()` releasing to the other. **That hazard
is a property of a HALF split, not of having two allocators.** AXL held
the PLAIN names while 49 of newlib's 625 objects allocate through the
REENTRANT `_r` family — so the two vocabularies were tangled. With
disjoint namespaces nothing can cross, and the design is sound.

He raised it three times before I measured instead of re-citing the
design doc. Performance was **my** framing, not his — the decision was
taken with "I'm okay with it being slower than `axl_malloc`". The point
was separation.

**Accepted cost, and it is smaller than it sounds.** `operator new`
reaches `malloc`, so C++ and third-party allocations leave AXL's leak
gate. But the staged `libaxl.a` is built `-DNDEBUG`, so **SDK consumers
never had that instrumentation** — no fences, no `0xDA`/`0xDF` fills, no
leak list, no quarantine. The loss is real only for in-tree C++ work that
links `libc.a` in a DEBUG build, which nothing currently does. That gap
is its own ROADMAP item now ("debug SDK variant").

An `#ifdef AXL_MEM_DEBUG` bridge to keep instrumentation was **considered,
measured and dropped as dead code**: the staged objects are RELEASE and
in-tree DEBUG builds never link `libc.a`, so the two configurations never
coincide. §2-DECISION records the table.

## 3. What each phase actually did

**P1' — two allocators** (`442c442f`). `sbrk` hands out 1 MiB chunks from
`axl_alloc_pages`. Growth is chunked and NOT contiguous (UEFI cannot
reserve address space), so dlmalloc starts a new segment per chunk;
negative increments trim within the current chunk only, against the chunk
BASE. `test-libc-alloc-qemu.sh`, 28/28 both arches.

*The bug worth remembering:* `axl_alloc_pages` returns `AXL_OK`/`AXL_ERR`,
not a bool, and `AXL_OK` is 0 — so `if (!axl_alloc_pages(...))` read
success as failure. Every newlib allocation returned NULL while
`axl_malloc` worked perfectly, which is indistinguishable from "the
design does not work". Caught only because the fixture asserts the two
allocators SEPARATELY.

**P2 — the porting layer** (`7cf04c31`). `write`/`read`/`close`/`lseek`/
`fstat`/`isatty` implemented over `AxlStream`, plus `open`/`unlink` which
AXL did not define at all. The fd table is an `AxlStream *` array — the
backend vtable already had the right shape. `fstat` is implemented (not a
stub): `<fcntl.h>` brings `<sys/stat.h>`, which collided with the old
`void *st` signature — and that signature existed BECAUSE the layout was
out of scope, so the collision removed its own justification.
`test-libc-stdio-qemu.sh`, 44/44.

**P3 — one provider** (`9d4cd144`, `6ec731d3`, `bb4bf68b`). `libc.a` on
every link in `axl-cc` AND the Makefile's four `LINK_EFI_*` macros;
`axl-str-compat.c` + `axl-intrinsics.c` deleted (195 lines). AXL/newlib
symbol overlap fell **14 -> 5** on x64. AXL now owns all four symbols of
`stack_protector.o`, having owned two.

## 4. What P4 is, with its numbers already taken — SUPERSEDED, see P4-RESULT

> **Read `AXL-Libc-Substrate-Design.md` P4-RESULT instead.** Two things
> below are measured-wrong: the +126,400 `.text` (real: +46,928, this
> predated `fa54572b`) and the scope (all 7 files went, not 2 — the AVX
> rationale for the other five expired with the hermetic toolchain).
> Kept because the "it is a SWAP, not an addition" reasoning is what made
> the phase tractable, and that part held.

Delete `src/runtime/axl-cxxabi-ops.cpp` (505 lines) and
`src/runtime/axl-cxx-list.cpp` (260), and let every C++ link carry
libstdc++ — which is what makes **iostreams** reachable.

Measured, `containers.cpp`, `-fno-exceptions`:

| | `libaxl-cxx.a` | libstdc++ | delta |
|---|---|---|---|
| `.text` | 33,712 | 160,112 | **+126,400 (+375%)** |
| `_Unwind_*` symbols | 0 | 34 | |
| libstdc++ members | 0 | 81 | |

**This cost is ACCEPTED** (Mike, 2026-08-17). It lands only on
`-fno-exceptions` C++ images; C images are untouched and `-fexceptions`
ones already link libstdc++.

**It is a SWAP, not an addition, and cannot be done incrementally:**
`libaxl-cxx.a` overlaps `libstdc++.a` on **53 of its 56 symbols**. The
two can never share a link.

Also measured and benign: `libaxl-cxx.a` overlaps `libc.a` on exactly one
symbol, `abort` — and `libc_a-abort.o` defines nothing else, so it can
only be pulled for that name, and `libaxl-cxx.a` is scanned first.

## 5. P5 is done because its premise was false

`AXL-Cxx-Stdlib-Surface.md` Tier 3 said glibc's locale subsystem "blocks
`<sstream>` `<fstream>` `<format>` `<regex>`". **All four compile, link
and RUN today**, booted in QEMU:

| header | proof | `.efi` | over the 119,691-byte `-fexceptions` baseline |
|---|---|---|---|
| `<fstream>` | `read=[from fstream]` — wrote and read back a file on the ESP | 1,078,943 | +959,252 |
| `<sstream>` | `n=42` | 1,090,906 | +971,215 |
| `<format>` | `n=42` | 1,146,441 | +1,026,750 |
| `<regex>` | `match=1 nomatch=0` | 1,194,294 | +1,074,603 |

The tier was measured against the OLD link shape. What those headers
needed was a C library, not a locale reimplementation, so P2/P3 delivered
it as a side effect. **Tier 3 is RETRACTED.** The wall is SIZE — ~1 MB
each, ~9x the baseline — which is a budget question, not a support one.

## 6. What it costs users to prefer libc over `axl_*`

Documented in `src/format/README.md` and `src/mem/README.md` (both are
pulled into the published Sphinx docs). Measured on `hello.c`, x64
`--release`, changing only the call:

| the program calls | `.efi` | delta |
|---|---|---|
| `axl_strcmp` -> `strcmp` | 47,265 | **+18** (free) |
| `axl_printf` (baseline) | 47,247 | — |
| `axl_malloc` -> `malloc` | 60,760 | **+13,513** |
| `axl_printf` -> `printf` | 109,147 | **+61,900 (2.3x)** |

`printf` is expensive because newlib's `vsnprintf` **is** stdio: one
format call brings `_vfprintf_r` (12,380) AND `_vfiprintf_r` (6,192),
`_dtoa_r` (6,066), and the allocator because `FILE` buffers allocate. The
image then carries each engine twice.

`malloc`'s 13.5 KB is only ~7 KB of content — **`.rela` is the largest
item (+6,240)**, because a `-fpic` UEFI image needs a relocation per
pointer and dlmalloc's bin array is 128 pointer pairs. That generalises:
a pointer-dense structure costs about twice its size here.

## 7. Open items

- ~~**P4** (§4)~~ **DONE** — see `AXL-Libc-Substrate-Design.md` P4-RESULT.
  It opened one new item: **C++ allocation failure is no longer
  injectable.** `axl_mem_fail_next_alloc()` reaches AxlMem, and
  `operator new` is libstdc++'s now and calls newlib `malloc`. Fixtures
  that need a failing C++ allocation must request an unsatisfiable size.
  A knob at `sbrk` would NOT fix it — dlmalloc serves small requests from
  its top chunk without calling `sbrk` at all.
- **Debug SDK variant** — ROADMAP item raised today. SDK consumers have
  **no** instrumented allocator: the staged `libaxl.a` is `-DNDEBUG`.
  Closing it means staging a DEBUG variant of `libaxl.a` + the glue and
  having `axl-cc --debug` select it.
- **P2 leftovers**: `stat()` and `rename()` are not implemented. Nothing
  calls them — `remove()` reaches `unlink()`, which is.
- **Introspection leftovers**: `mallinfo`, `malloc_stats`, `malloc_trim`
  are still newlib's. Safe: referencing one is a LOUD link error, unlike
  `malloc_usable_size`, whose failure was silent (it returned 56 for a
  64-byte block).
- Carried from the previous handoff, still untouched: **Distribution P1**
  (SDK tarball, `axl-cc --print-prefix`), **`cut-release.sh --from <ref>`**,
  and the **CMake port** (branch `worktree-cmake-build-system`, design
  merged, zero implementation).

## 8. Traps this session paid for — read before repeating them

1. **`verify.sh` runs NO integration test.** It runs the unit suites,
   lint, `make` gates and docs. I read ALL GREEN as full coverage for 16
   commits; when the integration suite finally ran it found **two real
   regressions** from P3 immediately. For anything touching a link line,
   an archive, a staged object or symbol ownership,
   `./test/integration/run-integration.sh` (161 suites, ~7 min) is the
   gate. Run BOTH.
2. **A failed link can read as a passing measurement.**
   `test-gfx-link-granularity.sh` counts ftgrays symbols; when P3 broke
   its hand-rolled probe link the count came back 0 — which is what the
   test wants to see for a non-rasterizing consumer. It failed loudly
   only because that suite carries a positive control asserting the check
   is LIVE.
3. **Owning MOST of an archive member is the bug.** A member is extracted
   for ANY symbol it defines. AXL owned two of `stack_protector.o`'s
   four. The converse makes several overlaps harmless — check what a
   member defines before deciding.
4. **Deleting a source leaves its `.o` in the archive.** Removing a
   prerequisite does not make a target out of date, so `libaxl.a`'s
   recipe (whose first line is `rm -f $@`) never runs. `make` prints
   "built" and `nm` still shows the symbol. RENAME is safe; DELETION is
   not. CLAUDE.md corrected.
5. **Waiters that cannot see completion — three times today.** Once a
   `pgrep` pattern matching its own argv; once an alternation where only
   one branch was escaped; once an `until grep -qE "^[0-9]+ passed"`
   against a line reading `integration: 153 passed…`. Background the real
   command and use the completion notification; never invent the pattern.
6. **A failed `git add` still commits.** Mine failed (the deletions were
   already staged by `git rm`), so the commit captured ONLY the deletions
   and would not have built. The non-zero exit was the tell; `git log
   --stat` after committing is the cheap confirmation.
7. **Restage before testing the SDK.** Editing `scripts/axl-cc` and then
   running `stage/bin/axl-cc` tests the old copy.
