# Shared-Driver Stdio Bridge — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a resident shared-driver's `axl_stdin`/`axl_stdout` transparently reflect the *launcher's* shell StdIn/StdOut, so a driver verb's `axl_readline(axl_stdin)` reads piped/`<`-redirected/interactive input — with no per-tool code.

**Architecture:** The launcher and driver are separate PE images with separate copies of the backend's static `mShellStdIn`. A backend-owned **stdio-bridge protocol** (fixed GUID, carries the launcher's `{StdIn,StdOut,StdErr}` shell-file handles) is installed by `axl_shared_driver_locate` (launcher side, per invocation) and consulted *live* by the backend shell-handle getters when the local image has no shell params (driver side). The bridge handle is never cached in the driver and is uninstalled at launcher exit (the launcher's StdIn dies on return — a stale bridge would feed a freed handle to a later direct call).

**Tech Stack:** C (freestanding UEFI), `axl_protocol_install`/`_find_guid`/`_uninstall` (UEFI handle DB), `axl_atexit`, QEMU shared-driver fixture.

**Spec:** `docs/superpowers/specs/2026-06-23-shared-driver-stdio-bridge-design.md`

## Global Constraints

- Coding style: `axl_snake_case` funcs, `AxlPascalCase` types, `AXL_SCREAMING_CASE` macros; 4-space indent, K&R, multi-line signatures. (`docs/AXL-Coding-Style.md`)
- Layering: the bridge GUID/struct + install/uninstall + getter fallback live in the **backend** (`src/backend/native/axl-backend-native.c` + a backend-internal decl). `src/util/axl-shared-driver.c` only *calls* the backend install — it must not own the bridge GUID (util is above the backend).
- **Never cache the bridge handle in the driver getters** — live `axl_protocol_find_guid` per call on the no-local-params path. Local (app/launcher) params stay cached as today.
- **Launcher uninstalls the bridge at exit** via `axl_atexit` (load-bearing: the launcher's StdIn handle is freed when the launcher returns).
- No-regression: a direct (non-launcher) driver call with no bridge → getter returns NULL → `axl_stdin` EOF, exactly as today.
- StdIn is the core deliverable. StdOut/StdErr bridging is **gated** on the Task-3 `>`-redirect observation (driver output may already honor `>` via the shell's `gST->ConOut` swap during the launcher window). Do not build output bridging speculatively.
- UEFI firmware-lifecycle test hazard (`feedback_uefi_firmware_test_hazards`): unit-test only safe positives/negatives (bridge install/find/uninstall round-trip in one image; absent→NULL). The cross-image pipe/`<`/`>` behavior is the QEMU integration fixture. Run a new unit binary in isolation first (`TEST_APPS_ONLY=...`).
- Build both arches; 0 warnings; `make check-ascii`/`check-docs` clean. Exact-string test assertions.

## Exact signatures (verified)

```c
/* protocol DB (axl-driver.h / axl-sys.h) */
int  axl_protocol_install(const AxlGuid *guid, void *iface, AxlHandle *handle);   /* *handle NULL -> fresh */
int  axl_protocol_uninstall(AxlHandle handle, const AxlGuid *guid, void *iface);
int  axl_protocol_find_guid(const AxlGuid *guid, void **interface);               /* AXL_OK + iface, else !AXL_OK */
/* atexit (axl-atexit.h) */
typedef void (*AxlAtexitFn)(void *data);
uint32_t axl_atexit(AxlAtexitFn fn, void *data);
/* backend getters (src/backend/axl-backend.h) */
AxlFileHandle axl_backend_shell_stdin(void);
AxlFileHandle axl_backend_shell_stdout(void);
/* shared-driver locate (axl-shared-driver.h) — actual name is axl_shared_driver_locate */
int axl_shared_driver_locate(const char *name, const char *driver_efi,
                             const void *embed_data, size_t embed_size, void **out_iface);
/* shell params struct (include/uefi/axl-uefi-extra.h): EFI_SHELL_PARAMETERS_PROTOCOL { SHELL_FILE_HANDLE StdIn, StdOut, StdErr; ... } */
/* fixed GUID literal pattern: static const AxlGuid X = AXL_GUID(d1,d2,d3, b0..b7); */
```

Current code: `probe_shell_std_handles()` (axl-backend-native.c ~1105) caches `mShellStdIn`/`mShellStdOut` from `gBS->HandleProtocol(gImageHandle, &gEfiShellParametersProtocolGuid, &sp)`; getters `axl_backend_shell_stdin/stdout` return the cached handles. `console_read` (axl-stream.c) calls `axl_backend_shell_stdin()` per read.

---

### Task 1: Backend helper — read all three shell std handles + stderr getter

**Files:**
- Modify: `src/backend/native/axl-backend-native.c` (refactor `probe_shell_std_handles`; add `mShellStdErr`, `axl_backend_shell_stderr`)
- Modify: `src/backend/axl-backend.h` (declare `axl_backend_shell_stderr`)
- Test: `test/unit/axl-test-platform.c` (a shell-params smoke — this binary runs as a shell app under QEMU, so its own StdIn/StdOut are non-NULL)

**Interfaces:**
- Produces: `AxlFileHandle axl_backend_shell_stderr(void);` and an internal `static void probe_shell_std_handles(void)` that now also caches `mShellStdErr` from `sp->StdErr`.

- [ ] **Step 1: Write the failing test.** Add to `test/unit/axl-test-platform.c` (find its `main`/runner and register the call):

```c
static void
test_shell_std_handles(void)
{
    /* This test binary is launched as a UEFI Shell app, so it HAS
       EFI_SHELL_PARAMETERS_PROTOCOL: its own stdin/stdout/stderr are
       non-NULL and the getters agree across repeated calls (cached). */
    AxlFileHandle in1 = axl_backend_shell_stdin();
    AxlFileHandle out1 = axl_backend_shell_stdout();
    AxlFileHandle err1 = axl_backend_shell_stderr();
    test_check(in1 != NULL,  "shell_stdin: non-NULL under a shell launch");
    test_check(out1 != NULL, "shell_stdout: non-NULL under a shell launch");
    test_check(err1 != NULL, "shell_stderr: non-NULL under a shell launch");
    test_check(axl_backend_shell_stdin() == in1, "shell_stdin: stable (cached)");
    test_check(axl_backend_shell_stderr() == err1, "shell_stderr: stable (cached)");
}
```

(`axl_backend_shell_stderr` is declared in `axl-backend.h` — include is already present in the test via `axl.h`. If the test can't see the backend header, expose the getter through the same path `axl_backend_shell_stdin` is reached, or call it from a tiny wrapper; check how the test currently reaches backend symbols.)

- [ ] **Step 2: Confirm RED.** `make tests && TEST_APPS_ONLY=AxlTestPlatform ./test/integration/test-axl.sh` → FAIL (`axl_backend_shell_stderr` undefined / link error).

- [ ] **Step 3: Implement.** In `axl-backend-native.c`: add `static SHELL_FILE_HANDLE mShellStdErr = NULL;`. In `probe_shell_std_handles`, after `mShellStdOut = sp->StdOut;` add `mShellStdErr = sp->StdErr;`. Add:

```c
AxlFileHandle
axl_backend_shell_stderr(void)
{
    probe_shell_std_handles();
    return (AxlFileHandle)mShellStdErr;
}
```

Declare it in `src/backend/axl-backend.h` next to `axl_backend_shell_stdout`.

- [ ] **Step 4: Confirm GREEN** (both arches). `make tests && TEST_APPS_ONLY=AxlTestPlatform ./test/integration/test-axl.sh` and `... --arch AARCH64`. PASS.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "backend: cache + expose shell StdErr handle (axl_backend_shell_stderr)"`

---

### Task 2: Backend stdio-bridge — protocol + getter fallback

**Files:**
- Create: `src/backend/axl-stdio-bridge.h` (internal: `AxlStdioBridge`, `AXL_STDIO_BRIDGE_GUID` extern, install/uninstall/`_bridge_stdin`/`_bridge_stdout` decls)
- Modify: `src/backend/native/axl-backend-native.c` (define the GUID, the bridge install/uninstall, the live lookup; wire the getter fallback)
- Test: `test/unit/axl-test-platform.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct { AxlFileHandle stdin_h, stdout_h, stderr_h; void *launcher_image; } AxlStdioBridge;
  void axl_backend_stdio_bridge_install(void);     /* capture local shell handles, install/refresh the protocol, arm atexit-uninstall once */
  void axl_backend_stdio_bridge_uninstall(void);   /* uninstall the protocol if installed */
  ```
  and the getter fallback behavior (below).
- Consumes: Task 1's `axl_backend_shell_stderr` + the existing `axl_backend_shell_stdin/stdout`; `axl_protocol_install/find_guid/uninstall`; `axl_atexit`.

- [ ] **Step 1: Write the failing tests.** Add to `axl-test-platform.c`:

```c
static void
test_stdio_bridge_roundtrip(void)
{
    /* Install captures THIS image's shell handles (it's a shell app). */
    axl_backend_stdio_bridge_install();
    void *iface = NULL;
    int rc = axl_protocol_find_guid(&AXL_STDIO_BRIDGE_GUID, &iface);
    test_check(rc == AXL_OK && iface != NULL, "stdio-bridge: found after install");
    AxlStdioBridge *b = iface;
    test_check(b->stdin_h == axl_backend_shell_stdin(),
               "stdio-bridge: stdin_h is the launcher's StdIn");
    test_check(b->stdout_h == axl_backend_shell_stdout(),
               "stdio-bridge: stdout_h is the launcher's StdOut");

    axl_backend_stdio_bridge_uninstall();
    iface = NULL;
    test_check(axl_protocol_find_guid(&AXL_STDIO_BRIDGE_GUID, &iface) != AXL_OK,
               "stdio-bridge: gone after uninstall");
}
```

Register it. (This tests the install/find/uninstall mechanics in one image — a safe positive. The *driver-with-no-local-params uses the bridge* path is the integration fixture in Task 3.)

- [ ] **Step 2: Confirm RED.** `make tests && TEST_APPS_ONLY=AxlTestPlatform ./test/integration/test-axl.sh` → FAIL (symbols undefined).

- [ ] **Step 3: Implement.** Create `src/backend/axl-stdio-bridge.h`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-stdio-bridge.h
    Internal: carries a launcher's shell stdio handles across the
    shared-driver image boundary so a resident driver's axl_stdin/
    axl_stdout reflect the launching app's. NOT a public API. */
#ifndef AXL_STDIO_BRIDGE_H
#define AXL_STDIO_BRIDGE_H
#include <axl/axl-sys.h>   /* AxlGuid, AxlFileHandle via backend? use void* handles */
#include "axl-backend.h"   /* AxlFileHandle */
typedef struct {
    AxlFileHandle stdin_h;
    AxlFileHandle stdout_h;
    AxlFileHandle stderr_h;
    void         *launcher_image;   /* EFI_HANDLE; reserved for a liveness check */
} AxlStdioBridge;
extern const AxlGuid AXL_STDIO_BRIDGE_GUID;
void axl_backend_stdio_bridge_install(void);
void axl_backend_stdio_bridge_uninstall(void);
#endif
```

In `axl-backend-native.c` (include the new header):

```c
/* {a fresh uuidgen v4} — fixed identity of the stdio-bridge protocol. */
const AxlGuid AXL_STDIO_BRIDGE_GUID = AXL_GUID(/* d1,d2,d3, b0..b7 — run uuidgen */);

static AxlStdioBridge mBridge;
static AxlHandle      mBridgeHandle = NULL;   /* install handle, NULL = not installed */
static uint32_t       mBridgeAtexit = 0;

static void
bridge_atexit(void *data)
{
    (void)data;
    axl_backend_stdio_bridge_uninstall();
}

void
axl_backend_stdio_bridge_install(void)
{
    /* Capture THIS image's shell handles. If we have none, nothing to bridge. */
    AxlFileHandle in  = axl_backend_shell_stdin();
    AxlFileHandle out = axl_backend_shell_stdout();
    AxlFileHandle err = axl_backend_shell_stderr();
    if (in == NULL && out == NULL && err == NULL) {
        return;
    }
    /* Refresh: uninstall a stale one first (handles change per invocation). */
    if (mBridgeHandle != NULL) {
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE_GUID, &mBridge);
        mBridgeHandle = NULL;
    }
    mBridge.stdin_h        = in;
    mBridge.stdout_h       = out;
    mBridge.stderr_h       = err;
    mBridge.launcher_image = (void *)gImageHandle;
    if (axl_protocol_install(&AXL_STDIO_BRIDGE_GUID, &mBridge, &mBridgeHandle) != AXL_OK) {
        mBridgeHandle = NULL;
        return;
    }
    if (mBridgeAtexit == 0) {
        mBridgeAtexit = axl_atexit(bridge_atexit, NULL);
    }
}

void
axl_backend_stdio_bridge_uninstall(void)
{
    if (mBridgeHandle != NULL) {
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE_GUID, &mBridge);
        mBridgeHandle = NULL;
    }
}

/* Live (uncached) lookup — only used on the no-local-shell-params (driver) path. */
static AxlFileHandle
bridge_lookup_stdin(void)
{
    void *iface = NULL;
    if (axl_protocol_find_guid(&AXL_STDIO_BRIDGE_GUID, &iface) == AXL_OK && iface != NULL) {
        return ((AxlStdioBridge *)iface)->stdin_h;
    }
    return NULL;
}
/* (bridge_lookup_stdout — symmetric, added in Task 4 only if needed.) */
```

Wire the getter fallback — change `axl_backend_shell_stdin`:

```c
AxlFileHandle
axl_backend_shell_stdin(void)
{
    probe_shell_std_handles();
    if (mShellStdIn != NULL) {
        return (AxlFileHandle)mShellStdIn;   /* app/launcher: own params */
    }
    return bridge_lookup_stdin();            /* driver: live bridge consult */
}
```

(Leave `axl_backend_shell_stdout` unchanged for now — output is Task 4.)

- [ ] **Step 4: Confirm GREEN** (both arches). PASS.

- [ ] **Step 5: Refactor while green + docs gate.** Ensure `make check-ascii` / `make check-docs` clean (internal header needs no doxygenfile — it is not public). Re-run.

- [ ] **Step 6: Commit.** `git add -A && git commit -m "backend: stdio-bridge protocol + driver-side StdIn fallback"`

---

### Task 3: Launcher hook + QEMU fixture (the acceptance test)

**Files:**
- Modify: `src/util/axl-shared-driver.c` (call `axl_backend_stdio_bridge_install()` at the end of the locate core, after the driver is resident + the vtable resolved)
- Create: `test/integration/stdio-bridge-driver.c` + `test/integration/stdio-bridge-launcher.c` (or extend `sdk/examples/shared-driver-demo/` — prefer a dedicated minimal fixture under `test/`)
- Create: `test/integration/test-driver-stdio-qemu.sh`

**Interfaces:**
- Consumes: `axl_backend_stdio_bridge_install` (Task 2), `axl_shared_driver_locate`, the `AXL_DRIVER`/`int main` fixture pattern from `sdk/examples/shared-driver-demo/`.

- [ ] **Step 1: Launcher hook.** In `src/util/axl-shared-driver.c`, locate the core that `axl_shared_driver_locate` (and `_with_load_options`/`_with_image_info`) funnels through (the one that does `axl_protocol_find_guid(&guid, out_iface)` ~line 209). Immediately **after** a successful resolve (the driver is resident and `*out_iface` is set), call `axl_backend_stdio_bridge_install();`. Include `"../backend/axl-stdio-bridge.h"`. This makes every launcher invocation refresh the bridge with its current shell handles — zero consumer code.

- [ ] **Step 2: Write the fixture (driver).** `test/integration/stdio-bridge-driver.c`: an `AXL_DRIVER` whose `DriverEntry` `axl_shared_driver_publish("stdio-bridge-fix", &gVtable, &gHandle)`. Vtable has `int (*run)(int argc, char **argv)`. The verb:
  - `run("echo")` → `char *l = axl_readline(axl_stdin); axl_printf("GOT:%s\n", l ? l : "<EOF>"); axl_free(l);`
  - `run("emit")` → `axl_printf("DRIVEROUT\n");` (for the `>` probe)
  Model the skeleton on `sdk/examples/shared-driver-demo/shared-driver-demo-driver.c`.

- [ ] **Step 3: Write the fixture (launcher).** `test/integration/stdio-bridge-launcher.c`: `int main(int argc, char **argv)` → `void *vt; axl_shared_driver_locate("stdio-bridge-fix", "stdio-bridge-driver.efi", AXL_EMBED_DATA(...), AXL_EMBED_SIZE(...), &vt); return ((FixVtable*)vt)->run(argc-1, argv+1);` Embed the driver via the `--embed`/`.incbin` pattern the other driver tests use (see `test/integration/test-driver.sh` for the embed wiring).

- [ ] **Step 4: Write `test-driver-stdio-qemu.sh`.** Model on `test/integration/test-driver-identity-qemu.sh`. Build the driver + launcher, stage on the ESP, and drive a `startup.nsh`:

```
echo hello | fixture echo          # expect: GOT:hello   (PIPE)
fixture echo < in.txt              # expect: GOT:<first line of in.txt>   (< REDIRECT)
fixture emit > out.txt             # then: type out.txt -> expect DRIVEROUT   (> REDIRECT probe)
fixture echo </dev/null-equivalent # or no input -> expect GOT:<EOF> cleanly (NO-REGRESSION)
```

Assert with exact-string grep (anchored) on the captured serial log: `GOT:hello`, the `<`-line, and — for the `>` case — read `out.txt` back and check whether it contains `DRIVEROUT`. **Record the `>` result**: it decides Task 4. `TEST_SKIP_RATCHET=1`. Skip-and-warn if the harness can't stage a driver on this box (match the other driver-test skip conventions).

- [ ] **Step 5: Run it.** `./test/integration/test-driver-stdio-qemu.sh` → PIPE + `<` PASS (the core deliverable). Note whether `> out.txt` captured `DRIVEROUT` (already-works) or not (needs Task 4). Run on both arches if the harness supports it.

- [ ] **Step 6: Commit.** `git add -A && git commit -m "fw/driver: bridge launcher stdio in axl_shared_driver_locate + QEMU pipe/redirect fixture"`

---

### Task 4: Output-side decision + docs

**Files:**
- (Conditional) Modify: `src/backend/native/axl-backend-native.c` (`axl_backend_shell_stdout` fallback) — ONLY if Task 3 showed `>` not captured
- Modify: `docs/AXL-Shared-Driver-Recipe.md` (note the transparent stdio bridge), `CHANGELOG.md` (`## Unreleased`)

- [ ] **Step 1: Decide from Task 3's `>` result.**
  - **If `fixture emit > out.txt` already captured `DRIVEROUT`** (driver output rides the shell's `gST->ConOut` swap during the launcher window): do NOT add output bridging. Document that output redirect already works; `axl_stdout_raw` already targets `axl_backend_shell_stdout()` so it's bridged for free where it matters.
  - **If NOT captured:** mirror Task 2's StdIn fallback for `axl_backend_shell_stdout` (add `bridge_lookup_stdout`, fall back when `mShellStdOut == NULL`), and extend `test-driver-stdio-qemu.sh` to assert `out.txt` contains `DRIVEROUT`. Confirm GREEN both arches.

- [ ] **Step 2: Docs.** Add a short section to `docs/AXL-Shared-Driver-Recipe.md`: "stdio is bridged automatically — a driver verb's `axl_readline(axl_stdin)` / `axl_print` reflect the launcher's pipes/redirects; no per-tool code." Add a `## Unreleased` CHANGELOG entry: "Shared-driver dispatch now transparently bridges the launcher's StdIn (and StdOut, if implemented) so resident-driver verbs read piped/redirected input."

- [ ] **Step 3: Gates + commit.** `make check-ascii && make check-docs`; full suite both arches green; `git commit -m "docs: shared-driver stdio bridge (recipe + changelog)[ + StdOut fallback]"`.

---

## Final gate

- [ ] Full suite both arches green (`./test/integration/test-axl.sh --arch X64` / `--arch AARCH64`), ratchet up, `test-driver-stdio-qemu.sh` PASS (pipe + `<` at minimum), 0 warnings, docs gates clean.
- [ ] Independent pre-commit review of the whole diff (`feedback_code_review_before_commit`), focused on the freed-launcher-handle lifetime (atexit uninstall + no driver-side caching) and the no-regression direct-call path.

## Self-review notes (author)

- **Spec coverage:** bridge protocol (T2) ✓; launcher install at locate (T3) ✓; driver getter live fallback, no caching (T2) ✓; atexit uninstall (T2) ✓; StdIn core (T2/T3) ✓; StdOut/StdErr gated on `>` test (T3 probe → T4) ✓; no-regression (T3 + T2 NULL path) ✓; fixture pipe/`<`/`>` (T3) ✓; docs (T4) ✓.
- **Layering:** bridge GUID/struct/install live in the backend; util only calls install — no inversion.
- **Test hazard:** unit tests are same-image safe positives + absent→NULL; cross-image behavior is the QEMU fixture; new fixture runs via its own `.sh`.
- **Open item flagged in-task:** the exact `>` behavior is empirically resolved in T3 before any output-bridge code is written (T4) — not assumed.
