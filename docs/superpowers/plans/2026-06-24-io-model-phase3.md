# I/O Model Phase 3 — shared-driver ergonomics (standard vtable + macros + dispatch/run)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).

**Goal:** Collapse the shared-driver launcher/driver boilerplate to app-logic-only: a standard `int run(int,char**)` vtable, `AXL_SHARED_DRIVER` / `AXL_SHARED_DRIVER_LAUNCHER` macros, and `axl_shared_driver_dispatch` / `axl_shared_driver_run` that own resolve + bridge + dispatch + exit-status.

**Architecture:** The SDK defines the cross-image contract (`AxlSharedDriverVtable`) now that stdin + exit-status are SDK-owned (Phases 1-2). Driver/launcher macros compose the existing `AXL_DRIVER` + `axl_shared_driver_publish`/`locate` + Phase-2 `install_stdio_bridge`/`apply_exit_status`. Proven by a fixture built entirely from the macros.

**Tech Stack:** C (freestanding UEFI), AXL macros, QEMU/OVMF + AAVMF harness.

## Global Constraints

- axl-sdk ≥ current `main`; ships in v2.7.0 (MINOR). Builds on Phase 2 (`e1819882`).
- Public API standard C types, `axl_snake_case`/`AxlPascalCase`/`AXL_SCREAMING_CASE`; no EDK2 leak. `docs/AXL-Coding-Style.md`. New public API ⇒ contract-first docstring + test-first.
- Both arches build clean; integration fixture GREEN on X64 + AARCH64.
- Exact-string assertions for output. `make check-ascii` + `make check-docs` clean. Direct commits to main; no downstream-consumer names in code.
- Compose existing primitives — do NOT reimplement resolve/bridge/exit-status. `AXL_DRIVER` (include/axl.h) generates `DriverEntry`; `AXL_EMBED_DECLARE/DATA/SIZE` (axl-embed.h) handle the blob; `axl_shared_driver_locate(name,file,embed,len,&iface)` resolves; `axl_shared_driver_install_stdio_bridge()` + `axl_shared_driver_apply_exit_status()` are the Phase-2 primitives.

## File Structure

- `include/axl/axl-shared-driver.h` — `AxlSharedDriverVtable` type; `axl_shared_driver_dispatch` + `axl_shared_driver_run` decls (contract-first).
- `include/axl.h` — `AXL_SHARED_DRIVER` + `AXL_SHARED_DRIVER_LAUNCHER` (+ `_THIN`) macros (beside `AXL_DRIVER`/`AXL_APP`).
- `src/util/axl-shared-driver.c` — `axl_shared_driver_dispatch` + `axl_shared_driver_run` impl.
- `test/integration/sd-ergo-driver.c`, `sd-ergo-launcher.c` — fixture using ONLY the macros.
- `test/integration/test-sd-ergo-qemu.sh` — proves stdin echo + `%lasterror%` via the turnkey path.
- `Makefile` — build targets.
- `CHANGELOG.md` — `### Added`.

---

### Task 1: Standard vtable + dispatch/run contracts + impl

**Files:**
- Modify: `include/axl/axl-shared-driver.h` (type + 2 decls)
- Modify: `src/util/axl-shared-driver.c` (2 impls)

**Interfaces:**
- Produces: `typedef struct { int (*run)(int argc, char **argv); } AxlSharedDriverVtable;`
- Produces: `int axl_shared_driver_dispatch(const AxlSharedDriverVtable *vt, int argc, char **argv);`
- Produces: `int axl_shared_driver_run(const char *name, const char *driver_filename, const unsigned char *embed_blob, size_t embed_len, int argc, char **argv);`

- [ ] **Step 1: Type + contracts** — in `include/axl/axl-shared-driver.h`, after the includes / before the existing `axl_shared_driver_guid` decl, add the standard vtable type; and after `axl_shared_driver_apply_exit_status`'s decl, add the two new function contracts:

```c
/**
 * @brief The SDK-standard shared-driver entry vtable.
 *
 * A driver built with @c AXL_SHARED_DRIVER publishes this; a launcher
 * built with @c AXL_SHARED_DRIVER_LAUNCHER (or calling
 * @ref axl_shared_driver_run / @ref axl_shared_driver_dispatch) drives it.
 * The single @c run entry has the canonical @c main signature, so once
 * stdin and exit status are bridged by the SDK the cross-image contract
 * is just "call an int(int,char**)". Consumers no longer define a custom
 * vtable or protocol header.
 */
typedef struct {
    int (*run)(int argc, char **argv);   ///< per-dispatch entry (== int main)
} AxlSharedDriverVtable;
```
```c
/**
 * @brief Dispatch into a resident driver with stdio + exit-status bridged.
 *
 * Brackets the cross-image vtable call: installs the stdio bridge (so the
 * driver's @c axl_stdin / stderr reflect THIS launcher), calls
 * @c vt->run(argc, argv), then applies any exit status the driver armed
 * (@ref axl_shared_driver_apply_exit_status) so the launcher exits with it.
 * For launchers that resolve the driver themselves; @ref axl_shared_driver_run
 * calls this after resolving.
 *
 * @return the driver's @c run return code (the launcher should return it
 *     from @c main); AXL_ERR if @p vt / @p vt->run is NULL.
 */
int
axl_shared_driver_dispatch(
    const AxlSharedDriverVtable *vt,    ///< resolved standard vtable
    int                          argc,  ///< forwarded argc
    char                       **argv   ///< forwarded argv
);

/**
 * @brief Resolve a resident shared-driver and dispatch — the whole launcher.
 *
 * Composes @ref axl_shared_driver_locate (resident → on-disk → embedded)
 * and @ref axl_shared_driver_dispatch. This IS a turnkey `int main` body:
 * @c AXL_SHARED_DRIVER_LAUNCHER expands to a call to it. Pass
 * @c embed_blob == NULL / @c embed_len == 0 for a thin (no-embed) launcher.
 *
 * @return the driver's exit code when it dispatched; a launcher-side error
 *     (nonzero; also arms @c EFI_NOT_FOUND via axl_set_exit_status) when the
 *     driver could not be located.
 */
int
axl_shared_driver_run(
    const char           *name,             ///< shared-driver identity
    const char           *driver_filename,  ///< on-disk filename
    const unsigned char  *embed_blob,       ///< embedded driver bytes (NULL → thin)
    size_t                embed_len,        ///< length of @p embed_blob (0 → thin)
    int                   argc,             ///< argc from main
    char                **argv              ///< argv from main
);
```

- [ ] **Step 2: Implement** — in `src/util/axl-shared-driver.c` (after `axl_shared_driver_apply_exit_status`), add:

```c
int
axl_shared_driver_dispatch(
    const AxlSharedDriverVtable *vt,
    int                          argc,
    char                       **argv
    )
{
    if (vt == NULL || vt->run == NULL) {
        return AXL_ERR;
    }
    /* Install the launcher-context bridge (stdin + exit-status channel),
       run the verb in the driver image, then pull any status it armed onto
       this launcher. apply is a no-op (AXL_ERR, ignored) when none armed. */
    axl_shared_driver_install_stdio_bridge();
    int rc = vt->run(argc, argv);
    (void)axl_shared_driver_apply_exit_status();
    return rc;
}

int
axl_shared_driver_run(
    const char           *name,
    const char           *driver_filename,
    const unsigned char  *embed_blob,
    size_t                embed_len,
    int                   argc,
    char                **argv
    )
{
    void *iface = NULL;
    if (axl_shared_driver_locate(name, driver_filename,
                                 embed_blob, embed_len, &iface) != AXL_OK
        || iface == NULL) {
        axl_warning("axl_shared_driver_run: failed to load driver '%s'",
                    driver_filename);
        axl_set_exit_status((AxlEfiStatus)0x800000000000000EULL); /* EFI_NOT_FOUND */
        return 1;
    }
    return axl_shared_driver_dispatch((const AxlSharedDriverVtable *)iface,
                                      argc, argv);
}
```
(Add `#include <axl/axl-signal.h>` if `axl_set_exit_status` isn't already visible in this TU.)

- [ ] **Step 3: Build both arches clean**

Run: `make ARCH=x64 all 2>&1 | grep -iE "error|warning"; make ARCH=aa64 all 2>&1 | grep -iE "error|warning"`
Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add include/axl/axl-shared-driver.h src/util/axl-shared-driver.c
git commit -m "shared-driver: standard vtable + dispatch/run (compose locate+bridge+exit-status)"
```

---

### Task 2: Driver + launcher macros

**Files:**
- Modify: `include/axl.h` (add `AXL_SHARED_DRIVER`, `AXL_SHARED_DRIVER_LAUNCHER`, `AXL_SHARED_DRIVER_LAUNCHER_THIN` beside `AXL_DRIVER`)

**Interfaces:**
- Consumes: `AxlSharedDriverVtable`, `axl_shared_driver_publish`/`_unpublish`/`_run`, `AXL_DRIVER`, `AXL_EMBED_*`.
- Produces: the three macros (used by Task 3's fixture).

- [ ] **Step 1: Add the macros** — in `include/axl.h`, after the `AXL_DRIVER` definition, add:

```c
/**
 * AXL_SHARED_DRIVER(name_str, init_fn, run_fn, unload_fn):
 *   int init_fn(void)           — heavy per-boot setup; 0 = ok (else abort load)
 *   int run_fn(int, char **)    — per-dispatch entry (== int main)
 *   int unload_fn(void)         — teardown; 0 = ok
 *
 * Emits the driver image's DriverEntry/Unload: runs init_fn once, publishes
 * the SDK-standard AxlSharedDriverVtable{.run=run_fn} under @p name_str; the
 * unload path unpublishes then calls unload_fn. The consumer writes only the
 * three functions — no vtable, no publish/unpublish, no AXL_DRIVER.
 */
#define AXL_SHARED_DRIVER(name_str, init_fn, run_fn, unload_fn)              \
  int init_fn(void);                                                        \
  int run_fn(int, char **);                                                 \
  int unload_fn(void);                                                      \
  static AxlSharedDriverVtable _axl_sd_vtable = { run_fn };                 \
  static AxlHandle             _axl_sd_handle = 0;                          \
  static int _axl_sd_entry(AxlHandle _h, AxlSystemTable *_st) {             \
    (void)_h; (void)_st;                                                    \
    int _rc = init_fn();                                                    \
    if (_rc != 0) { return _rc; }                                          \
    return axl_shared_driver_publish((name_str), &_axl_sd_vtable,          \
                                     &_axl_sd_handle);                       \
  }                                                                         \
  static int _axl_sd_unload(AxlHandle _h) {                                 \
    (void)_h;                                                               \
    if (_axl_sd_handle != 0) {                                              \
      axl_shared_driver_unpublish((name_str), _axl_sd_handle,             \
                                  &_axl_sd_vtable);                          \
    }                                                                       \
    return unload_fn();                                                     \
  }                                                                         \
  AXL_DRIVER(_axl_sd_entry, _axl_sd_unload)

/**
 * AXL_SHARED_DRIVER_LAUNCHER(name_str, driver_filename, embed_symbol):
 * The entire launcher `int main` — resolves the resident driver (resident →
 * on-disk → embedded @p embed_symbol) and dispatches with stdio + exit-status
 * bridged. @p embed_symbol is an AXL_EMBED name (the driver's .efi bytes
 * linked in via the build's embed step).
 */
#define AXL_SHARED_DRIVER_LAUNCHER(name_str, driver_filename, embed_symbol) \
  AXL_EMBED_DECLARE(embed_symbol);                                          \
  int main(int argc, char **argv) {                                         \
    return axl_shared_driver_run((name_str), (driver_filename),            \
                                 AXL_EMBED_DATA(embed_symbol),             \
                                 AXL_EMBED_SIZE(embed_symbol),             \
                                 argc, argv);                               \
  }

/**
 * AXL_SHARED_DRIVER_LAUNCHER_THIN(name_str, driver_filename):
 * Like AXL_SHARED_DRIVER_LAUNCHER but NO embedded blob — loads the driver
 * from disk only (resident → on-disk). Smallest per-command transfer.
 */
#define AXL_SHARED_DRIVER_LAUNCHER_THIN(name_str, driver_filename)          \
  int main(int argc, char **argv) {                                         \
    return axl_shared_driver_run((name_str), (driver_filename),            \
                                 (const unsigned char *)0, 0, argc, argv);  \
  }
```
(Ensure `<axl/axl-shared-driver.h>` and `<axl/axl-embed.h>` are included by `<axl.h>` before these macros — check and add if missing.)

- [ ] **Step 2: Sanity-build the umbrella** — the macros are header-only; confirm they parse by building the existing suite: `make ARCH=x64 all 2>&1 | grep -iE "error|warning"`. Expected: clean (no consumer uses them yet; this just confirms no syntax error in `<axl.h>`).

- [ ] **Step 3: Commit**

```bash
git add include/axl.h
git commit -m "axl: AXL_SHARED_DRIVER + AXL_SHARED_DRIVER_LAUNCHER macros (turnkey shared-driver)"
```

---

### Task 3: Macro-built fixture (end-to-end proof)

**Files:**
- Create: `test/integration/sd-ergo-driver.c` (driver via `AXL_SHARED_DRIVER`)
- Create: `test/integration/sd-ergo-launcher.c` (launcher via `AXL_SHARED_DRIVER_LAUNCHER`)
- Create: `test/integration/test-sd-ergo-qemu.sh`
- Modify: `Makefile` (targets; embed the driver into the launcher)

**Interfaces:** Consumes all of Tasks 1-2.

- [ ] **Step 1: Driver** — `test/integration/sd-ergo-driver.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* sd-ergo-driver.c — resident driver built ENTIRELY from AXL_SHARED_DRIVER.
 * Proves the turnkey ergonomics: app-logic-only run() reads stdin, writes
 * stdout, and arms an exit status; the SDK owns publish/bridge/exit-status. */
#include <axl.h>

static int ergo_init(void)   { return 0; }   /* no heavy setup for the test */
static int ergo_unload(void) { return 0; }

static int
ergo_run(int argc, char **argv)
{
    if (argc >= 1 && argv[0] != NULL && axl_strcmp(argv[0], "echotext") == 0) {
        AxlStream *t = axl_stdin_text();
        char *l = (t != NULL) ? axl_readline(t) : NULL;
        if (l != NULL) {
            size_t n = axl_strlen(l);
            while (n > 0 && (l[n-1] == '\n' || l[n-1] == '\r')) { l[--n] = '\0'; }
        }
        axl_printf("ERGO:%s\n", l != NULL ? l : "<EOF>");
        axl_free(l);
        if (t != NULL) { axl_fclose(t); }
        return 0;
    }
    if (argc >= 1 && argv[0] != NULL && axl_strcmp(argv[0], "status") == 0) {
        axl_set_exit_status((AxlEfiStatus)0x12345678);   /* bit-63 clear: observable via %lasterror% */
        axl_printf("ERGOSTAT\n");
        return 0;
    }
    axl_printf("ERGO:<BADVERB>\n");
    return 1;
}

AXL_SHARED_DRIVER("axl/sd-ergo", ergo_init, ergo_run, ergo_unload)
```

- [ ] **Step 2: Launcher** — `test/integration/sd-ergo-launcher.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* sd-ergo-launcher.c — the ENTIRE launcher: one macro. */
#include <axl.h>
AXL_SHARED_DRIVER_LAUNCHER("axl/sd-ergo", "sd-ergo-driver.efi", sd_ergo_driver)
```

- [ ] **Step 3: Makefile targets** — mirror the `stdio-bridge-fix` pattern (driver → `LINK_EFI_DRIVER`; `EMBED_BLOB(sd_ergo_driver, ...driver.efi)`; launcher app links the blob). Add `sd-ergo` to `.PHONY`:

```make
sd-ergo: $(PREFIX)/sd-ergo-launcher.efi $(PREFIX)/sd-ergo-driver.efi
	@echo "  Built: sd-ergo-launcher.efi + sd-ergo-driver.efi"

$(PREFIX)/sd-ergo-driver.efi: $(BUILDDIR)/sd-ergo-driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/sd-ergo-driver.o,$@)
$(BUILDDIR)/sd-ergo-driver.o: test/integration/sd-ergo-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(eval $(call EMBED_BLOB,sd_ergo_driver,$(PREFIX)/sd-ergo-driver.efi))

$(PREFIX)/sd-ergo-launcher.efi: $(BUILDDIR)/sd-ergo-launcher.o $(BLOB_OBJ_sd_ergo_driver) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/sd-ergo-launcher.o $(BLOB_OBJ_sd_ergo_driver),$@)
$(BUILDDIR)/sd-ergo-launcher.o: test/integration/sd-ergo-launcher.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
```

- [ ] **Step 4: Fixture** — `test/integration/test-sd-ergo-qemu.sh` (model on `test-driver-stdio-qemu.sh`): build `sd-ergo`, stage `sd-ergo-launcher.efi`, startup:

```bash
  echo "echo ERGO_BEGIN"
  echo "echo hello | sd-ergo-launcher.efi echotext"
  echo "sd-ergo-launcher.efi status"
  echo "echo ESTAT=%lasterror%"
  echo "echo ERGO_DONE"
```
Assert (exact-string): `^ERGO:hello$` present AND `ESTAT=0x12345678` present in the ERGO_BEGIN..ERGO_DONE window. GREEN gate requires both; SKIP-guard if the launcher didn't build. `TEST_SKIP_RATCHET=1`, `test-meta: arch=both`.

- [ ] **Step 5: Build + GREEN both arches**

Run: `make ARCH=x64 sd-ergo 2>&1 | tail -2; timeout 120 ./test/integration/test-sd-ergo-qemu.sh --arch X64 2>&1 | grep -E "ERGO|ESTAT|test:"` → `ERGO:hello`, `ESTAT=0x12345678`, `OK`. Then `--arch AARCH64` (`make ARCH=aa64 sd-ergo` first).

- [ ] **Step 6: RED check (retro)** — confirm the fixture actually depends on the new code: temporarily point the launcher's driver name at a bogus string, rerun → locate fails → non-`OK`; revert. (Documents the test isn't a tautology.) Optional but recommended; note the result in the report.

- [ ] **Step 7: Commit**

```bash
git add test/integration/sd-ergo-driver.c test/integration/sd-ergo-launcher.c test/integration/test-sd-ergo-qemu.sh Makefile
git commit -m "test: end-to-end fixture built entirely from AXL_SHARED_DRIVER + _LAUNCHER"
```

---

### Task 4: docs + CHANGELOG + Minor follow-ups

**Files:**
- Modify: `src/backend/native/axl-backend-native.c` (Phase-2 Minor M1 comment)
- Modify: `include/axl/axl-shared-driver.h` (Phase-2 Minor M2: `apply` docstring note)
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Phase-2 Minor M1** — in `src/backend/native/axl-backend-native.c`, at the exit-status reflect block in `axl_backend_set_exit_status`, add a one-line comment that the "resident driver" gate assumes SYNCHRONOUS dispatch (the shared-driver pattern has no event loop), so `bridge_find_live()` returns the dispatching launcher.

- [ ] **Step 2: Phase-2 Minor M2** — in `include/axl/axl-shared-driver.h`, add a sentence to `axl_shared_driver_apply_exit_status`'s docstring: it does NOT clear a previously-armed launcher status; a REPL launcher that wants per-dispatch semantics should `axl_clear_exit_status()` (if available) between dispatches, or rely on the AXL_ERR return to know nothing was applied this round.

- [ ] **Step 3: CHANGELOG** — under `## Unreleased` `### Added`, append:

```markdown
- **Turnkey shared-driver ergonomics** — `AxlSharedDriverVtable` (standard
  `int run(int,char**)` entry), `AXL_SHARED_DRIVER(name,init,run,unload)` and
  `AXL_SHARED_DRIVER_LAUNCHER(name,file,embed)` macros, and
  `axl_shared_driver_dispatch` / `axl_shared_driver_run`. A shared-driver
  launcher collapses to one macro and the driver to three functions; the SDK
  owns resolve + stdio bridge + exit-status. See `docs/AXL-Shared-Driver-Recipe.md`.
```

- [ ] **Step 4: Gates + commit**

Run: `make check-ascii 2>&1 | tail -1; make check-docs 2>&1 | tail -1` (clean).
```bash
git add src/backend/native/axl-backend-native.c include/axl/axl-shared-driver.h CHANGELOG.md
git commit -m "docs: turnkey ergonomics CHANGELOG + Phase-2 review minors (M1/M2)"
```

---

## Self-Review

- **Spec coverage (design §5.2, §5.3):** standard vtable + dispatch/run (Task 1), macros (Task 2), end-to-end macro-built proof (Task 3), docs + the two Phase-2 review Minors (Task 4). The full recipe rewrite + the polished example are Phase 4.
- **Placeholders:** none; Task 2 flags "confirm `<axl.h>` includes shared-driver.h + embed.h before the macros" (grounding). Task 3 Makefile mirrors the existing `stdio-bridge-fix` embed pattern (`EMBED_BLOB`/`LINK_EFI_DRIVER`/`LINK_EFI_APP`) — confirm those exact macro names in the current Makefile before writing.
- **Type consistency:** `AxlSharedDriverVtable` (Task 1) is consumed by `dispatch`/`run` (Task 1) and the macros (Task 2); `_axl_sd_vtable`/`_axl_sd_handle`/`_axl_sd_entry`/`_axl_sd_unload` are macro-internal names (one shared driver per image, so fixed names are safe). `run`/`dispatch`/`run` return the driver's rc; the launcher returns it from `main`; CRT0 applies the armed status.
- **Risk — macro hygiene:** `AXL_SHARED_DRIVER` forward-declares the three consumer fns (so order-independent) and defines file-scope statics; a second `AXL_SHARED_DRIVER` in one TU would collide (by design — one shared driver per driver image). `AXL_SHARED_DRIVER_LAUNCHER` defines `int main` (one per launcher image). Document both as one-per-image.
- **Risk — double bridge install:** `run` → `locate` (auto-installs the bridge) → `dispatch` (installs again). The second install is a cheap idempotent refresh (Phase-1 behavior), harmless. Noted so a reviewer doesn't flag it as a bug.
</content>
