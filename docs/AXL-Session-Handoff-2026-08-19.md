# Handoff — 2026-08-19: driver C++ ctors, image stripping, and the floor

> Self-contained. Every number was measured on this tree on this date.
> **Supersedes `docs/AXL-Session-Handoff-2026-08-18.md`**, whose §0 is fully
> closed (all three tasks done and CI-verified).
>
> Working tree clean apart from Mike's untracked `SCRATCH.txt` and several
> `docs/AXL-*.md` drafts — **do not commit those, and never `git add -A`.**
>
> **Nothing is pushed.** Seven commits sit on local `main` awaiting approval.

---

## 0. START HERE — the one open task

**Break the backend's log dependency so `--minimal-runtime` actually saves
bytes.** Everything else this session is finished.

### The problem, measured

`--minimal-runtime` now *means* minimal (everything opt-in, §3) but **saves
essentially zero bytes**, and the reason is a single dependency edge. From
`ld -Map --cref` on a do-nothing minimal-runtime image:

```
axl-crt0-minimal.o   --(gBS)-->          axl-backend-native.o
axl-backend-native.o --(axl_log_full)--> axl-log.o
axl-log.o            --(axl_strlen)-->   axl-str.o --> axl-format.o ...
```

**The backend logs.** Every image touches `gBS`, so every image pulls
`axl-backend-native.o`, which references `axl_log_full`, which pulls the log
layer, which pulls the printf engine. **51 archive members** land in an image
that does nothing — including `axl-digest-sha256.o` and `axl-driver.o`.

The weight that follows: `axl_vformat` 4,087 B, `log_dispatch` 1,541,
`axl_dtoa` 1,084, `axl_log_init_from_env` 714, `kCachedPowers` 696,
`axl_console_readline_ex` 798.

Two things already ruled out by measurement, so don't redo them:

- **Dropping `$(PORTING_OBJS)`** from the link — no change. (They *do*
  reference `axl_stdout`/`axl_fopen`/`axl_getenv`, but removing them still
  left the image at 36,864.)
- **Weak-linking the CRT0's calls** — done, and it changed nothing, because
  the pull is from the backend, not the CRT0.

### The suggested direction (Mike's)

Give the backend **logging hooks / callbacks or a vtable** instead of a direct
call to `axl_log_full`, so an image that never logs never links the log layer.

Design notes worth having before starting:

- The precedent already in-tree is the **weak-symbol** pattern:
  `axl_cxxrt_init` (`src/runtime/axl-cxxabi.c:95`) is declared
  `__attribute__((weak))` and called under a NULL check, so it costs a pure-C
  image nothing. A weak `axl_log_full` would be the smallest possible change
  and needs no new type.
- A vtable/callback is the bigger hammer and buys indirection nobody has asked
  for. Prefer the weak reference unless something needs *runtime* swapping.
- **The trap to avoid:** a backend that silently stops logging is worse than
  one that costs bytes. Whatever the mechanism, an image that DOES link the
  log layer must log identically, and one that does not must be provably
  silent — not accidentally silent. Grep for `axl_log_full` /
  `axl_debug|axl_info|axl_warning|axl_error` in `src/backend/` first and count
  the sites; that census is the scope of the task.
- Re-measure with the same `ld -Map --cref` recipe. `docs/AXL-Minimal-Image-Notes.md`
  documents the method; `sdk/examples/hello-minimal.*` is the ~4.6 KB floor to
  aim at.

### How to know it worked

```sh
make ARCH=x64 BUILD=RELEASE all
# link a do-nothing app against the minimal CRT0 and read the size
```

Today: full runtime 37,376 · `--minimal-runtime` 36,864 · no-libaxl 4,608.
Anything that moves the middle number materially is the win.

---

## 1. What shipped this session (7 commits, unpushed)

| commit | what |
|---|---|
| `d39f871b` | CI: `needs: build`, apt lists trimmed to host tooling |
| `0f33f58b` | docs: CI job graph + host-tooling rule (§11.5) |
| `09e23436` | CI: header said "NOT a per-push gate" above `push:` |
| `ef081b5b` | docs: §11.6 — the prose path filter |
| `ce082987` | **fix(driver): a driver image registered C++ ctors and ran none of them** |
| `bed1b2f5` | docs: the C++ driver lifecycle contract + the per-image floor |
| `9e425b5d` | **fix(backend): the event close ring is a GUARD, not a diagnostic** |
| `f8b634b2` | **fix(run-qemu): stale scratch dir made every run exit 1 silently** |
| `aa3c542e` | **build: strip every `.efi` (~20%), `--minimal-runtime` truly minimal, hello-minimal** |
| `434128c3` | docs: the floor table was wrong twice |

### 1a. Driver C++ constructors (`ce082987`)

A driver image registered `.init_array` entries and ran none of them —
`AXL_APP` reached the walker via `_axl_init`, `AXL_DRIVER` reached
`axl_driver_init`, and only the first called
`_axl_cxxabi_run_init_array`. Fixed for all three DriverEntry macros.

**Destructors were not a free choice.** A C++ static destructor emits **no
`.fini_array` entry at all** (measured, both arches) — it registers at run time
via `__cxa_atexit` → `axl_atexit`, and `axl_atexit` refuses registration while
its table is NULL. So walking `.init_array` without `_axl_atexit_init()` first
would have dropped every destructor silently.

**Two exit paths, and the second one bit me.** EDK2 reclaims a failed
`DriverEntry` through `CoreUnloadAndCloseImage`, which never calls `Unload` —
so a refused load leaked everything. Found by independent review, not by me.

New public API: `axl_driver_cleanup()`. Tests:
`test-cxx-driver-ctors-qemu.sh`, 17 assertions, five driver images, both
arches.

### 1b. Image stripping (`aa3c542e`)

Every `.efi` carried a COFF symbol table the firmware never reads. Now stripped
in both build paths (the generated CMake package delegates to `axl-cc`, so it
is two sites, not three), guarded by `check-pe-stripped`, which reads the
**artifact** rather than comparing command lines.

    smallest AXL app  47,365 -> 37,376   do.efi      70,731 -> 56,832
    hello.efi         59,914 -> 47,616   Hexview.efi 552,346 -> 435,200
    hello.efi (aa64)  69,203 -> 50,190   SoftBmc.efi  -0.6%  <- counter-case

`.so` keeps every symbol; `pe-set-debug` points the PE debug directory at it.

### 1c. `hello-minimal` (`aa3c542e`)

`sdk/examples/hello-minimal.{c,cpp}` — prints one command-line argument,
links no libaxl, **4,608 bytes** (x64) / 5,134 (aa64), C and C++
byte-identical. Compile-gated by `check-examples`, booted both arches by
`test-hello-minimal-qemu.sh`.

---

## 2. Traps this session paid for (read before touching sizes)

1. **`.bss` is not file bytes.** `PointerToRawData` is 0. `nm --size-sort`
   lists `.bss` beside `.text`, which is how I reported a 6 KB `.bss` table as
   part of a 47 KB *image* floor — to a consumer who then did fleet arithmetic
   on it.
2. **Never name a subtraction.** I computed "headers + padding" as
   file-size-minus-sections and labelled the residual. Most of it was the
   symbol table. Same error shape as (1), twice in one thread.
3. **A comment can misdescribe the code.** `mEventCloseRing` is called a
   "debug ring", is dated `DIAG 2026-04-27`, and **`return`s** — it is a crash
   guard preventing a DxeCore `#GP`. I described it to Mike as a diagnostic and
   was asked to act on my own description.
4. **A field nothing writes is optimised out.** A `static unsigned` intended to
   observe `.bss` compiled to `mov $0x0` — a tautology I had documented in
   three places as the load-bearing proof. `volatile` fixes it.
5. **`strtonum` is gawk-only** and CI runs mawk. Now gated by
   `check-awk-portability`.
6. **UEFI is the MS x64 ABI.** Entry *and every firmware function pointer* need
   `ms_abi`; it fails at run time with a bare `#GP`. aa64 has one convention,
   so an aa64-only test passes and x64 breaks.
7. **A wrong struct offset is not reliably a crash** — one build faulted, the
   other hung.
8. **`pkill -f`/`pgrep -f` match their own command line.** A `pkill` in a
   compound command killed its own shell and silently skipped the `git push`
   that followed.

---

## 3. `--minimal-runtime` as it stands now

```
--minimal-runtime             nothing
--minimal-runtime=stdio       force the stream layer in
--minimal-runtime=args        give main() its argc/argv
--minimal-runtime=stdio,args  the behaviour it had before
```

Mechanism is linkage: the CRT0 references those symbols **weakly**, and
`axl-cc` turns each feature into `-u SYMBOL`, which pulls the archive member
that defines it. stdio is self-correcting — an app calling `axl_printf` pulls
`axl-stream.o`, which *defines* `axl_stream_init`. An app that reaches
`axl_print` without it gets `-1`, not a fault.

`exit-status-selftest-minimal` (the one in-tree consumer) arms `argv[1]` and
now asks for `-u _axl_args_init -u axl_stream_init`.

---

## 4. Consumer state

- **AGT** decided against the shared-driver pattern for size: 34 launchers
  ≈ 3.87 MB vs one multi-call binary ≈ 1.2 MB. They costed a *zero* floor and
  it still lost. They verified this session's driver fix against a local
  checkout: `make test` 9918/0 both arches, `test-visual` 63/63, `axcon.so`
  carries the walker on both arches. **They stay on `v4.2.0`; nothing
  consumed.**
- **Replies pending in `../agt-prompts/`**:
  `2026-08-19-axl-sdk-reply-floor-correction-and-eh.md` (+ its paste prompt)
  corrects the `.bss` error in their fleet table. Not yet sent when this was
  written.
- **axl-utils** (`do.efi` 70,731 / `doDriver.efi` 281,596) is the consumer
  `hello-minimal` would actually help — it already has the launcher+driver
  shape, and per-command named launchers at ~4.6 KB become affordable.

---

## 5. Open, not started

- **AGT Finding 2**: `AXL_QEMU_TMPDIR` has an undocumented 108-byte limit
  (UNIX socket path). `make test` passes, `make test-visual` reports all 63
  failing with zero PNGs — reads exactly like a render regression. Validate at
  the point of use and fail loudly. `scripts/run-qemu.sh:790` documents the
  override but not the constraint.
- **Audit the 29 staged-SDK integration tests** for silent dependence on a leak
  verdict they cannot get (`install.sh` stages RELEASE, where `AXL_MEM_DEBUG`
  is off). Offered, not done.
- **`mEventCloseRing`'s depth** stays 256 in both builds deliberately; only its
  forensics are `AXL_MEM_DEBUG`-only.

---

## 6. Gate state at handoff

`verify.sh` ALL GREEN — 23 gate lines, both arches, **10,497** unit assertions.
Integration **X64 161/0**, **AARCH64 63/0** (before `hello-minimal`'s test,
which passes 5/5 on both arches standalone).

New gates this session: `check-pe-stripped`, `check-awk-portability`, and
`check-cxx-entry` extended to three macros × both arches in CI.
