# AXL Roadmap

The lean entry-point for AXL planning. **What shipped** lives in
[CHANGELOG.md](../CHANGELOG.md) (the authority, per version). **How a big
piece is designed** lives in its own design/plan doc (indexed below).
**Full historical phase detail** (every completed checkbox, hardware
findings, etc.) is preserved in
[ROADMAP-Archive.md](ROADMAP-Archive.md) — this file is the curated
front page that points into all of it.

Legend: `[x]` done · `[-]` in progress · `[ ]` pending.

---

## Design & plan docs (index)

Library / SDK foundations:
- [AXL-Design.md](AXL-Design.md) — library design (phases, API spec, style)
- [AXL-SDK-Design.md](AXL-SDK-Design.md) — SDK (toolchain, packaging)
- [AXL-Cxx-Stdlib-Handoff.md](AXL-Cxx-Stdlib-Handoff.md) — **DONE 2026-08-06.**
  `axl-c++ --hosted` ships, and `std::vector` / `std::string` / `std::map` /
  `std::unordered_map` run under UEFI on both arches
  (`test/integration/test-cxx-hosted-qemu.sh`). **§8's distribution question
  CLOSED 2026-08-08: the SDK is self-contained and links no `libstdc++.a` at
  all.** `--hosted` was pulling exactly two members (`tree.o`,
  `hash_bytes.o`); `src/runtime/axl-cxx-rbtree.cpp` and `axl-cxx-hash.cpp`
  supply those eleven functions clean-room under Apache-2.0, so the one act
  the GCC Runtime Library Exception does not cover — redistributing the
  runtime library — simply does not arise. `-frtti` remains the exception and
  links the consumer's own installed archive.

  **Not the same as "everything links"**, and the first commit message said
  "eleven functions were the whole gap", which was wrong. `std::list` and
  `shared_ptr`/`make_shared` still do not link — pre-existing, not
  regressions, and now one clean undefined symbol instead of an `_Unwind_*`
  cascade. Surveyed in AXL-Cxx-Stdlib-Surface.md §3b.

  `axl_rb_check_invariants()` / `axl_rb_black_height()` joined the public
  RB-tree API on the way: every RB test verified the tree by in-order walk,
  which proves SORTED and says nothing about BALANCED, so a tree degenerated
  into a list passed them all. Sabotage confirmed the gap was real — dropping
  one recolour in the erase fixup leaves `std::map` answering every query
  correctly while the structure is gone
- **FIXED 2026-08-06 (aa64 relocation split).** An aa64 link producing BOTH a
  linker-synthesized `.rela.dyn` and the script-placed `.rela` at
  non-contiguous addresses was mis-relocated at boot: `DT_RELA` pointed at the
  first, `DT_RELASZ` counted both, and the crt0 walk ran off the end and
  applied the following bytes as relocations. Naming the output section
  `.rela.dyn` makes ld absorb its own internal section instead of orphaning
  it. `make check-reloc-coverage` is the standing guard, and RTTI now runs on
  both arches
- [AXL-Cxx-Streams-Handoff.md](AXL-Cxx-Streams-Handoff.md) — **DONE 2026-08-07.**
  `axl::cout` / `axl::cin` / `axl::cerr` over `axl_printf` / `axl_readline`,
  plus **`axl::string`** — which the handoff did not anticipate. `std::`
  iostreams are out (measured: links, then #PF on a never-constructed
  `std::cout`), and the `axl::` spelling mirrors `std::` because `axl::err` is
  already taken. The `>>` failure model settled on BOTH paths over one sticky
  state: `if (!axl::cin)` for the chained form, `read<T>() -> axl::result<T>`
  for the checked one, with `read<T>()` setting the same bit so a loop over an
  overflowing token terminates. 75 assertions on each arch
  (`test/integration/test-cxx-streams-qemu.sh`).

  `axl::string` exists because the handoff's premise held only half: `<string>`
  is gated behind `bits/requires_hosted.h`, so the FREESTANDING configuration
  the whole layer targets has no owning string for `>>` to fill. It carries
  `std::string`'s interface and forwards the search family to
  `std::string_view` (freestanding, so those are libstdc++'s own algorithms). This reverses one sentence in `axl-cxx.hpp`'s file docs, which
  said there should never be an `axl::string` — true on `--hosted`, and the
  header now says which half applies where.

  **`axl::string` re-shaped 2026-08-08.** It shipped as a skin over
  `AxlString`, which `AXL-Cxx-Design.md` §4.5 had already measured and
  rejected ("GO for the structure, NO-GO for the skin") — I had not read that
  section before choosing the shape. Now standalone with a 23-byte
  small-string optimisation, sized so `\EFI\BOOT\BOOTX64.EFI` stays inline.
  Same benchmark before and after on one box, N=200000: ctor 33279 -> 4790 us
  (6.9x), copy 31120 -> 3324 us (9.4x). `AxlString` keeps the streaming-builder
  job §4.5 measured it tied on.

  The staged pre-commit review found **13 further bugs against a green
  suite** — four of them regressions this work introduced. Chief among them:
  fixing the `grow()` spin made the buffer-less paths SUCCEED where they had
  hung, but nothing wrote the terminator into that first allocation, turning a
  hang into a silent heap overread across `reserve()` and all three
  `prepend` variants. Full account in the handoff's §6b.

  Four latent bugs surfaced on the way and are fixed here: `axl_string_steal()`
  followed by any append **spun forever** (`grow()` sized the replacement by
  doubling a capacity the steal had set to 0); `len + need + 1` could wrap and
  let `grow()` "succeed" without resizing; `s += s.str()` read the buffer
  `grow()` had just reallocated; and **`CXXFLAGS` carried no `-MD -MP`**, so no
  `.cpp` object had ever had a header dependency — editing any `.hpp` rebuilt
  nothing, which made a sabotage of `axl-istream.hpp` read as UNDETECTED
- [AXL-Cxx-Unwinder-U1-Handoff.md](AXL-Cxx-Unwinder-U1-Handoff.md) — session
  handoff from the C++ arc: repo state, what shipped, and the traps.
  **Its "U1 is BLOCKED on the exceptions decision" framing is superseded** —
  the decision was taken 2026-08-10 (YES, with the C-boundary invariant);
  see `AXL-Cxx-Unwinder-Design.md` §2-RESULT
- [AXL-Cxx-Toolchain-Handoff.md](AXL-Cxx-Toolchain-Handoff.md) — **START
  HERE for the C++ toolchain work. WORKING 2026-08-10, 7 commits
  unpushed.** Real `try`/`catch` plus `std::vector`/`string`/`map` and
  `<stdexcept>` run under UEFI on BOTH arches, 7/7 under QEMU, with no
  host packages, no vendored runtime and no hand-written ABI layer. The
  fix was not a runtime: it was building an `x86_64-elf` **bare-metal
  toolchain** (`toolchain/`) so x64 matches what aa64 already had from
  ARM. Everything that failed did so because x64 was borrowing the
  host's glibc-targeted g++, whose libsupc++ keeps `__cxa_eh_globals` in
  `__thread` storage and reads a stack canary from `%fs:0x28` — neither
  of which UEFI provides. **WIRED IN 2026-08-13 (task T2)** — `axl-cc`, the
  Makefile and the generated CMake package all select our own
  `x86_64-elf-g++`, so C++ compiles bare-metal on both arches and the
  `.deb`/`.rpm` depend on nothing from the host. That surfaced one silent
  defect the spike's hand-link had hidden: GCC's `x86_64-*-elf` target emits
  global constructors into `.ctors`, which AXL's crt0 does not walk, so NONE
  of them ran. Fixed in the toolchain (`--enable-initfini-array`, published as
  `14.3.0-axl2`); see `AXL-Cxx-Design.md` §6a-T2. Exceptions themselves are
  still NOT wired — that is U2/U3 in `AXL-Cxx-Unwinder-Design.md`, and the
  7/7 demo remains hand-linked

- [AXL-Newlib-Investigation.md](AXL-Newlib-Investigation.md) — **the
  MEASUREMENTS behind the direction above; its "NOT SCHEDULED" status and its
  `_sbrk` blocker are both superseded by AXL-Libc-Substrate-Design.md.**
  Originally recorded because the option surfaced while solving the C++
  exception problem. Newlib as the libc substrate with the GLib-shaped
  `axl_*` API layered on top: the API identity and the substrate are
  orthogonal. Licence verified permissive (BSD-family, Red Hat + UC
  Regents, no copyleft). The libgloss port is 35 hooks, and nine of them
  are file operations `src/fs/axl-fs.c` already implements against the
  same `EFI_FILE_PROTOCOL`. **The blocker is `_sbrk`**: a linear-heap
  model against `AllocatePool` would carve a fixed arena, making the
  firmware's memory map inaccurate and costing the allocator
  instrumentation the leak gate depends on. So the question worth spiking
  is newlib-minus-malloc. Already proven in passing: newlib's HEADERS
  plus AXL's implementations ran containers and exceptions 7/7 on aa64.
  **That `_sbrk` objection is answered by inverting the allocator** rather
  than bridging to it — see the design doc — and newlib-minus-malloc is no
  longer merely "worth spiking": `src/cxxrt/` is a working instance of it

- [AXL-Libc-Substrate-Design.md](AXL-Libc-Substrate-Design.md) — **DIRECTION
  AGREED 2026-08-11.** `axl_*` sits on newlib the way GLib sits on libc, with
  the ALLOCATOR inverted rather than replaced: AXL's allocator keeps its
  implementation and takes the STANDARD NAMES, so `malloc` is AXL's over
  `AllocatePool` and `axl_malloc` is the same allocator plus call-site
  attribution. That removes `_sbrk` from the picture entirely, which is what
  `AXL-Newlib-Investigation.md` §4 called the blocker — no carved arena, the
  firmware's memory map stays accurate, and the leak gate keeps working. It
  also makes third-party allocations (mbedtls, lzma, stb, newlib) tracked for
  the first time. `src/cxxrt/` is already the first working instance.
  **§4.1 ANSWERED 2026-08-13 — the substrate stops short of stdio.** Newlib's
  printf DOES reintroduce the cycle: even its INTEGER-ONLY `vsniprintf` arrives
  with `mallocr`/`freer`/`reallocr`, the FILE machinery and `_impure_ptr`, at
  21.5 KB against `AxlFormat`'s 6.7 KB (the general `vsnprintf` is 53.8 KB
  across 47 archive members). `AxlFormat` stays permanently, as the second
  entry in the "AXL implements it because AXL's is better here" list after the
  allocator. Newlib remains the answer for `string`/`math`/`stdlib`.
  **§4.1b DONE 2026-08-13 — C compiles bare-metal on BOTH arches and
  `include/compat/` is deleted.** aa64 moved off the glibc-targeted Linux cross
  too; the compiler comes from `axl-toolchains.conf` with no host fallback, all
  four consumer entry points moved, and `test-axl-cc-hosted-headers.sh` now
  asserts a consumer's `<string.h>` resolves inside the toolchain with nothing
  under `/usr/include`. verify.sh ALL GREEN both arches (10393).
  **§4.1c DONE — the toolchain is published.** `toolchain-x86_64-elf-14.3.0`
  on `axl-sdk-releases`: 55 MB stripped tarball (1.5 GB as built, 235 MB
  stripped), its three upstream source archives for GPL §6(d), and SHA256SUMS.
  `install-toolchain.sh x64` is download-and-verify now, so CI and consumers
  pay a download instead of a 40-minute build.
  **§4b OPEN (not scheduled): alternatives to newlib, and where the seam
  belongs.** The tree uses newlib's HEADERS only — `-nostdlib`, and AXL defines
  all twelve standard-named symbols itself. picolibc and llvm-libc are the
  candidates worth measuring; musl is the wrong shape; edk2-libc is abandoned.
  Measure picolibc's printf against AxlFormat's 6,708 bytes before recommending
  anything.
  Original spike measurement follows. The
  whole tree builds with `CC=x86_64-elf-gcc` (271/271 objects, 0 errors) WITH
  or WITHOUT `include/compat` on the path, a compat-free `AxlTestLog.efi` runs
  67/67 with no leaks under QEMU, and the entire cost of retiring
  `include/compat/` for C is TWO fixes: `__assert_func` (15 refs from
  `deps/sdefl` via newlib's real `<assert.h>`) and the `time()` signature clash
  in `axl-mbedtls-platform.c`. AXL's own code never used compat at all — it
  includes only `stddef`/`stdint`/`stdbool`/`stdarg` — so compat is entirely a
  third-party shim. NOT yet measured: aa64, the C++ hosted path
- [AXL-Cxx-Unwinder-Design.md](AXL-Cxx-Unwinder-Design.md) — **U0 DONE 2026-08-09.**
  Tier 2 (the unwinder) reframed by measurement: our own `-fno-exceptions`
  objects reference **zero** `_Unwind_*` symbols, so three of the four things
  the tier claims to unblock need no unwinder at all. `std::list` + `sort` +
  `reverse` is already **proven running under QEMU** against five clean-room
  `_List_node_base` functions with no `libstdc++.a` — the same pattern as
  `tree.o`/`hash_bytes.o`. Phase U0 finishes that and is worth doing
  regardless. The actual unwinder is TWO libraries (level 1 `_Unwind_*` plus
  level 2 `__cxa_*`/personality), and it reverses `axl-cxx.hpp`'s
  "`-fno-exceptions` is not negotiable" — a decision before it is a project.
  **§2 DECIDED YES 2026-08-10; U1 SUPERSEDED 2026-08-10.** Neither library is
  vendored nor written: a bare-metal `x86_64-elf` toolchain supplies both
  levels, matching what aa64 already had from ARM, and real `try`/`catch` plus
  `std::vector`/`string`/`map` now run **7/7 under QEMU on both arches**.
  `deps/libunwind`, `src/cxxabi/` and `check-cxxabi-oracle` were removed as
  the level-2 track they served is cancelled. **PENDING:** none of it is wired
  into `axl-cc`/`axl-c++` or the Makefile — the runs were hand-linked — and a
  libstdc++ emergency-pool leak must be cleared before it can ship, because
  the leak gate is a hard gate. See
  [AXL-Cxx-Toolchain-Handoff.md](AXL-Cxx-Toolchain-Handoff.md)
- [AXL-Cxx-Stdlib-Surface.md](AXL-Cxx-Stdlib-Surface.md) — **measured**
  table of which STL facilities work freestanding, which need
  `--hosted`, which are opt-in (`-frtti`), and what the remaining "no"s
  would actually cost — in symbols, not adjectives. Four tiers, only one
  of which is a real wall (glibc's locale subsystem)
- [AXL-Cxx-Design.md](AXL-Cxx-Design.md) — **C0 + C1 SHIPPED 2026-08-06.** The
  layer writes neither containers nor algorithms: the standard supplies both,
  and what AXL supplies underneath them is `operator new`/`delete` over
  `axl_malloc`, the five `std::__throw_*` entry points `-fno-exceptions` calls
  instead of throwing, a `ceil` shim (x64 only — aa64 folds it to `frintp`),
  its own `_Prime_rehash_policy` so `std::unordered_map` links without an
  AVX-carrying archive member, and `axl::arena_allocator` for paths that must
  not halt on OOM. §4's spikes of which C structures to
  build ON versus BESIDE stand as the reasoning that got there: `AxlHashTable` skins at
  measured parity, `AxlTree` loses 1.4x on lookup to a templated AVL, and an
  `AxlArray` skin is memory-UNSAFE for non-trivial `T` (demonstrated
  use-after-free), and an `AxlString` skin loses 9x on short-string
  construct/copy because it cannot express inline storage. Three no-go
  verdicts, three different reasons, none predicting the others
- [AXL-Distribution-Design.md](AXL-Distribution-Design.md) — DRAFT: how the SDK is
  packaged, installed, discovered and version-pinned; the missing tarball and
  CMake toolchain file; consuming an unreleased checkout
- [AXL-Float-Conversion-Design.md](AXL-Float-Conversion-Design.md) — IMPLEMENTED 2026-07-31
  (21 commits on `3be79c4a`): correctly-rounded string <-> double, the integer reverse,
  `%f/%e/%g` in `axl_sscanf`, and one truncation convention across the renderers ·
  [AXL-Float-Conversion-Plan.md](AXL-Float-Conversion-Plan.md) — the implementation plan
- [AXL-Stream-Backend-Design.md](AXL-Stream-Backend-Design.md) — IMPLEMENTED 2026-07-31:
  AxlStream opened to consumer-supplied backends (`AxlStreamOps` +
  `axl_stream_open_custom`), capability queries, and the sink-boundary fault
  injection that reaches its backend-error paths. §8b landed on top: the Layer-2
  helpers loop over short transfers, positional I/O sets `err`, and `axl_fclose`
  stopped trying to free the static console streams. §11 migrated `axl_bufopen`
  onto the public constructor and §12 closed the last gap with
  `axl_stream_ctx` — a consumer can now build a stream-keyed accessor, and
  `axl-stream-buf.c` no longer includes any private header. §13 finished the
  job: `axl_fopen`, `axl_text_stream_wrap` and `axl_compress_writer` are built
  through the public constructor too, so nothing fills `struct AxlStream`
  directly except the static console streams. §14 closed the one gap §13
  recorded — a wrapper reading below its source's transcode — with **no new
  public API**: filters require their peer at `AXL_ENC_UTF8`, where the public
  `axl_read`/`axl_write` ARE the wire calls, and refuse anything else. The
  private header now exports nothing and has one consumer, and the same rule
  fixed two live corruption bugs in the compress filters
- [AXL-AP-Worker-Pool-Handoff.md](AXL-AP-Worker-Pool-Handoff.md) — HANDOFF: per-core
  XCR0/AVX enable on APs, and the arch-divergent StartupAllAPs semantics a persistent
  AP worker pool runs into (from NightRun's MP-Services bring-up)
- [AXL-Native-Backend-Design.md](AXL-Native-Backend-Design.md) — the native UEFI backend / CRT0
- [AXL-Coding-Style.md](AXL-Coding-Style.md) · [AXL-Lifecycle.md](AXL-Lifecycle.md) · [AXL-Concurrency.md](AXL-Concurrency.md)
- [AXLMM-Design.md](AXLMM-Design.md) — C++ (`libaxl-cxx`) bindings plan
- [AXL-EFI-Encapsulation-Plan.md](AXL-EFI-Encapsulation-Plan.md) — public-API UEFI-type hygiene / portability
- [AXL-Loop-Reentrancy-Plan.md](AXL-Loop-Reentrancy-Plan.md) — remediate blocking-on-a-running-loop (re-entrancy guard, deferred HTTP responses, async-first services, `axl_yield` split)
- [AXL-vs-EDK2-Scope.md](AXL-vs-EDK2-Scope.md) — what we replace vs deliberately omit; audience-facing gap list
- [AXL-Porting-Guide.md](AXL-Porting-Guide.md) · [RELEASING.md](RELEASING.md)

Subsystems:
- Drivers: [AXL-Driver-Authoring-Design.md](AXL-Driver-Authoring-Design.md) · [AXL-Driver-Authoring-Guide.md](AXL-Driver-Authoring-Guide.md) · [AXL-Shared-Driver-Recipe.md](AXL-Shared-Driver-Recipe.md) · [AXL-Network-Driver-Bundle-Design.md](AXL-Network-Driver-Bundle-Design.md)
- Graphics / UI: [AXL-Compositor-Design.md](AXL-Compositor-Design.md) · [AXL-Pointer-Cursor-Design.md](AXL-Pointer-Cursor-Design.md) · [AXL-Display-Design.md](AXL-Display-Design.md) · [AXL-Transform-Design.md](AXL-Transform-Design.md) · [AXL-Rich-UI-Plan.md](AXL-Rich-UI-Plan.md)
- Data: [AXL-JSON-Design.md](AXL-JSON-Design.md) — JSON redesign. **All phases done except P13**:
  one parser, `AXL_JSON_STRICT` is RFC 8259 verified against JSONTestSuite,
  granular JSON5 dialect flags, `get_number_str` + `ALLOW_NAN_INF`. Extended
  2026-07-29 by an API-completeness review against Jansson / JSON-GLib / yyjson
  → **two engines, four faces** (scan and emit are the engines; whole-document
  forms are conveniences over them), no reference counting, structured
  `AxlJsonError`, source/sink abstraction for Jansson I/O parity, one
  `AxlJsonType` vocabulary, and a deferred pull scanner. Execution order in the
  doc: `\uXXXX` decode fix ✅ → P9 errors ✅ → P10 source/sink ✅ → P11 type +
  `value_*` mirror + object iteration + public `axl_json_decode_string` ✅ →
  P5 writer formatting ✅ → P6 `ENSURE_ASCII` + `SORT_KEYS` ✅ → P7 reader
  `REJECT_DUPLICATES` + all three UTF-8 modes on read ✅ → P8 container-scoped
  writer overrides ✅ → P15 `axl_json_error_format` ✅ → P14
  `axl_json_get_double` ✅ → P12 pull scanner ✅ → P13 incremental input ✅.
  **All phases done.** P13 landed 2026-08-03: `AxlJsonScanner` reads from a
  pull `AxlJsonSource` through a window it owns and refills, at O(largest
  token) rather than O(document). A token straddling a refill is re-scanned
  from its start rather than resumed, so the five leaf scanners stay shared
  with the contiguous path — the design doc's premise that this needed a
  resumable sub-token state machine did not survive its own event contract
  (decision 39). P12 landed 2026-08-02/03 in six steps — scanner over refactored leaves, a 4.1M-case
  differential against the old parser, the writer's depth cap to 256, then the
  whole-document face rebuilt on the scanner and the 440-line recursive-descent
  parser DELETED. Recursion is gone from both faces, so `AXL_JSON_DEPTH` is now
  a policy number rather than a stack budget.
  Fuzzing of the read path was RESTORED 2026-08-02: `test/fuzz/json_fuzz` had
  not linked for months, so ASan/LSan coverage of JSON reads was zero for the
  whole redesign. It now builds against the real string substrate, runs a
  matrix of read flags rather than two bare dialects, and `make check-fuzz-link`
  (in `scripts/verify.sh`) keeps it from rotting again. Extended the same day to
  fuzz BOTH directions with oracles (write -> re-read -> compare, and
  plain-vs-`ENSURE_ASCII` spellings must decode alike), which immediately found
  four real writer defects — decisions 34-36 — plus a stack over-read in
  `axl_url_build`, which formats into a 512-byte stack buffer and then copied
  back the length `axl_snprintf` said it WOULD have needed ·
  [AXL-PieceTree-Design.md](AXL-PieceTree-Design.md) · [AXL-RBTree-Design.md](AXL-RBTree-Design.md) · [AXL-Config-Design.md](AXL-Config-Design.md)
- Networking: [2026-07-19-axl-9p-design.md](superpowers/specs/2026-07-19-axl-9p-design.md) — Axl9p 9P2000.L client + server + `fsN:` mount bridge
- Hardware fixtures / test: [AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md) · [HW-Testing-Workflow.md](HW-Testing-Workflow.md)
- CI / release cost: [AXL-CI-Release-Speed-Design.md](AXL-CI-Release-Speed-Design.md)
  — **ACCEPTED 2026-08-13, not yet implemented.** A release costs 75 billable
  minutes and ~60 minutes of waiting, 46 of them in ONE serial job: CI's QEMU
  runner picks `nproc-2` workers and a hosted runner has 2 cores, so the
  `--shard`/`est=` machinery already in `run-integration.sh` goes unused. Plan:
  a `plan` job choosing self-hosted (free, ~9 min) with a sharded hosted
  fallback, a gate policy that reserves full CI for `X.0.0`, and reuse of a CI
  run already green on the release commit's parent. Target ~90 min/month
  against an org-wide ~2,000 allowance that BOTH repos have already breached
  (axl-sdk April, agt June)

Active sub-projects (pre-code planning — see "Active sub-projects" below):
- [AXL-Dashboard-Server-Design.md](AXL-Dashboard-Server-Design.md) — native-SPA dashboard HTTP server
- [AXL-HII-Design.md](AXL-HII-Design.md) — headless HII forms engine (`axl-hii`)
- [AXL-SoftBMC-Port-Design.md](AXL-SoftBMC-Port-Design.md) — SoftBMC EDK2 → axl-sdk port (scoping)

---

## Shipped — milestone summary

[CHANGELOG.md](../CHANGELOG.md) is authoritative; this is the skim. Current:
**v1.7.1**, **6319 unit tests** both arches (X64 + AArch64), native backend
only (gcc + ld + objcopy). `scripts/cut-release.sh` automates the cut.

**Foundations (DONE):**
- **Library core** — AxlMem, AxlString/AxlStrBuf, AxlStream (console/file/buffer
  I-O), AXL_APP, the GLib-aligned data containers (HashTable/Array/List/SList/
  Queue/JSON), AxlLog, AxlLoop, AxlTask. Style + GLib-API-alignment passes done.
- **SDK** — `install.sh` packaging, `axl-cc` driver (app/driver/runtime, debug/
  release, `--run`, `--minimal-runtime`), CMake integration, .deb/.rpm + host-
  tools release artifacts, both arches.
- **Native UEFI backend** — own UEFI headers (manifest-generated from spec HTML),
  CRT0 (native + minimal), no EDK2 / gnu-efi. EDK2 & gnu-efi backends removed.

**Subsystems (DONE):**
- **Networking** — TCP/UDP/HTTP server+client/URL/WebSocket/WebDAV; TLS via
  mbedTLS (`AXL_TLS=1`) incl. `axl_tls_generate_self_signed`; HTTP server
  middleware / static / auth-hook / response-cache / upload-streaming / range.
- **BMC & platform access** — AxlIpmi (4 transports), AxlSmbus, AxlSpd
  (DDR4/DDR5), AxlAcpi, AxlPci, AxlUsb, AxlBoot, AxlNvstore, AxlMemPhys,
  AxlWatchdog, AxlRng, AxlImage; `tools/`: ipmi, lspci, lsusb, memspd, i2c,
  rfbrowse, dmidecode, mkfixture, fetch/grep/find/hexdump/sysinfo/netinfo.
- **Async** — AxlBufPool, AxlAsync (AP offload), AxlDefer, AxlPubsub, AxlEvent /
  AxlCancellable / AxlWait, AxlRuntime (signal/atexit), AxlService.
- **Graphics / UI** — AxlGfx (+ TTF, pixmap, EDID/HiDPI, multi-output),
  AxlCompositor (deferred surface compositor) + AxlCursor + AxlGfxRegion,
  `axl-input` (mouse/key + absolute-pointer/touch seat), AxlTransform. (AGT
  Phase-0 substrate shipped.)
- **Data / text** — AxlPieceTree editor substrate, AxlRegex (linear-time,
  ReDoS-free), AxlFind, AxlShm, cross-app AxlClipboard, AxlBytes, AxlHmac,
  AxlRand, AxlDigest, AxlCompress (gzip/zlib/DEFLATE), AxlTar (ustar), AxlSidecar,
  AxlNTree/AxlTree, AxlRadixTree.
- **Hardware fixtures** — `mkfixture` capture + `axl-emulate` replay (SMBIOS/
  ACPI/SPD/PCI/USB/net/video+EDID/NVMe manifests; HF2.3/2.4/HF4). See
  [AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md).

**Recent consumer-feedback releases (v1.2.0 → v1.7.1):** AxlCompress + AxlTar +
AxlEdid + AxlGfx display/HiDPI (1.2.0); AxlPci cap-walk fix + absolute-pointer
seat (1.3.1); regex `NOTBOL`/`NOTEOL` (1.4.0); AxlCursor absolute tracking +
ConsoleIn-only default + AxlArgs ASCII help (1.5.0) + console ASCII guard
(1.5.1); AxlArgs case-insensitive verbs (1.6.0); `axl_set_exit_status` +
AxlArgs compact DOS flags (1.7.0) + minimal-CRT0 exit-status fix (1.7.1).

---

## Active sub-projects (next up)

Forward-looking work, each with (or getting) its own design doc. Recommended
order is **SoftBMC-port-driven**: scope the port first, then build the
substrate it pulls (dashboard, HII) in consumer-validated order rather than
speculatively.

### SoftBMC — full EDK2 → axl-sdk port  *(flagship; drives the two below)* — [AXL-SoftBMC-Port-Design.md](AXL-SoftBMC-Port-Design.md)
Re-base SoftBMC (the BMC firmware app) off EDK2 onto axl-sdk + AGT, end to end.
- [-] Scoping doc drafted (strategy + mapping + gap list); **next: source-
      inventory pass over the SoftBMC repo** to fill the per-module table
- [ ] Build re-base onto the `axl-cc` native toolchain (no EDK2 tree)
- [ ] Networking on the AXL stack (HTTP/TLS); web dashboard on the native-SPA route
- [ ] BIOS attributes via the headless HII engine + AGT `AgtFormBrowser`
- [ ] Firmware update on AxlAsync + AxlBufPool; VNC on AxlGfx + pointer seat;
      sensors/EC/SEL on AxlIpmi + AxlSmbus + AxlPubsub

### Native-SPA dashboard server — [AXL-Dashboard-Server-Design.md](AXL-Dashboard-Server-Design.md)
Substrate polish on the existing HTTP server. Net-new (much already ships —
range, WebSocket, auth-hook, server-side cache, `add_static`, cert-gen).
Order reflects the SoftBMC inventory (SSE optional — consumer uses WebSocket):
- [ ] Static pipeline: `Accept-Encoding`/gzip negotiation, ETag/304, SPA fallback
- [ ] Session/RBAC **mechanism** + TLS cert **lifecycle** (replaces SoftBMC `Auth.c`/`Session.c`)
- [ ] `multipart/form-data` streaming parser (firmware / virtual-media upload)
- [ ] `axl-json` OData `$select`/`$expand` + `@odata` envelope (defer `$filter`)
- [ ] _(optional)_ SSE pubsub→push bridge — when a one-way stream wants it;
      design the **backpressure** policy first
- Non-goals: HTTP/2, an in-library asset bundler, baked-in role models (see doc)

### Headless HII forms engine `axl-hii` — [AXL-HII-Design.md](AXL-HII-Design.md)
Heavyweight module; phased. AGT renders via `AgtFormBrowser` (out of scope here).
- [ ] Parse + forms model + string resolve (buffer-in core, unit-tested)
- [ ] IFR **expression VM** (suppress/grayout/disable/inconsistentif) — centerpiece
- [ ] Config read / `ConfigResp` round-trip (covers Redfish *read*)
- [ ] Write + default stores (gated; the dangerous phase)
- [ ] Manifest: add HII IFR/package structs to `scripts/uefi-manifest.json5`

### Storage access (NVMe / ATA / SCSI) + SMART — [AXL-Storage-Design.md](AXL-Storage-Design.md)
Platform Access modules for device identity + health (the `smartctl` gap;
`storelib`/RAID is a non-goal). Per-transport, read-first, with a raw
pass-thru escape hatch and a normalized cross-transport health struct.
- [x] Design doc + `axl-nvme.h` contract (contract-first reviewed)
- [x] Phase 1 `AxlNvme` — Identify (Controller/Namespace) + SMART (Get Log Page 0x02) + Device Self-test + raw admin pass-thru; pure decoders unit-tested; `tools/nvme`; `mkfixture` refactored onto it; `test-nvme-qemu.sh` (`-device nvme`) in CI
- [x] Phase 2 `AxlAta` — IDENTIFY DEVICE + SMART (READ DATA + THRESHOLDS) + self-test; pure decoders unit-tested; `tools/ata`; AtaPassThru struct hand-written; `test-ata-qemu.sh` (`ich9-ahci` + SATA disk) in CI. Fixed the directly-attached-SATA device-walk (PortMultiplierPort 0xFFFF sentinel collision)
- [x] Phase 3 `AxlScsi` — INQUIRY (std + VPD 0x80 serial) + READ CAPACITY (16) + LOG SENSE health (IE page 0x2F + Temperature page 0x0D) + raw CDB pass-thru; pure decoders unit-tested; `tools/scsi`; ExtScsiPassThru struct hand-written; `test-scsi-qemu.sh` (`virtio-scsi` disk + CD) in CI. Walk filters phantom LUNs by INQUIRY peripheral qualifier. Self-test + VPD 0x83 deferred to the raw escape hatch
- [x] Phase 4 `AxlSmart` + `tools/smart` — normalized `AxlSmartHealth` rollup over the union device walk (`axl_storage_next` across NVMe/ATA/SCSI) + `axl_smart_health` dispatch + pure per-transport normalizers (`axl_smart_from_*`, unit-tested) + `axl_storage_get_location` (NVMe device-path / ATA port.pmp / SCSI target:lun). `test-smart-qemu.sh` (one device per transport) in CI; NVMe+ATA health end-to-end, SCSI health real-hardware-only
- Non-goals: RAID/HBA mgmt (storelib), block read/write, GPT, destructive typed commands (FORMAT/SANITIZE/fw-download)

### Axl9p — 9P2000.L client and server — [2026-07-19-axl-9p-design.md](superpowers/specs/2026-07-19-axl-9p-design.md)
Both halves of the 9P2000.L wire over `AxlTcp`: a synchronous client (with a
UEFI `fsN:` mount bridge) and an async server exporting an `AxlFs` subtree, so
firmware can read a Linux host's files and a Linux host can `mount -t 9p` the
firmware's. **All five phases DONE** (2026-07-19 → 2026-07-22).
- [x] Design doc + `axl-9p.h` contract, one internal codec shared by both halves so neither side can drift from the other's idea of the wire
- [x] Phase 1 codec + client core — framing/encode/decode, `axl_9p_connect` (`Tversion`/`Tattach`, `msize` clamp), `axl_9p_read_file`, `axl_9p_list`; codec unit-tested in `AxlTest9p`; `test-9p-qemu.sh` drives a guest client against a host Python 9P2000.L server
- [x] Phase 2 client write path — `axl_9p_write_file` (truncate-or-create, chunked), `axl_9p_mkdir`, `axl_9p_remove`, `axl_9p_rename`; chunked multi-`msize` round-trips pinned byte-exact
- [x] Phase 3 client `mount` — `AxlFsProvider` bridge + `axl_9p_mount`/`_unmount` publishing a Shell-visible `fsN:`, `read_only` enforced in the bridge's vtable rather than documented; `9p-mount-selftest.efi` proves the volume end to end. The resident driver descoped to Phase 5
- [x] Phase 4 server `Axl9pServer` — async on the caller's `AxlLoop`, `AxlFs` backend, per-connection fid table, all 15 handlers, a dispatch-level read-only gate, an FNV-1a `qid.path`, and bounded grow/`EXDEV` refusals that keep the single loop from stalling; `test-9p-server-qemu.sh` grades it on a host `p9-client.py`'s own stdout, functional plus adversarial (malformed + pipelined frames, 64-bit offsets, full fid table). Resident driver descoped to Phase 5
- [x] Phase 5 `tools/9p` — one-shot `ls`/`get`/`put`, resident `serve`/`serve-stop` and `mount`/`umount` deploying the embedded `9p-{serve,mount}-dxe.efi` through `AxlService`, plus `status`; `test-9p-tool-qemu.sh` + `test-9p-tool-serve-qemu.sh` gate the launcher on both arches. Pulled Phase 4's deferred `EXDEV` copy-then-unlink fallback into `axl_9p_rename` (bounded: no directories, 32 MiB cap, refuses an existing destination)
- Deviations from the design doc, recorded in its §12: `--listen-ip`/`--source-ip` not implemented (no library API takes a bind address); `9p` excluded from the busybox multiplexer (it links two embedded driver blobs); the headline `mount -t 9p` proof realized as an equivalent host Python client, with the kernel mount documented as manual and explicitly not claimed as tested
- Non-goals for v1: 9P-over-TLS, virtio-9p transport, base 9P2000/`.u` dialects, `Tauth`, mount-side read caching

---

## Open backlog

Grouped, terse; **detail lives in the linked design doc or
[ROADMAP-Archive.md](ROADMAP-Archive.md)**. Most are opportunistic / low-priority.

- **Is `axl-cc` still needed now the SDK ships its own toolchain?** Asked
  2026-08-13. Measured answer: **yes, but not for the reason the question
  assumes** — the toolchain never did this job. `axl-cc hello.c -o hello.efi`
  expands to FOUR commands and 75 arguments: `gcc` with 16 baked-in flags
  (`-ffreestanding -fshort-wchar -fno-builtin -fpic -mno-red-zone
  -mstack-protector-guard=global ...`), `ld -shared -Bsymbolic --no-undefined
  --gc-sections` against a per-arch linker script AND a version script,
  `objcopy` with a 12-entry `-j` list plus `--subsystem`, then `pe-set-debug`.
  None of that follows from having a compiler.

  Two escape routes were considered and both fail on measurement:

  - **Link PE directly, dropping `objcopy`.** Our x64 binutils does carry
    `i386pep`, so x64 could. **aa64 cannot** — ARM's `aarch64-none-elf-ld`
    lists no PE emulation at all (`aarch64elf`, `armelf`, `aarch64linux`).
    That would mean two different pipelines to save one step on one arch.
    LLVM's `lld` does emit arm64 PE (it is how Windows-on-ARM links), but
    adopting it means a second toolchain, against §4.1d's whole direction.
  - **A GCC specs file** (`-specs=axl-app.specs`), which is the GCC analogue
    of the target triple Rust's `x86_64-unknown-uefi` uses to carry exactly
    this policy. It can inject the flags, the linker script and the startfiles
    — but GCC has **no post-link hook**, so `objcopy` and `pe-set-debug` still
    need a wrapper. It would split the policy across two files instead of
    removing one, which makes `check-flag-parity` harder, not easier.

  **The sharper finding is next door.** The generated `axl-config.cmake` does
  not CALL `axl-cc` — it re-implements the entire pipeline in CMake (its own
  compile, `ld`, `objcopy`, `pe-set-debug`). That is the third build path
  `check-flag-parity` exists to police, and it is duplication by choice rather
  than necessity. Having the CMake package shell out to `axl-cc` would take
  three paths to two for a small change, and is worth doing whether or not the
  entry below ever happens.

- **CMake as THE build system, replacing the Makefile.** Mike's stated
  direction (2026-08-13), not scheduled. Today CMake is a CONSUMER-facing path
  only: `scripts/install.sh` generates an `axl-config.cmake` that wraps
  `axl-cc`-equivalent commands, while the library itself, all 43 test images,
  every tool and every gate are a ~2,000-line Makefile. Moving the LIBRARY
  build means porting: the build-state signature that wipes objects when
  `CC`/`CXX`/`CROSS`/`CFLAGS` change (CMake re-configures instead, which is
  the same idea done properly), the 19 `LINT_GATES` and `NONCLEAN_GOALS`
  machinery, the per-image `.efi` link + `objcopy` + `pe-set-debug` chain, the
  `AXL_TLS` source-set toggle, and `check-flag-parity`, whose whole job is to
  keep three build paths agreeing — a CMake port would reduce that to two, or
  arguably one, which is the strongest argument FOR it. Sequencing note: this
  overlaps the entry below and the `axl-cc` question above it; deciding those
  three together is cheaper than deciding them one at a time.

  **The duplication is now load-bearing, not just untidy.** `axl-c++
  -fexceptions` works on both arches, and the CMake package CANNOT do it: its
  re-implementation has no `_eh` linker script, no glue objects and no
  toolchain libraries, so a CMake consumer asking for exceptions gets an image
  that compiles, links, and dies at the first throw. `check-flag-parity` cannot
  see it — the `-j` lists agree. Having the package shell out to `axl-cc` fixes
  it by construction; writing the logic a third time is the alternative.

- **Distribution & consumption model** — [AXL-Distribution-Design.md](AXL-Distribution-Design.md).
  Package, install, discover and version-pin the SDK the way a real
  cross-toolchain does (tarball, CMake toolchain file, build dir vs install
  prefix), keeping the unreleased-checkout workflow first-class. **DRAFT; not
  started, nothing decided** — scope, phasing, decisions already taken,
  recorded blockers and six open questions all live in the design doc.

- **Backdrop-blur cache: stop paying for a hit** — `blur_output_rect`
  (`src/gfx/axl-compositor.c`) extracts the whole veil rect with a scalar
  per-pixel loop (stride arithmetic, not a `memcpy`) and only THEN `memcmp`s it
  against the cached source to decide the blur can be skipped. For a
  full-screen veil that is ~8 MB extracted pixel-by-pixel plus an 8 MB compare
  **every present**, just to discover nothing changed. A damage-derived
  validity signal — did any damage intersect the region beneath this veil? —
  would skip both. Note caching is disabled entirely on the partial path
  (`cache = !c->blur_partial`), so this only bites the full-repaint path, which
  is rarer now that consumers present damage rather than coalescing to FULL.
  Measure before and after with `test/integration/gfx-present-bench.c`.

- **Generation-tagged opaque handles for caller-held objects** — `axl_serial_close`
  is the known case: two guards make an ordinary double-close a no-op (a closed
  port's protocol pointer is cleared, and a pointer absent from the open-port
  list is never freed), but neither survives the allocator recycling the
  address. If a later `axl_serial_open` lands on the freed address, a stale
  second close matches the NEW port, unlinks it and frees it — silently
  releasing a live port's claim. **No check through a raw pointer can close
  this**, because the check must dereference the very pointer whose validity is
  in question. The fix is an opaque handle carrying an index + generation
  counter, validated against a table, so a stale handle is detectable rather
  than merely unlikely. Public API change; worth doing across the caller-held
  object types together rather than one at a time. Documented as a `@warning`
  on `axl_serial_close` until then.

- **Sync→async API split — build on demand** (→ [AXL-Concurrency.md § "Extending
  the model"](AXL-Concurrency.md#extending-the-model-which-apis-go-async-and-when)):
  the net stack's sync-wraps-async shape applies to any op that blocks on
  hardware/firmware completion. Ranked candidates: **IPMI/BMC** (KCS/SSIF
  busy-poll → loop Poll-tick; SoftBMC roadmap) and **storage** (NVMe/ATA/SCSI
  PassThru `Event`, BlockIo2; self-test/SMART/large-read; SoftBMC roadmap) are
  the near-term ones; MP-services (`StartupAllAPs` `WaitEvent`), USB async
  transfers, and TPM are lower. Un-defer each when a consumer needs it — do not
  build speculatively.
- **Hardware-fixture capture — remaining phases** (→ [AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md),
  Archive): TPM/PCR + TCG event-log capture/replay (swtpm); secure-boot + boot-var
  capture/inject; Redfish-mock capture/replay; in-band IPMI/KCS capture;
  SMBIOS-handle / non-EEPROM SMBus sensors / ESRT / NVMe-Identify replay;
  `--sanitize` (zero serials/asset tags); decide public `axl-fixtures` repo;
  `axl-emulate` polish (ACPI drop/keep flags, manifest summary, `--` passthrough);
  fold `qemu_launch` into `run-qemu.sh` as a daemon.
- **EFI encapsulation / portability** (→ [AXL-EFI-Encapsulation-Plan.md](AXL-EFI-Encapsulation-Plan.md),
  Archive): classify remaining UEFI-coupled modules; promote
  `axl_backend_{locate_protocol,alloc_pages,create_event,install_protocol,
  get/set_variable,exit}`; optional `src/core` / `src/platform/{uefi,coreboot}`
  split + a coreboot/Linux backend for `libaxl-core.a`.
- **C++ bindings (AxlMM)** (→ [AXLMM-Design.md](AXLMM-Design.md)): CPP1.7+ wrapper
  phases (Stream/Event/StrBuf/Arena, containers, networking, Sphinx docs).
- **API hygiene** (→ [AXL-API-Consistency-Audit.md](AXL-API-Consistency-Audit.md), a
  145-header audit — 12 categories, 6 prioritized batches): `AxlStatus` promotion +
  security `AXL_WARN_UNUSED` (Batch A), C++ RAII autoptr gap incl. AxlGfxBuffer (B),
  out-params-last param-order fixes incl. swapped void* pairs (C), constructor/
  result-passing normalization + the `axl_queue_free` stack-corruption bug (D),
  `axl_storage_*`/`axl_sntp_*` prefix splits (E), enum-flag/stdint/const tidy (F).
  The audit's completeness critic flagged a second pass — now also DONE: axl-math.h +
  gfx int-vs-void audited (defensible convention, carve-outs documented), axl-shm flags
  enum-wrapped (F), the gfx handle-family autoptr gap closed (B), and axl-port renamed to
  axl-io-port.h (guard/prefix/filename aligned). Remaining: only the consumer-repo update.
- **AxlTcp send/receive token queues (ACTIVE):** `docs/AXL-Tcp-Queue-Design.md`.
  Ports EDK2's socket-layer token queue so `axl_tcp_send_async` stops returning
  `AXL_BUSY` for "a prior send is in flight" — it survives only as genuine
  backpressure at `high_water`. Success criterion is DELETING the three ad-hoc
  serialisation mechanisms the absence of a queue forced (WS outbound queue, the
  `axl_tls_write_async` BUSY floor, `handshake_flush_async`'s resume).
  Not a cleanup: the ad-hoc approach is still producing new instances of one
  defect. `0a9f81fe` fixed the `!= AXL_OK` misclassification in
  `handshake_flush_async`; the identical test survives at FOUR submit sites in
  `axl-http-response.c` (`:361`, `:410`, `:616`, `:662`) and drops a healthy
  connection whenever the TLS handshake's final write is still in flight.
  Measured 2026-08-12: `test-https.sh`'s concurrent-handshake gate fails ~1 run
  in 3 on an idle box (`23/24 OK (width 4)`, client `curl 56`, server
  `rc=-10 = AXL_BUSY`). That is a FLOOR — `:361` resets with no warning, so the
  instrumentation that produced the number cannot see it. Left unpatched
  deliberately — a direct patch would be a FOURTH mechanism. The gate is the
  design's own acceptance test (§6 step 2), it is RED today, and `verify.sh`
  does not run it (only `run-integration.sh` does), so nothing in the fast loop
  observes either the red or a later regression.
- **Networking layering — POSIX-shaped substrate (revisit; DEFERRED to the
  C++/newlib toolchain work, 2026-08-12):** deliberately sequenced AFTER the
  substrate/toolchain track, which carries its own layering changes (see
  `docs/AXL-Libc-Substrate-Design.md`) — settling both at once avoids two
  independent refactors of the same boundary disagreeing. Explicitly NOT a
  prerequisite for the token queues above: `AXL-Tcp-Queue-Design.md` §4 keeps
  the queue free of new public API precisely so it does not prejudge this, and
  the queue fixes the `AXL_BUSY` defect class that this item would not touch.
  Original note follows.

  Today the real transport substrate is `AxlTcp`/`AxlUdp`
  (async, loop/callback-driven over EFI_TCP4/UDP4) and `AxlSocket` is a *BSD-compat
  veneer alongside* the protocols — HTTP/WS/9p build on `AxlTcp` directly, the
  inverse of the POSIX "everything on sockets" layering. This is a deliberate
  UEFI-appropriate choice (blocking sockets are a bad fit for single-threaded,
  no-preempt, loop-pumped firmware — a blocking `accept()` in a resident driver
  freezes the FW), but it has a real cost: transport features must be surfaced
  *twice* (e.g. the `AXL_TEARDOWN_RESET` port-releasing close reached HTTP for
  free but had to be re-exposed on the veneer via `axl_socket_free(., mode)` —
  `b0b4aefa`). IF a future
  socket-based server or a broader POSIX-compat push makes the veneer load-bearing,
  reconsider whether `AxlSocket` should become the single substrate with `AxlTcp`
  as its async engine (would require exposing the full async/abortive surface on
  the socket API — i.e. it stops being "simple BSD"). No consumer needs this today
  (only `sdk/examples` use `AxlSocket`); flagged so we do it on purpose, not by
  drift. See `src/net/README.md` §abortive-teardown.
- **Correctness / perf** (→ Archive §"Known Gaps"): a benchmark suite; AxlLoop
  fully event-driven driver mode (drop `driver_tick_ms`); async-TCP `Configure`
  retry non-blocking; `axl_yield()` API instrumentation; release-mode heap
  auto-sweep; robust exception-backed physical-access fault gate; watchdog
  opt-in helper.
- **Packaging / distro** (→ Archive): `-devel` split + `debian/` + `.spec` for
  upstream submission; evaluate meson/cmake+ninja build.
- **Real-hardware follow-ups** (→ Archive §"Real-hardware findings" — Dell
  PowerEdge / iDRAC10): assorted open items (iDRAC HTTPS routed-address timeout,
  USB-NIC unload entry points, `Load Error` on repeat `.efi` launch, log-timestamp
  `.usec` always zero, console-aware tool output mode, a real-hardware test
  runner). Several consumer-repo items (uefi-devkit / axl-webfs) live here too.
- **SDK/test:** consumer build verification (axl-webfs / uefi-devkit) in
  `test-axl.sh`; migrate tools' inline hex parsers onto `axl_hex_parse_u64`.

---

## Done / decided (one-liners, full detail in Archive)

- **`AxlXml` flags adopted the `AxlJsonFlags` shape** (2026-08-04) —
  `AxlXmlWriterFlags` + `AXL_XML_WRITER_DEFAULT/PRETTY` + `uint32_t flags`
  became `AxlXmlFlags` (typedef'd `uint64_t`) with `AXL_XML_DEFAULT` and
  `AXL_XML_INDENT(n)`. `AXL_XML_WRITER_PRETTY` was `1 << 0`, the same bit as
  `AXL_JSON_ALLOW_COMMENTS`, so passing one to the other compiled and silently
  meant something else. A distinct typedef does NOT fix that in C, so the fix
  is that the **same bit means the same thing** — `AXL_XML_INDENT` is
  bit-for-bit `AXL_JSON_INDENT`, asserted in the suite — plus
  `axl_xml_writer_init` refusing any bit outside `AXL_XML_KNOWN_MASK`. The
  indent width stopped being hardcoded at 2 as a side effect. `ENSURE_ASCII`
  for XML (numeric character references) is deliberately NOT in this change:
  it is a new escaping feature, not an alignment, and belongs on its own.

- **Phase B2 Redfish** — *no library module*; shipped `rfbrowse.efi` (HTTP client
  + JSON cover it). Extract a session helper later if SoftBMC needs one.
- **Phase 10 networking** — folded into the SoftBMC full-port sub-project above.
- **Repo merge / backend removal / project restructure** — complete.
- **AML interpretation, HTTP/2, in-library asset bundler, `$filter`** — explicit
  non-goals (rationale in the respective design docs).
