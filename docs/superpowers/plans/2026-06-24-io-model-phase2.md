# I/O Model Phase 2 — cross-image exit-status reflection

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** A resident shared-driver verb that calls `axl_set_exit_status(N)` makes the *launcher* exit with `N` — with zero consumer plumbing beyond one `apply` call — plus finish the driver-side raw-stdout bridge fallback for symmetry.

**Architecture:** Extend the internal `AxlStdioBridge` with an inline pending-exit-status cell. Driver-side `axl_backend_set_exit_status` reflects into the launcher's bridge cell (gated on "no local shell params" + a live bridge, reusing `bridge_find_live()` from Phase 1). A public `axl_shared_driver_apply_exit_status()` (launcher-side) drains the cell into the launcher's own pending status so CRT0 returns it verbatim. Proven by an integration fixture that checks `%lasterror%`.

**Tech Stack:** C (freestanding UEFI), AXL backend, QEMU/OVMF + AAVMF harness.

## Global Constraints

- axl-sdk ≥ current `main`; ships in v2.7.0 (MINOR). Builds on Phase 1 (`1b10d334`).
- Public API (`include/axl/*`) standard C types, `axl_snake_case`, UTF-8, no EDK2 leak. `docs/AXL-Coding-Style.md`.
- Both arches build clean; integration fixtures GREEN on X64 + AARCH64.
- Exact-string assertions for output tests. `make check-ascii` + `make check-docs` clean.
- Direct commits to main; no downstream-consumer names in code. New public API ⇒ contract-first docstring + test-first.
- Reuse Phase 1/v2.6.x machinery: `bridge_find_live()` (liveness-checked, self-healing), the `probe_shell_std_handles()` / `mShellStdIn`-is-NULL "resident driver" discriminator. Do NOT reintroduce the v2.6.1 UAF (never cache a bridge handle; consult live).

## File Structure

- `src/backend/axl-stdio-bridge.h` — add `pending_status` + `has_pending` to `AxlStdioBridge`; declare `axl_backend_bridge_take_exit_status`.
- `src/backend/native/axl-backend-native.c` — reset cell in `axl_backend_stdio_bridge_install`; reflect in `axl_backend_set_exit_status`; implement `axl_backend_bridge_take_exit_status`; add `bridge_lookup_stdout` + fallback in `axl_backend_shell_stdout`.
- `include/axl/axl-shared-driver.h` — declare `axl_shared_driver_apply_exit_status` (contract-first).
- `src/util/axl-shared-driver.c` — implement it (thin wrapper over the backend take).
- `test/integration/stdio-bridge-driver.c`, `stdio-bridge-fix.h`, `stdio-bridge-self.c` — add an `exitstatus` verb + apply call.
- `test/integration/test-driver-stdio-qemu.sh` — assert `%lasterror%`.
- `CHANGELOG.md` — `### Added` entry.

---

### Task 1: Contract + RED exit-status fixture

**Files:**
- Modify: `include/axl/axl-shared-driver.h` (declare `axl_shared_driver_apply_exit_status` with docstring)
- Modify: `test/integration/stdio-bridge-fix.h` (verb name constant if needed), `test/integration/stdio-bridge-driver.c` (add `exitstatus` verb), `test/integration/stdio-bridge-self.c` (call apply after dispatch)
- Modify: `test/integration/test-driver-stdio-qemu.sh` (assert `%lasterror%`)

**Interfaces:**
- Produces: `int axl_shared_driver_apply_exit_status(void)` — launcher-side; after dispatching into a resident driver, applies any exact exit status the driver armed (via `axl_set_exit_status`) to THIS launcher so CRT0 returns it. Returns `AXL_OK` if a status was applied, `AXL_ERR`/no-op semantics per docstring when none pending. (Full contract in the header docstring this task writes.)

- [ ] **Step 1: Write the contract** — in `include/axl/axl-shared-driver.h`, after `axl_shared_driver_install_stdio_bridge`'s declaration, add:

```c
/**
 * @brief Apply a resident driver's armed exit status to THIS launcher.
 *
 * Call from the LAUNCHER, immediately after dispatching into the resident
 * driver (i.e. after the driver's vtable call returns). If a driver verb
 * armed an exact status via @c axl_set_exit_status(), that status was
 * reflected across the stdio bridge into this launcher's pending-status
 * cell; this drains it into the launcher's own @c axl_set_exit_status so
 * the launcher's CRT0 returns it verbatim to the shell (`%lasterror%`).
 *
 * A no-op when the driver armed nothing this dispatch (the launcher then
 * exits by its own @c main return code, per the normal convention). Only
 * needed by launchers that dispatch into a resident driver and did NOT go
 * through a future @c axl_shared_driver_dispatch wrapper.
 *
 * @return AXL_OK if a reflected status was applied; AXL_ERR if none was
 *     pending (nothing to apply — not an error condition, just a signal).
 */
int
axl_shared_driver_apply_exit_status(void);
```

- [ ] **Step 2: Add an `exitstatus` verb to the fixture driver** — in `test/integration/stdio-bridge-driver.c`'s `fix_run`, add a branch (place beside the existing `emit` verb):

```c
    if (axl_strcmp(argv[0], "exitstatus") == 0) {
        /* Arm an exact, non-collapsible status; the launcher must exit
           with THIS value, proving cross-image reflection. 0x8000000000000042
           = an EFI error-class code (top bit set) distinct from SUCCESS. */
        axl_set_exit_status((AxlEfiStatus)0x8000000000000042ULL);
        axl_printf("EXITSET\n");
        return 0;
    }
```
(Ensure `#include <axl/axl-signal.h>` is present for `axl_set_exit_status`.)

- [ ] **Step 3: Make the self-locate launcher apply** — in `test/integration/stdio-bridge-self.c`, after the `vt->run(...)` dispatch and before returning, apply the reflected status. Change the tail so it becomes:

```c
    int rc = ((FixVtable *)vt)->run(argc - 1, argv + 1);
    axl_shared_driver_apply_exit_status();   /* pull the driver's armed status onto this launcher */
    return rc;
```

- [ ] **Step 4: Assert `%lasterror%`** — in `test/integration/test-driver-stdio-qemu.sh`, add to the startup script (after the SELF section):

```bash
  echo "echo ESTAT_BEGIN"
  echo "stdio-bridge-self.efi exitstatus"
  echo "echo ESTAT=%lasterror%"
  echo "echo ESTAT_DONE"
```
and add an assertion (the shell prints `%lasterror%` as `0x8000000000000042` when reflection works; `0x0` / `Success` when it doesn't):

```bash
estat=$(sed -n '/ESTAT_BEGIN/,/ESTAT_DONE/p' "$TEST_CLEAN_LOG" | grep -c 'ESTAT=0x8000000000000042' || true)
```
Add `&& "$estat" -ge 1` to the GREEN gate, and echo it in the Results line.

- [ ] **Step 5: Build + confirm RED**

Run: `make ARCH=x64 all && make ARCH=x64 stdio-bridge-fix stdio-bridge-self 2>&1 | tail -2; timeout 120 ./test/integration/test-driver-stdio-qemu.sh --arch X64 2>&1 | grep -E "ESTAT|Results:|test:"`
Expected: FAIL — `axl_shared_driver_apply_exit_status` doesn't exist yet (link error) OR, if you stub it to return AXL_ERR first to get a link, `%lasterror%` is `0x0`, not `0x8000000000000042` (the driver's status stayed in the driver image). Confirm the fixture FAILS the `estat` assertion.

- [ ] **Step 6: Commit**

```bash
git add include/axl/axl-shared-driver.h test/integration/stdio-bridge-driver.c test/integration/stdio-bridge-self.c test/integration/stdio-bridge-fix.h test/integration/test-driver-stdio-qemu.sh
git commit -m "test: RED cross-image exit-status fixture (%lasterror% not reflected yet)"
```

---

### Task 2: Bridge cell + reflect + apply (GREEN)

**Files:**
- Modify: `src/backend/axl-stdio-bridge.h`
- Modify: `src/backend/native/axl-backend-native.c`
- Modify: `src/util/axl-shared-driver.c`

**Interfaces:**
- Consumes: `bridge_find_live()`, `probe_shell_std_handles()`/`mShellStdIn`, `axl_backend_set_exit_status` (existing).
- Produces: `bool axl_backend_bridge_take_exit_status(uint64_t *out)` — if the launcher's bridge cell has a pending reflected status, writes it to `*out`, clears the cell, returns true; else false.

- [ ] **Step 1: Extend the bridge struct** — in `src/backend/axl-stdio-bridge.h`, add to `AxlStdioBridge` (after `launcher_image`) and declare the take fn:

```c
    uint64_t       pending_status;   /* driver-armed exit status reflected here */
    bool           has_pending;      /* true when a driver armed a status this dispatch */
```
```c
/* Launcher-side: drain a driver-reflected exit status from the local bridge
   cell. Returns true + writes *out (and clears) when one is pending. */
bool axl_backend_bridge_take_exit_status(uint64_t *out);
```
(Ensure `<stdint.h>`/`<stdbool.h>` are included in the header.)

- [ ] **Step 2: Reset the cell on install** — in `axl_backend_stdio_bridge_install` (native), where it (re)initializes the `mBridge` fields per dispatch, add `mBridge.has_pending = false;` (and `mBridge.pending_status = 0;`). This makes each launcher invocation start with no pending status.

- [ ] **Step 3: Reflect from the driver** — in `axl_backend_set_exit_status` (native, currently just sets `g_exit_status`/`g_exit_status_armed`), append the reflection:

```c
void
axl_backend_set_exit_status(uint64_t status)
{
    g_exit_status       = (EFI_STATUS)status;
    g_exit_status_armed = true;

    /* If THIS image is a resident driver (no shell params of its own) serving
       a launcher dispatch, reflect the status into the launcher's bridge cell
       so the launcher's apply/CRT0 returns it. A normal app/launcher (has
       shell params) skips this and uses its own g_exit_status. */
    probe_shell_std_handles();
    if (mShellStdIn == NULL) {
        AxlStdioBridge *b = bridge_find_live();
        if (b != NULL) {
            b->pending_status = status;
            b->has_pending    = true;
        }
    }
}
```

- [ ] **Step 4: Implement the take** — in native, add:

```c
bool
axl_backend_bridge_take_exit_status(uint64_t *out)
{
    /* Read THIS (launcher) image's own bridge cell — the driver wrote it
       through the installed protocol interface (== &mBridge here). */
    if (mBridgeHandle == NULL || !mBridge.has_pending) {
        return false;
    }
    if (out != NULL) {
        *out = mBridge.pending_status;
    }
    mBridge.has_pending = false;
    return true;
}
```
(Confirm `mBridge` / `mBridgeHandle` are the existing statics; adapt names to the current code.)

- [ ] **Step 5: Public wrapper** — in `src/util/axl-shared-driver.c`, implement:

```c
int
axl_shared_driver_apply_exit_status(void)
{
    uint64_t status = 0;
    if (!axl_backend_bridge_take_exit_status(&status)) {
        return AXL_ERR;   /* nothing pending — caller keeps its own rc */
    }
    axl_backend_set_exit_status(status);   /* arm on THIS (launcher) image */
    return AXL_OK;
}
```
(Declare `axl_backend_bridge_take_exit_status` via the backend header already included, or add the include.)

- [ ] **Step 6: Build both arches + GREEN both arches**

Run: `make ARCH=x64 all && make ARCH=aa64 all 2>&1 | grep -iE "error|warning"` (clean), then
`timeout 120 ./test/integration/test-driver-stdio-qemu.sh --arch X64 2>&1 | grep -E "ESTAT|test:"` and `--arch AARCH64` → `%lasterror%` = `0x8000000000000042`, `test: OK`.

- [ ] **Step 7: Commit**

```bash
git add src/backend/axl-stdio-bridge.h src/backend/native/axl-backend-native.c src/util/axl-shared-driver.c
git commit -m "bridge: reflect driver-armed exit status to the launcher (+public apply)"
```

---

### Task 3: raw-stdout driver bridge fallback (symmetry)

**Files:**
- Modify: `src/backend/native/axl-backend-native.c`

- [ ] **Step 1: Add the fallback** — mirror the Phase-1 stderr fallback. Add `bridge_lookup_stdout` (returns the live bridge's `stdout_h`) beside `bridge_lookup_stderr`, and change `axl_backend_shell_stdout` to fall back to it when the local `mShellStdOut` is NULL:

```c
static AxlFileHandle
bridge_lookup_stdout(void)
{
    AxlStdioBridge *b = bridge_find_live();
    return (b != NULL) ? b->stdout_h : NULL;
}
```
and in `axl_backend_shell_stdout`: `if (mShellStdOut != NULL) return (AxlFileHandle)mShellStdOut; return bridge_lookup_stdout();`

- [ ] **Step 2: Build both arches clean**

Run: `make ARCH=x64 all 2>&1 | grep -iE "error|warning"; make ARCH=aa64 all 2>&1 | grep -iE "error|warning"`
Expected: clean.

- [ ] **Step 3: No-regression run of the stdio fixture (both arches)**

Run: `timeout 120 ./test/integration/test-driver-stdio-qemu.sh --arch X64 2>&1 | grep -E "test:"; timeout 200 ./test/integration/test-driver-stdio-qemu.sh --arch AARCH64 2>&1 | grep -E "test:"`
Expected: `Driver stdio-bridge test: OK` both.

- [ ] **Step 4: Commit**

```bash
git add src/backend/native/axl-backend-native.c
git commit -m "backend: driver bridge fallback for raw stdout (symmetry with stderr)"
```

---

### Task 4: docs + CHANGELOG

**Files:**
- Modify: `docs/AXL-Shared-Driver-Recipe.md` (note the apply step for self-locating consumers)
- Modify: `CHANGELOG.md` (Unreleased `### Added`)

- [ ] **Step 1: Recipe note** — in `docs/AXL-Shared-Driver-Recipe.md`, in the stdio section, add that a self-locating launcher which wants a driver verb's exact exit status must call `axl_shared_driver_apply_exit_status()` after dispatch (and that a future `axl_shared_driver_dispatch` wrapper will fold this in). Keep it short; the full ergonomics land in Phase 3.

- [ ] **Step 2: CHANGELOG** — under `## Unreleased` `### Added`, append:

```markdown
- **`axl_shared_driver_apply_exit_status()` in `<axl/axl-shared-driver.h>`** — a
  resident driver verb's `axl_set_exit_status(N)` is reflected across the stdio
  bridge; the launcher calls this after dispatch to exit with `N` verbatim
  (`%lasterror%`). Also: raw stdout now works in a resident driver via the
  stdio bridge (sibling of the raw-stderr fallback).
```

- [ ] **Step 3: Gates**

Run: `make check-ascii 2>&1 | tail -1; make check-docs 2>&1 | tail -1`
Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add docs/AXL-Shared-Driver-Recipe.md CHANGELOG.md
git commit -m "docs: exit-status reflection + apply; CHANGELOG"
```

---

## Self-Review

- **Spec coverage (design §5.1, §5.4):** exit-status cell + driver reflect + public apply (Tasks 1-2), raw-stdout fallback completing the "handles need the bridge" set (Task 3), docs (Task 4). The `axl_shared_driver_dispatch`/`run` wrappers + macros are Phase 3 (this phase's apply is the primitive they compose).
- **Placeholders:** none; Task 2 flags "adapt `mBridge`/`mBridgeHandle`/install-site names to current code" — a grounding instruction, since the bridge internals evolved in v2.6.x.
- **Type consistency:** `axl_backend_bridge_take_exit_status(uint64_t*)` (declared/used Tasks 1-2); `axl_shared_driver_apply_exit_status(void)->int` (declared Task 1, impl Task 2); reflect uses existing `bridge_find_live()` + `mShellStdIn` discriminator.
- **Risk — reflection gating:** the "no shell params ⇒ resident driver" test must exactly match the stdin path; a launcher that happens to lack shell params would reflect into a bridge nobody drains (harmless: apply just won't find it, CRT0 uses rc). The integration fixture is the real proof. Do NOT cache the bridge pointer across the reflect (consult `bridge_find_live()` each call) — same UAF discipline as Phase 1.
</content>
