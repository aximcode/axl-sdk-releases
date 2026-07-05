# Sibling-Only Shared-Driver Locate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add version-pinned sibling-only shared-driver resolution and make the default multi-path search prefer the co-located driver.

**Architecture:** New `axl_shared_driver_locate_sibling` / `axl_shared_driver_run_sibling` (cold path = `axl_driver_load_sibling`, hard-fail, granular rc) + `AXL_SHARED_DRIVER_LAUNCHER_SIBLING` macro; reorder `driver_build_candidates` so the sibling is tried first; propagate `AXL_NOT_FOUND` through the `locate*` family instead of collapsing to `AXL_ERR`.

**Tech Stack:** C (UEFI), gcc+ld both arches, QEMU integration fixtures.

**Spec:** `docs/superpowers/specs/2026-07-04-shared-driver-sibling-locate-design.md` (read §2–§3).

## Global Constraints

- Ships in `CHANGELOG.md` `## Unreleased` → **v2.7.1** (user's deliberate choice to ship new API in a PATCH). `### Added`: the two functions + macro; `### Changed`: default search prefers the sibling.
- Both arches (`make ARCH=x64`/`aa64`) 0 warnings; `scripts/lint.sh` clean; `make check-docs check-ascii` clean; Sphinx builds 0 warnings.
- Tests use public headers only. Exact-value rc assertions (`AXL_NOT_FOUND` vs `AXL_OK`), not substring.
- Do NOT regress existing driver fixtures (reorder is a behavior change): `test-sd-ergo`, `test-driver*`, `test-stdio-bridge-*`, `test-io-redirect` must stay green both arches.
- Direct commits to `main`; no release without user tag approval.
- Mirror sources (read before editing): `axl_shared_driver_locate_with_image_info` + `axl_shared_driver_run` + `axl_shared_driver_dispatch` (`src/util/axl-shared-driver.c:179,311,295`); `driver_build_candidates` (`src/util/axl-driver.c:1103`, candidate blocks at 1139/1148); `_axl_driver_ensure_with_embedded_info` tail (`src/util/axl-driver.c:~1525` final `return AXL_ERR`); sd-ergo fixture (`test/integration/sd-ergo-{driver,launcher}.c`, `test-sd-ergo-qemu.sh`).

## File Structure

- `include/axl/axl-shared-driver.h` — declare `axl_shared_driver_locate_sibling`, `axl_shared_driver_run_sibling` (+ docstrings, granular-rc note).
- `include/axl.h` — `AXL_SHARED_DRIVER_LAUNCHER_SIBLING`.
- `src/util/axl-shared-driver.c` — impl both fns; propagate granular rc in the multi-path wrapper.
- `src/util/axl-driver.c` — reorder candidates; `_ensure_*` returns `AXL_NOT_FOUND` on nothing-loaded.
- `test/integration/sd-sibling-driver.c`, `sd-sibling-probe.c`, `test-sd-sibling-qemu.sh` — new fixture + runner.
- `Makefile` — build targets for the fixture (mirror `sd-ergo`).
- `CHANGELOG.md`, `docs/AXL-Shared-Driver-Recipe.md` — docs.

---

### Task 1: Header decls + stubs + macro (compile seam for the tests)

Declarations + docstrings first (the contract), plus stubs so tests link. No behavior yet.

**Files:** `include/axl/axl-shared-driver.h`, `include/axl.h`, `src/util/axl-shared-driver.c`

**Interfaces produced:**
- `int axl_shared_driver_locate_sibling(const char *name, const char *driver_filename, void **out_iface);`
- `int axl_shared_driver_run_sibling(const char *name, const char *driver_filename, int argc, char **argv);`
- `AXL_SHARED_DRIVER_LAUNCHER_SIBLING(name_str, driver_filename)`

- [ ] **Step 1: Declare both functions in the public header** with the docstrings from spec §2.2 and §2.4 (include the warm-path caveat and the `@return AXL_OK / AXL_NOT_FOUND / AXL_INVALID / AXL_ERR` contract). Place them after `axl_shared_driver_locate_with_image_info`.

- [ ] **Step 2: Add stub impls** in `src/util/axl-shared-driver.c` so the tree links:

```c
int
axl_shared_driver_locate_sibling(
    const char  *name,
    const char  *driver_filename,
    void       **out_iface
    )
{
    (void)name; (void)driver_filename;
    if (out_iface != NULL) { *out_iface = NULL; }
    return AXL_ERR;   /* STUB — Task 3 implements */
}

int
axl_shared_driver_run_sibling(
    const char  *name,
    const char  *driver_filename,
    int          argc,
    char       **argv
    )
{
    (void)name; (void)driver_filename; (void)argc; (void)argv;
    return 1;   /* STUB — Task 3 implements */
}
```

- [ ] **Step 3: Add the macro** to `include/axl.h` after `AXL_SHARED_DRIVER_LAUNCHER_THIN` (spec §2.4 form). Verify `axl_shared_driver_run_sibling` is visible via the `<axl/axl-shared-driver.h>` include that `axl.h` already pulls.

- [ ] **Step 4: Build both arches, 0 warnings.** `make ARCH=x64 && make ARCH=aa64`. A `-fsyntax-only` macro-expansion smoke of `AXL_SHARED_DRIVER_LAUNCHER_SIBLING("x","y.efi")` in a scratch TU is optional but nice.

- [ ] **Step 5: Commit.** `git add include/axl/axl-shared-driver.h include/axl.h src/util/axl-shared-driver.c && git commit -m "feat: declare shared-driver sibling-locate API (stubs)"`

---

### Task 2: RED fixture — divergence matrix

A trivial resident driver (from the standard `AXL_SHARED_DRIVER` macro) + a probe that calls both `axl_shared_driver_locate_sibling` and `axl_shared_driver_locate` and prints exact rc + which driver answered. Runner stages it two ways.

**Files:** `test/integration/sd-sibling-driver.c`, `test/integration/sd-sibling-probe.c`, `test/integration/test-sd-sibling-qemu.sh`, `Makefile`

- [ ] **Step 1: Write the driver fixture** modeled on `sd-ergo-driver.c`. A `DRIVER_TAG` compile define lets one source build two distinct drivers. The vtable `run` prints `SDSIB:tag=<TAG>\n` so the probe can tell which driver resolved. Shared name via a `sd-sibling.h` header (`#define SDSIB_NAME "axl/sd-sibling"`).

- [ ] **Step 2: Write the probe** (`sd-sibling-probe.c`) — a shell app that:
  - Reads `argv[1]` = scenario (`hardfail` or `reorder`).
  - `hardfail`: calls `axl_shared_driver_locate_sibling(SDSIB_NAME, "sd-sibling-driver.efi", &vt)`; prints `SDSIB:sibling_rc=<AXL_int>`; then `axl_shared_driver_locate(SDSIB_NAME, "sd-sibling-driver.efi", NULL, 0, &vt2)`; prints `SDSIB:multi_rc=<AXL_int>`. (Uses `axl_printf("SDSIB:sibling_rc=%d\n", rc)` with the raw AXL_* int; the runner asserts the exact numbers for `AXL_OK`/`AXL_NOT_FOUND` — capture those two constants' values in the runner from a generated header or hard-code with a comment.)
  - `reorder`: calls `axl_shared_driver_locate(...)` then `vt->run(0, NULL)` and lets the driver print its `SDSIB:tag=` marker; asserts it's the sibling's tag.
  - Prints a final `SDSIB: done` sentinel.

- [ ] **Step 3: Makefile targets** — mirror the `sd-ergo` block: build `sd-sibling-probe.efi`, and two drivers `sd-sibling-driver-a.efi` / `-b.efi` from `sd-sibling-driver.c` with `-DDRIVER_TAG=A` / `=B`. (Read the `sd-ergo` Makefile block and copy its recipe macros verbatim.)

- [ ] **Step 4: Runner `test-sd-sibling-qemu.sh`** (mirror `test-sd-ergo-qemu.sh` + the pass/fail-exit tail from `test-stdio-bridge-reap-qemu.sh`). Two image builds:
  - **Scenario hardfail:** stage `sd-sibling-probe.efi` at `app/probe.efi`, driver-A ONLY at `drivers/<arch>/sd-sibling-driver.efi` (NOT beside the probe). startup: `app\probe.efi hardfail`. Assert `SDSIB:sibling_rc` == the `AXL_NOT_FOUND` value AND `SDSIB:multi_rc` == the `AXL_OK` value.
  - **Scenario reorder:** stage probe at `app/probe.efi`, driver-A at `app/sd-sibling-driver.efi` (sibling), driver-B at `drivers/<arch>/sd-sibling-driver.efi`. startup: `app\probe.efi reorder`. Assert the dispatched tag is `A` (the sibling), proving sibling-first.
  Use `test_add_efi src dest` for placement; `arch_dir "$TEST_ARCH"` for `<arch>`. `chmod +x`.

- [ ] **Step 5: Confirm RED** against current code (Task-1 stubs + un-reordered search):
  - `hardfail`: `sibling_rc` = `AXL_ERR` (stub), not `AXL_NOT_FOUND` → **FAIL**.
  - `reorder`: `axl_shared_driver_locate` loads driver-B (`/drivers` first, current order) → tag `B` → **FAIL**.
  Run `timeout 200 ./test/integration/test-sd-sibling-qemu.sh --arch X64; echo EXIT=$?` → expect non-zero. Capture the log with `TEST_KEEP_LOG=/tmp/sib.log` and confirm both scenarios fail for the stated reasons.

- [ ] **Step 6: Commit the RED test.** `git add test/integration/sd-sibling-* test/integration/test-sd-sibling-qemu.sh Makefile && git commit -m "test: RED for sibling-locate hard-fail + default sibling-first reorder"`

---

### Task 3: GREEN — reorder, locate_sibling, run_sibling, granular rc

- [ ] **Step 1: Reorder `driver_build_candidates`** (`src/util/axl-driver.c`): move the sibling block (`2: <image_dir>/<name>`, ~1148–1162) so it is appended BEFORE the `1: drivers/<arch>/<name>` block (~1139–1146). Keep the `/drivers/<name>` root, root-`<name>`, and other-volume blocks after. Update the numbering comments.

- [ ] **Step 2: `_axl_driver_ensure_with_embedded_info` granular rc** (`src/util/axl-driver.c`): change the final nothing-loaded `return AXL_ERR;` (after the embedded-fallback block, ~line 1525) to `return AXL_NOT_FOUND;`. Leave the bad-args early `return AXL_ERR;` as-is.

- [ ] **Step 3: Propagate rc in the multi-path locate wrapper** (`src/util/axl-shared-driver.c`, `axl_shared_driver_locate_with_image_info`): capture the ensure rc and return it instead of forcing `AXL_ERR`:

```c
    int _rc = _axl_driver_ensure_with_embedded_info(
        &guid, driver_filename, embed_blob, embed_len,
        /* override_name */ NULL, load_options, load_options_size, info);
    if (_rc != AXL_OK) {
        axl_warning("axl_shared_driver_locate: failed to load '%s'",
                    driver_filename);
        return _rc;   /* was AXL_ERR — now AXL_NOT_FOUND when not found */
    }
```
Leave the second failure (`find_guid != AXL_OK || *out_iface == NULL` → "loaded but protocol not published") returning `AXL_ERR` — that's a genuine error, not not-found.

- [ ] **Step 4: Implement `axl_shared_driver_locate_sibling`** (replace the Task-1 stub), mirroring `_with_image_info` but sibling-only:

```c
int
axl_shared_driver_locate_sibling(
    const char  *name,
    const char  *driver_filename,
    void       **out_iface
    )
{
    if (name == NULL || driver_filename == NULL || out_iface == NULL) {
        return AXL_ERR;
    }
    *out_iface = NULL;

    AxlGuid guid;
    if (axl_shared_driver_guid(name, &guid) != AXL_OK) {
        return AXL_ERR;
    }
    /* Warm: resident driver of this identity already published — reuse it.
       Pinning governs only the cold path; the first cold load pins the boot. */
    void *warm = NULL;
    if (axl_protocol_find_guid(&guid, &warm) == AXL_OK && warm != NULL) {
        *out_iface = warm;
        axl_backend_stdio_bridge_install();
        return AXL_OK;
    }
    /* Cold: SIBLING-ONLY. Return the load rc verbatim so callers keep
       AXL_NOT_FOUND ("not staged beside us") / AXL_INVALID. */
    AxlDriverHandle h = NULL;
    int lrc = axl_driver_load_sibling(driver_filename, &h);
    if (lrc != AXL_OK) {
        axl_warning("axl_shared_driver_locate_sibling: '%s' not staged "
                    "beside the launcher", driver_filename);
        return lrc;
    }
    if (axl_driver_start(h) != AXL_OK) {
        axl_warning("axl_shared_driver_locate_sibling: start failed for '%s'",
                    driver_filename);
        axl_driver_unload(h);
        return AXL_ERR;
    }
    if (axl_protocol_find_guid(&guid, out_iface) != AXL_OK
        || *out_iface == NULL) {
        axl_warning("axl_shared_driver_locate_sibling: '%s' loaded but "
                    "protocol for '%s' not published", driver_filename, name);
        return AXL_ERR;
    }
    axl_backend_stdio_bridge_install();
    return AXL_OK;
}
```
Confirm `axl_driver_load_sibling` / `_start` / `_unload` are declared via the includes already in the file (`<axl/axl-driver.h>`); add if missing.

- [ ] **Step 5: Implement `axl_shared_driver_run_sibling`** (replace stub), mirroring `axl_shared_driver_run`:

```c
int
axl_shared_driver_run_sibling(
    const char  *name,
    const char  *driver_filename,
    int          argc,
    char       **argv
    )
{
    void *iface = NULL;
    if (axl_shared_driver_locate_sibling(name, driver_filename, &iface) != AXL_OK
        || iface == NULL) {
        axl_warning("axl_shared_driver_run_sibling: failed to load '%s'",
                    driver_filename);
        axl_set_exit_status(AXL_EFI_NOT_FOUND);
        return 1;
    }
    return axl_shared_driver_dispatch(
        (const AxlSharedDriverVtable *)iface, argc, argv);
}
```

- [ ] **Step 6: Build both arches 0 warnings**, then the fixture → GREEN both arches: `timeout 200 ./test/integration/test-sd-sibling-qemu.sh --arch X64; echo EXIT=$?` (expect 0) then `--arch AARCH64`. `hardfail`: `sibling_rc==AXL_NOT_FOUND`, `multi_rc==AXL_OK`. `reorder`: tag `A`.

- [ ] **Step 7: Regression — existing driver fixtures stay green (both arches)** (the reorder is a behavior change): `test-sd-ergo`, `test-driver.sh`, `test-driver-identity-qemu.sh`, `test-driver-leak.sh`, `test-driver-parent-leak-qemu.sh`, `test-stdio-bridge-reap-qemu.sh`, `test-driver-stdio-qemu.sh`, `test-io-redirect-qemu.sh`. Each exit 0. Also `grep -rn "== AXL_ERR" src include` for any caller that switched on the exact `AXL_ERR` the rc change now turns into `AXL_NOT_FOUND`; fix or confirm none.

- [ ] **Step 8: Refactor while green** — dedup any warm+bridge-install between `locate_sibling` and the other variants if it reads cleaner; re-run the fixture.

- [ ] **Step 9: Commit.** `git add src/ include/ && git commit -m "feat: sibling-only shared-driver locate + sibling-first default search + granular rc"`

---

### Task 4: Docs + verification

- [ ] **Step 1: CHANGELOG** — under the existing `## Unreleased`: add `### Added` (`axl_shared_driver_locate_sibling` / `axl_shared_driver_run_sibling` / `AXL_SHARED_DRIVER_LAUNCHER_SIBLING` — the version-pinned path) and `### Changed` (default multi-path search now prefers the co-located sibling over `/drivers/<arch>/`; `locate*` return `AXL_NOT_FOUND` instead of `AXL_ERR` when the driver isn't found).

- [ ] **Step 2: Recipe** (`docs/AXL-Shared-Driver-Recipe.md`) — document the sibling-only variant + macro + the version-pinning use case, and note the default now prefers the sibling. Follow the doc-sync rule: re-read surrounding prose for staleness.

- [ ] **Step 3: Doc gates** — `make check-docs check-ascii`; `./scripts/build-docs.sh` and confirm 0 warnings (the new docstrings render via the existing `axl-shared-driver.h` doxygenfile directive).

- [ ] **Step 4: Full verification** — `./test/integration/test-axl.sh --arch X64` and `--arch AARCH64` (ratchet); `./test/integration/run-integration.sh --arch X64` (0 failed, includes the new fixture); `./scripts/lint.sh` clean.

- [ ] **Step 5: Commit docs.** `git add CHANGELOG.md docs/AXL-Shared-Driver-Recipe.md && git commit -m "docs: sibling-locate + sibling-first default in recipe and CHANGELOG"`

## Self-Review

**Spec coverage:** §2.1 reorder → T3 S1 (RED T2 reorder scenario). §2.2 locate_sibling → T1 decl/stub, T3 S4, RED T2 hardfail. §2.3 granular rc → T3 S2–S3. §2.4 run_sibling + macro → T1 S2–S3, T3 S5. §3 tests → T2 + T3 S6–S7. §4 docs/gates → T4. ✓

**Placeholder scan:** all code shown except the fixture bodies (T2) and Makefile/runner (cited mirrors: sd-ergo + reap-test tail) — deliberate, the mirrors are the source of truth for recipe macros and the pass/fail-exit tail. The `AXL_OK`/`AXL_NOT_FOUND` numeric values the runner asserts must be read from `include/axl/axl-status.h` (or printed symbolically) during T2, not guessed.

**Type consistency:** `axl_shared_driver_locate_sibling(name, file, out_iface)` and `axl_shared_driver_run_sibling(name, file, argc, argv)` identical across header, impl, macro, probe. `AXL_NOT_FOUND`/`AXL_INVALID`/`AXL_OK`/`AXL_ERR` are the AxlStatus ints; `AXL_EFI_NOT_FOUND` is the EFI exit status (matches `axl_shared_driver_run`).
