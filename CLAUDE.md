# axl-sdk

GLib-inspired C library — with first-class C++ support — and SDK for
UEFI. Public API (`axl.h`) uses standard C types, snake_case naming,
UTF-8 strings, and is fully usable from C++ (the `axl-c++` driver,
RAII via `AXL_AUTOPTR`). No EDK2 headers leak through
the public API. The SDK packages the library for standalone UEFI app
development (no EDK2 source tree needed).

Part of the [AximCode](https://github.com/aximcode) project.
AXL = AximCode Library. Pronounced "axle."

## Design Docs (MANDATORY)

- ALWAYS read `docs/AXL-Design.md` before making library changes
- ALWAYS read `docs/AXL-SDK-Design.md` before making SDK changes
- Follow `docs/AXL-Coding-Style.md` exactly
- VERIFY new APIs match the design doc conventions

## Development Workflow — Test-First (MANDATORY)

Headline rule: write the test BEFORE the implementation for any
new behavior. Watch it fail. Then implement. The exact cadence
depends on what kind of work you're doing — see the matrix below
— but the discipline of "contract first, code second" is what
pays.

The QEMU-in-the-loop test cycle is ~30s/arch, so strict XP
"one test at a time" is too slow. Batch 5-10 failing tests per
phase, run once to confirm RED, implement to GREEN, run once to
confirm. This is still test-first; the cadence is just batched.

### Workflow per work type

**A. New public API (STRICT test-first, Red-Green-Refactor):**
1. Header declaration + docstring first; the docstring IS the
   contract.
2. **Contract-first review (when NOT already design-reviewed with
   the user):** independent review of *just the header +
   docstrings* before implementing — API/vtable shape, result
   fields, ownership, composability, docstring gaps. The contract
   is the most expensive thing to get wrong and the cheapest to
   change before an impl + adapters + tests depend on it. SKIP
   when we already designed it together in discussion (that IS
   the early review — don't double-pay). See
   `feedback_code_review_before_commit`.
3. Write failing tests pinning exact return values / strings.
4. Build, run, confirm RED.
5. Implement, run, confirm GREEN.
6. Refactor — clean up the implementation AND the tests while
   they stay green. Look for: duplication, names that read
   wrong now you've seen them in use, "test passes for the
   wrong reason" smells, dead branches the test never
   exercises. Re-run after each change. This is the cheapest
   moment to fix these — before review burns its budget on
   them.
7. Independent code-review pass per
   `feedback_code_review_before_commit`.
8. Apply review fixes. Commit.

For a **large multi-phase change**, add a mid-point review at the
first stable green (e.g. an engine core before the adapters layered
on it) so later phases aren't built on a flawed foundation —
`feedback_code_review_before_commit`.

**B. Output / format changes (STRICT test-first, EXACT strings):**
Use `axl_strcmp(buf, "exact") == 0`. NOT `axl_strstr` — substring
matches silently allow the regressions they're meant to catch
(commit b0e567b's `<unknown>` cleanup was the cautionary tale;
Phase D `test_check(true, ...)` reviewer-caught was the same).

**C. Refactors that preserve behavior (TEST-PROTECTED, not TDD):**
Existing tests are the safety net. Refactor. Existing tests must
still pass. Add new tests only for new behavior the refactor
enables.

**D. Bug fixes (STRICT test-first, mirroring A):**
Write the regression test first, confirm it fails against current
code, fix, confirm GREEN, then refactor while green (the fix often
exposes an existing smell worth cleaning up under test
protection). Test stays as the regression guard.

**E. New test infrastructure (INFRA-FIRST, then TDD):**
Write the infra. Run an existing test. Confirm infra works. THEN
apply STRICT test-first on top. Trying to TDD the infra itself
leads to chicken-and-egg awkwardness.

### Hard rules (no exceptions)

- **Exact-string assertions for output**. `axl_strcmp(buf, "...")
  == 0`, not `axl_strstr`. Substrings lie.
- **No `test_check(true, "documents intent")`**. Either delete or
  replace with a real assertion. Tautologies pretend coverage.
- **Confirm RED before implementing**. Otherwise you don't know
  if the test exercises the path you think it does.
- **Refactor while green** (buckets A, B, D). After GREEN, clean
  up implementation AND tests before the review pass. Tests
  must stay green at every step. Skipping this step ships
  uglier code than necessary and lets "passes for the wrong
  reason" smells reach review.
- **Balanced SKIP counts** per `feedback_balancer_count` — silent
  cross-arch ratchet drift is a real failure mode.
- **Leaks fail the run.** `test_check_leaks` (`common-test.sh`,
  called from `test_count_results`) greps the serial log for the
  teardown report `=== AxlMem leak report:` and fails. It also
  requires EVERY test binary to print a verdict — a binary that
  prints none (RELEASE build, a console silenced without
  `axl_log_set_console_enabled(true)`, a fault before teardown)
  fails too, because a gate that can't see is worse than none.
  A mid-run `axl_mem_dump_leaks()` is headed
  `(live allocations)` and is deliberately NOT matched. Do not
  widen the grep to go green: a leak in a test is a test bug, a
  leak in `src/` is a library defect, and an intentional
  never-freed allocation gets removed at the source (see
  `AXL_PROTOCOL_NAME_MAX`), not allowlisted in a shell script.
- **Empty is not an answer unless you know the question was asked.**
  When a check shells out, distinguish "the tool could not RUN"
  from "the tool ran and found NOTHING": capture the exit status,
  do not read the output alone. The two are the same empty string
  and opposite facts. Six instances in one day (2026-08-20):
  `check-cxx-entry` reported an `nm` that could not start as
  "missing or name-mangled in C++" and an `objdump` that could not
  load as "registered NO .init_array entry" -- a codegen diagnosis
  for a missing `libdebuginfod.so.1`, which failed the v4.3.0 tag
  run; `check-awk-portability` reported "clean -- 203 build files"
  over a `strtonum` it could not see, because its glob never
  matched the extensionless `axl-cc`; `check-release-semver` named
  one of two `### Breaking` entries through a `grep -A3` window;
  `watch-release-runs` would have reported a release green without
  ever waiting for Docs. `check-awk-portability`'s OWN docstring
  already recorded an earlier instance of the identical wrong
  message, which is why this belongs here and not in a comment
  only that file's next reader sees.
- **A detector's silence is worth nothing until you have shown it
  can fail.** Prove it SEES the thing before trusting that it
  found none: `scripts/sabotage.sh` for a gate, a poisoned `PATH`
  for a toolchain claim (`test-hermetic-toolchain.sh`), a stub on
  `PATH` for a network poller (`test-release-watch-scope.sh`).
  Every new detector gets a control assertion in the same test --
  and check the control does not pollute what it is proving, which
  is its own trap: the hermeticity test's control invoked a shim
  and had to clear the call log before asserting the log was empty.
- **Independent review is staged** per
  `feedback_code_review_before_commit`: contract-first for new
  public API (unless already designed with the user), an optional
  mid-point pass for large multi-phase work, and ALWAYS an
  integration pass before commit. Tests + review together catch
  what tests alone miss ("test passes for the wrong reason").

### Sabotage verification — use `scripts/sabotage.sh`

Sabotage by hand has two footguns that have each produced a WRONG
answer in this tree, so use the script:

```sh
scripts/sabotage.sh -s 'FILE:SED_EXPR' --expect-fail -- <command>
scripts/sabotage.sh -p PATCH           --expect-fail -- <command>
```

It snapshots, applies, runs, restores, and **`touch`es on restore**
after verifying the file is byte-identical again.

- Restoring with `sed -i.bak` + `mv` gives the source the BACKUP's
  older mtime; `make` then skips the rebuild and the run reports
  failures belonging to the PREVIOUS sabotage.
- A sed that matches nothing leaves the file untouched and the suite
  green — which reads as "no test covers this" when the truth is "no
  defect was introduced". The script refuses a no-op sabotage.
- `--expect-fail` makes "the suite must notice" the assertion, so a
  sabotage that changes nothing observable FAILS instead of quietly
  looking like a pass.

### Build gates (all must be green before commit)

`./scripts/verify.sh` runs everything below concurrently and prints
one table; prefer it over hand-running these individually.

**MATCH THE GATE TO THE CHANGE — the full set is not the default.**
`verify.sh` has exactly three jobs (`lint`, `make`, `docs`) and takes
`--only=<csv>` / `--no-docs`. A filtered run PRINTS WHAT IT DID NOT RUN,
so it cannot be mistaken for a full one.

| what changed | the gate |
|---|---|
| prose / design docs only | `verify.sh --only=docs` (~1m54s vs ~10m) |
| shell tooling (`axl`, `axl-cc`, `install.sh`) | the covering integration test(s) DIRECTLY — not the suite |
| C/C++/headers | `verify.sh` + the covering test |
| before a push | `run-integration.sh` — cache ON (default), DEFAULT worker count |
| cutting a release | add `--no-cache`; still the default worker count |

**NEVER run `build-docs.sh` after `verify.sh`.** It runs INSIDE
`verify.sh` (`scripts/verify.sh:121`), so calling it separately pays for
Sphinx TWICE. Its own header says so at `:29`.

**Run ONE test by executing it directly** — `./test/integration/test-foo.sh`.
`run-integration.sh` takes **`--only=a.sh,b.sh`** — an EXPLICIT list, named by
you; it refuses a name discovery did not find rather than running nothing, and
a filtered run is announced PARTIAL and cannot write the release stamp. It
chooses nothing itself, deliberately (§12.5). Its other filters are `--ci`
(drops `local-only=1`, cache OFF), `--only-local` (the inverse),
`--shard i/K` and `--no-build`. The suite is 201 tests; the ten dearest
declare ~1,260s between them, led by `test-cpu-spike-qemu.sh` (233s),
`test-host-toolchain-qemu.sh` (180s) and `test-consumer-install.sh`
(150s). Running all of it to check a message string is how a 30-second
edit costs fifteen minutes.

**Do not re-run a suite that was green earlier in the same session when
your change cannot reach it** — and say which you skipped and why. That
is a claim to justify, not an excuse: "nothing I touched can affect it"
is only honest after checking what CONSUMES what you changed. Output text
is an interface — a message string can be exact-matched by a test in
another file (`test-install-lifecycle.sh:546` broke exactly that way).
`grep -rn` the string before deciding.

Adding a gate is ONE edit: append it to `LINT_GATES` in the Makefile.
`verify.sh` reads that list back via `make -s print-lint-gates` rather
than keeping a second copy, and `NONCLEAN_GOALS` is built from it. The
two lists used to be maintained by hand and drifted, which is not a
cosmetic bug: an unexcluded gate WIPES `$(BUILDDIR)/*.o` and `libaxl.a`,
and `verify.sh` runs the make job concurrently with both arch builds, so
it deletes them mid-build. `check-tautology` did exactly that.

| Gate | Catches |
|------|---------|
| `make check-ascii` | non-ASCII in an emittable string (draws as a console block) |
| `make check-docs` | a public header with no `doxygenfile` directive |
| `make check-dogfood` | a NEW raw UEFI protocol call bypassing the backend |
| `make check-test-meta` | an integration test with no `# test-meta:` header |
| `make check-nul` | a literal NUL byte in a tracked text file |
| `make check-test-registered` | a `test_*` defined in `test/unit/` that nothing calls |
| `make check-cxx-entry` | a mangled C++ firmware entry point |
| `make check-examples` | an `sdk/examples/` source that no longer compiles — they ship in the distro packages, and 20 of 51 were reachable by no build rule at all |
| `make check-json-dialect` | JSON parsed with a non-strict dialect in `src/`/`tools/` without a `/* json-dialect: local-file */` justification (design decision 40) |
| `make check-uefi-scope` | UEFI reaching somewhere that promised not to have it: a type or `<uefi/…>` include in the PUBLIC API (held at zero, no allowlist), or shipped `tools/`/`sdk/examples/` code using UEFI without declaring why. Pairs with the `AXL_ALLOW_UEFI` guard in the uefi headers, which stops an application acquiring EDK2 by typing an `#include`; the gate catches what the guard cannot (a direct leaf-header include) |
| `make check-nx-compat` | a produced image missing NX_COMPAT |
| `make check-no-avx` | an UNGATED AVX instruction in a produced image (UEFI boots `CR4.OSXSAVE` clear, so it is `#UD`). Dispatched kernels are allowlisted per SYMBOL with a recorded justification -- not per object, so a second function acquiring VEX by accident still fails |
| `make check-bss-clear` | a crt0 that stopped zeroing .bss |
| `make check-flag-parity` | the three build paths (Makefile, `axl-cc`, the CMake package `install.sh` generates) disagreeing on an ABI/safety flag or an `objcopy -j` section. Both drifted in one afternoon: the stack protector reached 2 of 3, and a renamed relocation section reached 2 of 3 `-j` lists |
| `make check-libc-overlap` | a name BOTH `libaxl.a` and newlib define where AXL's is STRONG. Two strong providers inside `axl-cc`'s `--start-group` resolve by whichever reference happens to be outstanding when each archive is scanned — adding one object once turned an uncaught `throw 42;` into five `multiple definition` errors. The line falls where the RUNTIME DEPENDENCY falls: leaf functions (`memcpy`, `strlen`) are weak so newlib's better version wins; hooks that need AXL's own init/teardown (`__cxa_atexit`, `__stack_chk_fail`, `__stack_chk_guard`) stay strong because newlib's are inert under UEFI. Checks BOTH directions |
| `make check-build-mode` | `BUILD=release` (lowercase) meaning DEBUG. `CFLAGS_BUILD` compares against `RELEASE` while `PREFIX` lowercases, so it built debug objects into the directory named `release` — ALIASING the real release tree and wiping it on the next correct build |
| `make check-dep-tracking` | a C/C++ object compiled WITHOUT `-MD -MP`, so it has no header dependency and a header edit rebuilds nothing. `CXXFLAGS` lacked them for the whole life of the C++ layer, which made a sabotage of `axl-istream.hpp` read as UNDETECTED — the code was never recompiled. The `.axl-build-state` signature does NOT cover this: it hashes which FLAGS an object used, not which HEADERS it depends on. Assembly is exempt only while it `#include`s nothing, and the gate re-checks that rather than assuming it |
| `test-cxx-hosted-qemu.sh` (ctor fixture) | `--gc-sections` eating `.init_array`. Nothing REFERENCES an `.init_array` entry, so without `KEEP()` in the linker scripts it is collected and NO C++ global constructor runs -- silently, for as long as the C++ layer existed |
| `make check-reloc-coverage` | a `DT_RELA` relocation table SPLIT across two sections (the crt0 walks `DT_RELASZ` bytes and reads past the end), or dropped by `objcopy -j`, which takes EXACT section names. A latent aa64 bug for years; `-frtti` was the first workload to trigger it |
| `test-stack-guard-qemu.sh` | the stack protector silently not applying. `-fstack-protector-strong -mstack-protector-guard=global` is ON by default; the `guard=global` half is load-bearing on x64, where GCC otherwise reads the canary from `%fs:0x28` (glibc TLS) that UEFI never sets up |
| `scripts/lint.sh` | clang `-Wall -Wextra` diagnostics gcc never emits, plus clang-tidy over `src/` and (bugprone-only) `test/unit/` |
| `scripts/build-docs.sh` | any Doxygen/Sphinx warning |
| `test_check_leaks` (in every `test_count_results` run) | a teardown memory leak, or a test binary that printed no leak verdict at all |

### Test buckets

- `test/unit/axl-test-*.c` for library code (runs in QEMU under
  `test-axl.sh`, ratcheted).
- `test/integration/test-*-qemu.sh` for end-to-end tool/network
  scenarios (opt out of ratchet via `TEST_SKIP_RATCHET=1`).

### When test-first genuinely doesn't apply

Real-hardware-only paths (BMC scenarios, NIC quirks QEMU doesn't
emulate). Document explicitly in the commit message AND say so to
the user — never claim test coverage for code that wasn't actually
exercised. SKIP-path balancers handle topology-gated tests cleanly.

### Testing firmware-lifecycle UEFI APIs (no QEMU hangs)

For any API over a UEFI protocol with register/unregister/connect
lifecycle (`->Register`/`->Unregister`, `ConnectController`, driver
binding): the normal "misuse it to test the error path" instinct
backfires — UEFI has no memory-safety net, so the failure mode is a
hang/`#GP`, not a clean `AXL_ERR`. See
`feedback_uefi_firmware_test_hazards`.

- **Only assert safe negatives**: the errors your OWN validation
  produces before the firmware call (NULL / 0 / bad-enum) + protocol
  absent. NEVER a double-unregister, a freed/re-used device path, or a
  bogus handle — the firmware frees the device path on `Unregister`, so
  reusing it deref's freed memory and wedges.
- **Connect-on-synthetic-media is real-hardware territory.** Drive the
  positive round-trip with input the firmware binds/rejects *cleanly*
  (a raw disk); document the finicky driver as consumer/real-media
  tested (OVMF's ISO9660/El Torito driver LOOPS on a synthetic CD).
- **Run the single binary first.** `test-axl.sh` runs all unit binaries
  in ONE QEMU boot under ONE timeout, so a hang starves every later
  binary. `TEST_APPS_ONLY=AxlTestX ./test/integration/test-axl.sh` (or
  `run-qemu.sh --timeout 30 …AxlTestX.efi`) and confirm it prints its
  Results footer before the full suite. The harness now names a stalled
  binary loudly (`*** STALLED:` + the ratchet "Culprit:" line).

## Current State (May 2026)

All migration, cleanup, and style phases done. **10497 unit assertions and 171
integration tests, 0 failures (2026-08-20)** — `test/integration/.last-pass-count`
is the authority, this line is a snapshot and dates fast. Native backend only (gcc + ld + objcopy).
Test runner has ratchet check (fails if count drops below baseline).

| Module | Directory | Header |
|--------|-----------|--------|
| AxlMem | src/mem/ | axl/axl-mem.h (arena types live here too) |
| AxlFormat | src/format/ | (internal) |
| AxlLog | src/log/ | axl/axl-log.h |
| AxlData | src/data/ | axl/axl-hash-table.h, axl-array.h, axl-list.h, axl-slist.h, axl-queue.h, axl-json.h, axl-cache.h, axl-radix-tree.h, axl-ntree.h, axl-tree.h, axl-ring-buf.h, axl-str.h, axl-string.h, axl-digest.h, axl-hmac.h, axl-bytes.h, axl-sidecar.h |
| AxlStream | src/stream/ | axl/axl-stream.h |
| AxlFs | src/fs/ | axl/axl-fs.h |
| AxlUtil | src/util/ | axl/axl-path.h, axl-args.h, axl-config.h, axl-config-file.h (free-form key=value file map), axl-hexdump.h, axl-sort.h, axl-time.h, axl-env.h, axl-sys.h, axl-nvstore.h, axl-driver.h, axl-rng.h, axl-rand.h, axl-mem-phys.h (raw physical access), axl-mem-region.h (region map + fault-safe range access over GCD/EFI memory map) |
| AxlLoop | src/loop/ | axl/axl-loop.h, axl-defer.h, axl-pubsub.h |
| AxlEvent | src/event/ | axl/axl-event.h, axl-cancellable.h, axl-wait.h |
| AxlRuntime | src/runtime/ | axl/axl-runtime.h, axl-signal.h, axl-atexit.h |
| AxlService | src/service/ | axl/axl-service.h, axl-embed.h (lifecycle wrapper over AxlLoop; composes axl-loop + axl-config + axl-driver; AXL_SERVICE_DRIVER macro in axl.h) |
| AxlSharedDriver | src/util/axl-shared-driver.c | axl/axl-shared-driver.h (thin wrappers over axl-driver + axl-protocol for the synchronous-RPC "thin launcher + resident driver" pattern; no event loop; see docs/AXL-Shared-Driver-Recipe.md + sdk/examples/shared-driver-demo/) |
| AxlTask | src/task/ | axl/axl-task.h, axl-buf-pool.h, axl-async.h |
| C++ layer | src/cxxrt/axl-cxxrt-terminate.cpp (the only .cpp left in src/) | axl/axl-cxx.hpp (`axl::result`), axl-arena-allocator.hpp (`axl::arena_allocator`), axl-handle.hpp (`axl::unique_handle`, opt-in per type via `AXL_DEFINE_AUTOPTR_CLEANUP`). **C0-C7 ALL SHIPPED as of 2026-08-17** — the seam headers are axl-cstr.hpp (`axl::view`/`adopt`), axl-array.hpp (`array_span` over the new `axl_array_data()`), axl-ntree.hpp (four lazy ranges), axl-radix-tree.hpp, axl-gfx-surface.hpp (`gfx_target_scope`), and axl-json.hpp (`json_document`/`json_writer`/`json_scanner`, chaining `operator[]`). Every one is HEADER-ONLY; the layer compiles no .cpp beyond the runtime glue. Exceptions WORK (`axl-c++ -fexceptions`), but are a per-TU opt-in with `-fno-exceptions` the default, so every header must be usable in both modes — which is why errors are `axl::result` and nothing throws. The standard containers are the default and need no flag -- `std::vector`/`string`/`map`/`unordered_map` work on both arches (`--hosted` is removed; passing it is an error). **ONE C++ LINK SHAPE since P4 (2026-08-17):** every C++ link carries the toolchain's `libstdc++`/`libsupc++`, the four `axl-cxxrt-*.o` glue objects and the EXCEPTIONS linker script -- `libaxl-cxx.a` and its 7 substitute sources are DELETED. That is what makes `<iostream>`/`<sstream>`/`<fstream>` work (booted both arches, `test-cxx-iostreams-qemu.sh`); it costs +46,928 `.text` on a containers image -- but quote the **`.efi`**, +100,339 (58,758 -> 159,097), and note a fixture UNDERSTATES it: a consumer measured **+153,886 to +178,118 (+28-36%)** on four real x64 tools. An iostreams image is ~734 KB of `.text`, which is why `axl::cout` (~700 B over `axl_printf`) stays the default. `axl_mem_fail_next_alloc()` NO LONGER reaches `operator new` -- that is libstdc++'s and calls newlib `malloc`, so C++ OOM fixtures must request an unsatisfiable size instead. **`axl::string` is KEPT** (decided 2026-08-16, design §9c): `std::string` HALTS on OOM under `-fno-exceptions` while `axl::string` sets `bad()`, which `axl::cin` reads to report `AXL_NO_RESOURCES` -- and it measures 564 B against `std::string`'s 1045 B. This was an "open T5 question" for a while; T5 was a doc-restructure task that closed 2026-08-14, so that phrasing was always wrong. See docs/AXL-Cxx-Design.md |
| AxlNet | src/net/ | axl/axl-tcp.h, axl-udp.h, axl-url.h, axl-http-server.h, axl-http-client.h, axl-inet-address.h, axl-socket.h, axl-socket-client.h, axl-net.h (umbrella) |
| AxlTls | src/net/ | axl/axl-tls.h (mbedTLS is always compiled in; `--gc-sections` keeps it out of any image that never calls `axl_tls_init`) |
| AxlCrypto | src/net/ | axl/axl-crypto.h (PK sign/verify, AEAD, ECDH, AES-CTR). **`AXL_PK_ED25519` is RESERVED and unimplemented** — mbedTLS 3.6.3 has no twisted-Edwards curve at all, so it is a vendoring project, not a config flag (`docs/superpowers/specs/2026-08-21-ed25519-design.md`) |
| AxlJose | src/net/ | axl/axl-jose.h (JWS/JWT/JWK; ES256/ES384/RS256/PS256/HS256) |
| Axl9p | src/9p/ | axl/axl-9p.h (9P2000.L client AND server over AxlTcp; all five phases DONE 2026-07-22 -- client read+write, `axl_9p_mount` publishing a Shell-visible fsN:, async `Axl9pServer` exporting an AxlFs subtree, and the `9p` tool) |
| AxlGfx | src/gfx/ | axl/axl-gfx.h |
| AxlConsole/AxlVterm | src/util/, src/vterm/ | axl/axl-console-ops.h (producer-agnostic console op vtable), axl-console-tap.h (SIMPLE_TEXT_OUTPUT swap producer), axl-console-device.h (take-over producer), axl-console-mirror.h (console→VT byte-stream encoder + remote input), axl-console-term.h (on-screen AxlConsoleOps grid renderer), axl-vterm.h (VT/xterm byte-stream→ops, Layer 2 over vendored libvterm), axl-console-screen.h (server-side screen model + self-contained snapshot serializer for late-join repaint; shares src/util/axl-console-vt.h pen→SGR encoder with the mirror) |
| AxlSmbios | src/smbios/ | axl/axl-smbios.h |
| AxlAcpi | src/acpi/ | axl/axl-acpi.h |
| AxlPci | src/pci/ | axl/axl-pci.h (config-space access + ids/class JSON5 sidecars) |
| AxlUsb | src/usb/ | axl/axl-usb.h (enumeration, descriptors, hub-port tree, ids JSON5 sidecar) |
| AxlBlock | src/block/ | axl/axl-block.h (EFI_BLOCK_IO enumeration + media descriptor) |
| AxlNvme | src/nvme/ | axl/axl-nvme.h (NVMe identify + SMART/health + self-test + raw admin pass-thru) |
| AxlSerial | src/serial/ | axl/axl-serial.h (EFI_SERIAL_IO enumeration + line settings + control bits) |
| AxlFv | src/fv/ | axl/axl-fv.h (EFI_FIRMWARE_VOLUME2 enumeration + attributes + file count) |
| AxlFw | src/fw/ | axl/axl-fw.h (raw .fd / SPI-image parser: FV/FFS/section tree + on-demand LZMA/none decompress; offline sibling of runtime AxlFv; backs `fwtool` list/extract/find) |
| AxlTpm | src/tpm/ | axl/axl-tpm.h (TCG2 TPM 2.0 presence + capability) |
| AxlRamDisk | src/ramdisk/ | axl/axl-ramdisk.h (create/list/destroy FAT RAM disks over EFI_RAM_DISK_PROTOCOL; mkrd is a thin CLI over it) |
| AxlSmbus | src/smbus/ | axl/axl-smbus.h |
| AxlIpmi | src/ipmi/ | axl/axl-ipmi.h |
| AxlSpd | src/spd/ | axl/axl-spd.h (DDR4/DDR5 SPD reader + JEDEC vendor JSON5 sidecar) |

## Roadmap

See `docs/ROADMAP.md` for the unified phase tracker — it is the
single source of truth for what's done and what's pending.

## Backend Abstraction

Internal header `src/backend/axl-backend.h` provides backend-agnostic
functions for memory, console, time, file I/O, wide-string ops, events,
timers, console input, MP services, stall, and signal.

All library code uses backend functions and the `axl_efi_call` macro
for UEFI protocol calls. The single backend implementation is in
`src/backend/native/axl-backend-native.c`. UEFI types come from
AXL's own generated headers in `include/uefi/`.

## Coding Style

See `docs/AXL-Coding-Style.md`. Key rules:
- `axl_snake_case` functions, `AxlPascalCase` types, `AXL_SCREAMING_CASE` macros
- 4-space indent, K&R braces, no space before parens
- Standard C types in public API (never UEFI types)
- Doc comments: `///<` inline params, `@brief`/`@return` in block comment
- Multi-line function signatures (even single-param)

## Project Layout

```
include/
  axl.h                        Umbrella header (includes all public headers + AXL_APP)
  axl/                         Public headers (standard C types only)
  uefi/
    axl-uefi.h                 Umbrella — includes generated/all.h + axl-uefi-extra.h
    axl-uefi-extra.h           Hand-written: Shell protocols, SMBIOS_HANDLE_PI_RESERVED
    generated/                 Auto-generated from spec HTML (do not edit)
      all.h                    Umbrella with forward declarations
      types.h                  Scalar types (Table 2-4) + composite types + system tables
      status.h                 Status codes from Appendix D
      tables.h                 Revision defines, boot/runtime service funcptrs
      console.h                Simple Text Input/Output protocols
      network.h                TCP4, IP4, DNS4, IP4Config2, ServiceBinding
      mp-services.h            MP Services protocol (from PI spec)
      smbios.h                 SMBIOS protocol (from PI spec)
      guids.h                  All protocol GUIDs + EDK2-style aliases
      ...
src/
  mem/                         axl-mem.c (allocation), axl-arena.c (arena allocator)
  format/                      axl-format.c (printf engine, zero dependencies)
  data/                        axl-hash.c, axl-array.c, axl-str.c (basics + UTF-8/UCS-2 + number parsing) + axl-str-{bmh,base64,scan}.c sub-modules, axl-strbuf.c, axl-sidecar.c (shared JSON5 sidecar loader), axl-class-fmt.c (shared PCI/USB triplet output assembly), etc.
  stream/                      axl-stream.c (dispatch + console + encoding), axl-stream-buf.c, axl-stream-file.c, axl-stream-text.c
  fs/                          axl-fs.c (path-based file ops + dir + volume)
  log/                         axl-log.c, axl-log-ring.c, axl-log-file.c
  util/                        axl-args.c, axl-path.c, axl-hexdump.c, axl-time.c
  smbios/                      axl-smbios.c
  loop/                        axl-loop.c, axl-defer.c, axl-pubsub.c
  service/                     axl-service.c (structured-lifecycle wrapper composing loop+config+driver)
  event/                       axl-event.c (one-shot latch), axl-cancellable.c, axl-wait.c
  task/                        axl-task-pool.c, axl-buf-pool.c, axl-async.c
  net/                         axl-tcp.c, axl-http-{server,route,conn,request,dispatch,response,upload,ws}.c, axl-http-client.c, axl-tls.c, etc.
  posix/                       axl-app.c (argv from UEFI shell parameters)
  runtime/                     axl-runtime.c (init/cleanup/default-loop/yield), axl-registry.c (tier-1 resource registry), axl-atexit.c (LIFO), axl-signal.c (interrupt + axl_exit)
  backend/                     axl-backend.h (internal API)
    native/                    axl-backend-native.c (core backend), axl-backend-native-event.c (events + timers + close-debug ring), axl-backend-native-mp.c (MP services)
  crt0/                        axl-crt0-native.c (UEFI entry point stub, bridges int main → _AxlEntry)
test/
  unit/                        9 test binaries (346 tests total)
  integration/                 QEMU test runner + HTTP integration tests
    test-axl.sh                QEMU test runner (ratchet: fails if count drops)
    common-test.sh             Shared test helpers
tools/
  hexdump.c                    Hex/ASCII file viewer
  fetch.c                      HTTP client (GET/POST/PUT)
  find.c                       Recursive file finder
  grep.c                       Pattern search
  sysinfo.c                    System inventory (firmware, SMBIOS, memory)
  netinfo.c                    Network diagnostics and ping
  mkrd.c                       RAM disk management
sdk/
  examples/                    Standalone SDK examples (int main, axl-cc)
    hello.c
    CMakeLists.txt
scripts/
  build.sh                     Library builder (--arch, --release, --clean)
  install.sh                   SDK packager (builds library, packages headers+libs)
  axl-common.sh                Shared infra (logging, QEMU discovery)
  generate-uefi-headers.py     Manifest-driven UEFI header generator
  uefi-manifest.json5           Declares which types to extract from spec HTML
  download-uefi-specs.py       Downloads UEFI/PI/ACPI/Shell specs from uefi.org
deps/
  uefi-spec/                   UEFI 2.11 spec HTML (downloaded)
  pi-spec/                     PI 1.8 spec HTML (downloaded)
  acpi-spec/                   ACPI 6.5 spec HTML (downloaded, optional)
  shell-spec/                  Shell 2.2 spec PDF (downloaded, reference only)
docs/
  AXL-Design.md                Library design doc (phases, API spec, style)
  AXL-SDK-Design.md            SDK design doc (architecture, toolchains, phases)
  AXL-Coding-Style.md          Style guide
```

## UEFI Header Generation (Phase N6 — DONE)

UEFI type definitions are auto-generated from spec HTML, driven by a
manifest (`scripts/uefi-manifest.json5`). The manifest lists each type
by name and kind (struct, enum, funcptr, define, typedef, table).
The generator searches `<pre>` blocks in the spec HTML for matching
definitions and extracts them by their C boundaries.

```bash
# Download specs (one-time, needs Playwright for HTML specs)
python3 scripts/download-uefi-specs.py

# Regenerate headers from spec HTML
python3 scripts/generate-uefi-headers.py

# Dump all <pre> blocks for inspection
python3 scripts/generate-uefi-headers.py --dump /tmp/blocks.txt

# Check source code for types missing from manifest
python3 scripts/generate-uefi-headers.py \
  --check src/ include/axl/ \
  --extra-header include/uefi/axl-uefi-extra.h
```

**Adding a new UEFI type:** Add an entry to `uefi-manifest.json5` and
regenerate. Order in the manifest IS the dependency order. Unknown
struct member types are automatically replaced with `void *`. The
`--check` flag (also run in build.sh) warns about types used in source
but missing from the manifest.

**Sources:** UEFI Spec 2.11, PI Spec 1.8, ACPI Spec 6.5 (HTML).
Shell Spec 2.2 is PDF-only; Shell protocols remain hand-written in
`axl-uefi-extra.h`.

**Stats:** 275 definitions extracted, 327 GUIDs, 0 compile errors.

## Build

```bash
# Library (for development)
make                                        # X64
make ARCH=aa64                              # AARCH64 (cross-compile)
make tests                                  # build test EFIs
./test/integration/test-axl.sh               # 1081 unit tests in QEMU

# SDK (for consumers, local staged build)
./scripts/install.sh --arch x64
./out/bin/axl-cc sdk/examples/hello.c -o hello.efi

# `packaging/install.sh` is the install path for end users — release.yml
# publishes it plus VERSION, SHA256SUMS and three versioned tarballs.
# The .deb/.rpm RETIRED with D2 (AXL-Distribution-Design.md §17/§19), and D7
# deleted scripts/build-packages.sh with them -- there is no package build in
# the tree. A consumer whose policy mandates one gets it rebuilt from git
# history (§17.4), not from a script kept warm for a hypothetical. See
# README.md.
```

**A CFLAGS change forces a rebuild.** Objects do not depend on the Makefile,
so editing `CFLAGS_BASE` used to rebuild NOTHING — `make` printed the new
flags on whichever file it happened to recompile and left the other 200
objects built the old way. That produced four wrong readings while the stack
protector was added, including "0 objects instrumented" on a build whose
command line plainly carried the flag, and a sabotage that looked UNDETECTED
because the restored source was never rebuilt.

The build records a SIGNATURE (`$(BUILDDIR)/.axl-build-state`) covering a hash
of `CFLAGS`/`CXXFLAGS`/`INCLUDES` (how objects compile) and `CC`/`CXX` (which
compiler emits them); a change in either wipes the objects, both archives and
every `.efi`/`.so` under `$(PREFIX)`. It once covered a third input, the
`AXL_TLS` toggle (which sources compile) — that flag is gone, but the mechanism
stayed, because the other two produce the same class of silently-stale build. `$(BUILDDIR)/.axl-flags` holds that
compiler pair followed by the exact flag set the neighbouring objects were
built with — read it first when a build looks wrong.

`CC`/`CXX` joined the signature after `AXL_X64_GXX=... AXL_CPP=1 make` reused
host-g++ objects, so a full-suite run "with the bare-metal toolchain" silently
measured the host one. It captures the compiler's SPELLING, not its identity:
an in-place gcc upgrade or a ccache shim on PATH still hashes the same.

The flags are written with `$(file ...)` and hashed from disk rather than piped
through a shell: `CFLAGS` carries `-DMBEDTLS_CONFIG_FILE=...`, and in its
original `'<axl-mbedtls-config.h>'` spelling the embedded quotes ended the shell
argument and left `<...>` as a redirect — the first version hashed the empty
string, and a constant signature is a check that never fires. The macro is now
spelled `'"axl-mbedtls-config.h"'`, which removes the redirect hazard AND stops
`bugprone-macro-parentheses` firing on it: `<name.h>` parses as a comparison,
and a COMMAND-LINE macro has no file, so no clang-tidy header filter can
suppress it.

Incremental builds correctly handle public-header restructures
(add / remove / rename a struct field, type, or function): the
libaxl.a recipe deletes the archive before rebuilding it, so a
renamed-and-removed source file's stale .o can't survive across
builds. (Earlier the `ar rcs` insert-or-replace behavior left
stale members in place, requiring `make clean` after structural
changes — fixed since.)

**DELETING a source file is the case that still bites, and it looks
like your edit did nothing.** `rm`ing a .c and dropping it from
`LIB_SOURCES` shrinks `$(LIB_OBJS)` — and **removing a prerequisite
does not make a target out of date**, so the libaxl.a recipe never
runs, its `rm -f $@` never fires, and the deleted file's .o stays an
archive member. `make` prints "AXL library built" and the symbol is
still exported. Measured while deleting `axl-str-compat.c` /
`axl-intrinsics.c`: `nm` still showed `memcpy` after a clean-looking
build. The rename case above genuinely is safe (the new .o is a NEWER
prerequisite, so the recipe fires). After deleting a source, remove
its stale .o and the archive explicitly, or `make clean`.

## Documentation

API docs are auto-generated from header Doxygen comments and module
README files. The pipeline: Doxygen → XML → Breathe → Sphinx → HTML.

```bash
# Build docs locally (requires doxygen, sphinx, breathe, myst-parser)
./scripts/build-docs.sh
# Output: out/docs/html/  out/docs/man/
```

**Sphinx structure** (`docs/sphinx/`):
- `index.rst` — table of contents (lists all module pages)
- `modules/*.rst` — one page per module, each includes its `src/*/README.md`
  via `.. include::` and generates API reference via `.. doxygenfile::`
- `conf.py` — Sphinx config (ReadTheDocs theme, Breathe, myst-parser)
- `Doxyfile` — Doxygen config (XML-only output for Breathe)

**Doc sync is part of the workflow, not an afterthought** (MANDATORY —
this is where doc drift creeps in). Any time you add or change PUBLIC API
— a new header, a new function in an existing header, a new struct field,
OR a behavior change that a README *describes* — do the matching doc
update in the SAME change, before the pre-commit review:

**When adding a new public header:**
1. Add `///<` param docs and `@brief`/`@return` block comments to the header
2. Add `.. doxygenfile:: axl-new-header.h` to the appropriate module `.rst`
3. If it's a standalone type (like AxlCache, AxlRadixTree), create its own
   `modules/new-type.rst` page and add it to `index.rst`
4. Update `src/*/README.md` — this content is pulled into Sphinx automatically

**When changing an EXISTING header (new function / field / behavior):**
- The `.. doxygenfile::` already renders the new members automatically — but
  **re-read the module's `src/*/README.md` for staleness**. The prose is the
  trap: a README that says "this does not move bytes" after byte I/O lands is
  *worse* than no doc. A script can't catch this; you must. (Real example: the
  serial README claimed "does not move bytes" through a whole byte-I/O addition.)

**Gates (run before commit; see "Build gates" above for the full set):**
- `make check-docs` (`scripts/check-doc-coverage.py`, also in CI) fails if a
  public header has no `doxygenfile` directive — catches the *structural* gap
  (a new header with no docs). It does NOT catch prose staleness.
- `./scripts/build-docs.sh` is a **zero-warning gate** — it fails on any
  Doxygen or Sphinx warning, and its failure output names the usual causes.
  Run it before committing any public-header change. Two things it CANNOT see,
  because Doxygen emits nothing for them: a markdown code span whose content
  starts with an apostrophe or ends with a backslash (both desynchronize every
  following backtick), and a `#AXL_FOO` reference that no longer resolves.
  Note the docs *workflow* only runs on tagged releases, so this local run is
  the real enforcement point.
- Every public header needs a `@file` block. `EXTRACT_ALL` is `NO`, so without
  one Doxygen cannot reference ANY member of that header from anywhere — and
  a `\ref` to it fails with a message that points at the referring site, not
  at the real cause.

**Deployment**: `.github/workflows/docs.yml` runs on every release (any
`vX.Y.Z` tag) or on demand (`gh workflow run docs.yml --ref main`) — NOT on
every push to `main` (keeps Actions minutes for tagged releases; the local
suite is the push-time gate). Builds HTML and deploys to Cloudflare Pages
(`axl-sdk-docs` project). Consequence: API changes pushed to `main` between
tags are NOT on the published site until the next tag or a manual dispatch —
run `./scripts/build-docs.sh` locally to verify rendering in the meantime.

## Key Architecture Decisions

- AxlFormatLib is zero-dependency (breaks Log→Data circular dep)
- axl-mem.c uses local `mem_cpy`/`mem_set`/`mem_strlen` helpers
  (can't call axl_memcpy from AxlDataLib — circular dependency)
- AxlLogLib can't use axl_malloc (circular dep with AxlMemLib)

## Consumer Projects

- `aximcode/uefi-devkit` — build orchestrator + bootable USB
- `aximcode/axl-webfs` — axl-webfs-dxe + CmdServe
