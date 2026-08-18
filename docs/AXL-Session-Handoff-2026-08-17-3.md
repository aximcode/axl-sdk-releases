# Handoff — 2026-08-17 (night): the C++ layer is COMPLETE, and one approved change is pending

> Self-contained. Every number was measured on this tree on this date.
> **Supersedes `docs/AXL-Session-Handoff-2026-08-17-2.md`**, whose §7 lists
> C2/C3/C5/C6 as the next work — all four have since shipped.
>
> Working tree clean apart from Mike's untracked `SCRATCH.txt` and six
> `docs/AXL-*.md` drafts — **do not commit those, and never `git add -A`.**

---

## 0. START HERE — the one pending task

**Mike APPROVED adding `-fno-asynchronous-unwind-tables` to x64's
`CFLAGS_BASE`.** Nothing is written yet. It is measured, the safety argument is
built, and two verifications were agreed BEFORE the change lands. §6 is the
whole task with commands.

Everything else in this document is context. Nothing else is outstanding.

---

## 1. What shipped this session

Five commits on top of `ac25a74d`, all pushed to `main`:

| commit | |
|---|---|
| `8d04e818` | **radix tree: a NULL value is a VALUE** — a real bug fix |
| `24f24165` | `axl_array_data()` + `axl_array_element_size()` |
| `42d0fdd6` | **C2/C3/C5** — five C++ seam headers |
| `2ad184c2` | JSON C additions: 3 decoded-length queries + `axl_json_double` |
| `e91b77bc` | **C6** — `axl-json.hpp`, the C++ JSON API |
| `77209544` | doc corrections (exceptions "not negotiable" was false) |
| `badf3a51` | **`axl-c++ --no-eh-frame`** |
| `9815840f` + `1ca0d5c9` | the `--no-eh-frame` size decomposition, and a wrong causal claim corrected |

**The C++ layer is C0-C7 COMPLETE.** Read that as "every phase the design doc
scoped has landed and is tested", NOT "proven against real consumers": for C6
and the C5 radix wrapper the only in-tree consumer is the fixture. §9c's
acceptance test — **"AGT shrinks"** — has never been re-run since C2/C3/C5
landed, and that is the real outstanding measurement for the layer.

Current gate state: `verify.sh` ALL GREEN, `run-integration.sh` **159/159**,
unit ratchet **10485**, both arches agreeing.

---

## 2. The C++ layer, in one table

Six headers, all HEADER-ONLY. The layer compiles no `.cpp` beyond the runtime
glue in `src/cxxrt/`.

| header | what |
|---|---|
| `axl-cstr.hpp` | `axl::view` (NULL-safe) / `axl::adopt` (copy out, `axl_free`) |
| `axl-array.hpp` | `array_span` / `array_ptr_span` over the new `axl_array_data()` |
| `axl-ntree.hpp` | four lazy allocation-free ranges + `data_of<T>` |
| `axl-radix-tree.hpp` | `axl::radix_tree<T>` |
| `axl-gfx-surface.hpp` | `axl::gfx_target_scope` |
| `axl-json.hpp` | `json_document`/`json_value`/`json_writer`/`json_scanner` |

Design record: `docs/AXL-Cxx-Design.md` §9d (C2/C3/C5) and §9e (C6).

**Two guards are `static_assert`s, both verified to FIRE with the right
message:** `adopt<S>` refuses a borrowing `S` (would return a view over the
buffer it freed), and `array_span<T>` refuses an over-aligned `T`
(`axl_malloc` guarantees 8 bytes; `alignas(16)` matches the stride at 16 and
lands misaligned).

---

## 3. `--no-eh-frame`, and why it is not what was asked for

AGT asked for "a way to select the non-`_eh` linker script". Measurement said
that alone does two wrong things, and **both are worth not rediscovering**:

1. **It does not LINK.** `axl-cxxrt-eh.o` references `__eh_frame_start`, which
   only the `_eh` script defines, under `--no-undefined`.
2. **It does not degrade, it CRASHES.** `vector::at(99)` without a frame table
   takes an unhandled CPU fault and **wedges the machine** (34 s of spin, QEMU
   killed by timeout). An empty-but-valid `.eh_frame` was tried and faults
   identically — the fault is inherent to unwinding a frame with no FDE.

So the flag swaps `axl-cxxrt-eh.o` for **`axl-cxxrt-nothrow.o`**, whose
`__cxa_throw` intercepts before the unwinder and prints type + `what()` then
exits. `--wrap=__cxa_throw`, not a definition — libsupc++'s `eh_throw.o` lands
on the link anyway and a second definition is `multiple definition`.

**REFUSED with `-fexceptions`**, from the flag AND from an input object
referencing `__gxx_personality_v0`. That combination silently kills every catch
block, which is a correctness failure rather than a size trade.

Test: `test-cxx-noeh-qemu.sh`, 19 assertions per arch.
Design: `docs/AXL-Cxx-Unwinder-Design.md` §U5.

---

## 4. The size model — do NOT quote a percentage

    saving = .eh_frame + the gc'd throw path

| term | x64 | aa64 |
|---|---|---|
| `.eh_frame` | **scales with the program** | **constant** ~8.3 KB |
| gc'd throw path | ~13.5 KB constant | ~16.5 KB constant |

So `--no-eh-frame` is **~13% at any tool size on x64** and **~25 KB FLAT on
aa64**, where the percentage only tracks binary size. AGT measured 3.1-4.4% on
aa64 against a doc that said -15%.

**The mechanism**, and a claim this document previously got wrong:

    -fasynchronous-unwind-tables   x86_64-elf [enabled]   aarch64-none-elf [disabled]
                                   aarch64-linux-gnu [ENABLED]

It is a **per-TARGET default, not an architecture property** — same
architecture, opposite default one triple over. It is also not a mandate on
x64: `-fno-asynchronous-unwind-tables` takes a 6-function TU's 144 B of
`.eh_frame` to 0. What differs is ASYNCHRONOUS vs ON-DEMAND: aarch64-none-elf
emits tables when asked (`-fexceptions` gives 144 B); x86-64 also defaults the
async form on, which serves debuggers/profilers rather than exceptions.

---

## 5. Three rungs, measured (1500 REACHED functions, x64)

| | size | vs default | diagnostics | can catch? |
|---|--:|--:|---|---|
| A default `_eh` | 627,769 | — | full | yes with `-fexceptions` |
| **B `-fno-asynchronous-unwind-tables`** | 543,289 | **-13.5%** | **full** | **yes** |
| C `--no-eh-frame` | 515,815 | -17.8% | type + `what()` via interceptor | no (refused) |

**B recovers 75% of C and gives up nothing.** Verified under QEMU: the full
standard diagnostic survives, and `-fexceptions -fno-asynchronous-unwind-tables`
passes **all 7** cases of `cxx-exceptions-selftest` including throw-and-catch
from a global constructor before `main` and every destructor on the unwind path
running exactly once. `-fexceptions` implies SYNCHRONOUS tables, which are
exact at call sites — all exceptions need.

Two combinations measured as **byte-identical**, so do not re-test:
`--no-eh-frame + -fno-async` == `--no-eh-frame` (515,815 both), and
`aa64 + -fno-async` == `aa64` default (603,997 both).

**MEASUREMENT TRAP that cost two false readings:** the saving scales with
**REACHED** code. A small fixture shows 512 bytes; a 1500-function program
where `--gc-sections` collects 1499 unreferenced ones also shows 512 bytes.
Only when all 1500 are genuinely called does the 84,480 appear.

---

## 6. THE PENDING TASK — approved, not started

Add `-fno-asynchronous-unwind-tables` to **x64's** `CFLAGS_BASE` so `libaxl.a`
itself stops carrying async unwind tables.

### Measured benefit (controlled — one variable changed)

Same consumer object, same flags, only `libaxl.a` differs, default `_eh` build,
JSON-heavy consumer:

| `libaxl.a` built | `.efi` | `.eh_frame` in `.so` |
|---|--:|--:|
| with async tables (today) | 207,509 | 23,088 |
| without | **195,733** | **11,344** |

**-11,776 bytes (-5.7%)**, image `.eh_frame` halved. Residual 11,344 is
libstdc++/libsupc++ (see §7). Applies ONLY to the default `_eh` C++ build:
zero effect on C-only images and on `--no-eh-frame` images, both of which
already collect `.eh_frame` via `--gc-sections`.

### Why it is believed safe

The risk would be a C++ exception unwinding THROUGH an AXL C frame — a throw
inside a callback invoked from `axl_loop`, `AxlTaskProc`, the JSON `foreach`
trampoline. **`AXL_CB_NOEXCEPT` makes that impossible and the compiler enforces
it** (`docs/AXL-Cxx-Design.md` §6b): a throw cannot cross a C callback
boundary, it terminates there. And the crash handler walks the **frame-pointer
chain** — `-fno-omit-frame-pointer` is in `CFLAGS_BASE` for exactly that, and
`rsod-decode.py` resolves DWARF line info, not `.eh_frame`.

### The two verifications Mike agreed to BEFORE the change lands

Both exist because this session twice caught itself asserting from a comment
rather than a run.

1. **Crash-handler backtrace still resolves.** Build `crashhandler` +
   `crashtest`, run under QEMU, confirm `rsod-decode.py` produces a backtrace
   against a `libaxl.a` built WITHOUT async tables. The frame-pointer argument
   is sound but unproven end-to-end.
2. **No `-fexceptions` consumer regresses.** Run
   `test-cxx-exceptions-qemu.sh` against that library — the case where
   unwinding through AXL frames would matter if the `noexcept` boundary ever
   leaked.

### How to build the variant WITHOUT editing the Makefile

`CFLAGS_BUILD` is a plain `=` variable, so a command-line override composes.
**APPEND, do not replace** — replacing dropped `-g -gdwarf` and produced a
bogus 16.3 MB -> 2.5 MB reading:

```sh
AXL_CPP=1 make -s BUILD=RELEASE \
  CFLAGS_BUILD="-Os -g -gdwarf -ffunction-sections -fdata-sections -DNDEBUG -fno-asynchronous-unwind-tables"
```

The real RELEASE value is at `Makefile:350`; the DEBUG one at `:352`. Confirm
the effect with:

```sh
x86_64-elf-size -A out/native-x64-release/lib/libaxl.a | awk '/\.eh_frame/{s+=$2} END{print s}'
# 158976 today, 0 with the flag
```

### Then

Edit `CFLAGS_BASE` (x64 only — aa64 gains nothing, measured), full rebuild
both arches, `./scripts/install.sh --arch all --cpp`, `./scripts/verify.sh`,
`./test/integration/run-integration.sh`, plus the four gates verify.sh omits
(`check-no-avx`, `check-nx-compat`, `check-bss-clear`, `check-reloc-coverage`).
**A `CFLAGS` change wipes objects and archives** via the build-state signature,
so expect a full rebuild and do not run it concurrently with anything.

---

## 7. Possible follow-on: a toolchain rebuild — **REFUTED, see below**

> **CLOSED 2026-08-17 by measurement.** `-fexceptions` forces the unwind
> tables regardless of `-fno-asynchronous-unwind-tables` (byte-identical on
> two TUs; the `-fno-exceptions` control moves 1,992 -> 0), and libstdc++ is
> exception-enabled, so the rebuild saves **zero**. The residual also is not
> what this section claims: `libsupc++` contributes 0, a quarter is libgcc,
> and 18% is the consumer's own object. Full attribution and numbers in
> `docs/AXL-Cxx-Unwinder-Design.md` §U6. Do not re-open.


The residual 11,344 bytes of `.eh_frame` in a default `_eh` image comes from
**libstdc++/libsupc++**, which are prebuilt by the toolchain. Rebuilding them
with `-fno-asynchronous-unwind-tables` would in principle shrink that too.

**UNMEASURED and UNVERIFIED — treat as a hypothesis.** The reason to be careful:
libstdc++ is compiled WITH exceptions, so it needs SYNCHRONOUS tables and
removing the async ones should be safe by the same argument as §6 — but
"should be" is exactly the shape of reasoning this session kept catching. If
pursued: ARM ships its exact build recipe in
`<prefix>/14.3.rel1-*-manifest.txt` (see
[[reference_arm_ships_its_build_manifest]] in memory) — read it before
theorising about what the toolchain was configured with.

Do §6 first; it needs no toolchain work and delivers most of the value.

---

## 8. AGT correspondence state

AGT is green against HEAD, has **adopted `--no-eh-frame` on all four link
sites**, and needs nothing. x64 fleet 16.40 -> 14.12 MB across 34 binaries.

Replies written (in `../agt-prompts/`, not a git repo):
- `2026-08-17-axl-sdk-reply-eh-frame-optout.md`
- `2026-08-17-axl-sdk-reply-noeh-arch-mechanism.md` (carries the psABI
  correction and the §5 three-rung table)

Mike has a paste-prompt for the AGT session covering the correction and rung B.
**Nothing is owed to AGT.**

---

## 9. Traps this session paid for

1. **A default is not a requirement, and a target triple is not an
   architecture.** I asserted "x86-64's psABI requires async unwind tables";
   `aarch64-linux-gnu` has it enabled, which refutes the architecture framing
   in one command. Third instance in a week of inferring a cause from an
   observation.
2. **A sabotage must rebuild the tree the test STAGES from.** `install.sh`
   stages RELEASE; a bare `make` builds DEBUG. Two false "NOT DETECTED"
   readings. Hash the artifact the test actually links.
3. **Change ONE variable.** Replacing `CFLAGS_BUILD` instead of appending
   dropped `-g` and produced a 6.5x bogus reading.
4. **Measure REACHED code.** `--gc-sections` collects unreferenced functions
   and their CFI, so a synthetic benchmark must actually call what it builds.
5. **A green fixture is the weakest evidence for a SEAM.** Every defect the
   independent review found was a call SHAPE the one in-tree fixture never
   used — a `const` handle, a bare `nullptr`, a function name instead of a
   lambda, an algorithm returning an iterator.
6. **Template members are only instantiated on use** — untested means never
   COMPILED.
7. **Unsequenced side effects in one argument list.** gcc evaluates
   right-to-left; two side-effecting calls in one `axl_printf` reported a
   correct implementation as broken. Hit **three times** in one file.
8. **Grep the FAIL text, not the PASS text**, and not a message that changes
   when it fails (`no leak report (found 0)` becomes `(found 1)`).
