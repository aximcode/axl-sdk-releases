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
  older mtime; Ninja then skips the rebuild and the run reports
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
**Ask `scripts/which-gates.sh` and run what it names.** It prints the exact
`--only=` list, derives the covering tests, and greps for consumers of
strings your diff removed; the table below is what it encodes, kept here to
be argued with, not to be typed from memory.

`verify.sh` has SEVEN jobs (`x64 aa64 lint gates imggates qloop docs`; the two
gate jobs were `make` and `makeimg` until 5.0.0, and those names are refused
with the new one) and takes
`--only=<csv>`, `--no-docs`, `--apps=<csv>` (one unit binary) and
`--print-ctest` (the ctest calls it would make; runs nothing). It is a CTest
front end: ONE ctest call per tree -- x64 (units, lint, gates, imggates, docs)
and aa64 (units) concurrently -- then `qloop` alone. With the pool service
answering (`AXL_VERIFY_POOL`, default auto) every unit binary runs on its own
leased `unit` guest and qloop on one more, so a full verify boots NOTHING;
without it a full unit job is `unit.one-boot` (every binary in one guest, its
ratchet and budget). `--apps` runs the named binaries one test each.
**219s full** (2026-11-07, pooled: x64 188s and aa64 198s side by side beside
lint/gates/docs, then `qloop` 19s -- 38-48s booted) -- AxlTestLoop alone on
x64, the quiet slot that judges the driver idle-budget timing the concurrent
jobs SKIP (`docs/AXL-Build-System-Design.md` §10.14); it asks the pool to settle first, and its row says where it ran
(on a leased guest / booted (no pool) / booted after a pool miss). Three results read
`UNJUDGED`, never green (exit 3): every attempt void; a bound missed in the
only judged attempt (the row names every such bound); and a FAIL made only
of named timing misses while QEMU waited over `AXL_STARVE_MAX_MS` (50 ms,
`scripts/axl-common.sh`) for a CPU -- load, not a verdict. Any other FAIL (a
functional check such as an attach, a leak, a stall) stays FAIL. Every qloop
row carries that wait figure; read it before believing a timing FAIL.
**`test-net-latency-qemu.sh` follows the same rule** with the same witness,
read over each guest's round-trip window: a run whose failures are ALL timing
bounds (p90, first exchange) each missed while its own window waited over the
limit exits 77 with an `UNJUDGED:` line; a missing reply, a wrong tick, or a
miss in a window that did not wait (or was not measured) stays FAIL, and
every measurement line carries its window's `cpu_wait_ms=` (or
`cpu_wait=unmeasured`). `lib/run-one.sh` RETRIES an UNJUDGED test (the
retry's verdict stands) and reads UNJUDGED only when no attempt judged;
`run-integration.sh` names and counts it apart and exits 3; ctest sees the 77
skip. `release-suite.sh` (CI's dispatched suite too) then re-runs it ALONE and
certifies only a judged PASS -- still UNJUDGED alone is exit 3 and no stamp,
because that run IS the release certificate (`docs/RELEASING.md`).
A sustained slowdown that
does not preempt (an E-core, a frequency drop) is seen by nothing and can
still FAIL a timing bound -- `scripts/qloop-verdict.sh`'s header (the one
copy of the verdict; `gate.qloop` runs it too) says what covers what. A
filtered run PRINTS WHAT IT DID NOT RUN — and a scoped one prints what it no
longer PROVES — so neither can be mistaken for a full run.

**The long pole is the aa64 one-boot guest (~120s, TCG).** The x64 tree's
gates finish under it; cut the guest before optimising the gates.

| what changed | the gate |
|---|---|
| one unit binary's C code | `verify.sh --only=x64,aa64 --apps=AxlTestFoo` — **~20s against 173s**. It does NOT check the ratchet, the wall-clock budget, or most of the cross-arch equality, and says so in its own output |
| one or a few source files, for lint only | `verify.sh --only=lint` (or `scripts/lint.sh`). INCREMENTAL: the x64-lint CMake preset re-checks exactly the TUs a change reaches, headers included -- ~2s for one file, ~60s from a cold `build/x64-lint`. There is no file-scoped form any more; it proved less than this does |
| a tool or harness script | `run-integration.sh --for=<the paths>` — it asks `which-gates.sh --porcelain` which tests name them, prints the list, and REFUSES rather than widening when nothing covers them |
| a doc Sphinx BUILDS — the root `README.md`, `src/*/README.md`, or one of the twelve `docs/*.md` its guides include (`AXL-Design.md`, `AXL-SDK-Design.md`, `AXL-Coding-Style.md`, `ROADMAP.md`, `RELEASING.md`, …) | `verify.sh --only=docs` — it is a ZERO-WARNING gate |
| prose no `.rst` includes (a handoff, `deps/*/README.md`) | **nothing** — ask `which-gates.sh`, which greps `docs/sphinx/` rather than guessing from the path. Read it yourself; that is the review prose gets |
| `docs/sphinx/**`, the root `README.md`, `src/*/README.md` | `verify.sh --only=docs` |
| C/C++, internal headers, `include/uefi/*`, `tools/`, `test/unit/`, `sdk/examples/` | `verify.sh --only=x64,aa64,lint,gates,imggates` — Doxygen reads only `include/axl/*.h`, so docs cannot see it |
| a PUBLIC header (`include/axl/*.h`, `*.hpp`) | `verify.sh` — every job; Doxygen renders it |
| shell tooling (`axl`, `axl-cc`, `install.sh`) | the covering integration test(s) DIRECTLY — not the suite |
| before a push | the covering tests, plus `run-integration.sh` ONLY when the change is one the next row names |
| a change that reaches everything: the build system, the harness, a widely-included header, or any string/symbol `which-gates` finds a consumer for | `run-integration.sh` — cache ON (default), DEFAULT worker count |
| cutting a release | `scripts/release-suite.sh` (its suite step is `ctest --preset x64-integration` with the cache off, on a CPU budget of `-j nproc+4` slots that each test draws its test-meta `cpu=` from (CTest `PROCESSORS`), no `--test-load`, ordered by measured times kept across CI checkouts (`docs/AXL-CI-Release-Speed-Design.md` §15) -- CI's is `check-test-est.py && ctest --preset x64-ci` -- plus the unit tests, SDK stage and self-tests CI runs) -- or dispatch CI |

**Why the pre-push suite is now conditional, and what makes that safe.** A
push to `main` runs only CI's CHEAP jobs -- `gcc x64`, `gcc aa64` (a clean
checkout) and `clang-tidy`; the whole suite runs UNCACHED in CI only on
`workflow_dispatch` (`gh workflow run ci.yml --ref main`, or
`cut-release.sh --ci-gate`) -- owner decision 2026-10-21,
`docs/AXL-CI-Release-Speed-Design.md` §14. So nothing re-checks a scoped push
with the suite: an integration-only regression can sit on `main` until the
next dispatch or release. Scope honestly, and dispatch after a risky landing.
A release is certified by a green dispatched suite run, or by a complete local
`scripts/release-suite.sh` run (the SAME steps as CI's integration job; the
only writer of the release stamp) PLUS the cheap push CI green on that commit
-- and `cut-release.sh` REFUSES otherwise unless given `--ci-gate` (dispatch
and wait) or `--tag-untested` (recorded in the release notes).
What used to make scoping unsafe was that the
local run could not be trusted to see the change at all: the cache decision
ran BEFORE a test built its own fixture, and no cache key contained
`libaxl.a`, so an `src/net/axl-ssh-*.c` edit left five ssh tests reporting
`CACHED 0s` with the change never exercised (81 targets across 79 of 203
tests were exposed). Since each test declares `builds=` in its `test-meta`
header and the runner pre-builds that union before deciding, a cached
"green" means the tests actually ran against your code. Scope on that basis,
not on a feeling that the change is small.

**NEVER run `build-docs.sh` after `verify.sh`.** It runs INSIDE
`verify.sh` (`scripts/verify.sh:288`, the `docs` job), so calling it
separately pays for Sphinx TWICE. `verify.sh`'s own header says so, at the
`--only` rationale near the top.

**Run ONE test by executing it directly** — `./test/integration/test-foo.sh`.
`run-integration.sh` takes **`--only=a.sh,b.sh`** — an EXPLICIT list, named by
you; it refuses a name discovery did not find rather than running nothing, and
a filtered run is announced PARTIAL. (No run-integration.sh run writes the
release stamp any more -- only `scripts/release-suite.sh` does.) It
chooses nothing itself, deliberately (§12.5). Its other filters are `--ci`
(drops `local-only=1`, cache OFF), `--only-local` (the inverse),
`--shard i/K` and `--no-build`. **It POOLS BY DEFAULT** (2026-11-06): a
`guest=` test runs on a leased, already-booted guest whenever the user's
`axlpool` service serves the arch, and the run says so; `--no-pool` boots,
`--pool` refuses to run without the pool. EVERY CTest preset defaults the same
way (`AXL_IT_POOL_DEFAULT=auto`, so a caller's own `AXL_IT_POOL` still wins --
`AXL_IT_POOL=0` boots) -- `x64-ci` and `x64-integration` too since 2026-11-07
(Mike: the self-hosted runners on this host use the pool), and they lease
with the TREE's own agent build (`AXL_IT_POOL_AGENT=tree`) so CI exercises
the commit's agent; a runner with no service boots and says so
(`docs/AXL-Test-Guest-Pool-Design.md` §6.7).
**It is a front end over CTest** (since
2026-11-06, design §10.8 step 6): it chooses and refuses, then makes ONE
`ctest` call on `build/<arch>-debug` and renders that run's JUnit file
(`scripts/ctest-report.py`). `--print-ctest` shows the exact argv and
environment without running anything. With no `-j` its CPU budget is nproc+4
slots, each test holding its `cpu=` (default 1) of them, with no load gate;
`-jN` is a budget of exactly N. One run per tree: a second is
refused by name. The suite is 204 tests (`--list | grep -c`,
2026-09-26); the ten dearest
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

Adding a gate is ONE edit: an `_axl_gate(check-<name> lint|artifact ...)`
line in `cmake/AxlIntegrationTests.cmake.in` (§10.11 of
`docs/AXL-Build-System-Design.md`). Every gate is a CTest test,
`gate.check-<name>`, run straight against a CMake tree, and that
registration IS the list: verify.sh's `gates` job selects the non-artifact
ones, `imggates` the `artifact`-labelled ones (they read the tree's images
and archives, and require the prep that built them), and CI's lint job runs
`ctest -L check` on both trees. A gate that reads an arch's own compiler or
images registers in each arch's tree. Run one alone:
`ctest --test-dir build/x64-debug -R '^gate.check-<name>$'` (add `-N -V` to
print the command it runs -- the script names do not all follow the gate's).
The Makefile's `make check-<name>` goals went with it in 5.0.0, and so did
its two gates of itself (check-dep-tracking: Ninja tracks every header;
check-build-mode).

| Gate | Catches |
|------|---------|
| `gate.check-ascii` | non-ASCII in an emittable string (draws as a console block) |
| `gate.check-docs` | a public header with no `doxygenfile` directive |
| `gate.check-dogfood` | a NEW raw UEFI protocol call bypassing the backend |
| `gate.check-test-meta` | an integration test with no `# test-meta:` header, a test that still RUNS make (or declares the retired `build-backend=`), or a `builds=` declaration that disagrees with the test's own `scripts/axl-build.sh` line in either direction. `builds=` is what lets the runner build a test's fixtures BEFORE it decides the cache -- without it the cache keys on a fixture the test has not rebuilt yet, and 81 targets across 79 tests were in that position. It reads a relative `scripts/axl-build.sh` (six scripts `cd "$PROJECT_DIR"` first), one behind a command prefix (`flock`, `timeout 300`), and one split across a continuation; it strips comments first, because a build in prose is not a build; and it FAILS on an argument it cannot resolve rather than reporting clean. Getting *that* wrong is how the gate itself went blind twice |
| `gate.check-test-inputs` | a tree file a test hands its guest OUTSIDE the skip-cache key -- a false hit waiting for an edit. The key reads a test's literal `$PROJECT_DIR/<file>` paths and its `builds=` products itself (`lib/test-cache.sh`, `lib/build-products.sh`); this fails a copy, redirect or tee into `$TEST_STAGING` (or a `--mount`ed dir) of a tree file that is none of those and not recorded (`test_record_input`), and any `--mount` of an in-repo path, unless the line says `# test-input: not-keyed -- <why>`. It asks the key's own readers, fails when they cannot answer or when it finds no staging write at all, and counts what it could not resolve in its clean line (`-v` lists every verdict) |
| `gate.check-test-est` | a test CI cannot actually run within its own limit: an `est=` over the limit `ctest --preset x64-ci` applies, or CI running the suite some other way -- `release-suite.sh` without `--ci`, or a ctest call to that preset not preceded by this gate (ctest, unlike the runner's `--ci`, refuses nothing at run time). It reads the selection AND the limit from `ctest --show-only=json-v1` (`scripts/check-test-est.py`), so it keeps no copy of either, and fails before any test runs rather than relying on CI to kill the offender — `test-ssh-hostile-qemu.sh` was, at est=250 against 240, after every local run (900s, no per-test limit) had passed it. It never configures CMake: an unconfigured tree is a named failure |
| `gate.check-size-gate-paths` | a file the build links into a size-capped image (`size-gate=`, e.g. sshd.efi) that lies outside the test's declared `size-gate-paths=` prefixes -- which-gates routes a change to the size test by those prefixes alone, so an undeclared input would scope the cap out. Derived from the CMake tree's own record (`ninja -t inputs` + `-t deps`, `scripts/size-gate-inputs.sh`), which refuses a tree older than its build definition |
| `gate.check-nul` | a literal NUL byte in a tracked text file |
| `gate.check-test-registered` | a `test_*` defined in `test/unit/` that nothing calls |
| `gate.check-cxx-entry` | a mangled C++ firmware entry point |
| `gate.check-examples` | an `sdk/examples/` source that no longer compiles — they ship in the distro packages, and 20 of 51 were reachable by no build rule at all |
| `gate.check-json-dialect` | JSON parsed with a non-strict dialect in `src/`/`tools/` without a `/* json-dialect: local-file */` justification (design decision 40) |
| `gate.check-uefi-scope` | UEFI reaching somewhere that promised not to have it: a type or `<uefi/…>` include in the PUBLIC API (held at zero, no allowlist), or shipped `tools/`/`sdk/examples/` code using UEFI without declaring why. Pairs with the `AXL_ALLOW_UEFI` guard in the uefi headers, which stops an application acquiring EDK2 by typing an `#include`; the gate catches what the guard cannot (a direct leaf-header include) |
| `gate.check-nx-compat` | a produced image missing NX_COMPAT |
| `gate.check-no-avx` | an UNGATED AVX instruction in a produced image (UEFI boots `CR4.OSXSAVE` clear, so it is `#UD`). Dispatched kernels are allowlisted per SYMBOL with a recorded justification -- not per object, so a second function acquiring VEX by accident still fails |
| `gate.check-bss-clear` | a crt0 that stopped zeroing .bss |
| `gate.check-flag-parity` | the build paths (the CMake build and its pack wrap, `axl-cc`, the CMake package `cmake/axl-config.cmake.in` installs -- which must delegate to axl-cc) disagreeing on an ABI/safety flag, a link flag, or an `objcopy -j` section. Both drifted in one afternoon: the stack protector reached 2 of 3, and a renamed relocation section reached 2 of 3 `-j` lists |
| `gate.check-libc-overlap` | a name BOTH `libaxl.a` and newlib define where AXL's is STRONG. Two strong providers inside `axl-cc`'s `--start-group` resolve by whichever reference happens to be outstanding when each archive is scanned — adding one object once turned an uncaught `throw 42;` into five `multiple definition` errors. The line falls where the RUNTIME DEPENDENCY falls: leaf functions (`memcpy`, `strlen`) are weak so newlib's better version wins; hooks that need AXL's own init/teardown (`__cxa_atexit`, `__stack_chk_fail`, `__stack_chk_guard`) stay strong because newlib's are inert under UEFI. Checks BOTH directions |
| `test-cxx-hosted-qemu.sh` (ctor fixture) | `--gc-sections` eating `.init_array`. Nothing REFERENCES an `.init_array` entry, so without `KEEP()` in the linker scripts it is collected and NO C++ global constructor runs -- silently, for as long as the C++ layer existed |
| `gate.check-reloc-coverage` | a `DT_RELA` relocation table SPLIT across two sections (the crt0 walks `DT_RELASZ` bytes and reads past the end), or dropped by `objcopy -j`, which takes EXACT section names. A latent aa64 bug for years; `-frtti` was the first workload to trigger it |
| `test-stack-guard-qemu.sh` | the stack protector silently not applying. `-fstack-protector-strong -mstack-protector-guard=global` is ON by default; the `guard=global` half is load-bearing on x64, where GCC otherwise reads the canary from `%fs:0x28` (glibc TLS) that UEFI never sets up |
| `scripts/lint.sh` | clang `-Wall -Wextra` diagnostics gcc never emits, plus clang-tidy over `src/` (full config) and, bugprone-only, `test/unit/`, `tools/` and `test/integration/`, plus the C++ TUs. A wrapper over the **x64-lint CMake preset** (`cmake/AxlLint.cmake`): clang-tidy runs beside each compile and Ninja re-checks only the TUs whose source, headers, `.clang-tidy` or clang-tidy changed. It takes no file arguments. **A source in no linted target is a CONFIGURE FAILURE, not a skip** -- the preset refuses to configure and names it. That is the old "in the `find` but not the database" rule made structural: a file in no target is never analysed and would read clean (`tools/axl.c` and 60 `test/integration/*.c` once sat exactly there). The host tools (pe-set-debug, axl-efi-pack, fwtool-host), built by the host compiler, get the bugprone pass too. The preset's CTest test adds the clang warnings pass |
| `scripts/build-docs.sh` | any Doxygen/Sphinx warning |
| `test_check_leaks` (in every `test_count_results` run) | a teardown memory leak, or a test binary that printed no leak verdict at all |

### Test buckets

- `test/unit/axl-test-*.c` for library code (runs in QEMU under
  `test-axl.sh`, ratcheted).
- `test/integration/test-*-qemu.sh` for end-to-end tool/network
  scenarios (opt out of ratchet via `TEST_SKIP_RATCHET=1`).
- `test/integration/test-*-selftest.sh` check the HARNESS, not the
  library, and are deliberately excluded from discovery. Run them with
  **`scripts/run-selftests.sh`** (~265s wall for ~355s of work over ten
  files, 2026-11-08 — 444s serial in CI before; each file declares what it
  cannot share with `# selftest-schedule: exclusive|tree`, the rest run in
  parallel; each verdict line prints its file's time; the
  `11 passed` counts an extra stray-guest assertion — led by
  `test-runner-selftest.sh` (~140s), `test-duration-budget-selftest.sh`
  (~72s of real sleeps) and `test-gate-scoping-selftest.sh` (~50s);
  `test-lint-gate-selftest.sh` drives the real `lint.sh` (~9s warm, ~70s
  cold, as on every CI run) and runs in parallel, outside the tree lane;
  `test-discover-selftest.sh` fell from 91s to 2s when `test_meta_field`
  stopped forking); CI runs it as its own
  step AFTER the suite. Never inside the pool: as a discovered test they
  took the suite to 121 passed / 59 failed, because the runner
  self-test's SIGTERM case drives a hang and reaps it, and under six
  workers the reap left two qemu processes and 108 MB of `/dev/shm`
  competing with everything after them.

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
  binary loudly -- `*** STALLED:` when the guest ended on its own,
  `*** TRUNCATED:` when the harness's OWN timeout fired (which says the
  named binary was merely in flight, not that it hung) -- plus the matching
  ratchet "Culprit:" / "Cut short by" line.

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
# Library, tests, tools, fixtures (for development) -- CMake + Ninja
scripts/axl-build.sh ARCH=x64 all          # = cmake --build build/x64-debug
scripts/axl-build.sh ARCH=aa64 all         # AARCH64 (cross-compile)
scripts/axl-build.sh ARCH=x64 BUILD=RELEASE tools
cmake --preset x64-debug && cmake --build --preset x64-debug   # by hand: see "pinned CMake" below
./test/integration/test-axl.sh              # the unit tests in QEMU

# SDK (for consumers, local staged build)
./scripts/install.sh --arch x64
./stage/bin/axl-cc sdk/examples/hello.c -o hello.efi

# `packaging/install.sh` is the install path for end users — release.yml
# publishes it plus VERSION, SHA256SUMS and three versioned tarballs.
# The .deb/.rpm RETIRED with D2 (AXL-Distribution-Design.md §17/§19), and D7
# deleted scripts/build-packages.sh with them -- there is no package build in
# the tree. A consumer whose policy mandates one gets it rebuilt from git
# history (§17.4), not from a script kept warm for a hypothetical. See
# README.md.
```

**The build is CMake + Ninja only** (`docs/AXL-Build-System-Design.md`;
the Makefile was retired in 5.0.0, §10.12). Trees are `build/<preset>`
(`x64-debug`, `aa64-debug`, `x64-release`, `aa64-release`, plus `x64-lint`
and `fuzz`); `scripts/axl-build.sh --print-prefix ARCH=.. BUILD=..` names one.
Every caller builds through `scripts/axl-build.sh`, which keeps the make-shaped
arguments (`ARCH=`, `BUILD=`, goal names from `cmake/AxlGoals.cmake`), refuses
any other variable rather than dropping it, and holds a per-tree `flock`
(Ninja cannot run twice in one tree). Ninja tracks every header and every flag
(a flag change rewrites the command, and the edge reruns); `make clean` is
`rm -rf build/<preset>`.

**A tree keeps the toolchain its first configure chose**
(`cmake/AxlToolchain.cmake`'s `AXL_TC_*` cache): a reconfigure naming another
compiler or `AXL_TOOLCHAIN`, or a moved `axl-toolchains.conf` default, is
refused with the way out (`cmake --fresh`). An existing tree does not notice a
newly installed ccache (`cmake --preset <p>` again, §10.6).

**Every local entry point runs the pinned CMake (4.3.6), not the box's**
(`scripts/axl-cmake-pin.sh`, design §10.13). CI and the release pin it in
`scripts/ci-cmake.sh`; a test once passed on every local run (3.31) and failed
only on CI (4.2 writes compile_commands.json's `output` absolute). What a
developer needs to know:
- **No particular cmake on the box, or none at all.** axl-build.sh (and so
  install.sh, which builds through it), verify.sh, run-integration.sh, lint.sh,
  release-suite.sh, check-fuzz-link.sh and every test that runs cmake/ctest
  call `axl_use_pinned_cmake`, which puts the pinned cmake/ctest first on
  PATH. `test-pinned-cmake.sh` fails a script that runs cmake or ctest
  without calling it.
- **The first build downloads ~50 MB once** (Kitware's tarball, checksum-
  verified) into `${XDG_CACHE_HOME:-~/.cache}/axl-sdk/cmake/<version>/`,
  shared by every checkout and worktree, and says so in one line. A failed
  install (offline, checksum) is an ERROR naming the retry command -- never a
  silent fall-back to the system cmake. On CI the pinned one is already first
  on PATH and the helper does nothing.
- **Existing trees migrate themselves.** A tree whose `CMakeCache.txt`
  `CMAKE_COMMAND` is another cmake (not the one on PATH, and not reporting the
  pinned version) is reconfigured `cmake --fresh` (with a
  line saying why) by axl-build.sh, lint.sh, and -- before any ctest reads it
  -- verify.sh/run-integration.sh/release-suite.sh. PATH alone moves nothing:
  ninja's regeneration edge runs the cmake the cache names.
- **A bare `cmake` you type is still yours.** For a hand-run `cmake --preset`
  that matches CI, put the pinned bin first:
  `PATH=~/.cache/axl-sdk/cmake/4.3.6/cmake-4.3.6-linux-$(uname -m)/bin:$PATH`.
  If you do configure a tree with another cmake, the next axl-build.sh moves
  it back.
- **To bump the pin:** change `AXL_CI_CMAKE_VERSION` and both SHA-256s in
  `scripts/ci-cmake.sh` (from Kitware's `cmake-<v>-SHA-256.txt`). Every tree
  then reconfigures `--fresh` on its next build; delete the old version's
  cache dir when nothing needs it.
- **Consumers are untouched, and their floor is tested:** the installed SDK
  (axl-cc, the CMake package, sdk/examples) builds with the consumer's own
  CMake; nothing there sources the helper. The floor is **CMake 3.20**
  (sdk/examples' `cmake_minimum_required(VERSION 3.20)`; below it the
  generated package sets `axl_FOUND` false with a reason under
  find_package(axl), and FATAL_ERRORs when loaded any other way (a bare
  include(), another package's config) -- a package config must not call
  cmake_minimum_required). `test-cmake-package.sh` runs every case under the
  pin AND under CMake 3.20.6 (`ci-cmake.sh --floor-version`,
  `axl_floor_cmake_bin`, same cache, ~45 MB once), so a newer feature in the
  generated package or sdk/examples fails there, and checks the package
  REFUSES 3.16.9 with its exact message (`--below-floor-version`,
  `axl_below_floor_cmake_bin`, ~40 MB, x86_64 only). To move the floor:
  `AXL_FLOOR_CMAKE_VERSION` and both SHA-256s in `scripts/ci-cmake.sh`,
  every sdk/examples minimum and the package's version check (the test
  checks all of them against the floor).

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
- `gate.check-docs` (`scripts/check-doc-coverage.py`, also in CI) fails if a
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
