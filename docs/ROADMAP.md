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

## Agreed order of work — decided 2026-09-05

Mike picked this order explicitly, over a recommendation that had the SSH
track first. It is recorded here rather than in a session note because the
reasoning is not reconstructable from the individual entries: **1 makes 2
cheaper, and 3 is what 4 is blocked on**, so the sequence is doing the
enabling work before the work it enables, in both halves.

1. **Release / CI turnaround** — SHIPPED 2026-09-05, and **not what this line
   originally said.** It read "move `ci.yml` off the every-push trigger", which
   is the one thing the work concluded it must NOT do: a scheduled suite means
   no push run can ever satisfy the release gate, so every cut falls back to a
   local uncached suite or a dispatch — reintroducing the ~8.5 minutes the same
   change had just removed (`AXL-CI-Release-Speed-Design.md` §13.8). What
   shipped instead: the gate accepts CI's own green run (§13.1-13.2), watching
   `Release` became opt-in (§13.3), and a `plan` job classifies each push so
   prose no longer pays for the full matrix (§13.6-13.7). Remaining: §13.9
   and §13.10. §13.8 was answered on 2026-10-21 (§14): the suite moved to
   `workflow_dispatch` only (not nightly), cheap jobs stay on push, and the
   release gate composes a dispatched suite run, or a local uncached run plus
   the cheap push CI.
2. **Cut the release** carrying the `axl` output work (`b1c6fde6`) plus
   whatever 1 lands. Mike said **4.7.1**; note that `## Unreleased` currently
   holds an `### Added`, which `check-release-semver.sh` NOTEs as
   minor-shaped, so the number is worth a second look at cut time.
3. ~~**Ed25519 E2b**~~ — **SHIPPED 2026-09-05** (`286f6323`..`3cd75dc1`),
   security review passed. `AxlSsh` v1 can advertise `ssh-ed25519`.
4. ~~**AxlSsh P1 Task 6**~~ — **SHIPPED 2026-09-05** (`967a1978`). P1 is
   complete; a real `ssh(1)` completes the handshake, and P1's security review
   (spec §11) is closed on its blocking findings.
5. ~~**AxlSsh P2**~~ — **COMPLETE 2026-09-07** (`ddf8d517`..`9e7cb351`):
   `userauth` publickey against `authorized_keys`, both RFC 4252 forms,
   `ssh -i key` authenticating against the guest. **Security review (spec
   §11's merge gate) is CLOSED**: the initial pass returned 2 HIGH / 2
   MEDIUM / 6 LOW, both HIGH findings plus MEDIUM-1 and two of the LOWs
   were fixed (`6d39ab11`, `a2f5eec4`), and a scoped re-review confirmed
   all five addressed with no attacker-reachable regression. See the
   AxlSsh section below for the residue the gate closed WITH, not blocked
   on.
   **Scope REVISED 2026-09-17** (spec §0): the actual requirement is one user,
   possibly over several connections, reaching the **live UEFI Shell
   interactively with a username and password**, served from inside SoftBMC.
   That inverts two v1 non-goals (password auth; a shell instead of a command
   table) and reorders what remains: **P3** password auth, **P4** session
   channels **with rekey** (SHIPPED in P4d; before it a peer that asked to rekey was
   disconnected; OpenSSH's default is data-only -- `RekeyLimit default none`, ~1 GiB for
   chacha20-poly1305, *"no time based rekeying"*, `man 5 ssh_config` -- so
   what reaches it is an explicit `RekeyLimit`, a large transfer, or a client
   that is not OpenSSH), **P5** persistent host key, **P6** SoftBMC
   integration *in the softbmc repo*. The
   resident `AXL_SERVICE_DRIVER` phase dissolved and the client is
   unscheduled. Each phase is its own plan with the same security review as a
   merge gate. **Spec §13 lists open decisions** — two of them are live
   SoftBMC defects found while revising it (a password timing oracle in its
   web login, and no guessing throttle -- which AxlSsh now has for SSH, Task P
   2026-09-18, while SoftBMC's web login still has none).
6. **THEN the AxlTcp close path** — scheduled here, after AxlSsh, by Mike on
   2026-09-06. It is a pre-existing defect (introduced `94a603b5`,
   2026-07-19), not an AxlSsh one, and doing it inside AxlSsh means changing a
   guarantee three subsystems were built around while a fourth is still being
   written. `axl_tcp_close`'s abortive branch busy-polls
   inline for up to a second (`AXL_TCP_ABORT_WAIT_US`), so a server tearing
   down N connections from its own callbacks blocks its loop for N seconds.
   **STILL TRUE, and deliberately not fixed there.** Two attempts failed and
   both are worth knowing:
   - changing which teardown mode AxlSsh *asked for* was a no-op — the
     downgrade guard had already made those closes abortive;
   - making abortive connection closes go async (`6c2f5995`) broke
     AxlHttpServer and Axl9pServer, which free listener-first and rely on the
     listener's sweep to finalize their children's deferred closes. Reverted
     in `38585974`, with the reasoning recorded at the branch.

   The constant's own comment has the answer: a RST "signals almost
   immediately; this only bounds a **wedged firmware**". It is a backstop, not
   the expected cost, and the interim fix for a server that can stall on N of
   them is to bound N — `MAX_CONNS` plus a handshake grace, both already at
   the SSH layer.

   **What the real fix needs**, when it is picked up: HTTP and 9P in scope,
   not just AxlSsh; the four `test-http-rebind-{load,churn,storm,multi}-qemu.sh`
   tests plus `test-9p-server-qemu.sh` as the gate, because they are what
   encode the port-release guarantee; and a way to reach the failing path,
   which needs a running loop — every existing rebind test frees AFTER the
   loop stops, which is exactly why none of them caught the broken version.
   `test-service-reload-qemu.sh` is the closest existing shape and still
   misses it (it reloads on a timer with no client connected). Options worth
   weighing: reverse the free order in HTTP and 9P (connections first, as
   AxlSsh does) so the listener sweep works; give the deferred abortive close
   a bounded deadline so it cannot leak; or shorten the backstop, which the
   constant's comment suggests is safe and which no test would notice either
   way — that last one is a trap, not a fix.
7. **`axl_malloc` wraps on a huge size** — found by E2b's review and NOT
   fixed: `align_up()` rounds to a size_t boundary and wraps to 0 above
   `SIZE_MAX - 7`, so `axl_malloc(SIZE_MAX)` returns a **header-sized
   buffer** and reports success. Every caller that sizes an allocation from
   attacker-influenced input shares this. `axl-pk-ed25519.c` guards itself at
   `SIZE_MAX/2`; nothing else does. Needs a decision on scope.

---

## Design & plan docs (index)

Library / SDK foundations:
- [AXL-Design.md](AXL-Design.md) — library design (phases, API spec, style)
- [AXL-SDK-Design.md](AXL-SDK-Design.md) — SDK (toolchain, packaging)
- [AXL-Build-System-Design.md](AXL-Build-System-Design.md) — **DONE
  2026-11-09 (slice 6, §10.12): the Makefile is retired; CMake + Ninja is the
  only build**, for 5.0.0 (unreleased; `### Breaking (packaging)` in the
  CHANGELOG). CTest replaced the test schedulers (run-integration.sh and
  verify.sh are thin front ends); CPack was measured and NOT adopted -- the
  three tarball scripts stay until the CMake minimum reaches 4.3 (§10.9).
  Outside this tree: uefi-devkit's build.sh and Makefile, which ran `make -C
  <axl-sdk> tools` (and `clean-tools`, `sshd-rtk`, with an `AXL_TLS=1` that
  `scripts/axl-build.sh` refuses), build through `scripts/axl-build.sh` as of
  uefi-devkit `fbad8e6` (2026-11-09; committed there, push pending). No
  Makefile is left in this tree outside the vendored `deps/mbedtls`, whose
  sources CMake compiles directly; `make` survives only in
  `toolchain/x86_64-elf/build-toolchain.sh`, which builds the upstream
  autotools projects (binutils, GCC, newlib).

  History of the entry follows. Decision (CMake over Meson) and the measured port
  surface — the `.efi` pipeline (~180 images per arch), the
  ARCH x BUILD prefix rule, 17 gates, 149 make callers, and why
  `axl-cc` is NOT part of the port.

  **NOT 4.0.0, and it is breaking.** The entry used to say "for 4.0.0; we stop
  shipping Makefiles", on the premise that the Makefile was not in the shipped
  surface. **That premise is retracted** (design doc §5.1): `release.yml`
  publishes the whole tree via `git archive HEAD`, there is no `.gitattributes`
  so nothing is `export-ignore`d, and `README.md` gives
  `git clone axl-sdk-releases && make CROSS=<prefix>-` as the *only* documented
  build for macOS and native Windows, which have no binary package. So the port
  needs a `### Breaking` entry, a README rewrite, and a major of its own.
  4.0.0 shipped 2026-08-15 without it.

  **`CROSS=` ANSWERED 2026-08-16 (`00206e7a`): the README path is removed and
  the port reproduces nothing.** It selected binutils only (since `0bf6ed51`),
  and a glibc-targeted cross cannot build the tree at all now `include/compat`
  is gone. `AXL_TOOLCHAIN` replaces it as a VARIANT beside the locators, the
  pairing Zephyr / EDK2 / the kernel all use — so the port inherits no prefix
  convention. **`AXL-Distribution-Design.md`'s P2 is also decoupled**: the
  "shared 149-caller surface" both docs claimed measures at SEVEN files, and
  P2's own half shipped in `52f5f369`. Still open is Distribution's O1 (does
  the default move from `out/` to `stage/`), now one edit rather than a sweep.
- [AXL-Cxx-Stdlib-Handoff.md](AXL-Cxx-Stdlib-Handoff.md) — **DONE 2026-08-06.**
  `axl-c++ --hosted` ships, and `std::vector` / `std::string` / `std::map` /
  `std::unordered_map` run under UEFI on both arches
  (`test/integration/test-cxx-hosted-qemu.sh`). **§8's distribution question
  was CLOSED 2026-08-08 ("the SDK links no `libstdc++.a` at all") and
  RESETTLED 2026-08-17 by P4: every C++ link carries the CONSUMER's installed
  libstdc++, and the SDK still conveys none of it.** The RLE constraint is
  untouched — what it forbids is us redistributing the runtime library, and
  `axl-cc` resolves the consumer's copy via `-print-file-name`, as the
  `-frtti` path already did. The five clean-room substitutes that made the old
  answer possible went with `libaxl-cxx.a`; see
  `AXL-Libc-Substrate-Design.md` P4-RESULT for why keeping them was worth
  ~3 KB against a +47 KB budget.

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
  it. `gate.check-reloc-coverage` is the standing guard, and RTTI now runs on
  both arches
- **C++ owning handle — DONE 2026-08-16.** `axl::unique_handle<T>`
  (`include/axl/axl-handle.hpp`), the class-member / movable / factory-returnable
  counterpart to `AXL_AUTOPTR`, which stays as-is for C. Requested by AGT for 13
  types; shipped covering **61**, because the deleter is generated by the
  existing `AXL_DEFINE_AUTOPTR_CLEANUP` rather than listed by hand — so it also
  covers the 3 `_ARG` teardown-mode types a `template <auto Free>` cannot
  express, and cannot drift from the destroy function it names. Opt-in per type
  makes the two exclusions structural: `AxlSurface` (borrowed node in a tree
  `axl_compositor_free` destroys whole) and `AxlJsonReader` (value struct, not a
  handle) are compile errors that state their ownership rule via
  `AXL_DEFINE_NO_HANDLE`. Gated by `gate.check-handle-exclusions` (one fixture,
  three compiles, matches the poison text) and 12 run assertions per arch in
  `test-cxx-hosted-qemu.sh`. See AXL-Cxx-Design.md §9b.
- **C++ seams over the C API — C2/C3/C5 DONE 2026-08-17.** Five headers, 67
  assertions per arch in `test-cxx-seam-qemu.sh` plus 18 C-side unit
  assertions, 11 sabotages verified.
  - **C2** `axl::view` / `axl::adopt` (`axl-cstr.hpp`) — the ~20 public
    functions returning an owned `char *` become one expression instead of
    four leak-prone lines. The `AxlString *` half of the scoped seam was
    DROPPED: zero C++ consumers reference the type, which is §4.4's measured
    inversion applied to C2.
  - **C3** `axl_array_data()` + `axl_array_element_size()` on the C side, and
    `axl::array_span` / `axl::array_ptr_span` (`axl-array.hpp`). The scoped
    `axl::c_array_ref` was **not written** — with a base pointer a borrowed
    array is a `std::span` outright, with real `T *` iterators instead of a
    proxy. This reverses §7 item 3, whose "only if a container is wanted"
    clause missed that a VIEW has the same need. A stride mismatch HALTS
    naming both sizes.
  - **C5** four lazy allocation-free `AxlNTree` ranges (`axl-ntree.hpp`),
    `axl::radix_tree<T>` (`axl-radix-tree.hpp`), `axl::gfx_target_scope`
    (`axl-gfx-surface.hpp`, replacing AGT's hand-written `saved_target_`).
    `level_order`/`in_order` are deliberately absent — they need a queue, and
    a range that allocated silently would read like the four that do not.
    The radix wrapper was built with **zero** C++ consumers, as an explicit
    surface-completeness call rather than a measured one.
  - **C6 DONE 2026-08-17** — `axl-json.hpp`: `axl::json_document`/`json_value`
    with simdjson-style chaining navigation, array and object ranges,
    `axl::json_writer` with RAII container scopes and templated `add`,
    `splice()` (the reader->writer bridge, which needed no C change), and
    `axl::json_scanner`. 52 assertions per arch. Required four C additions,
    which serve C callers too: `axl_json_get_string_len`,
    `axl_json_value_string_len`, `axl_json_object_peek_key_len` (a PEEK, since
    object iteration reports a truncated key only after consuming the pair),
    and `axl_json_double`/`axl_json_kv_double`, completing the scalar mirror
    the reader opened at P14. See AXL-Cxx-Design.md §9d and §9e.
  - **`axl-c++ --no-eh-frame` — DONE 2026-08-17**, on a measured request from
    AGT (5.3 MB of fleet growth from P4's one-link-shape collapse). Drops the
    unwind tables. AGT has since ADOPTED it on all four link sites: **x64 fleet
    16.40 -> 14.12 MB across 34 binaries**, gate green (9650/0 both arches,
    63/63 visual).

    **Budget from the decomposition, not a percentage.** Saving = `.eh_frame` +
    a constant ~13.5 KB (x64) / ~16.5 KB (aa64) of collected throw path, and
    `.eh_frame` scales with program size on x64 while being CONSTANT (~8.3 KB)
    on aa64 — because `-fasynchronous-unwind-tables` is a per-TARGET default,
    on for `x86_64-elf` and off for `aarch64-none-elf`, so aa64 consumer code
    emits no CFI under `-fno-exceptions`. (Not an architecture property:
    `aarch64-linux-gnu` has it ON. An earlier revision of this line said
    "psABI" and was wrong.) x64 lands ~13% at any tool size; aa64 is **~25 KB flat**, which AGT
    measured as 3.1-4.4%. The -18%/-15% first quoted here came from one small
    fixture and under-predicted aa64 by ~4x.

    The request was "select the non-`_eh` linker script", and measurement said
    that alone does two wrong things: it does not LINK (`axl-cxxrt-eh.o`
    references `__eh_frame_start` under `--no-undefined`), and it does not
    degrade the terminate message, it takes a CPU fault and **wedges the
    machine**. So the flag also swaps in `axl-cxxrt-nothrow.o`, whose
    `__cxa_throw` intercepts before the unwinder is entered and still prints
    the type and `what()`. REFUSED with `-fexceptions`, because that
    combination silently kills every catch block — a correctness failure, not
    a size trade. `test-cxx-noeh-qemu.sh`, 19 assertions per arch. See
    AXL-Cxx-Unwinder-Design.md §U5.
  - **The C++ layer is now COMPLETE**: C0-C7 all shipped.

    Read "complete" as "every phase the design doc scoped has landed and is
    tested", not as "proven against real consumers". The honest caveat is the
    one §9c already records for `axl::string`: for the C6 JSON API and the C5
    radix wrapper, the only in-tree consumer is the fixture that tests them.
    AGT exercises `unique_handle`, `std::string` and the `AxlArray` sites; the
    rest is a public SDK surface waiting for its first outside caller, and
    §9c's acceptance test ("AGT shrinks") is the measure that has NOT been
    re-run since C2/C3/C5 landed.
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

- **Debug SDK variant — PROPOSED 2026-08-17, not scheduled.** SDK consumers
  have **no instrumented allocator at all**: the staged `libaxl.a` is built
  `-DNDEBUG`, so no fences, no `0xDA`/`0xDF` fills, no leak list, no
  quarantine, no file/line — `axl_mem_dump_leaks()` exists and reports nothing.
  `axl-cc --debug` does not change this; it sets the CONSUMER's compile flags,
  while the staged library it links was built once at `BUILD=RELEASE`.
  Closing it means staging a DEBUG variant of `libaxl.a` plus the glue objects
  and having `axl-cc --debug` select it. Found while pricing
  [AXL-Libc-Substrate-Design.md](AXL-Libc-Substrate-Design.md) §2-DECISION,
  which is also what it would unblock: an `AXL_MEM_DEBUG` allocator bridge for
  the C/C++ world is dead code until a debug SDK exists to compile it into.
- [AXL-Libc-Substrate-Design.md](AXL-Libc-Substrate-Design.md) — **DIRECTION
  AGREED 2026-08-11; PHASES COSTED 2026-08-17 (§4c).** The C half is free —
  six real images linked with `libc.a` and the stand-ins deleted grew by
  nothing and pulled no stdio, so `axl-str-compat.c` + `axl-intrinsics.c`
  (195 lines) are deletable. **ALL FIVE PHASES DONE 2026-08-17.** The C++ half
  (P4) cost **+46,928 `.text` (+138%)**, and **+100,339 on the `.efi`** (58,758 -> 159,097), on a `-fno-exceptions` image — not the
  +126 KB costed here, which pre-dated the terminate-handler shrink — and
  released `libaxl-cxx.a` ENTIRELY (7 files, 1,696 lines), buying real
  stdio and working `<iostream>`/`<sstream>`/`<fstream>`. Sequencing is
  forced: the `_r` allocator bridge comes FIRST
  because stdio's `findfp.o`/`fvwrite.o` allocate through `_malloc_r`, so the
  porting layer alone would not deliver stdio. **The P2 shape is settled
  (§4c.1): newlib's stdio sits ON `AxlStream`, not the reverse.** newlib
  defines 0 of the 18 porting-layer symbols, so those are AXL's in every
  design; `AxlStreamBackend`'s vtable already has the
  read/write/pread/pwrite/seek/close shape newlib wants, so the fd table is an
  `AxlStream *` array rather than a subsystem. **`fopen`/`FILE` ARE
  supported** — newlib's FILE machinery sits on those six entry points, so
  third-party C that opens files needs no porting; what stays direct is
  AXL's OWN file APIs, which do not route through `FILE`. And **`AxlFormat` is out of scope permanently** — two formatters on
  purpose, per §4.1. **Phased in §4d (P1-P5), not started:** P1 the allocator
  takes the standard names (plain AND `_r`, from `libaxl.a` itself), P2 the
  porting layer over `AxlStream`, P3 `libc.a` everywhere + the 195-line
  deletion, P4 the libstdc++ swap (+126 KB, accepted) for iostreams, P5
  locale. **P5 MEASURED 2026-08-17 and there is nothing to do:**
  `<sstream>` `<fstream>` `<format>` `<regex>` all compile, link and RUN
  today — what they needed was a C library, not glibc's locale, so
  P2/P3 delivered it as a side effect. `AXL-Cxx-Stdlib-Surface.md`'s
  Tier 3 ("blocks all four") is RETRACTED. The wall is SIZE: ~1 MB each,
  ~9x the 119,691-byte -fexceptions baseline. **P1 LANDED 2026-08-17** (`c991ccfc`,
  `68caad05`): newlib's internals allocate through AXL, `strdup()` works
  where it returned NULL, dlmalloc is absent from the image, and
  `malloc_usable_size` is AXL's — newlib's returned 56 for a 64-byte
  block, silently. **REPLACED SAME DAY by §2-DECISION: TWO allocators, split
  by namespace; P3 LANDED, the 195 lines are deleted and the AXL/newlib
  symbol overlap fell 14 -> 5** — newlib keeps its own dlmalloc on an AXL `sbrk` over
  `axl_alloc_pages`, `axl_malloc` stays separate over `AllocatePool`, and
  nothing crosses because the names are disjoint. Accepted cost: C++ and
  third-party allocations leave AXL's leak gate. **P2 LANDED 2026-08-17** (`7cf04c31`): real `printf` and
  `fopen` for third-party C, newlib's FILE running on `AxlStream`, 28/28
  both arches. **The one ordering that is a trap is P3
  before P1** — third-party `malloc` would reach newlib's dlmalloc and
  return NULL silently rather than failing the link.
  **DIRECTION AGREED 2026-08-11.** `axl_*` sits on newlib the way GLib sits on libc, with
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
  the level-2 track they served is cancelled.
  **U2/U3 DONE 2026-08-13 — `axl-c++ -fexceptions` ships.** Both things this
  entry listed as pending are closed: the runs are not hand-linked any more
  (`axl-cc` derives the exceptions-capable link from `-fexceptions` itself, or
  from an input object referencing `__gxx_personality_v0`), and the libstdc++
  emergency-pool leak that blocked the hard leak gate is cleared. The committed
  fixture is `test-cxx-exceptions-qemu.sh`, whose first assertion is a global
  constructor that throws and catches BEFORE `main` — so it fails if frame
  registration does not precede the `.init_array` walk, rather than assuming
  the ordering. **Measured cost, both arches:** a container-using image goes
  ~61 KB to ~279 KB on x64 (~69 KB to ~274 KB on aa64) with `-fexceptions`, of
  which only ~22 KB is the unwinder — roughly half is libstdc++'s verbose
  terminate handler pulling the C++ demangler and newlib `stdio`.
  **U5 DONE — that handler is preempted and the image is ~43-46% smaller.**
  `src/cxxrt/axl-cxxrt-terminate.cpp` is the fourth object `axl-cc` names on
  the `-fexceptions` path: 264,185 → 150,920 on x64 and 259,933 → 139,651 on
  aa64, measured with and without it. The stock handler printed **nothing**
  under UEFI (its `stderr` is a newlib one nothing wires up), so the 112 KB
  bought negative value; ours names the exception's type and `what()` for
  +89 B / +655 B over a fixed string, and is asserted both ways —
  `nm` for the absent demangler, a booted uncaught throw for the output. See
  [AXL-Cxx-Unwinder-Design.md](AXL-Cxx-Unwinder-Design.md) §4a and
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
- [AXL-Distribution-Design.md](AXL-Distribution-Design.md) — how the SDK is
  packaged, installed, discovered and version-pinned. **D1–D7 and §20 M1/M2 all
  SHIPPED**; the §14–§19 series closed 2026-09-02 when the packages retired for
  real, and §19b records what fell out of testing it afterwards. No longer a
  DRAFT: the tarball and the CMake toolchain file it called "the gap" both
  shipped, and consuming an unreleased checkout is answered by
  `install.sh --prefix`. **§21 (an install receipt) is the one open section,
  and it is now HALF built**: ownership-based pruning of TOOLCHAIN roots
  shipped 2026-09-03 (`3ed108d4`) — `install-toolchain.sh` writes
  `.axl-receipt` and `axl prune` requires one, which is what §12.5 asked for.
  Still open is the provenance receipt for SDK/manager roots (source URL, tag,
  sha256, insecure-fetch flag), deliberately deferred until something reads it
- [AXL-Host-Toolchain-Design.md](AXL-Host-Toolchain-Design.md) — **SHIPPED
  2026-09-03.** Which compiler builds a consumer's x64 image, and
  how one is chosen. The feasibility spike is GREEN: Ubuntu 24.04's gcc 13.3
  built and booted AXL images in a container with `/opt` empty. `AXL_TOOLCHAIN`
  gains `host` and `auto`, and `auto` becomes the x64 default — bare-metal
  toolchain present selects it, absent falls back to the host compiler — so a
  C-only consumer installs no 239 MB toolchain at all. Measured motivation:
  3 of 4 AXL consumers are C-only. **Not a breaking change** (a machine with a
  toolchain installed behaves identically); aa64 is untouched, because ARM's
  binutils is what supplies `pei-aarch64-little` and the host's has it not at
  all. Adds `axl toolchain uninstall <arch>`, guarded by §21's receipt
- **Install layout, versioned roots and the `axl` dispatcher** — ACCEPTED
  2026-08-29; **P5 (the dispatcher) and P8 (root-free toolchain install) SHIPPED**, P1's SDK tarball SHIPPED too (it was P6's prerequisite), as did P6's pruning half (`axl prune`, current + one previous, both axes) and the self-containment contract it rests on. P6's "change install.sh's default prefix" was WRONG and is retracted -- `stage/` is right for a source checkout and the tarball's top-level dir already is the versioned root. P7 SHIPPED (its content half had already ridden in with P5). P8 was twice scoped larger than it was -- no conf change and no CMake port were needed, because only the INSTALLER could not target a user prefix; building against one already worked. Folded into
  [AXL-Distribution-Design.md](AXL-Distribution-Design.md) **§12–§13** rather
  than a fourth doc: that set states "one owner per shared fact", and a
  separate doc would have opened a second `P1`..`P4` namespace meaning
  different things. Packages keep `/usr`; the installer gains a VERSIONED
  `/opt/axl-sdk-<ver>` + `current` symlink, or `~/.local` when not root;
  host-tools folds in behind weak deps; a host-side **`axl` dispatcher** makes
  the install location stop mattering to every doc line and printed hint.
  Closes O1 and O4. Measured in-session: `/opt` IS a default CMake prefix and
  `find_package(axl)` already resolves from `/opt/axl-sdk*`, and a `--prefix`
  user install already compiles — so this is defaults + packaging, not
  relocation. New phases P5 (dispatcher, first), P6 (versioned root; needs
  P1's unshipped tarball and P8), P7 (package merge), P8 (root-free toolchain
  install). P8 was first scoped to ride with the CMake port and that was
  wrong: the `_DEFAULT` override convention is already make- and sh-safe for
  six keys, so it needs no port — which matters, because the port has ZERO
  implementation and its branch is fully merged into main with 0 unique
  commits
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
- [AXLMM-Design.md](AXLMM-Design.md) — C++ bindings plan. **Design complete;
  implementation deferred pending AGT.** Note this line used to call it a
  "`libaxl-cxx` bindings plan": that archive was DELETED by P4
  (`Makefile:1157`, AXL-Libc-Substrate-Design.md §4d), and every C++ link now
  carries the toolchain's own libstdc++/libsupc++. Remaining phases are in
  the Open backlog
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
  matrix of read flags rather than two bare dialects, and `gate.check-fuzz-link`
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
**v3.2.3**, **10405 unit tests** both arches (X64 + AArch64), built with AXL's
own bare-metal crosses on both arches (nothing from the host — see
[AXL-Libc-Substrate-Design.md](AXL-Libc-Substrate-Design.md) §4.1d, inventory
empty). `scripts/cut-release.sh` automates a cut from `main`, and
`--branch release-X.Y.Z` automates the other flow: a release that must NOT
carry everything on `main` is cut from the previous tag on a release branch,
which is how 3.2.1, 3.2.2 and 3.2.3 shipped — see
[RELEASING.md](RELEASING.md) "Which flow".

> **`git describe` on `main` reports v3.2.0, not v3.2.3.** Those three tags
> live on release branches whose content reached `main` by cherry-pick, so they
> are not ancestors of it. Consequence worth knowing before the next cut:
> `cut-release.sh` prints "commits since v3.2.0" and lists 61 of them. Reachable
> ancestry could be restored with `git merge -s ours <tag>` per tag (the content
> is already present, so it records the ancestry without changing a byte) — not
> done here because it alters `main`'s graph and is worth deciding
> deliberately.

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
  mbedTLS incl. `axl_tls_generate_self_signed`; HTTP server
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

### AxlCrypto — Ed25519 signing/verification — [2026-08-21-ed25519-design.md](superpowers/specs/2026-08-21-ed25519-design.md)
Fills in `AXL_PK_ED25519`, reserved since `axl-crypto.h` was written because
mbedTLS 3.6.3 has no twisted-Edwards curve at all — not a config flag away,
a vendoring project. Phased E1-E5; the fast ref10 implementation costs ~30 KB
of resident `.rodata`, so it must stay opt-in at link time (a weak-symbol
seam following `axl-log-dispatch.h`) rather than land in every image.
- [x] **E1** `AXL_CHECKSUM_SHA512` — a thin adapter over `mbedtls_sha512_*`
      behind the existing `axl-digest-internal.h` prototypes; `AxlHmac`
      gained the matching 128-byte block. FIPS 180-4 + RFC 4231 vectors pass
      both arches. Measured cost: **2,301 B** of mbedTLS in any image
      touching any checksum algorithm — accepted deliberately, not a
      regression to chase (spec §3b/§10)
- [x] **E2a** `AxlPkKey` is a tagged union behind an `AxlPkProvider` vtable,
      carrying only ECDSA and RSA. It used to *be* an `mbedtls_pk_context`,
      which Ed25519 cannot be. No behaviour changed — every existing vector
      passes unchanged, verified by moving ten helpers and both crypto cores
      byte-identical. Added `axl_pk_key_from_raw_public` /
      `_get_raw_public` / `axl_pk_alg_available`. **Measured cost: +8,976 B
      `.text`** on a verify/sign-only consumer, because the provider object
      holds strong references to all eleven operations and `--gc-sections`
      can no longer separate keygen and the DER writers from what a consumer
      calls (spec §10). That is E5's floor and it is the one number worth
      revisiting before E5 relies on it
- [x] **E2b** ref10 vendored + RFC 8410 DER behind the finished seam —
      **SHIPPED 2026-09-05**, `286f6323`..`3cd75dc1`, plan
      [2026-08-23-ed25519-e2b-ref10.md](superpowers/plans/2026-08-23-ed25519-e2b-ref10.md).
      `AXL_PK_ED25519` works in an image that opts in with
      `-u _axl_pk_provider_ed25519`; RFC 8032 vectors 1-3 and the negative
      set pass on both arches, and the dispatcher was not edited. **Task 2's
      licence-parity gate was skipped by decision** — D7 deleted
      `build-packages.sh` and the two remaining lists are legitimately
      different artifacts, so a set-equality gate would fail on day one;
      `test-sdk-tarball.sh` covers the staging and is sabotage-verified.
      Five things the plan did not anticipate, all measured:
      - the vendored source is SUPERCOP's **`ref`, not `ref10`** — base table
        108,800 B, so opting in costs **124,567 B**, ~4× the assumed 30,720
      - **ref10's small-order check covers only the identity.** An order-2
        forgery (no private key, ~half of all messages) was ACCEPTED before
        the fix; `ed25519_is_small_order()` now refuses all 14 torsion
        encodings
      - **`sm[63] & 224` is dead code** — subsumed by OpenSSH's later
        `sc25519_inrange`, so it can never be the deciding check
      - **`md_for_alg` / `ec_order_bytes`'s Ed25519 arms are STILL
        unreachable** — E2a deferred them here on the theory an Ed25519 key
        would reach them, and it does not: both are called only from the
        mbedTLS provider
      - a size_t overflow in the SM adapter, and **the same wrap in
        `axl_malloc`** (see the AxlMem note below)
- [ ] **E3** `check-ed25519-linkage` + the `check-no-avx.py` ADCX/ADOX
      extension, both proven able to fail
- [ ] **E4** ADX/BMI2 field backend + aa64 `FEAT_SHA512` via
      `MBEDTLS_SHA512_PROCESS_ALT` -- **which means carrying BOTH SHA-512
      compressors in AXL, not only detection** (corrected 2026-09-21): under
      `PROCESS_ALT` mbedTLS compiles no compressor (`sha512.c:592`), and
      `check_config.h:780` refuses to combine it with its own A64 kernel,
      so ~330 lines of upstream's portable and FEAT_SHA512 compressors move
      into AXL (spec §6). **The ADX half is now an open question**,
      not a plan: current OpenSSH ships one amalgamated `ed25519.c` with every
      field operation `static`, so a second field backend cannot be a
      link-time substitution. Spec §6b records the three options and their
      costs; §10's own note that the SSH workload is one signature per
      connection argues that dropping it costs nothing. **The `PROCESS_ALT`
      half must not be able to fail** (2026-09-20): a failed hash inside
      ref10 was a key-disclosing signature or a universal forgery. The glue
      `#error`s on it unless `AXL_SHA512_PROCESS_ALT_CANNOT_FAIL` is defined,
      and the provider refuses any operation during which a hash failed
      (spec §6). Both compressors E4 carries are pure functions over one
      block, so its dispatcher can make that promise
- [ ] **E5** Consumed by `AxlSsh` — `AXL_PK_ED25519` host key; unblocks
      AxlSsh P1 Task 6

### `AxlSsh` — an SSH server, built here, consumed by SoftBMC  *(P1 COMPLETE 2026-09-05; P2 COMPLETE 2026-09-07 — implementation, security review, fix wave and re-review all done, gate CLOSED; scope REVISED 2026-09-17 for interactive password login to the UEFI Shell; P3 COMPLETE 2026-09-17 — implementation, security review, fix wave and re-review all done, gate CLOSED (its one MEDIUM residue, NEW-1, RESOLVED 2026-09-26: hooks stay strictly serial, and the hook watchdog plus the reaper that says why SHIPPED the same day -- spec section 12); Task G (settable login grace) SHIPPED 2026-09-17; Task P (per-source penalties) SHIPPED 2026-09-18; the OpenSSH parity audit's last two CLOSE items, D4 (settable password cap) and D9 (pre-authentication banner), SHIPPED 2026-09-25; P4-P6 below)*
Same relationship AxlHttp and AxlTls already have with SoftBMC: the protocol
lives in axl-sdk, the product consumes it. The lineage argues for it — this
project began as **TelCon/Netcon**, a *telnet* console server, and telnet is the
one link in that chain with no confidentiality and no authentication. There is
no SSH-**server** prior art to port from SoftBMC: its remote-management design
uses `ssh` only as a client-side `ssh -L` tunnel to the HTTPS API.

**Why not a third-party library — license decides it before architecture does.**

| | license | fits an Apache-2.0 SDK? | architecture |
|---|---|---|---|
| **wolfSSH** | **GPLv3** or commercial, and requires wolfSSL/wolfCrypt on the same terms | **NO** | good — embedded-oriented, shell optional |
| **Dropbear** | MIT + permissive bundles (LibTom, TweetNaCl PD, 2-clause BSD) | yes | **bad** — `fork`/`forkpty`/`select` IS its structure |
| **libssh** (server-capable) | LGPL-2.1 | poorly — static-only firmware linking makes the relink clause awkward | medium |
| **AxlSsh** | ours, Apache-2.0 | yes | native `AxlTcp` + `AxlLoop` |

axl-sdk is Apache-2.0 and **every dep LINKED INTO our binaries is permissive** —
mbedtls Apache, libvterm MIT, freetype FTL, LZMA public domain, stb and sdefl
public-domain-or-MIT. wolfSSH would be the first GPL code *linked* into an SDK
that ships `.deb`/`.rpm`, and would relicense the combined work. The distinction
is linking, not shipping: the tools tarball carried iPXE's GPL
`ipxe-all.efidrv` (until 2026-10-21) as **mere aggregation** (GPL-2.0 §3),
linked into nothing of ours. wolfSSH could not be handled that way. Dropbear's licence is fine and its *architecture* is the problem: porting
it means rewriting `svr-main`, `svr-chansession`, the `select` session loop and
auth — most of the server — to land somewhere worse-fitting than our own
primitives. UEFI has no processes; `src/service/README.md` says so outright.

**Most of the crypto is already done and permissively licensed — but not all
of it, and this table said otherwise until 2026-08-23.** `AxlCrypto` (over
vendored mbedtls) carries most of what a default `ssh(1)` negotiates:

| SSH needs | AXL has | state |
|---|---|---|
| `curve25519-sha256` kex | `AXL_ECDH_X25519` (also `AXL_ECDH_P256`) | ready |
| `ssh-ed25519` host key | `AXL_PK_ED25519` | **READY 2026-09-05** — E2b shipped it (vendored ref, opt-in -- since 2026-10-20 by calling `axl_pk_enable_ed25519()`, which `axl_ssh_server_new()` does itself; originally `-u _axl_pk_provider_ed25519`). See the Ed25519 track above |
| `chacha20-poly1305@openssh.com` | `src/net/axl-ssh-crypt.c` | **BUILT 2026-09-05.** Correctly NOT `AXL_AEAD_CHACHA20_POLY1305`: OpenSSH's is raw ChaCha20 with a two-key split and a separately encrypted length field, and it is the ORIGINAL 64-bit-nonce ChaCha20 rather than RFC 8439's, so the SSH nonce becomes an mbedTLS nonce with four zero bytes in front. Built directly on `mbedtls_chacha20_*` + `mbedtls_poly1305_mac` |
| `aes256-gcm@openssh.com` | `AXL_AEAD_AES_256_GCM` | ready, and NOT used — it was the cheaper route, but Task 4 had already shipped and pinned the ChaCha name, and changing a tested decision to save ~60 lines is a bad trade. Still the obvious fallback if a second cipher is ever wanted |
| transport / event loop | `AxlTcp`, `AxlLoop`, `AXL_SERVICE_DRIVER` | ready |

So the work is **mostly framing and a state machine**. The one real crypto
dependency, Ed25519, shipped as E2b on 2026-09-05 and P1 completed the same
day.

**The precedent is measured.** `Axl9p` is **4,512 lines for client AND server,
built in three days** (2026-07-19 → 07-22), on the identical primitives:
`axl_tcp_listen` / `_accept_async` / `_recv_async` / `_send_async` over
`AxlLoop`. `AxlTls` is 1,065 lines over vendored mbedtls; AxlHttp's server core
is 594. Server-only SSH with a narrow algorithm set should sit in the same
range — call it ~3–4k lines, at a materially higher correctness bar.

**OpenSSH is the conformance oracle.** `ssh(1)` from the host against a QEMU
guest via `--hostfwd` gives a real second implementation to test against, which
most of our protocols never get.

Scope for v1 — the non-goals are the security argument, not paperwork.
*Revised 2026-09-17 (spec §0): two items below inverted for SoftBMC's
interactive-console requirement; each says what it was.*
- [ ] Server only; **publickey AND password** auth, no keyboard-interactive.
      *Was "publickey auth ONLY".* The password is checked by an application
      callback, so AxlSsh stores no credential and hashes nothing — SoftBMC's
      existing PBKDF2/SCRAM user database stays the owner
- [ ] **One algorithm per slot** (curve25519-sha256 / ssh-ed25519 /
      chacha20-poly1305). Negotiation is a classic downgrade surface and we do
      not need the matrix
- [ ] **No process spawning; AxlSsh executes nothing itself.** *Was "no pty and
      no shell exec, a registered command table".* `pty-req`/`shell`/`exec`/
      `window-change` are surfaced to the application; SoftBMC binds them to
      its existing console take-over (`AxlConsoleDevice` + `AxlConsoleVtEnc`,
      NOT `AxlConsoleMirror`), the same one its browser Terminal already serves
- [ ] No port forwarding, no agent forwarding. *Was "no SFTP/SCP
      subsystem" too:* the `sshd` tool serves SFTP since 2026-10 (design
      `docs/superpowers/specs/2026-10-22-axl-sftp-design.md`) -- `sftp`, and
      `scp` from OpenSSH 9.0 on, through the `SftpServer.efi` helper behind an
      `exec`-style `subsystem` request. The library still serves no subsystem
      itself; it surfaces the request and the application answers it.
- [ ] Rekeying is not optional (RFC 4253 §9 RECOMMENDS it by ~1 GB / 1 h):
      implement it or cap the session and say so. **That is the RFC's
      recommendation, not what a stock peer does** — and conflating the two
      is what put a wrong number in the spec, the ROADMAP's status
      paragraph, a source comment and a handoff (all corrected 2026-09-20;
      the ROADMAP's copy said "one-hour", which a sweep grepping for "1 h"
      walked past, so sweep for the idea, not one spelling). OpenSSH's default
      `RekeyLimit default none` is the cipher's DATA limit only, ~1 GiB for
      chacha20-poly1305, with *"no time based rekeying"*
      (`man 5 ssh_config`, the page for the client that would ask;
      `sshd_config` carries the same sentence)
- [ ] Host key persistence via `AxlNvstore`
- [ ] **Security review is a GATE, not a phase.** This is network-facing crypto
      protocol code in firmware, before ExitBootServices, with no OS isolation
      beneath it. The mitigation is the narrowing above plus the fact that the
      primitives are audited mbedtls rather than hand-rolled

New protocol work: binary packet protocol (RFC 4253), version exchange, KEXINIT
+ key derivation, userauth publickey and password (RFC 4252), session channels
(RFC 4254).

**Spec:** [`superpowers/specs/2026-08-21-axl-ssh-design.md`](superpowers/specs/2026-08-21-axl-ssh-design.md)
— phased P1-P6 (revised 2026-09-17; P6 lives in the softbmc repo), each phase
its own plan, following the Axl9p pattern. **P3 (password auth) implementation
is SHIPPED 2026-09-17 -- see the "P3 plan" section below.** **Remaining, in
order:**

- [ ] **P4** session channels **+ rekey** — callbacks, §5.2 window flow control,
      exit status, several channels and connections, idle timeout, answered
      `GLOBAL_REQUEST`. The per-packet 35 KB payload allocation was pulled
      FORWARD out of P4 and is **DONE**: the inbound buffer now has the
      connection's lifetime, allocated once at the NEWKEYS transition. P4 owes
      it one thing — rekey must not allocate a second one (the allocation is
      guarded, and the guard's reason is written where it sits)

      **DESIGNED 2026-09-27 — spec §12a.** Five sub-phases, each its own
      plan, review and commit; the yardstick for every call is what sshd
      does, and the deviations are listed in §12a.7 with their reasons:
      - [x] **P4a** admission by eviction (`max_startups` 10 of 16
            pre-auth; a full server evicts the connection least far along
            in key exchange, the oldest among equals, and refuses only when
            none is still in it) + `REQUEST_FAILURE` for
            `GLOBAL_REQUEST` (**DONE 2026-09-27**: `REQUEST_FAILURE`, then
            `axl_ssh_server_set_max_startups`, then admission wired into
            `on_accept` -- the accept no longer pauses at the connection cap;
            `test-ssh-admission-qemu.sh` proves the victim order, the refusal
            with no victim, and that an authenticated connection is never
            evicted)
      - [x] **P4b** outbound coalescing (one send in flight per connection)
            + a pooled padding RNG + output backpressure (**DONE
            2026-10-08**: RELEASE, load 1-4, x64 61.4 → 7.8 µs per
            pipelined round trip, aa64 277 → 53.5; padding costs one
            firmware RNG call per 256 bytes; a peer that stops reading
            pauses the connection's input at 128 KiB queued instead of
            growing it without bound. Getting there fixed AxlTcp's large
            sends -- 8 KB paced pieces, a bounce buffer, never cancelling an
            armed Transmit -- which took a 1 MiB HTTP GET from 7-10 s to
            ~0.06 s. Spec §12a.2 "As built (P4b)")
      - [x] **P4c** session channels (**DONE 2026-10-09**): integer
            `AxlSshChannel` handles, opt-in through
            `axl_ssh_server_set_channel_ops()`, `session` type only;
            all-or-nothing `axl_ssh_channel_write()` with `AXL_BUSY` and an
            edge-triggered `writable` hook; 256 KB per channel, 4 MB server
            wide, with a fair-share floor so stalled peers cannot starve the
            rest; a retired handle keeps its wire slot until both CLOSEs;
            sshd's inbound rules (window 0 until the session starts, 10%
            overrun grace, nothing after the peer's CLOSE); contract-first.
            Measured on both arches: 17 session scenarios with a real
            ssh(1), 74 checks, including ten `ControlMaster` sessions on one
            connection with the eleventh refused `resource shortage`; a
            byte-level hostile suite of 14 scenarios, 43 checks; a
            zero-window peer holds the guest at 9-56 milli-cores where a
            sabotaged flush polling every iteration reads 996-997. Two
            open-failure reason codes differ from sshd's (§12a.7). Spec
            §12a.3 "As built (P4c)"
      - [x] **P4d** rekey (**DONE 2026-10-09**), peer- and server-initiated
            (`axl_ssh_server_set_rekey_limit()`, 1 GiB default, 2³¹ packets),
            keys staged per direction. Every rule is OpenSSH 9.9p1's, read
            from its source, which corrected the first design on four points:
            connection-layer messages are still processed after the peer's
            KEXINIT, refusals inside 1-49 and a second KEXINIT draw
            UNIMPLEMENTED, 5/6/7 are held outbound with 50+, and the
            counters reset per direction. One deviation: the held queue is
            capped at 128 KiB (sshd's is not). Measured with a real ssh(1)
            on both arches: 37 checks in the rekey suite (23 client-started
            exchanges in the interactive session, 8 server-started with 6
            sealing held packets) and 44 checks in a hostile-peer suite whose
            peer runs its own second key exchange and polices the wire order
            (the early-flush sabotage, which ssh(1) cannot see, fails it);
            CPU after listen 3.3-5.6% mean on both arches. Spec §12a.4 "As
            built (P4d)"
      - [x] **P4e** idle handling (**DONE 2026-10-10**): the unused-connection
            timeout (`axl_ssh_server_set_unused_timeout()`, 300 s) and
            client-alive (`axl_ssh_server_set_client_alive()`, 60 s × 3),
            both ON by default and both sshd's rules read from OpenSSH
            9.9p1: bytes restart the clock, every decoded packet the count,
            the count is incremented before it is compared, and the end is
            silent and abortive (no DISCONNECT), with sshd's two log lines.
            Three deviations: a probe due while output is paused is counted
            but not queued, a probe goes out on every tick it is due, and
            the reaper tick is the finest of 5 s and half of each period.
            It bounds the paused peer that half-closes, P4b's open case:
            ended 6.1-6.4 s (aa64) and 6.4-7.2 s (x64) after last heard
            against a 9 s bound, where the control without client-alive held
            its slot. Measured with a real ssh(1) and a byte-level peer on
            both arches: 53 + 54 checks, CPU after listen 0.015-0.018 cores
            (aa64) and 0.022-0.027 (x64) at a 1 s tick. Two library
            defects found and fixed on the way: `resume()` lost a receive
            wakeup when a hook dispatched the loop (`438b4918`) and the x86
            TSC was calibrated from one overshooting 10 ms stall
            (`87aeb576`; the 0.6 % fixed per-Stall bias that left was then
            cancelled by differencing a short and a long Stall, kept within
            2 % under the long Stall alone so a disturbed short Stall cannot
            make the clock run fast without bound). Spec
            §12a.5 "As built (P4e)"
      - [x] **P4f — `sshd`: the live UEFI Shell over SSH in QEMU (the MVP,
            added by the owner 2026-10-09).** A resident `sshd.efi` binding
            session channels to `AxlConsoleDevice`'s take-over of the Shell
            at the prompt: VT-encoded output to every session, typed input
            injected, `pty-req`/`window-change` sizing the console, a full
            repaint for a late joiner. `authorized_keys` read from the ESP,
            an optional password. `scripts/ssh-demo.sh` boots QEMU with a
            host port forward, so `ssh -p 2222 user@localhost` reaches the
            Shell from the host. A real-`ssh(1)` suite drives the actual
            Shell on both arches. The owner verifies it hands-on before the
            security gate, P5 and P6. It is P6 rehearsed in axl-sdk.
            **SHIPPED 2026-10-10** (spec §12a "As built (P4f)"): `sshd.efi`,
            `scripts/ssh-demo.sh` (`--check` runs in CI as
            `test-ssh-demo-script.sh`), `test-sshd-qemu.sh` and
            `test-sshd-lifecycle-qemu.sh`. Follow-ups, none blocking:
            - [ ] Reshape on request: treat `window-change` as a reshape
                  request snapped to the nearest mode (spec §13.1 b). Today
                  the console keeps its size: a larger terminal is confined
                  to it by a scroll region, a smaller one is held behind a
                  notice until it is enlarged.
            - [x] A persistent host key (P5): `sshd` keeps it in a UEFI
                  variable (NV+BS), made at the first start; `HostKey`,
                  `sshd hostkey` [--regenerate|--import].
            - [ ] The firmware-refused release and unload paths are unit-
                  pinned only (the decisions), not run: a guest cannot make
                  the firmware refuse.
            - [ ] Other test helpers may share the stale-staging pattern a
                  helper had (it staged an old `sshd.efi` on early aa64
                  runs); grep before trusting a green run there.
            - [ ] A refused, ordered control key re-fires its key notifies on
                  every retry while the ring is full (a pasted `^S` may halt
                  again per key); bounded and cosmetic.
      - [x] **P4g — compressed `.efi` images, so `sshd` loads fast over a
            slow link (added by the owner 2026-10-20, ahead of the P4
            security gate; DONE 2026-10-20).** A remote user boots tools from a USB stick
            attached through BMC virtual media, so every image crosses a slow
            upload link each run, and `sshd.efi` is 534 KB. A self-extracting
            wrapper (LZMA, the existing `AxlCompress` decoder) carries any
            `.efi` compressed and loads the original in memory, preserving
            argv, redirection, exit status and its own file identity. Every
            release tool and staged driver it meaningfully shrinks ships
            wrapped; AXL's driver loaders take wrapped or raw images alike,
            by content; downstream builds opt in. Target: `sshd.efi` x64
            around 200 KB or less. Security is out of scope (the gate
            follows). Spec:
            `docs/superpowers/specs/2026-10-20-axl-compressed-efi-design.md`.
            **Measured (RELEASE):** `sshd.efi` x64 534,528 -> **201,728 B**
            (target 204,800, limit 215,040), aa64 555,022 -> 184,334; all 39
            tools x64 4,466,688 -> 2,866,688 (23 wrap), aa64 4,642,850 ->
            2,615,842 (30 wrap); iPXE 1,149,440 -> 474,624 (x64), 1,150,464
            -> 417,806 (aa64); the stub 16,896 B (x64), 16,910 B (aa64). The
            per-tool table is in `src/pack/README.md`.
            `test-efi-pack-release-qemu.sh` runs the wrapped release images
            from a read-only USB stick on both arches (`sshd` key and
            password logins, iPXE through `netload`, a wrapped crash decoded
            by name). Follow-ups, none blocking:
            - [ ] A hand-made file over 64 MiB that names `.axlpack` goes to
                  the firmware, which loads the stub; the stub then refuses
                  its payload with one line (`axl-efi-pack` cannot make one).
            - [ ] **P4 security gate:** wrapped images and Secure Boot: sign
                  the inner image; decide whether the packer should sign or
                  refuse.
            - [ ] Format tests: a fail-after-N allocator for the decoder's
                  `SZ_ERROR_MEM` path and encoder OOM.
            - [ ] Format tests: split the four-field check; add
                  filter == NONE; comment the unreachable encoder OUTPUT_EOF.
            - [ ] `axl_pack_find_payload`: two `.axlpack` sections (first
                  wins) and a zero-size one untested.
            - [ ] `pe_build_wrapped` (AxlTestPack) lacks a NULL check.
            - [ ] `src/pack/README.md` nits: the NONE "not shipped" line;
                  `headers_short` unmentioned; a heading splits the stub
                  channel table.
            - [ ] Host packer: `deps/lzma` headers are not prerequisites of
                  its rule.
            - [ ] `test-efi-pack-host.sh`: `synth_mixed`'s message when
                  openssl is missing misleads; the unwritable-output case
                  asserts only rc 2.
            - [ ] `check-reloc-coverage.py` reports "objdump could not run"
                  as "no RELA" (pre-existing; it now gates the stub).
            - [ ] `test-efi-pack-qemu.sh`: the driver-load window is
                  asserted by single-line greps.
            - [ ] The stub's device-path-dup OOM path is untestable
                  (accepted: pool exhaustion is not injectable).
            - [ ] Tools: `.so` placement and wrapped-ness are asserted for
                  `sshd` only.
            - [ ] `$(PREFIX)/release-drivers` is not in the build-signature
                  wipe; `clean-tools` leaves `pack/` intermediates.
            - [ ] `axl-cc`: a stale `<out>.raw.efi` survives a later build
                  without `--compress`.
            - [ ] `axl-cc`: a packed `--service` launcher is never booted.
            - [ ] `check-flag-parity`: no ld-flag parity for the stub link.
            - [ ] `pack-toggle-raw.efi`: no error rule when the private raw
                  is not staged.
            - [ ] Release test: a `builds=` comment for `sshd`; a blank size
                  echo when `stat` fails.
            - [ ] Over-long CHANGELOG/ROADMAP lines from P4g.
            - [x] iPXE: a stale `.unavailable` can leave a CACHED SKIP after
                  a hand build. (Moot: the bundled iPXE was removed.)
            - [x] iPXE: the test's own `make ipxe` retries inside the timed
                  region when offline. (Moot: removed.)
            - [x] iPXE: a failed rebuild after a pin bump keeps the old
                  driver. (Moot: removed.)
            - [ ] Pre-existing, not P4g: `kbtune-drv` hangs the Shell if left
                  loaded after `kbtune` exits.
            - [ ] Pre-existing: `verify.sh`'s x64 job overruns its 95 s
                  wall-clock budget when all six jobs run (passes alone).
                  Measured 2026-10-21, back to back on one box: 105 s before
                  P4h (`3e5cd4be`) and 107 s after it with all six jobs;
                  57 s and 59 s alone. P4h did not cause it.
      - [x] **P4h — `sshd` that behaves like the real Shell: key fidelity,
            `sshd.conf`, `lsusb` notes, test releases (added by the owner
            2026-10-20, ahead of the P4 security gate; DONE 2026-10-21).** A
            remote user reaching the Shell over `ssh` instead of a BMC
            virtual console must find it behaving exactly like that console.
            Spec: `docs/superpowers/specs/2026-10-20-sshd-fidelity-config-design.md`.
            - **Keys:** every key `inject_text` decodes has the shape an
              OVMF USB keyboard delivers (`KeyShiftState` VALID plus its
              modifiers, `KeyToggleState` `0xC0`): C0 bytes are Ctrl chords,
              xterm modifier parameters, SS3, `ESC[Z`, ESC-prefix Alt,
              Ctrl+Space, CSI-u and modifyOtherKeys decode. Measured, not
              tabulated: `test-sshd-keys-qemu.sh` (54 xterm rows) and
              `test-sshd-keys-ext-qemu.sh` (10 CSI-u rows) press each on
              QEMU's `usb-kbd` (the oracle, proved to be the only keyboard)
              and over ssh through tmux, and the two `KEY` lines must be
              byte-identical (67 + 30 checks, both arches).
              Ctrl-End in `edit` shows "Unknown Command" over ssh, as on the
              USB keyboard. An Alt chord (ESC + a key) forms only within
              one ssh packet; across a packet boundary it stays Esc, then
              the key (so `^C` is still the break, and Esc then `x` is two
              keys).
            - **Local type-ahead:** the console device splits a key's
              arrival from its delivery (a 256-key holding queue behind the
              63-key ring), so a long paste at the local console arrives
              whole while `sshd` runs, and a local Ctrl-C or a consumer's
              hotkey acts on arrival.
            - **`sshd.conf`:** beside `sshd.efi`, OpenSSH `sshd_config(5)`
              syntax and keywords (15 OpenSSH keywords plus four `Axl*`),
              defaults < file < flags, the rest of OpenSSH 9.9p1's keywords
              refused by name (the ones OpenSSH ignores are ignored, with
              its warning), `sshd -T` in OpenSSH's format; `AxlConfigFile`
              gained the OpenSSH grammar with line numbers,
              `axl_args_is_set()` and `axl_ssh_penalty_range_check()` the
              rest. `-k ""` / `-P ""` mean none.
            - **`edit` over ssh:** open, modify, save (F2), exit (F3)
              cycles with the file's exact bytes asserted after each
              (`test-sshd-edit-qemu.sh`).
            - **`lsusb`** says `no USB host controller found` / `no USB
              devices found` (exit 0) instead of printing nothing, exits 1
              on an unmatched filter, and is tested against a real QEMU
              topology on both arches (`test-lsusb-qemu.sh`).
            - **Test releases:** `scripts/make-test-release.sh` builds both
              arches, gates each on the release-image test booting the very
              tarball it publishes, and publishes a Pre-release tagged
              `test-<YYYYMMDD>-<sha8>` on `aximcode/axl-sdk-releases`; no CI,
              no `v*` tag. The release-image test also runs a compressed
              tool, a pipe, `help` and an `edit` save over ssh.
            - **Found and fixed on the way:** a DHCP bring-up returned as
              soon as ANY NIC had an address (`e712db9a`; real servers have
              several NICs); a Reset dropped keys typed while it ran; every
              `axl-cc --embed` link printed an ld exec-stack warning (now
              `-z noexecstack`, and in-tree links fail on any ld warning);
              `qemu_stage_disk` returned 0 after a failed `dd` or
              `mkfs.vfat`.
            - **Measured:** `sshd.efi` x64 RELEASE 214,528 B after the third
              fix wave (214,016 after the D8 fixes, 213,504 before them; P4g
              201,728; limit 215,040 -- 512 B of headroom left), aa64
              195,598 B (unchanged by the third wave; 195,086 before D8; P4g
              184,334).
            - **Limit raised to 219,136 B (214 KiB), 2026-10-21,** by owner
              decision, for the start-up diagnostics (`-v`/`-d`, `LogLevel`,
              the bounded network bring-up that skips link-down NICs, the
              serving address): x64 RELEASE measured 217,088 B with them,
              leaving 2,048 B of headroom.
            - **`sshd` alone gets a box online (netload's engine), and the
              limit is raised to 235,520 B (230 KiB), 2026-10-21,** by owner
              decision ("don't worry about the size limit for now"). A
              dual-socket server whose onboard NICs were all link-down and whose
              firmware had no driver for the USB NIC showed the gap; `sshd`
              now runs `axl_net_auto_init_opts` -- firmware-first, the link
              gate, a crash-safe sweep of `drivers\<arch>` beside `sshd.efi`
              with `netload-drivers.json5` companions, one 60 s deadline,
              Ctrl-C -- instead of its own bring-up. The engine's companion
              loader and reachability check became opt-in in code; `sshd`
              installs the loader (the change is +15,872 B packed, about 8 KB
              of it the JSON5 sidecar reader) and not the check. x64 RELEASE
              232,960 B, aa64 211,982 B; the limit is x64
              rounded up to the next 2,048 B plus 2,048 B.
              Trimming candidates if the limit has to come back down: drop
              the companion loader from `sshd` (about 8 KB packed) -- USB
              CDC/RNDIS dongles would then need `netload` first.
            - **`AxlNetworkTimeout` / `--net-timeout`, and the sweep's review
              minors, 2026-10-21:** the bring-up's deadline is an `sshd.conf`
              keyword (1-86400 s, default 60; `-T` prints it), so a box with
              a slow USB NIC or many cabled ports is not stuck at 60 s. The
              link gate now lists NICs afresh during its wait, so a NIC
              whose interface appears then (a USB dongle still enumerating)
              is found -- `test-net-gate-qemu.sh` hot-plugs QEMU's usb-net
              into the wait (x64; aarch64's firmware binds no driver to it).
              The same test sweeps a candidate whose Unload refuses: it stays
              resident, its companions are unloaded, `dh -v` still walks the
              box. `axl-net.h` says a companion the winner relies on without
              declaring it is unloaded. `test-sshd-start` aarch64: 182 s ->
              151 s (the no-lease boot is two NICs under a 25 s
              `AxlNetworkTimeout`). Size unchanged: x64 RELEASE 232,960 B,
              aa64 211,982 B; the limit stays 235,520 B.
            - **`sshd-rtk.efi`, 2026-10-21:** sshd with Realtek's USB NIC
              driver embedded (`make sshd-rtk RTK_USB_UNDI=...`, packed),
              tried first after the firmware's NICs through the engine's new
              `AxlNetAutoOpts.embedded`; one service with `sshd`; shipped only
              by `--with-rtk DIR` on the release scripts. QEMU proves it with
              an embedded e1000 driver (`test-sshd-rtk-qemu.sh`); the Realtek
              driver itself is real-hardware territory.
            - **DHCP on every link-up NIC at once, 2026-10-21:** the link
              gate's pass sets DHCP on all its link-up NICs and waits one
              budget for the first lease (Linux `ip=dhcp`, systemd-networkd,
              dhcpcd); the losers' IP4Config2 policies are put back. Three
              server-less firmware NICs plus a swept USB-class NIC: 49 s ->
              19 s to serving (x64).
            - **Measured after the three, 2026-10-21:** `sshd.efi` x64 RELEASE
              235,008 B, aa64 214,030 B (after the review fix round); the
              limit is 237,568 B (x64 rounded up to the next 2,048 B plus
              2,048 B).
            - **Hands-on pass and final review (D8), fixed in phase:**
              a tool's error lines (StdErr) never reached a session -- the
              console device now joins ConSplitter's StdErr fan-out
              (`gEfiStandardErrorDeviceGuid`); a `Banner` or
              `authorized_keys` written in the Shell (UTF-16LE + BOM) was
              refused -- one decoder, `axl_text_to_utf8()`, reads every text
              file sshd and `AxlConfigFile` read; the goodbye on a terminal
              larger than the console landed on row 2 over the console's
              text -- DECSC/DECRC around the region reset, erase to end of
              line; "beside `sshd.efi`" meant the volume root, so a
              `sshd.conf` in the release's versioned directory was ignored --
              it is now `sshd.efi`'s directory, and a start names the file it
              read or looked for; Esc then a key in the next packet became
              Alt+key. Harness/gates: a SIGKILLed test no longer orphans its
              QEMU (`setpriv --pdeathsig` in `test_run_background`);
              `check-flag-parity` compares the two image links' ld flags; the
              tools tarball stages `SHIPPED_TOOL_NAMES` by name, not
              `tools/*.efi`; the sshd suites fail, not skip, under CI without
              `ssh`/`ssh-keygen`.
            - **Second hands-on pass, fixed in phase:** `sshd`'s own log
              (`ssh: Accepted password ...`) was drawn in every session once
              the device joined StdErr -- while it serves, sshd now logs to
              the first serial port in the console's format, through the new
              `axl_log_set_console_serial()` (OpenSSH logs to syslog, never
              to the user's terminal); on firmware whose `ErrOut` lists the
              display, a take-over now moves that screen off StdErr with
              ConOut and back on uninstall, so an error line is drawn once
              (`test-console-device-errout-qemu.sh`, which copies ConOut into
              ErrOut to make OVMF such firmware); the StdErr check now reads
              the session's RENDERED screen (tmux), with ground truth taken
              from the GOP first -- the session and the screen agree;
              `axl_ssh_server_load_authorized_keys` no longer returns
              `AXL_NO_RESOURCES` (= "more than 16 keys") for out of memory;
              `axl_text_to_utf8()` decodes UTF-16 in place and sets its
              length outputs on every return; the release-image test stages
              the stick with the tarball's versioned directory kept.
            - **Third fix wave:** one "beside the running image",
              `axl_app_image_dir_path()` in AxlApp, replaces sshd's and netload's
              own copies (a relative name under the `.efi`'s directory, a
              leading backslash from the volume root, a volume path as it
              is); sshd's stop puts
              the console log back as it found it
              (`axl_log_get_console_enabled()`, new); a take-over's uninstall
              re-tags a screen's ConOut before its StdErr, in separate calls,
              so a StdErr tag that came back meanwhile cannot keep the screen
              out of ConOut (`test-console-device-errout-qemu.sh` boot 3);
              `test-log-redirect-qemu.sh` reads the redirect's raw bytes --
              colourless, silenced by `set_console_enabled(false)`, standard
              error's colour again after NULL; netload's nested-stick case
              fails by name when the beside-image lookup is reverted, and
              passes on aa64.
            Follow-ups, none blocking:
            - [x] OWNER DECISION: mid-screen, the Shell draws its next prompt
                  over the first columns of a StdErr line (it places the
                  prompt where IT left the cursor, which a StdErr write does
                  not move). A real screen in `ErrOut` does the same -- the
                  GOP shows `FS0:\> x bogus is not an IPv4 ...` exactly as the
                  session does -- so the session is faithful; at the bottom
                  of the screen the line stays whole on both. Fixed for
                  every console (2026-10-22): on the console, standard
                  error is written through ConOut, the stream the prompt
                  is placed from (`src/stream/README.md`, *Standard error
                  on the console*).
            - [x] `netload`'s default driver directory is `drivers\<arch>`
                  (then `drivers`) beside `netload.efi`, falling back to the
                  volume root's forms for a flat stick; `--dir` overrides.
            - [ ] `axl_net_get_driver_info()` names a disk-loaded driver by
                  the FIRST `MEDIA_FILEPATH` node of its image's path
                  (`resolve_driver_cb`): a firmware that splits a path across
                  nodes, one per component, would report only the first.
                  OVMF's Shell loads with one node.
            - [x] Loading `ipxe-all.efidrv` from a USB stick takes the USB
                  controller: after `netload -p ipxe-all.efidrv` both USB
                  volumes leave the map (`map -r` shows only BLK0) and the
                  Shell drops its current directory. A stick on BMC virtual
                  media is USB too. Resolved by removing the bundled iPXE
                  (2026-10-21).
            - [ ] An out-of-memory base64 decode of an `authorized_keys`
                  line reports `AXL_INVALID` ("malformed line"):
                  `axl_base64_decode` returns `AXL_ERR` for both.
            - [ ] PuTTY's `ESC[11~`..`ESC[14~` F-keys (a non-goal for P4h),
                  Alt with a non-ASCII key, old xterm's `ESC ESC [A`
                  Alt-arrows.
            - [ ] `LoginGraceTime 0` (OpenSSH: no limit) is refused, as are a
                  second `Port` and an `AuthorizedKeysFile` naming several
                  files (sshd listens on one port and reads one file).
            - [ ] A `Port` too large says `integer value too large.` where
                  OpenSSH says `Badly formatted port number.`; `sshd -T`'s
                  `rekeylimit` prints the byte limit in force where OpenSSH
                  prints `0 0`.
            - [ ] A live proof of every sshd driver setter (only some are
                  driven end to end); the unnamed assertions that passed
                  before Task 3's implementation (its RED was retroactive).
            - [ ] `lsusb`'s `cannot enumerate USB devices` has no automated
                  test (no injectable failure in QEMU); `test-lsusb-qemu.sh`'s
                  est=60 per leg is tight against aa64's 150 s.
            - [ ] The release suite's title-bar poll has no explicit failure
                  message.
            - [ ] Keys beyond the 256 held stay in the evicted firmware queue
                  across a guest Reset; dropping the take-over discards up to
                  319 arrived keys (teardown semantics).
            - [ ] tmux 3.4's path through the edit and keys suites is not
                  exercised in CI (the self-hosted runner finds 3.5).
            - [ ] The release tarball's root directory is mode 0700
                  (pre-existing): make it 755, as a CHANGELOG-noted change.
            - [ ] `check-tool-docs` anchors on the exact `./scripts/`
                  spelling (a `python3 scripts/...` rewrite would false-fail,
                  the safe direction).
            - [ ] An `LDFLAGS_EFI` change does not relink existing images
                  (the build-state signature covers compile flags only).

      **P4 has a measured throughput profile — use it instead of guessing**
      (spike 2026-09-26, transport-level, x64 KVM + aa64 TCG, RELEASE; the
      numbers and their caveats are in
      `.superpowers/sdd/2026-09-17-axl-ssh-p3-password/task-throughput-spike-report.md`).
      The use case it was built around is `idrac-push.sh`: bulk bytes through
      an interactive pty because the peer has no sftp subsystem, 2.1 MB in
      ~10 s by zmodem and ~32 s by base64 on real hardware.

      - **The transport is not the constraint.** It fits a
        **1.88 µs/packet + 34.7 MB/s** line on x64 (nearly flat from 64 B to
        32 KB) and 16.0 µs + 8.60 MB/s on aa64/TCG. Projected against that
        use case: 0.07-0.2 s on x64, 0.34-1.17 s on aa64, against a 10-32 s
        reference. **So choose the window and packet size for correctness
        and memory, not for speed.**
      - **Window 2 MB** (OpenSSH's default). By default (ON_ARRIVAL) it is an
        accounting number, not a buffer — inbound is consumed synchronously
        in `pump`; an ON_CONSUME channel (W4) makes it the bound on what the
        application holds — so bound
        memory with `RX_BACKLOG_MAX`, not by shrinking it. Arithmetic over a
        measured 5.7-9.6 ms RTT: anything under ~256 KB caps below the
        transport, and 32 KB over a 50 ms WAN gives 0.66 MB/s.
      - **Max packet 32 KB.** No cliff, and the connection already owns a
        35,000-byte buffer.
      - **Do NOT make the data path allocation-free.** Measured 0.23% of wall
        time at 32 KB, 4.9% at 64 B on the slower guest. It is 1 allocation
        inbound and 6 outbound; removing them buys under 1%.
      - **BUILD FIRST: coalesce outbound.** An outbound packet costs ~95 µs
        against ~4.5 µs inbound — **20x** — and ~97% of it is the
        `axl_tcp_send_async` completion path, not CPU. A channel that emits
        one SSH packet per application write caps near **10,500 writes/s at
        any write size**, so pty output must be coalesced into ≤32 KB
        packets.
      - **Then buffer the padding RNG.** `axl_rng_bytes` in the seal is a
        synchronous `EFI_RNG_PROTOCOL` call: 700 ns on x64, **18,170 ns on
        aa64 — 52% of a small outbound packet**, and the only per-packet item
        whose share GROWS as the target slows (26x between guests, against
        8.5x for per-packet cost generally).
      - **`RX_CHUNK` is not a throughput knob** — measured: EFI_TCP4 returns
        about one MSS (1,463 B) per `Receive()` whatever buffer it is given,
        so 2048/8192/32768 made no difference.
      - **The sink does not dominate here, and that says less than it looks**:
        2.1 MB through an AxlFs writer is ~16 ms on x64 (~1.3 ms of writes
        plus a flat ~15 ms `close()`). But the per-byte rate is the host page
        cache behind a virtual disk, so a P4 test that looks fast here proves
        NOTHING about real eMMC or NVMe. Keep the two useful numbers — ~1.13 µs
        fixed per write call, ~15 ms per close — and discard the bandwidth.
      - **Allocator numbers taken from a test run overstate the shipped cost**:
        the suite builds DEBUG, where the allocator is 8.7x dearer at 32 KB.
- [x] **P5** persistent host key + disconnect hygiene -- `<axl/axl-ssh-host-key.h>`
      (a UEFI variable, NV+BS, never regenerated silently; provider-free
      parse/fingerprint), `axl_ssh_server_set_host_key()`, sshd `HostKey` /
      `sshd hostkey`; every DISCONNECT on a healthy stream delivered, or reset
      at `AXL_SSH_DISCONNECT_LINGER_MS`. Survives a reboot on both arches
      (test-sshd-hostkey-qemu.sh); test-ssh-disconnect-qemu.sh
- [ ] **P6** *(softbmc repo)* bind channels to the console take-over, password
      callback over SoftBMC auth, the `console_ws` role gate, licence/config
      gates
      - Follow-up: a BMC application's console binding frees its encoder
        after an ignored `axl_console_device_uninstall()` (its reload path,
        from a tick). Handle `AXL_BUSY` (retry next tick) and `AXL_ERR`
        (keep the encoder, stay loaded) as fbcon and `sshd` do; the call is
        `AXL_WARN_UNUSED` now, so the build points at it.
      - Follow-up: the encoder now writes EFI_YELLOW as SGR 93 (it was 33),
        so a browser terminal (xterm.js) draws the Shell prompt in its
        brightYellow, not its yellow -- in a theme where those differ
        (#ffcc44 against #ffaa00), the dashboard's prompt changes colour.
        Check the look when the binding lands.
- [ ] **Open decisions** before P6 — spec §13: terminal geometry (SSH has no
      server-to-client resize), SoftBMC's password timing oracle, and a
      guessing throttle that backs off rather than locking out the only admin
      -- for SSH, RESOLVED 2026-09-18 in AxlSsh (Task P, spec §13.4); SoftBMC's
      web login still needs one.
      (§13.2, how a publickey login gets a role, and the §13.5 defaults were
      settled 2026-09-17.)
- [x] **Inbound backpressure: withhold `WINDOW_ADJUST` until the application
      consumes** (W4, DONE). `axl_ssh_channel_set_window_mode()` /
      `axl_ssh_channel_consumed()` / `axl_ssh_channel_unconsumed()`:
      `AXL_SSH_CHANNEL_WINDOW_ON_CONSUME` credits a byte only when the
      application reports it; ON_ARRIVAL (today's behaviour) stays the
      default. Adjusts follow OpenSSH's rule (three maximum packets). sshd's
      `sftp` channel adopted it. **sshd's SHELL deliberately did not**
      (controller ruling): ON_CONSUME would raise its input heap from the
      64 KB queue to ~2.2 MB per session and lose "Ctrl-C breaks a script
      behind a paste" (and `~.` at a zero window), so the shell keeps
      dropping input past its 64 KB queue, logged at DEBUG.
**P1 plan (transport):**
[`superpowers/plans/2026-08-21-axl-ssh-p1-transport.md`](superpowers/plans/2026-08-21-axl-ssh-p1-transport.md)
— **ALL SIX TASKS SHIPPED 2026-09-05.** `src/net/axl-ssh-{buf,packet,kex,crypt,server}.c`,
`axl-ssh-internal.h`, the two public headers, ~120 unit assertions and a
9-assertion `ssh(1)` conformance test:

- [x] **Task 1** SSH wire codec, attacker-controlled lengths checked (`2d2cea7a`)
- [x] **Task 2** version exchange, `AXL_INCOMPLETE` distinct from malformed (`0e978806`)
- [x] **Task 3** binary packet protocol, both length fields bounded (`3e58042c`)
- [x] **Task 4** KEXINIT, one algorithm per slot, refusing rather than downgrading (`6c2427a6`)
- [x] **Task 5** RFC 4253 key derivation, pinned to independently computed bytes (`4c005264`)
- [x] **Task 6** curve25519 kex + AEAD transport + the server (`967a1978`) —
      **P1 COMPLETE 2026-09-05.** OpenSSH 9.9p1 completes the handshake against
      the guest and is refused at userauth: 9 passed / 0 failed, both arches
      (`test/integration/test-ssh-transport-qemu.sh`). Adds mpint, the host key
      and signature blobs, the exchange hash, `KEX_ECDH_INIT`/`_REPLY`,
      `chacha20-poly1305@openssh.com` and the state machine. Two sabotages that
      leave every UNIT test green — the two ChaCha keys swapped, and `Q_C`/`Q_S`
      swapped in the hash — fail against `ssh(1)` at two different boundaries,
      which is the oracle argument stopping being a slogan.
      **Step 7's security review is DONE and its blocking findings are
      fixed** (`58fde4dc`, `6c2f5995`): two pre-auth denials of service, an
      unbounded pre-auth outbound queue, K left unscrubbed in the UEFI pool,
      a silently-ignored rekey, and an unread `first_kex_packet_follows`.
      `test-ssh-hostile-qemu.sh` is the regression guard the review said was
      missing — the conformance test drives one SUCCESSFUL session and so
      never takes an error path.
      A scoped re-review of the fixes (`38585974`) then found a denial of
      service the connection cap had CREATED — 16 silent peers pinning the cap
      forever, since only `conn_drop` re-armed the accept — plus a silently
      lost accept re-arm, two unscrubbed key buffers on error paths, and
      `first_kex_packet_follows` checking only half of what RFC 4253 §7.1
      names. All fixed there.

      **Five LOW findings remain open** and are the gate's residue, not
      blockers: a V_C skew when an ident line contains a bare LF,
      `packet_wrap` bounding the payload where `unwrap` bounds the frame,
      `put_string(NULL, n>0)` returning OK, a caller-only overflow in
      `aead_seal`, and a 35 KB per-packet allocation that could be hoisted to
      the connection. **The sixth, `SSH_MSG_IGNORE`/`DEBUG` being fatal before
      `ST_AUTH`, is CLOSED (2026-09-20)** — not by relaxing the parser but by
      advertising `kex-strict-s-v00@openssh.com`, which makes terminating on
      those messages during key exchange the *required* behaviour rather than
      an over-strict one (OpenSSH `PROTOCOL` §1.10 rule (a); Terrapin,
      CVE-2023-48795). Outside key exchange AxlSsh already tolerates them, so
      RFC 4253 §11 is met where strict KEX does not override it.
      **`SSH_MSG_UNIMPLEMENTED` is now SENT** (2026-09-20) for any message
      AxlSsh does not implement, which RFC 4253 §11.4 makes a MUST and which
      shipped as a silent ignore. Bounded at 8 replies per connection --
      unbounded, it is the same pre-auth amplifier `MAX_SERVICE_REQUESTS`
      closed -- and past the bound the message is ignored exactly as before,
      so the cap bounds the reply and never the connection.
      **`GLOBAL_REQUEST` is now answered with RFC 4254 §4's
      `REQUEST_FAILURE`** (P4a, 2026-09-27) rather than this UNIMPLEMENTED
      stand-in -- including `keepalive@openssh.com`, where the reply IS the
      client's liveness signal.
      Also worth a pass: five constants defined twice with no
      compile-time link between the public header and the implementation

The plan was materially wrong in places and was corrected while implementing:
a census over its `axl_*`/`AXL_*` tokens found **12 symbols that do not
exist** (`AxlStrBuf` is `AxlString`, `axl_digest_*` is `axl_checksum_*`;
`AXL_INCOMPLETE` was *added* to `AxlStatus` as −11 rather than renamed), and it
named two registration points for a new test binary where there are three.
Re-censused at Task 6 and the same 12 were still wrong. Two more things it got
wrong, both found while implementing: its integration-test sketch used
`run-qemu.sh --hostfwd auto:22` directly where every other test in the tree
goes through `common-test.sh`'s `test_setup`/`test_port`/`test_add_port_forward`
— and its `grep -q …` + `check "$?"` idiom CANNOT REPORT A FAILURE under the
`set -euo pipefail` that file installs, which is how a sabotage came back
"detected" with no FAIL line. Worth knowing before writing P2's plan.

**P2 plan (userauth):**
[`superpowers/plans/2026-09-07-axl-ssh-p2-userauth.md`](superpowers/plans/2026-09-07-axl-ssh-p2-userauth.md)
— **ALL SIX TASKS SHIPPED 2026-09-07.** `src/net/axl-ssh-auth.{c,h}`, plus
additions to `axl-ssh-internal.h`, `axl-ssh-kex.c`, `axl-ssh-buf.c` and
`axl-ssh-server.c`, the two new public functions on `AxlSshServer`, and a
purpose-built fake `ssh-agent` for the hostile-input test:

- [x] **Task 1** `authorized_keys` store and its line parser — an options
      prefix (`command=`, `no-pty`, `from=`) is refused rather than silently
      dropped (`b2c55a85`, `e3804b8b`, `009acc4d`, `fe19f226`)
- [x] **Task 2** blob parsers and the RFC 4252 signed blob (`534c439f`,
      `0a17fdfd`)
- [x] **Task 3** `axl_ssh_server_add_authorized_key` /
      `_load_authorized_keys`, and `axl_ssh_server_listen` now refuses to
      bind with no authorized key (`709991f4`, `c958ef38`)
- [x] **Task 4** the userauth state machine, both RFC 4252 forms
      (`8d678930`, `ee72184c`)
- [x] **Task 5** `ssh -i key` as the P2 conformance test — a real key
      authenticates, a wrong key does not: 7 passed / 0 failed, both arches
      (`test/integration/test-ssh-auth-qemu.sh`) (`a7b3db3f`, `701a125d`,
      `625edc16`)
- [x] **Task 6** the auth phase under abuse, reached through a fake
      `ssh-agent` (`test/integration/ssh-fake-agent.py`, stdlib only) —
      the existing hostile driver speaks no crypto by design and cannot get
      past key exchange on its own. `test-ssh-hostile-qemu.sh` is now 22
      passed / 0 failed, both arches (`6fa298de`, `9e7cb351`)

**The failure policy is asymmetric, and that is the point.** A key not in
the authorized set is refused **without dropping the connection** — `ssh`
walking its key list, not an attack — while a forged signature under an
AUTHORIZED key ends the connection immediately, whether or not the client
asked about the key first. A key
that is well-formed but cryptographically unusable (a non-canonical
encoding, a small-order point) loads successfully and is refused only at
verify time, because the Ed25519 provider validates at the one door every
signature passes through rather than at every import site.

Commit range: `ddf8d517`..`9e7cb351`. **Task 7's security review (spec
§11's merge gate) is CLOSED.** The initial pass returned 2 HIGH / 2 MEDIUM
/ 6 LOW. It found no path to `USERAUTH_SUCCESS` without a verified
signature, confirmed the signed region is the verbatim arrived bytes
covering every attacker-alterable field, and confirmed authentication is
bound to the session id — every finding was resource-lifetime or
secret-hygiene around or beneath the new code, not in the authentication
decision itself. HIGH-1 (an unbounded pre-auth outbound queue: an
unanswered `SERVICE_REQUEST` had no per-connection cap, unlike the userauth
free form right beside it), HIGH-2 (`born_ms == 0` meaning both
"authenticated, never reap" and "the monotonic clock is unreadable" — on a
platform whose clock cannot be read this made every connection look
already-authenticated, silently disabling the login grace so that 16 idle
connections could pin the connection cap forever), MEDIUM-1 (the shared
secret and both directions' derived AEAD keys surviving, unscrubbed, in a
freed digest context) and LOW-1/LOW-2 (an elidable `axl_memset` scrubbing
session keys, and `signed_data` freed without scrubbing) were fixed
(`6d39ab11`, `a2f5eec4`).

A scoped re-review then attacked the fixes themselves rather than taking
the reasoning on faith and confirmed all five addressed — including that
neither HIGH fix reintroduces the previous phase's failure mode under
attack, and that the `SERVICE_REQUEST` cap does not trip on any conforming
client flow. It found four new LOW/informational findings. **NEW-2**
(neither half of the HIGH-2 fix had a committed regression guard) is now
half-closed: `axl_ssh_reap_reason()` (`src/net/axl-ssh-reap.h`) pulls the
whole authenticated/deadline decision out of `on_reap` as a pure function
of plain values, `AxlTestSsh` pins that an authenticated connection is
exempt from reaping even past an expired deadline, and a sabotage that
removes the exemption (so an authenticated connection falls through to the
same deadline check as an unauthenticated one) turns exactly those two
assertions red and no others. The listen-time clock-failure probe — NEW-2's
other half — still has no injection seam and remains unguarded; building
one to fake a hardware clock failure was judged not worth it for this pass.

**MEDIUM-2 is FIXED** (`2787c468`, 2026-09-07). `axl_mbedtls_entropy_poll`
no longer reports the clock-seeded fallback as gathered entropy. On a
platform with no working `EFI_RNG_PROTOCOL` the per-source threshold now
goes unmet, `mbedtls_ctr_drbg_seed()` fails, and every consumer the DRBG
seeds -- `axl_pk_key_new()`, `axl_ecdh_new()`, `axl_tls_init()`,
`axl_ssh_server_new()`, `axl_aead_seal()` -- returns its error or `NULL`
sentinel instead of a predictable key. **Fail closed by default**: the
tradeoff this item used to pose is resolved in favor of "a key nobody can
generate beats a key an attacker can guess."

An explicit, off-by-default opt-in exists on `<axl/axl-crypto.h>` for a
platform that genuinely has no RNG whose operator accepts the risk:
`axl_crypto_set_weak_entropy_allowed(bool)`, read back with
`axl_crypto_is_weak_entropy_allowed()`. Seeding is lazy and cached for the
process lifetime, so the call must precede `axl_pk_key_new()`,
`axl_ecdh_new()`, `axl_aead_seal()`, `axl_tls_init()`,
`axl_ssh_server_new()` or any TLS handshake. Every draw that actually
falls back logs at `axl_warning`, every time it happens, not once.
Measured before committing to fail-closed: `EFI_RNG_PROTOCOL` is present
and working on both OVMF (x64) and AAVMF (aa64), so fail-closed costs this
project's own test suite nothing.

**What proving the fix uncovered is worse than what it fixed, and gets its
own entry here rather than a footnote to a fixed item.** The hardware
branch this fallback was supposedly a fallback FROM had never once
worked: the poll called `((RNG_GET_RNG *)rng_proto)[0]` -- vtable member
0. In `EFI_RNG_PROTOCOL`, member 0 is `GetInfo` and member 1 is `GetRNG`
-- the tree's own tested binding says so at `src/util/axl-rng.c:24-34`.
So the "hardware RNG" path was calling `GetInfo(This, NULL, output)`,
which returns `EFI_INVALID_PARAMETER`, and falling through to the weak
path -- **on every platform, for the entire life of that code.** Every
key this SDK has ever generated came from the xorshift-from-clock
fallback, not hardware: Ed25519 SSH host keys, X25519 ephemerals, and TLS
session keys alike, not only on boards lacking an RNG -- on all of them.
Fixed in the same commit by deleting the hand-rolled binding and calling
`axl_rng_bytes()`, which has the struct right and is unit-tested on both
arches -- one binding per protocol, the rule `gate.check-dogfood` exists
for.

**Any key persisted from an earlier build is clock-derived and must be
rotated, not trusted.** SSH host key persistence is P4, not yet built, so
today's blast radius is smaller than it could have been -- but say it
anyway: the same DRBG backs every `axl_pk_key_new()` and `axl_tls_init()`
caller in the SDK, and anything already exported or pinned by a consumer
from a build before `2787c468` is suspect, regardless of algorithm.

**The lesson, because it is the whole argument for fail-closed:** a
silent fallback did not merely weaken the keys, it hid the fact that the
strong path was broken. A fail-closed design would have surfaced this the
first time anyone ran it. Weigh that the next time "should this degrade
gracefully?" looks like the kind option -- a silent fallback and "the
primary path can fail invisibly forever" are the same mechanism.

**The residue, all LOW or informational and none blocking, deliberately
left as-is:**
- **LOW-3** `conn_drop`'s accept re-arm does not reset `s->accept_errs`,
  so once `MAX_ACCEPT_ERRS` has been reached a re-arm from `conn_drop` is
  immediately re-paused by the next failed accept; `on_reap` does reset
  it, so recovery is bounded at one reap tick (5 s) — a degradation, not
  an outage.
- **LOW-4** V_C reconstruction in `pump` (walking back to the previous
  bare LF) disagrees with `axl_ssh_parse_ident`'s strict CRLF split when a
  peer's identification line contains a bare LF, so the two sides compute
  different exchange hashes and the connection dies at the host-key
  signature check — not a security hole (a client cannot make the two
  sides *agree* on different transcripts), just a parser/reconstructor
  disagreement worth removing.
- **LOW-5** `ST_SESSION`'s `CHANNEL_OPEN_FAILURE` reply has no
  per-connection cap (the HIGH-1 shape, post-auth), and 16
  authenticated-but-idle connections could lock out the administrator.
  Reaching `ST_SESSION` required a private key when this was written; P3
  (2026-09-17) added password auth, so it no longer does -- still LOW
  pending exposure, since it costs a completed login either way.
  **P4 opens a real channel type and makes this live**, so it should be
  settled before or during that phase rather than after.

  *Wording corrected 2026-09-20.* This used to be filed as "no idle
  timeout", which is not a divergence from OpenSSH at all: `man 5
  sshd_config` gives `ClientAliveInterval` a default of 0 ("these messages
  will not be sent to the client") and `UnusedConnectionTimeout` a default
  of `none` ("never expire connections for having no open channels"). sshd
  has no idle timeout either. **The divergence is the 16-slot cap**: sshd
  has no total connection cap (`MaxStartups` counts unauthenticated
  connections only), so an idle authenticated session costs it one process,
  while AxlSsh's `MAX_CONNS` 16 is shared between authenticated and
  unauthenticated connections and the reaper exempts the authenticated ones
  unconditionally (correctly -- that exemption is the point of the
  three-field split). So the fix P4 owes FIRST is splitting the cap, which
  needs no channels; the `UnusedConnectionTimeout` analogue comes after,
  because "no open channel" is only a real predicate once channels exist.
- **LOW-6** Two lenient parsers are reachable from `authorized_keys`:
  `axl_base64_decode` only checks for `=` in the last two positions of a
  quantum (a mid-string `=` decodes to garbage instead of erroring), and
  `axl_file_get_contents` has no size cap. Neither is exploitable here
  (`axl_ssh_hostkey_blob_parse` rejects anything but the exact 51-byte
  structure, and the file is local configuration) but both are shared
  with the JOSE/JWK paths and worth tightening at the source.
- **NEW-1** The reaper is silent in the one state where it examines
  nothing: if its own clock read fails mid-run, every armed-deadline
  connection is skipped with no log line — HIGH-2's outage shape again,
  behind a door the re-review could not open (the clock frequency is
  memoised after the first successful read on x64, and read raw with no
  failure mode on aarch64), hence LOW rather than HIGH. Worth a log line
  anyway: a reaper that examines nothing looks identical to a reaper with
  nothing to do.
- **NEW-3** `axl_ssh_string_free_secure` only scrubs the CURRENT buffer;
  `axl_string_append_len`'s growth `realloc` frees the OLD block
  unscrubbed, so a string assembled by appends leaves earlier copies of
  its own prefix in the pool. Concrete for LOW-2's `signed_data`
  (`string(session_id)` then the appended request — the growth realloc
  serving the second append copies-and-frees a block holding the session
  id), so that fix's exposure is reduced, not eliminated. Checked and
  ruled out for the shared secret K itself: at both sites that hash K,
  mpint(K) is appended last and `grow()` reallocs before the memcpy, so no
  freed block ever holds K. Pre-existing, not introduced by this fix wave.
- **NEW-4** (informational) The `axl_ssh_server_listen` clock probe proves
  the monotonic clock is READABLE, not SANE — it accepts any successful
  reading, and aarch64 takes `CNTFRQ_EL0` raw with no bounds check (x64
  bounds the cached value to 1 MHz-100 GHz). A firmware that programs a
  wildly wrong `CNTFRQ_EL0` passes the probe and then expires every
  connection's deadline within milliseconds of accept. Not a regression —
  the old predicate behaved identically on a fast clock — and a
  broken-platform condition rather than an attack, but the probe reads as
  a stronger guarantee than it actually gives.

Following the P1 pattern, fixes and the re-review land as their own
commits rather than amending this range.

**P3 plan (password auth):**
[`superpowers/plans/2026-09-17-axl-ssh-p3-password.md`](superpowers/plans/2026-09-17-axl-ssh-p3-password.md)
— **implementation SHIPPED 2026-09-17.** New `src/net/axl-ssh-auth.{c,h}`,
additions to `include/axl/axl-ssh-server.h` and `src/net/axl-ssh-server.c`,
and the fixture/test files below:

- [x] **Task 1** OpenSSH's minimum failure time as pure arithmetic -- 5 ms
      plus a per-user 0-4.29 ms from `SHA-512(secret||user)`, doubling,
      nothing past 5 s -- and the listen predicate (`a5492f3a`)
- [x] **Task 2** the public contract: `AxlSshAuthOps`,
      `axl_ssh_server_set_auth_ops`, `axl_ssh_auth_complete`, a per-server
      timing secret, `listen` now requires an authorized key OR a password
      hook (`7b8207ef`)
- [x] **Task 3** the state machine: pending requests (no parsing while a
      verdict or reply is pending, RFC 4252 §5.1), one server-wide reply
      timer, the password method, the publickey principal hook after a
      valid signature, 3 password failures disconnect, the change-password
      form refused, the `CHANGE_FIRST` banner, decrypted payloads scrubbed
      (`827b31ec`)
- [x] **Task 4** fixture hooks + `ssh(1)` password conformance, both
      completion paths, the 3-failure cap counted via askpass prompts with
      `NumberOfPasswordPrompts=5` (`0198e9b6`)
- [x] **Task 5** a hostile `auth-pending-drop`: a verdict that arrives after
      its client left is refused as stale, and its principal is released
      exactly once (`ac21e9e4`)
- [x] **Not in the plan, added by the repo owner's decision after review:**
      an authentication hook (or `release_principal`) may dispatch the
      event loop -- which `axl-loop.h` documents as safe from any callback
      -- and before this fix the server did not survive it: while the hook
      dispatched the loop, the 60 s login grace could reap the very
      connection whose hook was on the stack, and `issue_request`, `pump`
      and `on_recv` then resumed on freed memory -- a remote client
      triggers it by answering its password late in the grace. `conn_drop`
      now defers a drop whose connection has its hook on the stack instead
      of freeing it, and `on_reap` rescans after each drop and skips the
      deferred one. The hostile `auth-hook-reenter` scenario proves it: an
      askpass that sleeps 50 s, a fixture hook that dispatches the loop for
      20 s, and a reaper tick landing inside that window (`a64b7757`)

Commit range (implementation): `a5492f3a`..`a64b7757`.

**Task 6's security review (spec §11's merge gate) ran and returned 4
MEDIUM / 4 LOW / 5 INFO.** All four MEDIUMs and LOW-1 through LOW-3 were
fixed in three commits: `d651a84d` (S1 -- `axl_loop_iterate_until` fails
closed instead of waiting on a source it could not register, closing
MED-2), `9adf11d2` (S2a -- a completion after `axl_ssh_server_free()` is
refused rather than reading the freed server, MED-4's UAF and cross-instance
match; handles are now issued from one image-wide counter; the listener is
closed before connections are dropped, LOW-3) and `9aecf82f` (S2b -- the
`app_calls` counter that lets `axl_ssh_server_free()` stop a server at once
and finish freeing it from the loop once no application frame is left on
the stack, closing MED-1's hang/UAF; the one-hook-at-a-time cap with its
FIFO resume queue, closing MED-3's LIFO pinning; `axl_ssh_auth_complete()`
no longer resumes parsing inline, closing LOW-1). LOW-4 was left open and
deferred to P4. It was recorded as "an authenticated connection never times
out"; *corrected 2026-09-20*, that is also OpenSSH's default
(`ClientAliveInterval 0`, `UnusedConnectionTimeout none`), and the real
divergence is that AxlSsh's 16-slot `MAX_CONNS` is shared with
unauthenticated connections where sshd caps nothing in total. See LOW-5
above for what P4 owes and in what order.

**A scoped re-review of those three fix commits then confirmed all seven
addressed** -- not merely attempted -- and found six further findings of
its own (NEW-1..NEW-6), all smaller than anything they replaced: none is
memory corruption in AXL, an authentication bypass, or a permanent
pre-auth DoS. **NEW-2 and NEW-4 were fixed** in this pass (S3): the header's
"no hook is called again" corrected to say a connection whose own hook is
running is closed, and its principal released, after `axl_ssh_server_free()`
has already returned rather than before (NEW-2); `axl_ssh_server_add_
authorized_key()` and `axl_ssh_server_load_authorized_keys()` now refuse a
non-live server the same way every other public entry point already did,
and the free()/listen()/set_auth_ops() header comments now say plainly that
the liveness check is by ADDRESS, so a server freed and replaced at the
same address is acted on as if it were the new one -- true for those calls,
though not for a completion, whose handles are image-global (NEW-4). NEW-3
(release_principal nests up to the connection count, uncapped, because
capping it would only defer teardown) and NEW-5/NEW-6 (two comments that
overstated what they proved) were documented rather than changed, with the
reasoning recorded next to each. **The gate is now CLOSED.**

**NEW-1 — RESOLVED 2026-09-26 by the owner: hooks stay STRICTLY SERIAL;
the work it owed SHIPPED the same day.**
The decision and its reasoning are in spec §12, along with what shipped:
`axl_ssh_server_set_auth_hook_budget_ms()` (logging only, default 1 s, 0
to report nothing, latched at hook entry), a watchdog that writes one line
when a hook returns over budget and a best-effort line while one is still
running — riding the login-grace reaper's sweep, not a timer of its own —
and a reaper that names what a still-queued connection was waiting on
instead of reading like a silent peer. The one case none of it can see is
recorded rather than glossed: a hook that blocks WITHOUT dispatching the
loop produces no mid-hook line, because the sweep that would write it runs
on the stack that hook is holding — that case needs a watchdog outside the
loop (`axl_watchdog_set()`). The
argument that produced it is below, kept because the trade is what a later
reader will want, not the verdict alone. The one-hook-at-a-time
cap that closed MED-3 turns LIFO pinning into FIFO starvation: a hook that
waits W seconds by dispatching the loop (the fallback shape the header
documents for a check that cannot be made asynchronous) holds every OTHER
connection's authentication input unread for as long as it runs. A peer
holding 15 of the 16 connection slots, each
sending a password, queues 15*W of latency behind any hook that answers
from a blocking wait -- so W above roughly `LOGIN_GRACE_SEC / MAX_CONNS`
(about 3.7 s today) starts starving legitimate password logins past their
60 s grace, and a hook that never returns denies authentication
server-wide (not just for its own connection, as before this wave) until
the image is rebooted -- fails safe rather than corrupting memory, since
the counter can only leak upward and the server is never freed underneath
it, but it is a durable, server-wide denial of service. The shape the
header already recommends -- answer from `axl_ssh_auth_complete()` after
hashing on another core, never block inside the hook -- is immune, because
it never holds the one-hook slot for any measurable time. **The decision
owed:** keep hooks strictly serial (today's behaviour, cheapest, but the
one that starves), raise the cap above 1, bound how long a connection may
wait in the queue before it is answered DENY on its own, or require
non-blocking hooks outright and refuse `set_auth_ops` otherwise. Each is a
real trade against a latency budget that should be set once, by the repo
owner, rather than assumed here.

Measured: `AxlTestSsh` 282/0, `test-ssh-hostile-qemu.sh` 50/0 and
`test-ssh-auth-qemu.sh` 16/0, all three both arches; `test-ssh-transport-qemu.sh`
9/0. Unit ratchet (`test/integration/.last-pass-count`): 11192.

Not covered by any test that ran: the `schedule_drop` path; a client
leaving during a failure reply's 5-9 ms window; `set_auth_ops` after
`listen`; handle wrap after 2^32-1 requests; the per-request
clock-failure fallbacks; a peer reset during a hook reached from `resume`
(only a pipelining client reaches it; `ssh(1)` does not); a publickey hook
dispatching the loop; the fail-closed `auth_apply` check and the
`!running` parse check; the dead-connection check after a hook returns
(defence in depth -- `raw_send` refuses first); and the new
`axl_ssh_server_add_authorized_key()`/`_load_authorized_keys()` liveness
checks added in this pass, which no test exercises.

The optional `abandon(req, ud)` hook the original review recommended (so a
consumer can cancel outstanding work and scrub its own password copy when
a client leaves before its verdict) remains unbuilt. It and the hook
watchdog were both written here as things that "would help NEW-1 without
deciding it"; the 2026-09-26 decision made the watchdog PART of the answer
rather than an adjunct to it -- serial hooks plus a stall an operator can
see -- and it shipped that day. `abandon` stays the companion piece, worth
most to SoftBMC, which holds a credential copy while it hashes.

**Task G (settable login grace) SHIPPED 2026-09-17**, prompted by the final
review's finding that the hostile suite spends ~140 s of its ~280 s
waiting out the fixed 60 s constant twice (no single written "P4
suggestion" bullet named this specific ask; NEW-1's residue above is the
closest documented antecedent -- a settable grace is a lever NEW-1's own
arithmetic assumed did not exist). `axl_ssh_server_set_login_grace()`
(`include/axl/axl-ssh-server.h`) replaces `LOGIN_GRACE_SEC` with a
per-server field, before-`listen()`-only like `set_auth_ops()`, refusing 0
(the exact DoS the grace exists to prevent) and anything over one hour,
and deriving the reaper's own tick down from the default 5 s for a short
grace rather than leaving a fixed lag a 1 s grace would not survive. It
does NOT resolve NEW-1 -- a queued connection's grace still runs down
while it waits its turn behind a blocking hook, and the docstring says so.

`test-ssh-hostile-qemu.sh` now sets a 40 s grace for its whole boot
(`GRACE_SEC`) instead of waiting out the shipped default twice: `auth-stall`
measured 44 s on x64 / 41 s on aa64 (was ~65 s) and `auth-hook-reenter`'s
askpass sleep moved from 50 s to 30 s against the same margin arithmetic.
The full default run (all nine scenarios, one boot) measured 3m24s on x64
and 3m28s on aa64, both 50/0 -- down from an est=280 baseline, about a 27%
cut. `auth-hook-nest`'s three `NEST_US` hooks (18 s in series) were left
untouched -- they stay comfortably inside 40 s without retuning (both B
and C still logged in, not dropped, on both arches), and retuning them was
outside this task's scope; a much shorter grace would need that too.
`AxlTestSsh` grew from 282/0 to 292/0 (the setter's own validation: NULL,
zero, over-the-bound, exactly-the-bound, a freed server, and -- on both
arches -- a real bind refusing the setter once listening).

**Sabotage proved the timing assertions actually grade the grace, not just
elapsed wall time.** A setter that keeps its argument's assignment but
targets the wrong field (`s->login_grace_sec = s->login_grace_sec` for
`= seconds`) leaves the server at the shipped 60 s default. `auth-stall`'s
band against that shipped 60 s grace was `[45, 85]`; rescaling that shape
to GRACE_SEC (40) gave `[25, 65]`, and that FIRST version did NOT catch
the sabotage -- 64 s measured, inside `[25, 65]`, "the sabotage was NOT
detected" -- because 64 s is also within one reap tick of the old 60 s
default. Only the shipped grace's own ceiling carried over into that
first rescale; the 25 floor was new, sized for 40 s, not inherited from
anywhere. The band was tightened to [25, 50] (comfortably above every
GRACE_SEC=40 measurement, comfortably below every 60 s one) and re-run
detected it: `auth-stall: reaped at the login grace, not before or long
after (65s, want 25-50)` FAILS, alone, everything else green.
`auth-hook-reenter` needed no such tuning -- under the same sabotage the
hook's 20 s dispatch never crosses a deadline stuck at 60 s, so the
connection authenticates normally, failing `auth-hook-reenter: the
connection reaped during its hook never authenticated` and (as a genuine
consequence of the same root cause, not a second defect) `teardown: one
principal release per successful login`.

**Task P (per-source penalties) SHIPPED 2026-09-18**, asked for by Mike: "the
real OpenSSH server kills the connection after 3 failed attempts, but if the
client reconnects, it will allow them to retry possibly after a period of
time... I want this to behave like the real OpenSSH server."
`axl_ssh_server_set_penalties()` / `axl_ssh_server_set_penalty_exempt()`
(`include/axl/axl-ssh-server.h`, `AxlSshPenalties`) implement OpenSSH 9.9p1's
`PerSourcePenalties`, **on by default** at OpenSSH's numbers, with the pure
decisions in `src/net/axl-ssh-penalty.c` pinned by unit tests the way
`axl-ssh-reap.h` pins the login grace. Resolves spec §13.4 for SSH; SoftBMC's
web login still has no throttle.

**The contract was wrong after two reviews, and a measurement caught it.**
It first said a source is refused "while remaining exceeds `min`". Measured
against a throwaway `sshd` on this host (non-root, `LogLevel DEBUG1`): sshd
logged `activating ipv4 penalty of 19 seconds` after four failed connections
and refused a good key once a second for the next 20 s -- the WHOLE penalty,
not the 4 s above `min`. `min` is an activation threshold. The same probe
settled three more: a success after a failure on one connection accrues
nothing; a success clears nothing; refusals accrue nothing. An independent
re-read of the corrected model then found eviction still ranked by raw time
left, which under the new model evicts a live refusal near the end of its
decay in favour of a deferred entry that never refused anything; retention
now ranks active first (ruling P4).

`test-ssh-hostile-qemu.sh` gained a `penalty` scenario on a SECOND server (its
own table, so it cannot poison the rest of the boot), while the first server
exempts the one address every forwarded connection arrives from (10.0.2.2,
measured from the guest's own activation line and asserted in the same run).
Two wrong-password connections; a right-password login refused before
identification; a 70-connection flood at the refused source with the other
server still serving; the right password retried until it gets in, timed at
~24 s on both arches against an expected ~23.8 s; then the same sequence
against the exempt server, served. Six sabotages, each detected by the
assertion aimed at it: accrue-but-never-refuse, never-decay, exempt list
ignored, the old refuse-while-above-`min` model (unit AND integration),
forgery charged `noauth` (unit), and ranking by raw time left (unit).
`AxlTestSsh` grew from 298 to 406 assertions (108 new).

**Fix round 1 (the Task P review: 0 critical, 3 important, 8 minor).** The
headline DoS question was clean: EDK2 completes an abortive `Close()`
synchronously, so a refusal costs less than serving the peer did. What
changed:
- the table went from 32 to **256** sources (ruling P5), because a full
  table is a bypass for the next address, not a slowdown, and 32 had been
  argued from the connection cap; the first dropped accrual per `max`
  window now logs at INFO;
- a connection the SERVER failed to set up, or could not time, is charged
  nothing (P6) -- those two paths only; a parse-time failure of the server's
  own is charged like a protocol error;
- the stated reason for the RST was wrong. A graceful close from the accept
  callback would not cost a loop source; it would wait inline for TIME_WAIT
  on every refusal, which is worse, and the docs now say so;
- `axl_ipv4_parse` stored each octet through `%u`'s unsigned int before
  range-checking it, so a BARE `4294967306.0.0.0` parsed as though its first
  octet were 10 (2^32 + 10 wrapped), and an exempt list accepted it. The reviewer's `/8` form was already refused by
  the CIDR path's 16-byte copy. A field width of 3 fixes it for every
  caller.

The `penalty` scenario now:
- makes its first failure a forged signature, so the forgery wiring has a
  guard;
- lets one silent connection's 3 s grace run out on the second server,
  which must accrue grace-exceeded;
- replaces its 70-connection burst with up to 40 sequential knocks, run
  last, inside that second penalty. None may get through, most must be
  refused by the server, and the guest's heartbeat must keep pace. A burst
  under QEMU's user networking both misses `on_accept` (backlog) and
  arrives late through SYN retransmits: three stragglers once activated a
  penalty of their own mid-scenario. Knock latency is reported, not graded:
  a first version graded it, passed alone, and failed under six suite
  workers (median 19 ms unloaded, ~510 ms loaded).

Two more sabotages were detected: deleting `pen_forged = true` fails the
refusal assertions, and deleting the grace wiring fails the second
activation's cause.

**D4 (settable password cap) SHIPPED 2026-09-25**, the OpenSSH parity
audit's row for sshd's `MaxAuthTries`.
`axl_ssh_server_set_max_password_failures()` replaces the compile-time
`MAX_PASSWORD_FAILURES` (3, now `AXL_SSH_PASSWORD_FAILURES_DEFAULT`) and
accepts 1..`AXL_SSH_PASSWORD_FAILURES_MAX` (10), the ceiling tied to
`MAX_AUTH_REQUESTS` by a `_Static_assert` rather than by a comment --
the "five constants defined twice with no compile-time link" complaint
above, paid down by one. **It is LIVE**, which departs from spec §13.6's
before-`listen()` rule deliberately and is recorded as §13.7: the cap is
copied into each connection at accept, so a change reaches connections
accepted afterwards and leaves open ones alone, which is what sshd's
fork-per-connection gives on a reload. Before-`listen()` would have cost
SoftBMC's settings page a free + new + `listen()` to move one number --
regenerating the host key (persistence is P5) and, from P4 on, dropping
every logged-in session.

**D9 (pre-authentication banner) SHIPPED 2026-09-25**, the audit's row for
sshd's `Banner` and the last of its CLOSE items.
`axl_ssh_server_set_banner()` sends `SSH_MSG_USERAUTH_BANNER` (RFC 4252
§5.4) ahead of the reply to a connection's first authentication request,
once per connection, whatever name or method it asks with. 4096 bytes,
because the size is set by what it carries: the DoD Notice and Consent
Banner is about 1.3 KB, which `AXL_SSH_AUTH_BANNER_MAX`'s 1024 refuses --
and an over-long one is refused rather than truncated, since a truncated
mandated notice is a non-compliant one that looks compliant. LIVE for the
same reason sshd rereads its `Banner` file per connection (spec §13.8,
which also records the four invariants that make freeing heap under a
running server safe), and FAIL-CLOSED: a banner that cannot be sent drops
the connection rather than authenticating without the notice. Its UTF-8
check is the new public `axl_utf8_validate()` in `axl-str.h`, which
incidentally makes `axl-stream.h`'s years-old reference to that function
true.

**How `ssh(1)` renders it was MEASURED, not guessed** -- the audit's D9 row
carried an [UNVERIFIED] sentence, and the first measurement proposed for it
went through `sshd`, which is circular: it assumes the very thing it would
be checking. Against AxlSsh itself, with ssh's stderr captured to a FILE and
read with `od -c` (OpenSSH 9.9p1, two locales, four log levels): TAB, LF and
CR pass through unchanged, every other control byte becomes a
four-character octal escape, non-ASCII survives only under a UTF-8 locale, a
banner with no final newline leaves its last line unterminated, and both
`-q` and `LogLevel=ERROR` suppress the banner. Those are the two sentences in
the docstring that were placeholders, and `test-ssh-policy-qemu.sh`
re-measures them on every suite run rather than leaving them a claim made
once.

**The first version of that last clause said "prints nothing at all", and
the review measured it FALSE.** The render cells use a SUCCEEDING publickey
login, so at `-q` and at `LogLevel=ERROR` they capture nothing -- and nothing
cannot tell "ssh withheld the banner" from "ssh had nothing to say". Re-run
against a login the server DENIES, same boot, same server: the default level
gives 158 bytes (banner, then `Permission denied`), `LogLevel=ERROR` gives 58
bytes (the error alone -- the banner gone, the error kept), and `-q` gives 0.
Three cells were added for exactly that, with the default-level one as the
control that proves a failing login is sent a banner at all. A textbook
"empty is not an answer unless you know the question was asked", caught by a
reviewer rather than by the test that was supposed to be the measurement.

**`test-ssh-policy-qemu.sh` is new, and it is a file of its own for a
reason.** `test-ssh-auth-qemu.sh` sits exactly on the shipped per-source
penalty threshold, by its own long comment; D4 needs two connections that
spend a whole cap and D9 needs one per measurement cell, and the first
extra failing connection there would be REFUSED at accept and fail with a
`kex_exchange_identification` transcript that has nothing to do with what
it was testing. The new file exempts the one address every forwarded
connection arrives from, so penalties never enter into it.

### Persistent-guest test runner — *(proposed 2026-08-21; NOT started)*
**This does NOT want SSH**, and that separation is deliberate: the harness needs
a command channel, not an authenticated remote shell. `run-qemu.sh` already has
every piece (`--background`, `--serial-socket`, `--serial-log`, `--mount`), so
the transport costs nothing and adds no attack surface. `AxlSsh` above is a
product feature; this is a test tool. Keep them apart.

**As a speed play this is DEAD — measured, do not re-open it as one.** Profiled
uncached X64 run, 172 tests / **220 boots** / 3473 s, every test classified by
whether a shared guest could absorb its boots:

| class | tests | boots | secs |
|---|---|---|---|
| **CANDIDATE** (shareable) | 18 | 21 | 198 |
| isolation-requiring | 86 | 112 | 1908 |
| **topology-pinned** | 44 | 87 | 1212 |
| no-boot | 24 | 0 | 155 |

A shared guest absorbing every candidate saves **~140 s of 3473 s = 4.0%**. The
absurd ceiling — every non-topology boot sharing one guest, pretending isolation
is free — is 26.6%. The blocker is **topology, not isolation**: 44 tests are
pinned to a specific machine shape (`--gpu`, `--net`, `--bridges`,
`--nic-model`, `--cpu`, `--mac`, `--reboot`, `--boot-target`,
`--screenshot`/`--sendkey`, `--qemu-arg`) and a shared guest has one shape.
Compare the `AXL_SHELL_LAUNCHER` item above: **29% for one environment
variable**, against 4% for retrofitting two launch paths.

**As a COVERAGE play it is worth building, and that is the reason to do it.**
Every test today starts from a virgin image, so *"app B misbehaves only after
app A ran"* is not merely untested — it is unexpressible in this suite. The
`test_check_leaks` gate cannot see it either: it asserts at per-binary teardown,
so a leak that appears only on the third load has nowhere to surface. This tree
has shipped exactly that class:

- the console-device restore path, where a stale ConSplitter fan-out entry made
  *the next* `OutputString` jump through a freed vtable and hang
- `mEventCloseRing`, which exists solely to skip a **second** `CloseEvent` that
  would `#GP` inside DxeCore
- the driver image leak; driver ctors registered and never run; the never-freed
  `AXL_PROTOCOL_NAME_MAX` allocation

Every one was found by hand or by a consumer, never by the suite. 27 tests
already stage 3+ artifacts into one image, so multi-app images exist; what does
not exist is **asserting after a sequence**.

**Shape — every part already exists, and there is no guest-side code:**
`run-qemu.sh --background --serial-socket S --serial-log L --mount <artifacts>`
boots one guest; a host driver writes `fsN:\app.efi args` to the socket, reads
to a sentinel, and repeats. The UEFI shell is already the command interpreter.
`--mount` is what makes it practical — a freshly built `.efi` appears at `fsN:`
with no disk re-stage and no reboot. `--serial-socket` injection is already
proven by `test-console-readline-qemu.sh`.

- [ ] **Scope it as ADDITIVE**: a handful of new soak scenarios, migrating
      nothing. For coverage you add tests; only the (dead) speed goal would have
      required moving the existing 172.
- [ ] Assert on **accumulated** state after a sequence — leak report, handle
      count, protocol registrations, free-memory delta across N load/unload
      cycles. That is the assertion no current test can make.
- [ ] **Design against the known failure mode, which this tree already
      documents**: `test-axl.sh` runs ~40 unit binaries in one boot and
      "a hang starves every later binary". Per-command timeouts and naming the
      wedged command are requirements, not polish.
- [ ] Keep fresh-boot the DEFAULT and the shared guest opt-in per test. A
      virgin image is what catches state-dependent bugs, and this tree has been
      bitten repeatedly (stale staged SDK, stale `.efi` surviving a rebuild,
      `.bss` not zeroed).

### Local gate wall time — [AXL-CI-Release-Speed-Design.md](AXL-CI-Release-Speed-Design.md) §12
The pre-commit gate a human waits on is 15m04s, and that document had spent
eleven sections optimising CI without ever measuring it. Measured 2026-08-19:
`verify.sh` 61 s + integration X64 569 s + AARCH64 274 s = **904 s**, of which
**93% is the integration suite**. The suite is **233 guest boots** and
essentially nothing else (mean 14.5 s/boot), and at a ~7 s floor per boot
**47% of it is overhead that tests nothing**.

Two findings shape every phase below and are worth carrying into them:
**the pool is already 99% packed** (3,385 s over 6 workers is a 564 s floor,
measured 569 s), so no scheduling change can help and only doing less work
does; and **relevance-guessing by module is unsafe here** — `8af4e530` touched
`src/log/` and changed every image in the tree through a link-time edge no
directory map can see (§12.5). Association is therefore by staged artifact,
never by path.

- [x] **Phase 0 — measure, then instrument.** §12.1 wall clock + packing
  arithmetic; `--only-local` (§12.10), the inverse of `--ci`, taking the inner
  loop **904 s -> 495 s (-45%)**; boot instrumentation `AXL_TEST_PROFILE` +
  `lib/profile-report.py` (§12.11) at **zero measured cost** (568.15 s profiled
  vs 569 s not), including `--affected` for artifact-based association.
  Two corrections recorded rather than smoothed over: the estimate for
  `--only-local` was 122 s and it measured 382 s (the scoped run is
  *tail*-bound, not work-bound), and the first instrumentation covered 25% of
  tests while looking complete
- [x] **Phase 1 — `test-console-device-qemu.sh` split four ways.** 15 serial
  DEBUG-OVMF boots at 25.3 s each, unmergeable (each scenario loads a different
  driver and screenshots it), so split — and sized, not guessed: four is the
  first split whose largest piece (~107 s) gets under the 122 s work floor, so a
  fifth buys nothing. **`--only-local` X64 382 s -> 194 s; inner loop 495 s ->
  306 s.** All 42 assertions preserved exactly; the full run is unchanged (573 s
  vs 569 s) because it is work-bound and splitting moves work rather than
  removing it. **Exit was <= 300 s and this is 306 s — a 1.9% miss, recorded
  rather than rounded.** The constraint moved: six local-only tests are now
  >= 87 s and fill all six workers at once, so packing is 65% against the full
  run's 99%. Phase 2 clears the remainder
- [x] **Phase 2 — longest-first scheduling, not the planned split.** The pool
  consumed tests in the glob's alphabetical order; feeding a fixed job set to N
  workers is makespan scheduling, and longest-processing-time-first is within
  4/3 of optimal where arbitrary order has no bound. One `sort` on the `est=`
  every test already declares: **`--only-local` X64 194 s -> 134 s (65% -> 93%
  packing), inner loop 306 s -> 246 s**, no test changed. Also clears Phase 1's
  6 s miss. The full run does not move, as predicted — it was already 99% packed
- [x] **Phase 3 — merge boots (pilot).** `test-tar-qemu.sh` 7 boots -> 1,
  **50 s -> 7.3 s**, 11 assertions preserved + 1 added; full X64 571 s -> 564 s,
  boots 226 -> 220. It merged because nothing needed isolation: every boot
  mounted the same host dir and the scenarios were already coupled through it.
  **The payoff estimate fell twice — -35%, then -22%, then a measured ~-9% —
  and the reason is the finding: the boots you CAN remove are the cheap ones.**
  Tar's were ~7 s each; the 15 s suite mean is dragged up by boots that are not
  candidates (console-device 25 s, cpu-spike 44 s). Expensive boots are
  expensive because they do something, and that something usually forbids
  merging. **Recommendation: stop or cherry-pick** — the remaining ~19 buy ~43 s
  on a once-per-push run, for 19 rewrites each carrying cascade risk
- [ ] **Phase 2b — the remaining multi-boot tests, exit RESTATED.** The original
  exit (full X64 -> <= 450 s) is **not reachable by splitting**: the full run is
  work-bound, so splitting moves work and removes none. Only MERGING boots
  reduces it, and those boots exist for state isolation (a different driver, or
  clean firmware), so merging trades wall clock for cross-contamination.
  Post-LPT the scoped run has ~9 s of headroom left. **Recommendation: stop
  unless a specific test can shed boots without losing isolation**
- [x] **Phase 3b — is the digest cache worth building? YES, measured.** A
  library change (`src/mem/axl-mem.c`) changes only **44 of 118** staged
  artifacts, leaving **46% of the work skippable — full X64 560 s -> 304 s**; a
  tool-only change leaves 86% of tests skippable. **This reverses §12.7**,
  which claimed it buys nothing on a libaxl change: that conflated RELINKING
  with CHANGING — `--gc-sections` plus selective archive linking leaves most
  images byte-identical. Getting there needed the artifact capture fixed first
  (82 -> 149 of 169 tests recorded), and two no-op perturbations were thrown
  away en route (a comment emits identical code, so the probe measured nothing
  while appearing to confirm §12.7)
- [x] **Phase 4 — SHIPPED: `run-integration.sh --cache`.** Full X64
  **564 s -> 71-78 s** warm (136 cached, 34 ran; three runs 77.6/72.7/70.9); `--only-local` 133 s -> 24 s.
  Default path unchanged (re-measured 564 s / 247 s). Soundness shown by
  perturbing `tools/grep.c` and watching exactly the three tests that stage
  `grep.efi` wake up, everything else stay cached, 170/0. `test-test-cache.sh`
  holds 13 property assertions, all but two asserting a MISS (the dangerous
  direction is a false hit). Excluded from the release-gate stamp and prints
  `PARTIAL`. Does NOT cover the host environment; 37 tests always run and each
  is a "cannot prove", not a "probably fine" (§12.16.2). **78 s is a NO-CHANGE
  re-run; after a library change it is ~304 s (§12.15)**
- [ ] **NOT SCHEDULED — cache `verify.sh`'s jobs too (§12.17).** Recorded as a
  level to reach for **only if the local gate starts feeling slow again**.
  `verify.sh` is CONCURRENCY-bound (61 s wall against a 197 s sum, 12 s above
  its longest job), so unlike the integration suite skipping only helps when it
  removes the critical-path job. The two unit suites are 49 s and 48 s of it and
  are structurally identical to an integration test, so the §12.16 cache would
  extend to them: expected ~43 s, or ~30 s with docs cached, giving a ~40 s
  inner loop. **Not built because the risk outgrows the payoff**: skipping a
  unit job means not running 10,497 assertions, and a subtly wrong key hides a
  regression rather than costing time. Also records a correction — §12.2 ranked
  this last at 6.7% of a 904 s gate; the inner loop is now ~85 s and
  `verify.sh` is ~85% of it, so the reason expired even though the 61 s did not
- [ ] **Phase 5 — clang-tidy scoping, for CI only.** Deliberately last: it is
  the intuitive first target and §12.2 shows it is worth **at most 61 s**
  locally. CI's ratio is reversed, but by less than this line used to claim:
  re-measured 2026-08-20, the job is **2 m 38 s** of an 813 s run, of which
  `lint.sh` is 77 s — so scoping attacks 77 s, not ~7 min (§9)
- ~~Deliberately NOT on this list: **making boots cheaper.**~~ **RE-OPENED
  2026-08-21, and it is now the LARGEST measured win available.** The parking
  note said "`run-qemu.sh` already skips the Boot Manager countdown and ~7 s is
  close to what an OVMF boot costs" — and then asked that it not be re-proposed
  "without a measurement". The measurement was never taken, and the premise
  conflated two different timers: the **Boot Manager** countdown (which is
  skipped) and the **UEFI Shell's `startup.nsh` skip prompt** (which is not).
  Every default boot prints `Press ESC in 5…1 seconds` and pays it.

  | | X64 boot | AARCH64 boot | countdown lines |
  |---|---|---|---|
  | default | 6.14–6.34 s | 10.77 s | `Press ESC in 5…1` |
  | `AXL_SHELL_LAUNCHER=1` | **1.48–1.67 s** | **6.13 s** | **0** |

  **~4.6 s per boot, both arches**, app reached correctly and leak report clean
  in each case. Against the profiled X64 suite (172 tests, **220 boots**,
  3473 s) that is **~1012 s ≈ 29%** — larger than every other item on this list
  and larger than the shared-guest idea below by 7x.

  **The mechanism already exists and is already built**: `stage_boot_shell` in
  `axl-common.sh` stages an AXL launcher that chains the Shell with `-delay 0`.
  It **was opt-in at the time this was re-opened** (`AXL_SHELL_LAUNCHER=1`,
  default `0`) for a stated reason — it "has been observed to hang some
  firmware (the guest loads `\EFI\BOOT\BOOTX64.EFI` but never chains to the
  Shell)". It is **on by default now**; see the final sub-item.

  - [x] **Diagnosed 2026-08-21, and the answer inverts the premise: THE CODE
        WAS NEVER THE VARIABLE.** The launcher works at commit `1ae66ccd`
        itself — the commit that disabled it — verified by building that tree
        in a worktree and running its own `test-shell-launcher-qemu.sh`, which
        PASSES. It also works across three OVMF builds, both arches, and a full
        uncached 172-test suite. The first hypothesis (that `24c6c529`'s
        double-`FreePool` fix had repaired it) is therefore WRONG: the launcher
        is fine two days *before* that fix landed.

        What changed is the **machine's firmware**. The custom OVMF of that era
        is gone from this box — the installed one is distro stock — and the
        hang's trigger went with it. **So it is not repaired, it is
        unreproducible**, and the latent risk is real for any other firmware.
  - [x] **Taken for the SUITE, left alone for consumers** (`run-integration.sh`
        sets `AXL_SHELL_LAUNCHER=1`; a standalone `run-qemu.sh` is unchanged).
        The reward is ours, on firmware we test daily; the risk is a consumer's,
        on firmware we cannot test, and `run-qemu.sh` ships to them in
        host-tools. Splitting them takes all of the win and none of the risk.

        Measured back-to-back, uncached, one machine, all runs green:

        | arch | OFF | ON | saving |
        |---|---|---|---|
        | X64 (172 tests) | 489 s | **368 s** | 121 s = **24.7%** |
        | AARCH64 (73 tests) | 322 s | **188 s** | 134 s = **41.6%** |

        Combined integration gate **811 s -> 556 s = 31.4%**. The whole suite is
        now the regression guard, where before it was one opt-in test.
  - [x] **DONE 2026-08-21 — the CONSUMER default is now ON, and safe because
        it VERIFIES ITSELF rather than being verified in advance.** The
        premise of the previous entry ("needs the firmware that hung") assumed
        safety meant fixing the fault, which needs a repro. It does not: the
        default can check, on whatever firmware it actually meets, whether the
        launcher worked — and undo itself when it did not. We never need to
        know why a given OVMF hangs.

        The oracle already existed and was already emitted on every affected
        run: the staged `startup.nsh` echoes `___AXL_APP_OUTPUT_BEGIN___`
        BEFORE the app line, so the sentinel can only be missing when the
        Shell never ran `startup.nsh` at all — never for an app-level
        failure. `run-qemu.sh` now records a negative memo against
        (arch, firmware, launcher) and re-runs itself without the launcher.

        **Only failures are recorded**, which is what makes the cache safe:
        working firmware needs no entry and pays nothing, and a stale,
        unwritable or corrupt memo can only cost the saving, never
        correctness. (Borrowed from glibc's syscall fallbacks, which memoize
        `ENOSYS` and never success.)

        **Two corrections this measured, both of which had been assumed:**

        - **The stated consumer risk was overstated.** "`run-qemu.sh` ships to
          them in host-tools" is true, but the package is scripts ONLY — no
          `axl-shell-launcher.efi`, no Makefile, no `libaxl.a` (zero hits in
          `release.yml`). `find_shell_launcher` returns 1 in that layout, so
          packaged consumers could not get the launcher before this change and
          still cannot. The population affected is source-tree users.
        - **The failure space has two shapes and only one needed a retry.** A
          launcher that RETURNS is already self-healed by firmware: BdsDxe
          falls through to `Boot0002 "EFI Internal Shell"`, which runs
          `startup.nsh` anyway (measured: run exits 0, sentinel present, user
          pays only the countdown). A launcher that HANGS never returns
          control — that is the reported shape, and the one the retry handles.

        **The fault is now reproducible without the firmware**, which is what
        actually unblocked this: `AXL_SHELL_LAUNCHER_BIN` stages any PE as the
        launcher, and `kbprobe` (blocks forever in
        `axl_console_read_key(UINT64_MAX)`) puts the guest in exactly the
        reported state — loads `BOOTX64.EFI`, never reaches the Shell.
        `test-shell-launcher-fallback-qemu.sh` asserts the whole contract and
        was verified by four sabotages, each tripping only its own assertion.

        `AXL_SHELL_LAUNCHER=1` still means "always, no fallback, stay loud",
        which is what `run-integration.sh` and `test-shell-launcher-qemu.sh`
        rely on; `=0` still bisects.
- **Doc rule for this project:** each phase updates §12 with what it MEASURED,
  not what it intended, and ROADMAP ticks only after `verify.sh` and both
  integration arches are green

---

## Open backlog

- [ ] **Upstream EDK2 defect: TerminalDxe's non-UTF-8 terminals print a
  character's LOW BYTE (2026-10-10).** A VT100-family serial terminal (VT100,
  VT100+, PC-ANSI, TTY-Term, Linux, XtermR6, VT400, SCO) is sent, for a
  non-ASCII UCS-2 character, its low byte whenever that byte is printable
  ASCII, and `?` only otherwise: `TerminalConOut.c` truncates first
  (`GraphicChar = (CHAR8)*WString;`, line 233 in edk2-stable202505, the same
  in 202408) and then tests the TRUNCATED byte with `TerminalIsValidAscii()`
  (line 235) -- the CHAR16 parameter never sees 0x4E2D. So U+4E2D goes out as
  `-`, U+65E5 (0xE5) as `?`. Seen on aa64 (ArmVirtQemu's serial console is
  VT100) by the console-visual `wide` case, which now uses U+65E5 U+6587 so
  its oracle shows `?` as `sshd` does; a real user of such a terminal sees
  wrong ASCII where the firmware means a CJK character. Not ours to fix
  here: report upstream (the fix is to test `*WString` before the cast).
  AXL's own encoder sends `?` for every such codepoint on a
  one-cell-per-codepoint console.

- [ ] **AxlSsh P4c follow-ups (2026-10-09).**
  - TcpDxe's behaviour toward a client that genuinely advertises a tiny
    receive window is **untested**. QEMU's user network always shows the
    guest a 65535 window whatever the peer does, so no test has made the
    guest's TCP face a small one (`stall-investigation.md` in the P4c
    workspace: guest-side windows stayed 65535 and TcpDxe's persist timer
    never ran). Real-client impact: none measured.
  - A channel flush that re-defers itself would cost about 100 wake-ups a
    second, and the zero-window idle-CPU check cannot see it: deferred work
    runs once per 10 ms loop tick, so the sabotage read 14-19 milli-cores
    against the 500 limit. The check does catch a flush that polls every
    loop iteration (996-997 milli-cores); the first shape would need a
    wake-up count to be seen.
  - **The ~24 s delivery stall seen with `ssh-peer.py --rcvbuf 4096` is a
    host-loopback artifact, not an AxlSsh or TcpDxe defect** -- recorded so
    nobody rediscovers it. A 4 KiB receive buffer on the peer caps the
    Linux sender's MSS at 2048 and the peer's window at 2028, and Linux then
    sends only on its ~200 ms probe timer (102 steps of +2028 bytes). It
    reproduced with the x11 flood (10 of 12 runs with a 4096 buffer and a 2 s
    hold), never with default buffers (0 of 29) or `--rcvbuf 8192` (0 of 3);
    the guest finished sending within about 1.3 s even in stalled runs.
    `--rcvbuf` is kept in `ssh-peer.py` for experiments, used by no test.
  - From the final review, deferred as non-blocking: `src/net/README.md`
    says twice that other channel types stay refused, and states the
    NUL-in-exec/TERM and second-`pty-req` rules without the header's
    "before the start" and "with a pty hook" qualifiers; a session
    connection parked at the parse gate is handed its resume turn only with
    no AUTHENTICATION hook of any connection on the stack
    (`on_reply_timer`'s `hooks > 0` test), a latency coupling sshd does not
    have -- exempt `ST_SESSION` there or document it; `chan_flush_lost`
    stays set when `chan_schedule` finds no work, so the reaper re-calls it
    every tick (harmless; clear it at the top of `chan_schedule`).

- [ ] **AxlSsh P4d follow-ups (2026-10-09).**
  - **The chacha nonce is a 64-bit counter where OpenSSH wraps at 32 bits**
    (`axl-ssh-crypt.c`, `pump`'s `seq` comment; sshd's `seqnr` is a `u_int32_t`
    that wraps, `packet.c`). On a NON-strict connection no NEWKEYS resets the
    sequence, so after 2^32 packets the two nonces diverge and the connection
    dies with a bad MAC. Not wrapping was deliberate before rekey existed
    (a wrap meant nonce reuse); the 2^31-packet rekey now guarantees a key
    never sees one, so wrapping like OpenSSH is safe to do.
  - Deferred from the hostile-suite review, in `ssh-peer.py`:
    `verify_reply` does not pin the host key to the first exchange's
    (ssh(1) re-verifies every exchange; add `peer: host key changed`). The
    rest of that list closed in P4e: the flood-writer deadlock is fixed
    (and selftested), and the `Rekey.ours_first` comment is corrected.
    Also closed: P4d's untested input-pause-during-rekey half, now proven
    for a PEER-started `KEXINIT` waiting behind a pause; a SERVER-started
    rekey cannot coexist with a pause, because its replies go to the held
    queue and the output queue never reaches the pause mark.
  - The rekey suite's allocation test sees only 1 of 9 server-started
    exchanges holding output (most start from the read-side trigger, after
    the client's WINDOW_ADJUST). If it falls to 0 the PRECONDITION fails
    loudly; a wider margin needs a bigger flood or a fixture write path that
    does not wait on writable.
  - `chan_schedule` in `rekey_our_newkeys` is redundant by construction
    (`on_sent` reschedules whenever a completion leaves the queue under
    `TXQ_LOW`) and no test can see its removal. Kept, with a comment, as the explicit resume point.

- [ ] **AxlSsh P4e follow-ups (2026-10-10).**
  - **Session-suite headroom.** `test-ssh-session-qemu.sh` (est 210) and
    `test-ssh-hostile-qemu.sh` (est 204) already exceed the 200 s ceiling
    applied to new suites; the idle suites declare 190 and 185 against
    measured 175 s and 172 s, so none of the four has room to grow.
  - **The x64 wall-clock budget** (95 s) in `verify.sh` is exceeded under
    contention (96-155 s, 0 assertion failures; 62 s alone).
  - **K_S pin, the P5 entry criterion.** `ssh-peer.py` does not pin the host
    key across exchanges (above); P5 is host-key persistence.
  - **The x86 TSC frequency, what calibration cannot fix.** The fixed
    per-Stall bias is gone and the fast side bounded to 2 % (the clock reads
    0.999-1.002 idle, 0.989-1.000 under 12 burners, where it read
    0.993-0.995 and as low as 0.22), so the
    calibration work does NOT supersede the two items left open with it: a
    CPUID 0x15/0x16 (bare metal) or 0x40000010 (hypervisor) fast path that
    reads the frequency instead of measuring it, and re-validating a
    published frequency -- one disturbed first calibration still sets the
    clock for the whole boot.
  - **`task-pool-mp`'s root cause** is still unknown: it failed once in CI
    ("slot reclaim: cycle=19 workers=2 expected=3"), did not reproduce in
    21 runs, and its grep now names the cause next time.
  - Deferred from the reviews: a library line that times an input pause (the
    paused-half-close scenarios read the fill from a 2 s quiet period and
    the pause span from the host clock; adding the line touches one the
    admission suite parses); `bytes_ms` and the `resume_due` term of the
    input-held set are pinned by unit tests or not at all (spec §12a.5);
    `run-integration.sh`'s cache keyed on no host helper script (fixed
    2026-11-09, see the `run-integration.sh` entry below).

- [ ] **OWNER DECISION — `axl_loop_run` returns -1 on ANY quit (found
  2026-09-29).** `axl_loop_quit` and the Ctrl-C path both set
  `quit_requested`, and the loop returns `quit_requested ? -1 : 0`, which
  contradicts its own doc ("0 on normal exit, -1 on Ctrl-C") and has since
  `8e023281` (Phase M4). `axl_http_server_run` passes it through. Fixing it
  changes what every caller sees, so it is the owner's call: fix the code to
  the doc, or the doc to the code.

- [ ] **Timing checks that fail only under 6-way concurrent `verify.sh`
  (2026-09-29; all pass run alone).** aa64 unit `loop: a busy source added
  mid-tick extends that same tick` (a thin margin, flagged in its own
  review: widen or re-anchor it); the x64 wall-clock budget (95 s) is
  routinely 98-107 s under contention (101-104 s in P4d's full
  `verify.sh` runs, 0 failures each time, 56-66 s run alone). The x64 `console read_key` early
  return is its own entry below, and recurred the same day.
  `test-http-throughput-driver-qemu.sh` joined them 2026-10-08: in a full
  suite (10 workers) beside another session's mutation run, host load 9-11,
  x64 GETs took 1.3-2.3 s against its 2.0 s bound and idle CPU read 0.072
  against 0.07, on both attempts; run alone three times it passed 7/0 at
  0.39-0.57 s, tracking the load. Its bounds are tight for a loaded box.

- [ ] **Driver-mode single 1 MiB GET may land one 50 ms tick later since the
  AxlTcp bounce buffer (2026-09-29).** 8 samples: 5 at 0.39 s and 3 at
  0.34 s after, against 2 and 6 before. Re-measure with enough samples to
  tell a shift from noise before calling it either way.

- [ ] **AxlHttp, pre-existing, found during P4b's AxlTcp work.** Pipelined
  keep-alive GETs get only the FIRST response (RFC 9112 §9.3.2 requires
  every one, in order). A 16 KB pacing chunk with a multi-client WebSocket
  broadcast stalls 1.6-2.6 s, cause unknown (the shipped 8 KB chunk does
  not). `EFI_SIMPLE_NETWORK_RECEIVE_FILTERS.MCastFilter` carries a spec typo
  that needs a manifest override.

- [ ] **`run-integration.sh` (2026-09-29).** `--shard i/K` is 0-based and
  accepts `i = K`, which runs an empty shard and exits 1 -- reject it.
  (Its cache keying on no host helper script -- `ssh-peer.py`,
  `ssh-hostile.py` -- is FIXED 2026-11-09: the key hashes the whole harness
  by glob, `_tc_harness_files` in `test/integration/lib/test-cache.sh`.)

- [ ] **Untested by design, stated in their commits (P4b round).** The
  socket AxlTcp deliberately leaks when the firmware fails to release it
  (no seam reaches that failure); AxlHttp's WebSocket-upgrade OOM path; the
  `ws_out_on_sent` / `on_tls_handshake_data` / `on_accept_ready` dispatch
  wrappers under a server free from inside a handler. Each needs a seam
  before it can have a test.

- [ ] **DO NOT DISMISS — a timed wait at raised TPL returned EARLY, once
  (2026-09-18). Needs an owner.** During Task P's gate, one `verify.sh` run
  failed `console read_key: honored the timeout at TPL_CALLBACK (raised-TPL
  WaitForEvent fallback waited, not instant-fail)` (`test/unit/axl-test-io.c`,
  `test_console_read_key_raised_tpl`): a 40 ms `axl_console_read_key` at
  `TPL_CALLBACK` must take at least 20 ms, and it took less. It passed on the
  immediate re-run.
  - **What is ruled out: the change under test.** `AxlTestIO.efi` built from
    that change was BYTE-IDENTICAL to the one built at its parent, `.text`,
    `.data` and `.rodata` included — no SSH code is in that binary at all.
  - **What is NOT ruled out: a real defect.** Returning EARLY is the wrong
    direction for host load to push a wait — load makes a wait late, never
    short. That points at the firmware's tick catching up (a burst of timer
    interrupts delivered at once after the vCPU was descheduled, advancing
    the clock the wait measures from faster than wall time), or at the
    raised-TPL `CheckEvent` sweep seeing the timer event signalled early.
    `verify.sh` was running both arches, lint and docs concurrently at the
    time, which is exactly when a vCPU gets descheduled.
  - **To capture it next time**, before re-running: keep the serial log
    (`TEST_KEEP_LOG`), note the host load, and record `elapsed` itself — the
    test prints only pass/fail, so a second occurrence would say no more than
    this one did. Printing the measured `elapsed` on failure is the cheapest
    first step.
  - **Recurred 2026-09-29** under the same conditions (six-way concurrent
    `verify.sh`), and passed twice run alone at load 9.9. A SHORTER-than-real
    elapsed under contention points at the x64 monotonic clock's TSC
    calibration having been taken on a starved vCPU.

- [ ] **Console log timestamps go backwards within a second — now that an
  audit log is read by them, revisit (seen 2026-09-20).** Two lines from
  one hostile run's kept guest serial log, the second written after the
  first:
  `04:00:04.841566 [INFO]  ssh: Accepted password ...` then
  `04:00:04.151361 [INFO]  ssh: maximum authentication attempts exceeded ...`
  (two more in later runs: `03:00:43.795395` then `03:00:43.174371`, and
  `04:34:43.941378` then `04:34:43.141115`). **It is a documented trade-off,
  not an unknown:** `print_console_timestamp` (`src/log/axl-log.c`) takes
  HH:MM:SS from the RTC and the fraction from the monotonic counter, whose
  epoch is unrelated, and its comment calls that acceptable "for
  human-eyeball debugging", pointing machine ordering at the ring handler's
  `AxlLogEntry.timestamp`. What changed is the reader: the SSH audit log
  (2026-09-20) is read on the console by an operator reconstructing the
  ORDER of events, and a fraction that runs backwards inverts it. Options,
  none chosen: derive the fraction from the same source as the seconds
  (sub-second RTC is 0 on most firmware), anchor the monotonic fraction to
  the RTC second at each tick, or print monotonic time alone. Out of scope
  for the SSH work that noticed it.

- [ ] **Doubled log domain prefixes (seen 2026-09-20).** A message that
  starts with its own `AXL_LOG_DOMAIN` prints it twice, e.g.
  `[ERROR] ssh: ssh: refusing to listen with no way to authenticate`
  (`src/net/axl-ssh-server.c`, seven `axl_error("ssh: ...")` calls under
  `AXL_LOG_DOMAIN("ssh")`). The Ed25519 glue's two
  (`ed25519: ed25519: SHA-512 failed ...`) were rewritten on 2026-09-20 for
  other reasons and no longer do it. A sweep for `axl_*("<domain>: ` against
  each file's domain, and a gate once it is clean, would stop it
  recurring; out of scope for the SSH work that noticed it.

- [ ] **Refused SSH connections take ~520 ms on x64 only — diagnose (seen
  2026-09-18, Task P).** `test-ssh-hostile-qemu.sh`'s penalty knocks are
  refused by an RST from `on_accept`, and the peer's view of one refusal is a
  median of ~20 ms on BOTH arches unloaded. On x64 only, a share of them
  take ~520 ms; on aa64 none ever has (worst 20 ms), under the same user
  network.
  - **The platforms differ in more than the arch.** x64 runs q35 under KVM
    with an e1000 NIC, whose UEFI driver is QEMU's iPXE option ROM
    (`efi-e1000.rom`). aa64 runs TCG with virtio-net-pci
    (`scripts/axl-common.sh`, `_test_nic_device` in `common-test.sh`).
  - **Why it is NOT AxlSsh's close cap:**
    - The ssh-level refusal that runs first, with nothing queued ahead of
      it, already took 550 ms in one run.
    - The guest's heartbeat kept exact pace through them under six
      suite workers (4 ticks in 8.5 s, over 26 knocks).
    - The cap would show ~1.01 s, not 520 ms, and it can only stall AFTER
      the RST is on the wire.
    - EDK2 completes an abortive close synchronously inside `Close()`.
  - **Working hypothesis:** QEMU's user network does not act on the first
    RST from this NIC path and closes the host side on its own 500 ms slow
    timer.
  - **Any one of these settles it:**
    - a packet capture during the knocks (`-object
      filter-dump,id=f0,netdev=net0,file=...`), looking for two RSTs ~500 ms
      apart, or an RST followed by nothing;
    - the same knocks on x64 with virtio-net, and on x64 without KVM;
    - one-off guest timestamps around `axl_tcp_close` in the refusal branch.

- [ ] **Cut a release without spending 15 minutes of a human's attention.**
  Two releases were cut on 2026-09-02 and each one blocked Mike for most of
  its duration. Measured, so the next attempt optimises the right thing:

  | stage | v4.5.0 | v4.6.0 | note |
  |---|---|---|---|
  | local uncached suite (the gate) | 477s | 509s | **blocking, before the cut** |
  | `Release` workflow | 7m05s | 4m11s | watched to the published release |
  | `Docs` workflow | 5m20s | 3m18s | parallel with Release |
  | `CI` on the release push | **16m46s** | **28s (skipped)** | already fixed |

  **Already done today**, and it removed the largest single item: the release
  commit carries `[release-cut]` and every `ci.yml` job skips on it, so the
  push before the tag no longer re-runs the full suite on the same box
  (`RELEASING.md` §"The release commit skips CI, on purpose"). The remaining
  cost is the **local uncached gate (~8.5 min)** plus watching Release.

  **The task: stop running CI on every push to `main`, and re-derive what the
  release gate has to be.** Mike's proposal is nightly CI. Sketch, not a
  decision:

  - `ci.yml` moves to a `schedule:` (nightly on `main`) plus
    `workflow_dispatch`. Pushes stop triggering it.
  - The release gate then leans on the most recent nightly **plus** whatever
    changed since it — which needs the release to know that, so the existing
    CI stamp (`--ci-gate`, `test-release-gate.sh`) is the machinery to reuse
    rather than replace.
  - `cut-release.sh` could stop *watching* Release to completion and instead
    hand back a URL plus `check-published-release.sh`, which already verifies
    the published bytes independently (§16.2 job 2). That converts ~4 minutes
    of waiting into a notification.

  **Two things not to lose while doing it.** The local suite is the
  authoritative pre-release gate precisely *because* a tag does not trigger CI
  (`RELEASING.md`), so removing push-CI without replacing that coverage means
  a release is gated by nothing. And `run-integration.sh` must keep its
  default worker count: `-j$(nproc)` produced a false red on the 4.5.0 gate
  for three seconds of wall time.

- **The x64 and aa64 toolchains now MATCH (2026-08-17, `-axl3` published).**
  newlib 4.4.0 -> 4.5.0 and ARM's six newlib flags adopted verbatim from the
  manifest ARM ships inside its own toolchain
  (`14.3.rel1-*-manifest.txt`). Two divergences closed: newlib 4.4.0's
  `struct mallinfo` filled ten `int` into a struct its own `<malloc.h>`
  declares as ten `size_t`, and `printf("%zu", (size_t)4096)` emitted the
  literal text `zu` on x64 while working on aa64 (that is
  `--enable-newlib-io-c99-formats`, not `io-long-long`).

  **Not matchable, and recorded so nobody retries it:** ARM's `libc.a` defines
  `open/read/write/lseek/sbrk/...` and ours does not, because newlib's
  `configure.host` grants `syscall_dir=syscalls` to `aarch64*-*-*` and gives
  `x86_64` no entry. Both builds pass `--disable-newlib-supplied-syscalls`;
  verified by building x64 without it and observing no change. AXL's porting
  layer handles it instead — underscore forms are the implementations, plain
  names are weak aliases, so either winner is correct.

  Leftover: the `AXL_NEWLIB_MALLINFO_INT` block in `axl-cxxrt-alloc.c` is now
  dead (its build-time probe self-disabled) and can be deleted.


Grouped, terse; **detail lives in the linked design doc or
[ROADMAP-Archive.md](ROADMAP-Archive.md)**. Most are opportunistic / low-priority.

- **Hardware crypto/hash instructions are DETECTED and used by NOTHING.**
  Surveyed 2026-08-17. `AxlCpuFeatures` (`include/axl/axl-cpu.h`) already
  reports the whole family on both arches — x86 `aes` / `pclmulqdq` / `sha`
  (SHA-NI) / `vaes` / `vpclmulqdq` / `sse42` / `rdrand`, aarch64 `aes_a64` /
  `pmull` / `sha1` / `sha2` / `sha512` / `sha3` / `crc32` — and
  `src/util/axl-cpu.c` populates the aarch64 half by reading
  `ID_AA64ISAR0_EL1` directly with `mrs`, no `getauxval` and no OS
  dependency. **Grep says nothing in `src/` or `tools/` reads any of those
  bits**: the only `AxlCpuFeatures` consumers are the gfx SIMD dispatchers.
  So the detection layer is built, shipped and idle.

  **SHA-256 is the target, not AES**, and the ranking is what makes this
  worth scheduling at all:

  | workload | why | aarch64 | x86-64 |
  |---|---|---|---|
  | `axl_pbkdf2_hmac_sha256` | *deliberately* iterated — thousands of SHA-256 rounds, the only pure CPU-bound hashing in the SDK with no I/O to hide behind | `SHA256H/H2/SU0/SU1` | SHA-NI |
  | SCRAM auth (`src/net/axl-scram.c`) | calls PBKDF2 per authentication at a server-chosen count (4096+) — direct user-visible **latency**, not throughput | ″ | ″ |
  | gzip/zlib CRC (`src/data/axl-compress.c`) | CRC-32 + Adler-32 over every byte of every payload | `CRC32B/H/W/X` | ⚠ below |
  | JOSE (`src/net/axl-jose.c`) | HS256/RS256/ES256 all hash the payload | SHA2 | SHA-NI |
  | AES-GCM / AES-CTR | **TLS only, and network-bound** — the least valuable of the set here | AESE + PMULL | AES-NI |

  Load-bearing detail: the core hashing APIs do not use mbedTLS at all.
  `axl-digest.c`, `axl-digest-sha256.c`, `axl-digest-crc.c`, `axl-hmac.c` and
  `axl-pbkdf2.c` are unconditional in `LIB_SOURCES`, and the SHA-256 round
  constants live in `axl-digest-sha256.c` — this is AXL's own code, not
  mbedTLS's, so mbedTLS config flags do not reach it. Every consumer gets
  these with no TLS build.

  **⚠ The CRC polynomials differ between arches and this is a real trap.**
  ARM's `FEAT_CRC32` implements the *gzip/zlib/PNG* polynomial natively, so
  `axl_crc32` maps straight onto `CRC32B/H/W/X`. x86's SSE4.2 `crc32`
  instruction is **Castagnoli (CRC-32C)** — a DIFFERENT polynomial — and does
  not accelerate `axl_crc32` at all; x86 needs `PCLMULQDQ` folding instead.
  The `sse42` doc comment in `axl-cpu.h` says "also CRC32 instruction"
  without that caveat, which is how a fast, confidently wrong x86
  implementation gets written. Fix the comment when this is picked up.

  **TASK, cheapest-first, and step 1 is the whole decision:**

  1. **Measure `axl_pbkdf2_hmac_sha256`** at a realistic iteration count
     (4096 and 10000) under QEMU on both arches. Note QEMU must expose the
     features — `-cpu max` on aa64 — or the probe reports false and the
     measurement is of the C path twice. Warm the heap first
     (`feedback_warm_the_heap_before_benchmarking`).
  2. If it pays (2-4x is the usual figure), add `sha256_block_sha2()` behind
     `axl_cpu_features()->sha2` / `->sha`, following the existing dispatch
     precedent — `blend_row_over_avx2` behind `axl_cpu_avx_usable()`, which
     is allowlisted per SYMBOL in `check-no-avx` with a recorded
     justification. A new dispatched kernel needs the same allowlist entry.
  3. CRC-32 second, per-arch and NOT shared, for the polynomial reason above.
  4. AES last, and only if something measures TLS as cipher-bound rather
     than bound by the UEFI SNP driver. `aesni.c` (33 KB) and `aesce.c`
     (20 KB) already sit unbuilt in `deps/mbedtls/library/`, so it is a
     config change — but note `aesce.c` detects via `getauxval(AT_HWCAP)`,
     which does not exist under UEFI, so it would need AXL's probe wired in
     rather than being a pure flag flip.

  Not scheduled. Recorded so it is decided on purpose rather than
  rediscovered.

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

- **CMake as THE build system, replacing the Makefile** —
  **[AXL-Build-System-Design.md](AXL-Build-System-Design.md)** — **DONE
  2026-11-09** (slices 1-6; §10.12). The paragraphs below are the 2026-08
  proposal, kept as history: **PROPOSED
  2026-08-15, retargeted to 5.0.0 on 2026-08-16 — NOT STARTED.** A branch
  `worktree-cmake-build-system` is reserved but carries **zero** commits main
  does not have, and is 123 commits behind it (checked 2026-08-20). This entry
  read "IN PROGRESS on branch ..." while the doc index above read "PROPOSED",
  which is the kind of disagreement that makes a reader believe work is
  underway when a branch name is all that exists. Mike's direction, with
  the constraint stated
  explicitly: **we will no longer ship Makefiles** — replaced, not
  supplemented. Today CMake is a CONSUMER-facing path only
  (`scripts/install.sh` generates `axl-config.cmake`), while the library, 42
  test images, every tool and every gate are a **3,579-line** Makefile.

  Three numbers in this entry were wrong until 2026-08-15 and are corrected in
  the design doc: the Makefile is 3,579 lines not ~2,000, there are **17**
  `LINT_GATES` not 19, and `check-flag-parity` guards **two** build paths not
  three — `2229abc0` made the generated CMake package CALL `axl-cc` instead of
  reimplementing it. That last correction matters, because "three paths → one"
  was this entry's strongest argument FOR the port and the honest version is
  "two → one" — and only if the port also swallows `axl-cc`, which §3 of the
  design doc argues it must not (direct-PE linking has no aa64 emulation;
  specs files have no post-link hook). The strongest argument is instead that
  the hand-rolled build-state signature IS a re-configure, done by us because
  make cannot express "flags are an input".

  **Sequencing DECIDED 2026-08-15: ship 4.0.0 first, port after, on a
  branch.** 4.0.0's content is finished and green while the port is unbounded
  until it starts. The branch is the mechanism, not a detail — `cut-release.sh`
  dates whatever sits under `## Unreleased`, so port commits on `main` would
  ship inside 4.0.0 and reproduce the v3.2.3 incident.

  **The port is NOT consumer-breaking and needs no major** (design doc §5.1,
  measured): `git ls-files` matches exactly two Makefiles (root +
  `test/fuzz/`), `install.sh` ships neither, and `sdk/` ships only
  `CMakeLists.txt`. We have never SHIPPED a Makefile — we use one. That kills
  the only argument for porting before 4.0.0.
  [AXL-Distribution-Design.md](AXL-Distribution-Design.md) §10 reached the same
  conclusion independently, from the consumer side.

  **The surface is bigger than the Makefile.** 3,579 lines is the thing being
  replaced; **157 files invoke `make` (215 invocations)**, 129 of them
  integration scripts, and **50 of those source nothing shared** — so the
  indirection must be a script, like `build-prefix.sh`. For scale, the
  `AXL_TLS` prefix split was 98 refs / 66 scripts and cost 48 failing suites.
  Design doc §2.1.

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
- **Networking layering — POSIX-shaped substrate (revisit; DEFERRAL CLEARED
  2026-08-21, still NOT scheduled):** it was deferred (2026-08-12) behind the
  C++/newlib substrate track, which carries its own layering changes (see
  `docs/AXL-Libc-Substrate-Design.md`) — settling both at once avoids two
  independent refactors of the same boundary disagreeing. **That track is
  COMPLETE** (all of P1'-P5 landed 2026-08-17; its status header said
  "P4-P5 not started" until 2026-08-21 while its own `P4-RESULT` section and a
  ✅ P5 row sat 800 lines below, which is why this looked blocked longer than it
  was). So the gate is open — and the trigger below still has not fired: no
  consumer needs the veneer. **`AxlSsh` must not become that trigger by
  accident**: build it on `AxlTcp`/`AxlLoop` as HTTP and 9P are, and this stays
  a deliberate decision rather than drift. Explicitly NOT a
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

- [ ] **Remote GDB debugging of UEFI apps on real hardware, over SSH.**
  *(Proposed 2026-09-17; not started; wants a design doc first. Transport,
  decided 2026-10-11: the `sshd` tool gains a single-user debug mode -- e.g.
  `sshd --gdb` -- that accepts one authenticated connection and one channel,
  takes no console, refuses `shell`, and serves exactly one `exec` (`gdbstub`)
  whose channel data is piped to the in-firmware stub. The library's `exec`
  hook already exists; the shipped `sshd` refuses `exec` only because its
  console is shared. Workstation side: `gdb -ex 'target remote | ssh -p 2222
  admin@bmc gdbstub'`.)* Attach a stock `gdb` from a workstation to a
  UEFI app running on a physical machine — breakpoints, single-step,
  registers, memory, backtrace — reached over SSH, with no debug probe, no
  serial cable and no QEMU.

  **What the tree has, and why it stops at QEMU.** `docs/DEBUGGING.md` already
  documents GDB against QEMU, and none of that tooling transfers to a physical
  box. Two library calls do:

  | in tree | gives | on real hardware |
  |---|---|---|
  | `run-qemu.sh --gdb` / `--gdb-halt` | a GDB stub | it is QEMU's own stub; a physical machine has none |
  | `scripts/gdb-syms.py` (`axl gdb-syms`) | image load addresses for `add-symbol-file` | scrapes OVMF DEBUG's debugcon `Loading driver at` lines, which production firmware never prints |
  | `axl_cpu_register_exception` (`src/util/axl-cpu.c`) | a typed register snapshot on a CPU exception | **works, with a gap.** Single-step arrives as `AXL_CPU_EXCEPTION_DEBUG` (#DB), and an aa64 `brk` under the `SYNCHRONOUS` umbrella — but **x64 has no breakpoint kind**: the enum goes `DEBUG` (vector 1) to `OVERFLOW` (vector 4), skipping #BP (vector 3), so an `int3` cannot be registered today |
  | `axl_efi_find_config_table` (`include/axl/axl-runtime.h`) | one-call lookup of a UEFI configuration table | **works** — its own docstring names `EFI_DEBUG_IMAGE_INFO_TABLE_GUID`, so finding the loaded-image table on target is already done; nothing walks the table yet |

  **Prior art — borrow mechanisms, port nothing** (EDK2 items checked in the
  local tree):

  | | offers | take from it |
  |---|---|---|
  | EDK2 `SourceLevelDebugPkg` — `DebugAgent` with serial, USB and USB3 debug transports | a firmware-resident debug agent on real hardware | the transport lesson: its channels are polled, because nothing event-driven runs while the target is stopped. It speaks Intel's UDK debug protocol rather than GDB's, so it needs Intel's host bridge |
  | EDK2 `BaseTools/Scripts/efi_gdb.py` and `efi_debugging.py` | a GDB extension that finds loaded images | it walks `EFI_DEBUG_IMAGE_INFO_TABLE` instead of parsing log lines — the mechanism that works on any conforming firmware, and what should replace `gdb-syms.py`'s debugcon scrape |
  | GDB's remote serial protocol, attached through a command | `gdb` speaks it over any command's stdin and stdout | pointed at `ssh admin@bmc gdbstub`, it rides an SSH `exec` channel — so no port forwarding is needed, and the AxlSsh spec's no-forwarding non-goal survives |
  | Windows KDNET | kernel debugging over a NIC while the kernel is halted | proof a polled network path under a stopped target is buildable — and its cost: a dedicated polled NIC driver separate from the OS network stack |

  **The hard part, so a design starts from it: a stopped target does not run
  its transport.** SSH rides on `AxlLoop` event dispatch and `AxlTcp`. A
  breakpoint that parks the CPU inside an exception handler stops both — and on
  a SoftBMC box the server and the debuggee share that CPU — so the stop reply
  cannot be sent and `continue` cannot arrive. Two directions to spike before
  designing:

  - **Cooperative stop**, for code at `TPL_APPLICATION`: the exception callback
    does not hold the CPU; it diverts the debuggee into a parking routine that
    keeps events dispatching, so the loop and the SSH session keep running while
    the app is logically paused. Small, and plausibly enough for *apps*; no use
    for code at raised TPL or inside the firmware core.
  - **A polled channel for true stop-the-world**: SSH stays the authenticated
    attach path, and the stopped-state conversation moves to a transport that
    works with events dead — the EDK2 and KDNET answer. Much larger.

  Which is sufficient depends on what must be debuggable — the app alone, or
  whatever it calls into at raised TPL. That is the design doc's first question.

  **Symbols are the other prerequisite on hardware.** Every shipped `.efi` is
  built with `--strip-all`; its symbols live in the ELF `.so` it was converted
  from, and a deployed image carries no build identity to match the two — the
  same gap already recorded for decoding RSOD crash dumps.

  **Likely needs its own UEFI DXE driver** — a resident boot-service driver, as
  CrashHandler is — rather than a stub compiled into the app being debugged.
  Four reasons, each from this tree:

  - **Lifetime.** A stub inside the debuggee dies with it: its exception
    handlers point into freed code the moment the app exits or is unloaded —
    the same hazard CrashHandler's teardown guards against for its image-load
    notify (`drivers/crashhandler/entrypoint.c`). A resident driver outlives
    each app, can catch a fault on the app's way out, and can debug one app
    after another.
  - **It must be armed before the debuggee starts.** A breakpoint on an app's
    entry point has to exist before `StartImage` runs it. A driver sees every
    image as it loads through `axl_image_watch_loads` — which CrashHandler
    already uses to keep its loaded-image table — and that also gives gdb each
    image's load address as it appears.
  - **The exception vectors have one owner, and the last registrant silently
    wins.** `axl_cpu_register_exception` unhooks whatever already owns a vector
    before installing its own handler (`src/util/axl-cpu.c:368-373`). A stub
    must hold the breakpoint and single-step vectors for a whole session; a
    debuggee that registers its own handlers through that API would silently
    take them. A deliberately loaded driver can arbitrate that; a stub inside
    the app cannot.
  - **There is precedent to build from.** `drivers/crashhandler/` is already a
    resident driver that hooks CPU exceptions through
    `axl_cpu_register_exception`, tracks image loads, and publishes a sentinel
    protocol so a second instance refuses to load. The thin-launcher plus
    resident-driver pattern (`axl-shared-driver.h`,
    `docs/AXL-Shared-Driver-Recipe.md`) covers exposing such a driver to a
    separate client.

  **It collides with CrashHandler, and that must be designed rather than
  discovered.** CrashHandler claims exactly the vectors a debugger needs:
  `AXL_CPU_EXCEPTION_DEBUG` (#DB, single-step) on x64, and on aa64 the
  `SYNCHRONOUS` umbrella where a `brk` arrives. With both drivers loaded,
  whichever registers last owns them — so a breakpoint could be written to
  NVRAM as a crash, or a real crash swallowed by an idle debugger. Detaching
  cannot simply hand the vectors back either: `axl_cpu_unregister_exception`
  works by kind, not by handler, so a detach would also remove a handler some
  other driver registered afterwards — the reason CrashHandler deliberately
  never unregisters. The likely shape is that the debug driver owns the vectors
  and passes every exception it did not cause on to CrashHandler, which needs an
  explicit hand-off between the two; load order is not a design.

  **Where the driver lives is open:**

  - **Inside SoftBMC's resident driver.** SSH is already there and no protocol
    boundary is needed. But SoftBMC itself could then not be debugged, and no
    debugger would exist on a machine without SoftBMC.
  - **A standalone debug driver publishing a protocol**, which SoftBMC's
    `exec gdbstub` handler relays the remote-protocol bytes to. Usable without
    SoftBMC, over serial too, and able to debug SoftBMC itself — at the cost of
    a protocol boundary and one more image to deploy.

---

## Done / decided (one-liners, full detail in Archive)

- **Removed: bundled iPXE** (owner decision, 2026-10-21). The tools
  tarball no longer carries `drivers/<arch>/ipxe-all.efidrv`: it never
  brought a NIC up in practice where the EDK2 USB-network drivers did, and
  loading it takes the USB host controller from the firmware, so a USB stick
  (BMC virtual media included) leaves the map. Removed: `scripts/build-ipxe.sh`
  (upstream pinned at `df4eec8cfb4fda2b4bc1ce87fd101b80205c1e92`), the
  Makefile's `ipxe` target and `release-drivers`' iPXE half, the
  `make-uefi-tools-tarball.sh` staging and `--ipxe-optional`, `release.yml`'s
  iPXE build, `third_party/ipxe/`, its `THIRD_PARTY.md` entry, the README's
  GPL notice and section 3(b) written offer, and the iPXE section of
  `test-efi-pack-release-qemu.sh`; and (ruling R32, after a real-hardware
  sshd hang with Ctrl-C dead on a BMC console) `axl_net_ensure_drivers`'
  silent "Phase 2" iPXE fallback, which loaded `ipxe-*.efi` from any volume
  when its core list brought up no SNP -- the library never auto-loads iPXE
  now (`test-net-no-ipxe-autoload-qemu.sh`). Its long-driver-path coverage moved to
  `test-netload-qemu.sh` (`--_fwwin`, `--_log`) and AxlTestNet. Kept: AXL's
  tolerance of an iPXE someone else supplies or the firmware contains
  (`axl_net_driver_is_ipxe()`, the try-it-last order and watchdog disarm,
  AxlTcp's iPXE-SNP pacing, `run-qemu.sh --nic-no-rom`). **The last commit
  with the bundle is `dca94436`**, the parent of the removal commit, whose
  subject is "Remove the bundled iPXE; the library never auto-loads iPXE"
  (find it with `git log --grep`). To restore the bundle,
  `git revert` that commit -- keeping R32, so the library still never
  auto-loads it -- or restore the files above from `dca94436`; the releases
  up to then that shipped the driver carried
  a three-year GPL-2.0 section 3(b) offer for its source, which the pinned
  upstream commit above satisfies.

- **`%.*s` bound the terminator scan by precision, not after it**
  (`c90887f6`, 2026-09-07) -- non-conforming (C99 7.21.6.1p8) and a real
  out-of-bounds read: AxlFormat's `'s'` case scanned for a NUL
  unconditionally and applied the precision afterward, so any caller
  passing a bounded, unterminated buffer -- exactly what `%.*s` exists to
  allow -- read past the end of it. Reproduced under AddressSanitizer
  before a line changed. A census of the tree's 31 precision-bounded `%s`
  call sites found 18 reading out of bounds, none in `src/`; the worst was
  `tools/rfbrowse.c` scanning past a network-controlled HTTP body
  allocated at exactly content-length. The engine fix closes all 18 with
  no caller changed. **No output assertion can catch this** -- the
  emitted bytes are `min(first_nul, prec)` whether the scan is bounded or
  not -- so the regression guard is a new ASan fuzz harness
  (`test/fuzz/format_fuzz.c`), not the QEMU unit suite.

- **Two ways `verify.sh` reported green over a red gate** (`6ef18424`,
  2026-09-07). (1) The DETAIL column counted `': clean'` lines in a job's
  log regardless of its exit code, so a failing `make` job printed "N
  gate lines clean" beside a `SOMETHING FAILED` footer; a `fail_detail()`
  helper outside the per-job `case` now makes that structurally
  impossible for any future gate. (2) `check-libc-overlap` graded
  whichever of the staged and build archives existed first rather than
  the freshest, so it reported clean over a real `explicit_bzero`
  regression for roughly two weeks while `verify.sh` was reported ALL
  GREEN more than once. Both were reproduced by sabotage before being
  fixed. Three further instances of the same failure class turned up
  while proving these two, including one inside the gate itself where a
  weak definition in a later archive member silently erased a strong
  one.

- **Library log levels: the rule, the gate, and the sweep** (2026-08-14/15,
  shipped as v3.2.1 → v3.2.3) — a consumer moving its default to
  `AXL_LOG_INFO` saw AXL output on a healthy run. Three releases: the eight
  observed sites, then a census of all 42 `axl_info` (41 demoted, one kept
  because `axl_mem_dump_leaks` returns void and the QEMU leak gate greps it),
  then 213 of 358 `axl_warning`. Census 138/358/1/169 → 138/145/1/382. The
  durable half is the rule in [AXL-Coding-Style.md](AXL-Coding-Style.md) "Log
  Levels in Library Code" plus `gate.check-log-levels`, which requires a
  `/* log-level: */` marker rather than guessing from vocabulary. The
  discriminator is *can the caller observe this any other way?* — and it is one
  frame deeper than it looks, since a status can be checked by the immediate
  caller and discarded by its caller.

- ~~**`AXL_TLS` gets its own build tree** (2026-08-15)~~ — **SUPERSEDED
  2026-08-21: the flag is GONE.** `PREFIX` carried a `-tls` suffix because
  `AXL_TLS` was in the build-state signature and toggling it wiped ~300 objects
  (measured 321 one way, 270 back). mbedTLS is compiled unconditionally now, so
  there is one tree and nothing to toggle — measured, `hello.efi` is
  byte-identical by sha256 across the old non-TLS build, the old TLS build and
  the new one. See `docs/superpowers/specs/2026-08-21-axl-tls-flag-removal-design.md`.

  What survives is the rule the split forced: **ask `scripts/build-prefix.sh`,
  never compose `out/native-$arch` by hand.** That was 98 paths across 66
  scripts when the suffix appeared, and it is why REMOVING the suffix needed no
  corresponding sweep.

- **`cut-release.sh` refuses a breaking change under a non-major**
  (2026-08-15) — `## Unreleased` is branch-wide state and a release is a commit
  range; they agree only when the release is everything since the last tag.
  A v3.2.3 cut from `main` dated 43 unrelated commits, two of them
  `### Breaking`, into a patch and published before anyone read the version
  against the content. `scripts/check-release-semver.sh` now refuses that
  shape, with `--allow-breaking` as the deliberate exception. **It fires today**
  on `main`, whose `## Unreleased` carries the toolchain rework's breaking
  entries — so the next release from `main` must be a major.

- **Gate wall-clock: 115s → 74s** (2026-08-15) — `verify.sh` runs its jobs
  concurrently, so the wall clock is the slowest one. Docs was 115s and is now
  7s warm (HTML and man built together, `sphinx -j auto`); clang-tidy was ~45s
  and is now ~7s (`scripts/lint-cache.py` keys each TU on its own text, its
  headers' text, the flags and the checker, and skips what cannot differ).
  Both keep their teeth — verified by injecting a dangling `:ref:` and a
  `bugprone-branch-clone` and confirming each still fails the gate.

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
