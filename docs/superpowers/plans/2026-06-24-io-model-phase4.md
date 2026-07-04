# I/O Model Phase 4 — recipe rewrite + teaching example + v2.7.0

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).

**Goal:** Make the turnkey shared-driver ergonomics the *documented default* — rewrite the recipe around the macros, turn `sdk/examples/shared-driver-demo/` into a thoroughly-commented teaching example (stdin/stdout/stderr + exit status, zero plumbing), finalize the CHANGELOG, then cut **v2.7.0**.

**Architecture:** Docs + example only (no library code). The `sd-ergo` integration fixture (Phase 3, GREEN both arches) is the authoritative runtime proof; the example is the teaching mirror, verified to build via the staged SDK.

**Tech Stack:** Markdown docs; C SDK example (built via `scripts/install.sh` + `axl-cc`/CMake).

## Global Constraints

- axl-sdk ≥ current `main`; this phase completes **v2.7.0** (MINOR). Builds on Phase 3 (`3148bc18`).
- Doc-sync is mandatory (CLAUDE.md): the recipe must not describe the old manual pattern as the default once the macros exist. `make check-ascii` + `make check-docs` clean.
- Example must use standard C types / public API only; no EDK2 leak; no downstream-consumer names.
- Release: `scripts/cut-release.sh 2.7.0` — a `v*` tag publishes artifacts; **confirm the tag with the user before cutting** (`feedback_release_approval_gate`). Use `bump-version.sh` via the cut script; watch via `scripts/watch-release-runs.sh`.
- Facts to keep accurate: `run` == canonical `int main` (verbatim argv, argv[0]=program); the shell strips `MAX_BIT` from error-class `%lasterror%`; redirected stdout is shell-encoded (not UTF-8 — `axl_fopen` for UTF-8 files); stderr → `gST->StdErr` (`2>`); logs → stderr.

## File Structure

- `docs/AXL-Shared-Driver-Recipe.md` — rewrite the Code-shape section around the macros; keep the manual primitives as an "under the hood / advanced" subsection.
- `sdk/examples/shared-driver-demo/*` — rewrite driver + launcher to the macros; update the shared header + CMake if needed; heavy teaching comments.
- `CHANGELOG.md` — finalize the `## Unreleased` block for v2.7.0 (stderr change + exit-status + ergonomics + `_THIN`).

---

### Task 1: Rewrite AXL-Shared-Driver-Recipe.md around the macros

**Files:** Modify `docs/AXL-Shared-Driver-Recipe.md`

- [ ] **Step 1: Read the current recipe** end-to-end (it currently teaches the manual `axl_shared_driver_publish` + custom vtable + `axl_shared_driver_locate` + `int main` pattern).

- [ ] **Step 2: Rewrite the "Code shape" section** to lead with the turnkey path:
  - **Driver:** three `static` functions (`init`/`run`/`unload`) defined FIRST, then `AXL_SHARED_DRIVER("vendor/tool", init, run, unload)` LAST. State the static-first/macro-last idiom and the "one DriverEntry-emitting macro per TU" rule. Note `run` is the canonical `int main` (verbatim argv; argv[0]=program name, verb/args from argv[1]).
  - **Launcher:** the entire `int main` is `AXL_SHARED_DRIVER_LAUNCHER("vendor/tool", "toolDxe.efi", tool_driver_embed)`; document `AXL_SHARED_DRIVER_LAUNCHER_THIN("vendor/tool", "toolDxe.efi")` for the no-embed (disk-only) case (**M-a fix**).
  - **What the SDK owns now:** resolve (resident → disk → embedded), the stdio bridge (stdin/stderr reflect the launcher), and exit-status reflection (`run` calls `axl_set_exit_status(N)` like any app → launcher exits with N). Include the per-stream behavior table from the design (stdin via `axl_stdin_text()`; stdout/stderr honor `>`/`2>`; raw variants via the shell handles) and the two caveats: shell `MAX_BIT` truncation of error-class `%lasterror%`; redirected output is shell-encoded, use `axl_fopen` for UTF-8 files.
  - **Advanced / under the hood:** keep the manual `publish`/`unpublish`/`locate`/`dispatch`/`apply_exit_status` primitives in a clearly-labeled subsection for consumers that roll their own resolution (self-locate, REPL, `--reload`) — noting `axl_shared_driver_dispatch(vt, argc, argv)` is the bracket they call per dispatch.

- [ ] **Step 3: Gates** — `make check-ascii 2>&1 | tail -1; make check-docs 2>&1 | tail -1` clean.

- [ ] **Step 4: Commit** — `git add docs/AXL-Shared-Driver-Recipe.md && git commit -m "docs: rewrite shared-driver recipe around the turnkey macros"`

---

### Task 2: Rewrite the shared-driver-demo example (turnkey + heavily commented)

**Files:** Modify `sdk/examples/shared-driver-demo/shared-driver-demo-driver.c`, `-launcher.c`, `shared-driver-demo.h`, `CMakeLists.txt` (as needed)

- [ ] **Step 1: Read the current example** (all files) to preserve its build wiring (CMake `axl_add_driver`/`axl_add_app` + EMBEDS) and the shared-format-TU pattern.

- [ ] **Step 2: Rewrite the driver** to `AXL_SHARED_DRIVER("axl/demo", demo_init, demo_run, demo_unload)` (macro LAST). `demo_run(argc, argv)` — parse the verb at `argv[1]` (it's `int main`) — demonstrates ALL of it with plain app code and teaching comments:
  - reads a line from `axl_stdin_text()` and echoes it to **stdout** (`axl_print`);
  - writes a diagnostic to **stderr** (`axl_printerr` / `axl_warning`) — comment that it lands on `2>`, not `>`;
  - a verb that arms an **exit status** via `axl_set_exit_status(N)` — comment that the launcher exits with N (small-int survives `%lasterror%`; note the `MAX_BIT` caveat for error-class);
  - header comment block walking through what the SDK does behind the scenes (resolve → bridge → dispatch → exit-status) and the per-stream table.
  - Keep `demo_init` doing token "heavy" setup (a comment: this runs once per boot) and `demo_unload` the teardown.

- [ ] **Step 3: Rewrite the launcher** to the single macro: `AXL_SHARED_DRIVER_LAUNCHER("axl/demo", "shared-driver-demo-driver.efi", demo_driver_embed)` — with a comment that this one line IS the whole launcher. Update `shared-driver-demo.h` (drop the now-unneeded custom vtable typedef; keep only shared constants/format decls). Update `CMakeLists.txt` embed symbol name if it changed.

- [ ] **Step 4: Verify it builds via the staged SDK.** Run `./scripts/install.sh --arch x64` to stage the SDK, then build the example the way a consumer would (its `CMakeLists.txt` via `axl-cc`, per `sdk/examples/`'s convention — read the example's README/CMake for the exact command; typically `find_package(axl)` + `cmake`/`make`, or `out/bin/axl-cc` directly). Confirm both the driver `.efi` and launcher `.efi` build with no errors. (Runtime behavior is already proven by the Phase-3 `sd-ergo` fixture, which is the same macros; this step proves the EXAMPLE compiles against the packaged SDK.) If the SDK-consumer build is not reproducible in this environment, fall back to compiling both TUs with the dev toolchain's driver/app CFLAGS (`$(CC) $(CFLAGS) $(INCLUDES) -c`) and say so in the report.

- [ ] **Step 5: Gates + commit** — `make check-ascii`/`check-docs` clean. `git add sdk/examples/shared-driver-demo && git commit -m "example: shared-driver-demo rewritten to the turnkey macros (stdin/stdout/stderr + exit status)"`

---

### Task 3: Finalize CHANGELOG for v2.7.0

**Files:** Modify `CHANGELOG.md`

- [ ] **Step 1: Review the whole `## Unreleased` block** — confirm it coherently covers the entire v2.7.0 story: **Changed** (stderr → `gST->StdErr`, `2>` captures it / `>` no longer does; logs → stderr) and **Added** (`axl_stderr_raw`; `axl_shared_driver_apply_exit_status` + exit-status reflection; raw stdout in a driver; the turnkey ergonomics — `AxlSharedDriverVtable`, `AXL_SHARED_DRIVER`/`_LAUNCHER`/`_LAUNCHER_THIN` (**ensure `_THIN` is listed — M-a**), `axl_shared_driver_dispatch`/`run`). Merge/dedupe entries added across phases into a clean, reader-facing block. No behavior claim that isn't true (verify against the design's caveats).

- [ ] **Step 2: Gates** — `make check-ascii`/`check-docs` clean.

- [ ] **Step 3: Commit** — `git add CHANGELOG.md && git commit -m "docs: finalize CHANGELOG for v2.7.0"`

---

### Task 4: Cut v2.7.0 (controller-run, after user tag approval)

> NOT a subagent task — the controller runs this after Tasks 1-3 land and the user approves the tag.

- [ ] Run the full local suite as the release gate: `./test/integration/run-integration.sh -j$(nproc)` (note any SDK-not-staged env failures as in prior releases; the affected-path fixtures — `test-io-redirect-qemu.sh`, `test-driver-stdio-qemu.sh`, `test-sd-ergo-qemu.sh` — must be GREEN both arches).
- [ ] `scripts/cut-release.sh 2.7.0 --dry-run`, review, then `scripts/cut-release.sh 2.7.0 --yes` (commits version bump + dates CHANGELOG, pushes, tags, publishes). Watch to green; report the published version.

---

## Self-Review

- **Spec coverage (design §7, §8):** recipe rewrite (Task 1), teaching example (Task 2 — the artifact the user explicitly asked for), CHANGELOG (Task 3), release (Task 4). Covers the two Phase-3 review Minors that belong here: M-a (`_THIN` now documented in recipe + CHANGELOG). M-b (old fixtures strip argv[0]) is left as-is per the whole-branch review (not public API); optionally a one-line comment could be added but it's out of this phase's doc/example scope.
- **Placeholders:** none; Task 2 Step 4 flags the SDK-consumer build command as "read the example's CMake/README for the exact invocation" — a grounding instruction, since the example builds via the packaged SDK, not the dev Makefile.
- **Risk — example build reproducibility:** the example builds via `install.sh` + `axl-cc`, which may not be exercised in every environment; Task 2 Step 4 has an explicit dev-toolchain compile fallback so the example is at least compile-verified, and the runtime behavior is already proven by the identical-macro `sd-ergo` fixture.
- **Release gate:** confirm the tag with the user before `cut-release.sh` (hard rule).
</content>
