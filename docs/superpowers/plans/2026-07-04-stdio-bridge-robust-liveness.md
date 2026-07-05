# Fully-Robust Stdio-Bridge Liveness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the stdio-bridge's freed-memory `LoadedImage`-proto liveness heuristic with a per-dispatch monotonic token owned by driver-resident memory, closing the handle-reuse false-alive.

**Architecture:** A resident driver installs one persistent `AxlDispatchToken` cell (`{uint64_t current;}`) behind a fixed-GUID internal protocol at `axl_shared_driver_publish`. Each dispatch the launcher stamps a fresh `GetNextMonotonicCount()` token into both its bridge and that cell (before the reap-at-install). The driver's liveness gate becomes `bridge->token == cell->current`. `token` replaces `launcher_image_proto` at the same 8-byte offset, keeping `AxlStdioBridge` layout-identical to v2.7.0.

**Tech Stack:** C (UEFI backend), AXL internal backend headers, QEMU integration fixtures (Makefile + `test-*-qemu.sh`), gcc + ld, both `x64` and `aa64`.

**Spec:** `docs/superpowers/specs/2026-07-04-stdio-bridge-robust-liveness-design.md` (read §4.1–§4.7 before Task 2).

## Global Constraints

- **No public API change.** All edits under `src/backend/` + `src/util/axl-shared-driver.c` (the publish wrapper) + `test/integration/`. `include/axl/*` public headers untouched.
- **Tests use public headers only** (`feedback_test_public_headers`): the fixture mirrors internal GUIDs/structs locally, exactly as `test/integration/stdio-bridge-reap-test.c` mirrors `AXL_STDIO_BRIDGE_GUID`. Never `#include "src/backend/axl-stdio-bridge.h"` from a test.
- **Never regress the v2.6.1 UAF:** never cache a bridge pointer; consult live; keep the enumerate + reap structure; deref `stdin_h` only after a live match.
- **Exact-string / exact-value assertions** for observable output (`feedback_tdd_mandatory`).
- **Both arches, 0 warnings:** `make ARCH=x64` and `make ARCH=aa64` must build clean; fix warnings before moving on.
- **New GUID (fixed identity):** `AXL_DISPATCH_TOKEN_GUID` = `02dd6813-d275-4734-98f8-c7f60331958d`. Backend form: `AXL_GUID(0x02dd6813, 0xd275, 0x4734, 0x98, 0xf8, 0xc7, 0xf6, 0x03, 0x31, 0x95, 0x8d)`. Test-mirror form (`EFI_GUID`): `{0x02dd6813, 0xd275, 0x4734, {0x98, 0xf8, 0xc7, 0xf6, 0x03, 0x31, 0x95, 0x8d}}`.
- **Token guard:** `GetNextMonotonicCount` result of 0 is remapped to non-zero (`t | 1`); 0 is the reserved "no active dispatch" sentinel.
- **Direct commits to `main`** (`feedback_direct_commits_solo`); push freely (`feedback_push_freely_axl_sdk`) but **no release without explicit user tag approval** (`feedback_release_approval_gate`).

## File Structure

- `test/integration/stdio-bridge-liveness-test.c` — **new** RED fixture (self-contained, public headers + local mirrors).
- `test/integration/test-stdio-bridge-liveness-qemu.sh` — **new** QEMU runner for the fixture.
- `Makefile` — **modify**: add `stdio-bridge-liveness-test` target (mirror the existing `stdio-bridge-reap-test` target) + add it to the `.PHONY` list.
- `src/backend/axl-stdio-bridge.h` — **modify**: `AxlStdioBridge` field swap; add `AxlDispatchToken`, `AXL_DISPATCH_TOKEN_GUID`, `axl_backend_dispatch_token_ensure()`.
- `src/backend/native/axl-backend-native.c` — **modify**: GUID def + cell statics + `axl_backend_dispatch_token_ensure`; token-stamp + reorder in `axl_backend_stdio_bridge_install`; rewrite `bridge_launcher_alive` + its two call sites; clear-current in `axl_backend_stdio_bridge_reap`; comment updates.
- `src/util/axl-shared-driver.c` — **modify**: call `axl_backend_dispatch_token_ensure()` from `axl_shared_driver_publish`.

---

### Task 1: RED — the reap-path discrimination fixture

Builds a bridge that **fools the current proto-match** (this image is genuinely alive; the recorded-proto slot holds this image's real `LoadedImage*`) but must be rejected by the token gate. Drives the reap via `axl_shared_driver_unload` and asserts the bridge handle loses the protocol. Fails against current code (proto-match keeps it), passes after Task 2.

**Files:**
- Create: `test/integration/stdio-bridge-liveness-test.c`
- Create: `test/integration/test-stdio-bridge-liveness-qemu.sh`
- Modify: `Makefile` (new target + `.PHONY`)

**Interfaces:**
- Consumes (public API, already exists): `axl_shared_driver_unload(const char *name)`, `axl_printf`, `gBS->{AllocatePool,LocateHandleBuffer,HandleProtocol,InstallProtocolInterface,GetNextMonotonicCount,FreePool}`, `gImageHandle`, `gEfiLoadedImageProtocolGuid`.
- Produces: `out/native-<arch>/stdio-bridge-liveness-test.efi`, printing a `PASS:`/`FAIL:` line and a `... N passed, M failed` footer; exit 0 iff all pass.

- [ ] **Step 1: Write the fixture**

Create `test/integration/stdio-bridge-liveness-test.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-liveness-test.c — the handle-reuse false-alive regression.
 *
 * Installs one stdio-bridge instance that FOOLS the pre-v2.7.1 liveness
 * heuristic: launcher_image = this (alive) image, and the 8-byte slot that
 * the old code reads as launcher_image_proto holds this image's real
 * EFI_LOADED_IMAGE_PROTOCOL* — so the proto-match reports it "alive". The
 * post-v2.7.1 gate reads that same slot as `token` and compares it against a
 * driver-resident AxlDispatchToken cell whose `current` we set to a distinct
 * monotonic value, so the gate reports it dead. We then trigger the reap path
 * (axl_shared_driver_unload clears current=0 and reaps) and assert the bridge
 * handle no longer carries the protocol.
 *
 *   RED  (pre-fix): proto-match keeps the bridge -> HandleProtocol succeeds.
 *   GREEN (post-fix): token gate drops it -> HandleProtocol fails.
 *
 * Public headers only: the internal AxlStdioBridge / AxlDispatchToken layouts
 * and GUIDs are mirrored locally (as stdio-bridge-reap-test.c mirrors the
 * bridge GUID). Keep these in lockstep with src/backend/axl-stdio-bridge.h.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

/* Mirrors AXL_STDIO_BRIDGE_GUID (c8f517d7-…) in the backend. */
static const EFI_GUID STDIO_BRIDGE_GUID = {
    0xc8f517d7, 0x36cc, 0x458d,
    {0x98, 0xd6, 0xb1, 0x16, 0x82, 0x5e, 0x30, 0xbf}
};

/* Mirrors AXL_DISPATCH_TOKEN_GUID (02dd6813-…) in the backend. */
static const EFI_GUID DISPATCH_TOKEN_GUID = {
    0x02dd6813, 0xd275, 0x4734,
    {0x98, 0xf8, 0xc7, 0xf6, 0x03, 0x31, 0x95, 0x8d}
};

/* Mirrors AxlStdioBridge (src/backend/axl-stdio-bridge.h). The 5th field is
   `token` post-fix and was `launcher_image_proto` (void*) pre-fix — same
   8-byte slot, so this one struct drives both code versions. */
typedef struct {
    void     *stdin_h;
    void     *stdout_h;
    void     *stderr_h;
    void     *launcher_image;
    uint64_t  token;            /* aka launcher_image_proto slot */
    uint64_t  pending_status;
    bool      has_pending;
} MirrorBridge;

/* Mirrors AxlDispatchToken. */
typedef struct {
    uint64_t  current;
} MirrorToken;

static int g_pass = 0;
static int g_fail = 0;

static void
check(bool ok, const char *msg)
{
    axl_printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok) { g_pass++; } else { g_fail++; }
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;
    axl_printf("stdio-bridge-liveness-test: start\n");

    /* This image's real LoadedImage* — the value that makes the pre-fix
       proto-match accept our bridge as alive. */
    void     *li  = NULL;
    EFI_GUID  lig = gEfiLoadedImageProtocolGuid;
    if (EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)gImageHandle, &lig, &li))
        || li == NULL) {
        check(false, "resolve this image's LoadedImage protocol");
        axl_printf("stdio-bridge-liveness-test: %d passed, %d failed\n",
                   g_pass, g_fail);
        return 1;
    }

    /* Build a bridge that fools the proto-match: alive image + slot == li. */
    static MirrorBridge b;
    b.stdin_h        = NULL;
    b.stdout_h       = NULL;
    b.stderr_h       = NULL;
    b.launcher_image = (void *)gImageHandle;   /* genuinely alive */
    b.token          = (uint64_t)(uintptr_t)li; /* == launcher_image_proto slot */
    b.pending_status = 0;
    b.has_pending    = false;

    EFI_HANDLE bridge_handle = NULL;
    if (EFI_ERROR(gBS->InstallProtocolInterface(
            &bridge_handle, (EFI_GUID *)&STDIO_BRIDGE_GUID,
            EFI_NATIVE_INTERFACE, &b))) {
        check(false, "install the decoy bridge instance");
        axl_printf("stdio-bridge-liveness-test: %d passed, %d failed\n",
                   g_pass, g_fail);
        return 1;
    }

    /* Install (if absent) an AxlDispatchToken cell and set current to a fresh
       monotonic value — small, guaranteed != the pointer-valued token slot. */
    static MirrorToken cell;
    cell.current = 0;
    EFI_HANDLE cell_handle = NULL;
    (void)gBS->InstallProtocolInterface(
        &cell_handle, (EFI_GUID *)&DISPATCH_TOKEN_GUID,
        EFI_NATIVE_INTERFACE, &cell);
    /* If a resident driver already installed the real cell, write through it so
       the backend reads what we set; otherwise our just-installed cell is the
       one LocateProtocol finds. Locate the live one and stamp it. */
    MirrorToken *live_cell = NULL;
    if (!EFI_ERROR(gBS->LocateProtocol(
            (EFI_GUID *)&DISPATCH_TOKEN_GUID, NULL, (void **)&live_cell))
        && live_cell != NULL) {
        uint64_t t = 0;
        gBS->GetNextMonotonicCount(&t);
        if (t == 0) { t = 1; }
        live_cell->current = t;                 /* != b.token (a pointer) */
    }

    /* Trigger the reap path. The name need not be resident: unload reaps
       before the not-found early return. */
    (void)axl_shared_driver_unload("stdio-bridge-liveness-probe");

    /* Post-fix: token gate + current-clear reaped our decoy. Pre-fix:
       proto-match kept it alive. */
    void *iface = NULL;
    EFI_STATUS st = gBS->HandleProtocol(
        bridge_handle, (EFI_GUID *)&STDIO_BRIDGE_GUID, &iface);
    check(EFI_ERROR(st),
          "reap dropped the proto-match-fooling bridge (token gate)");

    axl_printf("stdio-bridge-liveness-test: %d passed, %d failed\n",
               g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add the Makefile target**

In `Makefile`, immediately after the `stdio-bridge-reap-test` block (the `$(BUILDDIR)/stdio-bridge-reap-test.o:` rule, ~line 1362), add:

```make
# Build stdio-bridge-liveness-test.efi — regression for the handle-reuse
# false-alive: a bridge that fools the old LoadedImage-proto match must be
# rejected by the per-dispatch token gate. Self-contained (no leaker helper).
stdio-bridge-liveness-test: $(PREFIX)/stdio-bridge-liveness-test.efi
	@echo "  Built: $(PREFIX)/stdio-bridge-liveness-test.efi"

$(PREFIX)/stdio-bridge-liveness-test.efi: $(BUILDDIR)/stdio-bridge-liveness-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/stdio-bridge-liveness-test.o,$@)

$(BUILDDIR)/stdio-bridge-liveness-test.o: test/integration/stdio-bridge-liveness-test.c | $(BUILDDIR)
	$(call COMPILE_APP,$<,$@)
```

Then add `stdio-bridge-liveness-test` to the `.PHONY:` list on ~line 606 (append it after `stdio-bridge-reap-test`).

> Note: mirror the *exact* recipe macros used by the neighboring `stdio-bridge-reap-test` rules. If that block uses a different compile macro than `COMPILE_APP` (e.g. an inline `$(CC) ...`), copy that form verbatim instead — read lines 1350–1364 first and match them.

- [ ] **Step 3: Add the QEMU runner**

Create `test/integration/test-stdio-bridge-liveness-qemu.sh` (mirror `test-stdio-bridge-reap-qemu.sh`):

```bash
#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# Handle-reuse false-alive regression: a stdio-bridge instance that fools the
# pre-v2.7.1 LoadedImage-proto liveness match (alive image + matching proto)
# must be rejected by the per-dispatch token gate and reaped. The fixture
# installs such a decoy, sets a distinct AxlDispatchToken current, triggers the
# reap via axl_shared_driver_unload, and asserts the bridge handle lost the
# protocol. RED before the fix (proto-match keeps it); GREEN after.
#
# Usage: ./test/integration/test-stdio-bridge-liveness-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    stdio-bridge-liveness-test 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$NATIVE_DIR/stdio-bridge-liveness-test.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "stdio-bridge-liveness-test.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Stdio-bridge Liveness Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log
```

Then `chmod +x test/integration/test-stdio-bridge-liveness-qemu.sh`.

- [ ] **Step 4: Build the fixture (x64)**

Run: `make -C /home/mgosha/projects/aximcode/axl-sdk stdio-bridge-liveness-test 2>&1 | tail -5`
Expected: `Built: .../stdio-bridge-liveness-test.efi`, 0 warnings. If it fails to compile, fix the fixture (e.g. `EFI_NATIVE_INTERFACE`/`InstallProtocolInterface` signatures) before proceeding — verify against `include/uefi/generated/`.

- [ ] **Step 5: Run to verify it FAILS (RED) against current code**

Run: `TEST_APPS_ONLY=stdio-bridge-liveness-test ./test/integration/test-stdio-bridge-liveness-qemu.sh --arch X64 2>&1 | grep -E "PASS:|FAIL:|passed,"`
Expected: `FAIL: reap dropped the proto-match-fooling bridge (token gate)` and `... 0 passed, 1 failed`.
This confirms the test exercises the false-alive path: current code's proto-match keeps the decoy bridge alive, so the reap does not drop it.

> If it unexpectedly PASSES against current code, STOP — the test isn't pinning the gap. Likely causes: current code has no `AxlDispatchToken`, so `axl_shared_driver_unload`'s reap uses the proto-match; confirm the decoy's `launcher_image == gImageHandle` (alive) and slot `== li` so the proto-match reports alive. Do not proceed until RED is genuine.

- [ ] **Step 6: Commit the RED test**

```bash
cd /home/mgosha/projects/aximcode/axl-sdk
git add test/integration/stdio-bridge-liveness-test.c \
        test/integration/test-stdio-bridge-liveness-qemu.sh Makefile
git commit -m "test: RED for stdio-bridge handle-reuse false-alive (token gate)"
```

---

### Task 2: GREEN — implement the per-dispatch token gate

Atomic backend change: the `AxlStdioBridge` field swap forces the gate rewrite, so header + backend + publish wiring land together. Ends with the Task 1 test GREEN and all existing stdio-bridge fixtures green.

**Files:**
- Modify: `src/backend/axl-stdio-bridge.h`
- Modify: `src/backend/native/axl-backend-native.c`
- Modify: `src/util/axl-shared-driver.c`

**Interfaces:**
- Produces (internal, consumed by the backend + the publish wrapper):
  - `typedef struct { uint64_t current; } AxlDispatchToken;`
  - `extern const AxlGuid AXL_DISPATCH_TOKEN_GUID;`
  - `int axl_backend_dispatch_token_ensure(void);` — install-if-absent; returns `AXL_OK` when the cell exists afterward (or was already present), `AXL_ERR` on alloc/install failure.
  - `AxlStdioBridge.token` (`uint64_t`) replacing `launcher_image_proto` at the same offset.
- Consumes: `axl_protocol_find_guid(const AxlGuid *, void **)`, `axl_protocol_install(const AxlGuid *, void *, AxlHandle *)`, `gBS->{AllocatePool,GetNextMonotonicCount}`.

- [ ] **Step 1: Edit the header — struct field swap + new decls**

In `src/backend/axl-stdio-bridge.h`, replace the `launcher_image_proto` field in `AxlStdioBridge` with:

```c
    uint64_t       token;            /* per-dispatch monotonic token (was
                                        launcher_image_proto; same 8-byte
                                        offset). The driver's liveness gate:
                                        a bridge is live iff this equals the
                                        driver-resident AxlDispatchToken.current
                                        the launcher stamped this dispatch. */
```

Update the `launcher_image` comment to drop the "liveness gate" claim (it's now debug/identity only). After the `AxlStdioBridge` typedef + the `AXL_STDIO_BRIDGE_GUID` extern, add:

```c
/* Driver-resident per-dispatch liveness reference. A resident driver installs
   exactly one of these (install-if-absent) at publish time on a dedicated
   persistent handle; each dispatch the launcher stamps the fresh token into
   both its bridge and this cell. `current == 0` means "no active dispatch". */
typedef struct {
    uint64_t  current;
} AxlDispatchToken;

/* uuid 02dd6813-d275-4734-98f8-c7f60331958d — fixed identity of the
   dispatch-token protocol. */
extern const AxlGuid AXL_DISPATCH_TOKEN_GUID;

/* Install the dispatch-token cell if none exists yet (idempotent, cross-image).
   Called by a resident driver at publish. AXL_OK when the cell exists
   afterward. */
int axl_backend_dispatch_token_ensure(void);
```

- [ ] **Step 2: Backend — GUID, statics, ensure()**

In `src/backend/native/axl-backend-native.c`, after the `AXL_STDIO_BRIDGE_GUID` definition (~line 1185), add:

```c
const AxlGuid AXL_DISPATCH_TOKEN_GUID =
    AXL_GUID(0x02dd6813, 0xd275, 0x4734,
             0x98, 0xf8, 0xc7, 0xf6, 0x03, 0x31, 0x95, 0x8d);

/* The one dispatch-token cell + its dedicated persistent handle. Allocated in
   pool (image-independent) and never uninstalled — infra that must outlive the
   images that use it, like the fixed bridge GUID identity. */
static AxlDispatchToken *mDispatchCell   = NULL;
static AxlHandle         mDispatchHandle = NULL;
```

Then add the ensure function (near the bridge functions):

```c
int
axl_backend_dispatch_token_ensure(void)
{
    /* Already resident (this image or another) — reuse it. */
    void *found = NULL;
    if (axl_protocol_find_guid(&AXL_DISPATCH_TOKEN_GUID, &found) == AXL_OK
        && found != NULL) {
        mDispatchCell = (AxlDispatchToken *)found;
        return AXL_OK;
    }
    /* Create the singleton cell in pool memory on a fresh handle. */
    void *mem = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiBootServicesData,
                                    sizeof(AxlDispatchToken), &mem))
        || mem == NULL) {
        return AXL_ERR;
    }
    ((AxlDispatchToken *)mem)->current = 0;
    mDispatchHandle = NULL;   /* NULL => allocate a fresh handle */
    if (axl_protocol_install(&AXL_DISPATCH_TOKEN_GUID, mem, &mDispatchHandle)
        != AXL_OK) {
        gBS->FreePool(mem);
        return AXL_ERR;
    }
    mDispatchCell = (AxlDispatchToken *)mem;
    return AXL_OK;
}

/* Locate the live dispatch-token cell (any image's), or NULL if none. */
static AxlDispatchToken *
dispatch_cell(void)
{
    void *found = NULL;
    if (axl_protocol_find_guid(&AXL_DISPATCH_TOKEN_GUID, &found) == AXL_OK) {
        return (AxlDispatchToken *)found;
    }
    return NULL;
}
```

> Confirm `EfiBootServicesData` and `axl_protocol_find_guid`/`axl_protocol_install` are already declared/visible in this file (grep first). `axl_protocol_*` come from `<axl/axl-driver.h>`/`<axl/axl-sys.h>` — add the include if missing (the shared-driver layer already uses them, so the symbols exist in libaxl).

- [ ] **Step 3: Backend — rewrite the liveness gate + forward decl + call sites**

Replace the forward decl (~line 1202):

```c
static bool bridge_launcher_alive(const AxlStdioBridge *b);
```

Replace the `bridge_launcher_alive` definition (~lines 1337-1352) and its preceding comment with:

```c
/* A bridge is live iff its per-dispatch token equals the driver-resident
   AxlDispatchToken.current the launcher stamped this dispatch. The reference
   lives in driver memory (not the freed, recyclable bridge), so this is robust
   against the correlated pool recycling that defeated the old LoadedImage-proto
   match. Reading b->token is a value compare on mapped memory — stdin_h and the
   status cell are only touched AFTER a live match, preserving the v2.6.1 UAF
   fix. current==0 (no active dispatch) or no cell => nothing is live. */
static bool
bridge_launcher_alive(
    const AxlStdioBridge  *b
    )
{
    if (b == NULL) {
        return false;
    }
    AxlDispatchToken *cell = dispatch_cell();
    if (cell == NULL || cell->current == 0) {
        return false;
    }
    return b->token == cell->current;
}
```

Fix the two call sites to pass the bridge pointer:
- In `bridge_reap_dead` (~line 1226): `if (!bridge_launcher_alive(b)) {`
- In `bridge_find_live` (~line 1383): `if (bridge_launcher_alive(b)) {`

- [ ] **Step 4: Backend — stamp + reorder in install; drop proto recording**

In `axl_backend_stdio_bridge_install` (~line 1242): the current body sets fields then calls `bridge_reap_dead()` early (~line 1256) and records `launcher_image_proto` (~lines 1266-1280). Replace the reap-then-record shape with: **stamp current before the reap, delete the proto block, set `token`.**

Concretely, change the region so the order is:

```c
    /* Fresh per-dispatch token: firmware-global monotonic, unique across
       images (a per-image counter would collide). 0 is the "no dispatch"
       sentinel, so remap it. */
    uint64_t t = 0;
    gBS->GetNextMonotonicCount(&t);
    if (t == 0) {
        t = 1;
    }
    /* Publish the token to the driver-resident cell BEFORE reaping, so the
       reap-at-install below uses the NEW token as its liveness reference and
       correctly sweeps prior leaked bridges (all bearing older tokens). No
       resident driver => no cell => reap drops every not-yet-installed
       instance, which is fine (nothing consults a bridge without a driver). */
    {
        AxlDispatchToken *cell = dispatch_cell();
        if (cell != NULL) {
            cell->current = t;
        }
    }
    /* Sweep bridges leaked by prior launchers before publishing ours. */
    bridge_reap_dead();
    /* Refresh: uninstall a stale one first (handles change per invocation). */
    if (mBridgeHandle != NULL) {
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE_GUID, &mBridge);
        mBridgeHandle = NULL;
    }
    mBridge.stdin_h        = in;
    mBridge.stdout_h       = out;
    mBridge.stderr_h       = err;
    mBridge.launcher_image = (void *)gImageHandle;
    mBridge.token          = t;
    mBridge.pending_status = 0;
    mBridge.has_pending    = false;
```

Delete the old `bridge_reap_dead()` call that was near the top (it's now moved below the stamp) and the entire `launcher_image_proto` recording block (`void *li … mBridge.launcher_image_proto = …`). Keep the `axl_protocol_install(&AXL_STDIO_BRIDGE_GUID, &mBridge, &mBridgeHandle)` + atexit arming that follow. Refresh the comment above the install to describe the token, not the proto.

> Read the current function top-to-bottom first (lines 1242-1297) and reconcile: there must be exactly ONE `bridge_reap_dead()` call, now positioned AFTER `cell->current = t`.

- [ ] **Step 5: Backend — standalone reap clears current**

In `axl_backend_stdio_bridge_reap` (~line 1236), clear the dispatch marker before reaping so a last leaked bridge (whose token still equals a frozen `current`) is also dropped:

```c
void
axl_backend_stdio_bridge_reap(void)
{
    /* No active dispatch on this teardown path (do -u / unload): clear the
       marker so EVERY installed bridge (all tokens != 0) is dead and reaped,
       including a final leaked instance whose token still matches a stale
       current. */
    AxlDispatchToken *cell = dispatch_cell();
    if (cell != NULL) {
        cell->current = 0;
    }
    bridge_reap_dead();
}
```

- [ ] **Step 6: Wire ensure() into publish**

In `src/util/axl-shared-driver.c`, in `axl_shared_driver_publish`, after the successful `axl_protocol_install` (~line 88-93), add the cell creation (best-effort; a failure must not fail publish):

```c
    /* Ensure the driver-resident dispatch-token cell exists so launchers can
       stamp per-dispatch liveness tokens the bridge gate reads. Best-effort:
       stdio bridging degrades to EOF fallback if this fails, never fatal. */
    (void)axl_backend_dispatch_token_ensure();
```

`axl-shared-driver.c` already includes `../backend/axl-stdio-bridge.h`, so the declaration is visible.

- [ ] **Step 7: Build both arches, 0 warnings**

Run: `make -C /home/mgosha/projects/aximcode/axl-sdk ARCH=x64 2>&1 | tail -5 && make -C /home/mgosha/projects/aximcode/axl-sdk ARCH=aa64 2>&1 | tail -5`
Expected: both link `libaxl.a` with 0 warnings. Fix any warning before continuing (`feedback_small_batches_verify_clean`).

- [ ] **Step 8: Run the Task-1 test → GREEN (both arches)**

Run: `TEST_APPS_ONLY=stdio-bridge-liveness-test ./test/integration/test-stdio-bridge-liveness-qemu.sh --arch X64 2>&1 | grep -E "PASS:|FAIL:|passed,"`
Expected: `PASS: reap dropped the proto-match-fooling bridge (token gate)` and `... 1 passed, 0 failed`.
Then repeat `--arch AARCH64`; expected identical PASS.

- [ ] **Step 9: Run the existing stdio-bridge fixtures → stay GREEN (both arches)**

Run each, both arches, and confirm each prints its Results footer with 0 failures:
```
./test/integration/test-stdio-bridge-reap-qemu.sh --arch X64
./test/integration/test-driver-stdio-qemu.sh --arch X64
./test/integration/test-io-redirect-qemu.sh --arch X64
./test/integration/test-sd-ergo-qemu.sh --arch X64
```
Repeat all four with `--arch AARCH64`. All must pass. `test-stdio-bridge-reap-qemu.sh` is the critical one for the §4.7 ordering (`after_unload == 0`) — if it regresses, re-check Step 4 (stamp-before-reap) and Step 5 (clear-current).

- [ ] **Step 10: Refactor while green**

Re-read the edited region of `axl-backend-native.c`. Clean up: confirm exactly one `bridge_reap_dead()` call in install, no dead `launcher_image_proto` references remain (`grep -n launcher_image_proto src/backend`), comments describe the token gate accurately (no "narrows/does not eliminate" wording left; you may state the correlated-recycling case is now closed). Re-run Step 8 after any change.

- [ ] **Step 11: Doc-sync check**

Run: `make -C /home/mgosha/projects/aximcode/axl-sdk check-docs check-ascii 2>&1 | tail -5`
Expected: pass. There is no `src/backend/README.md` bridge prose to update (verified), so header comments are the only doc surface — confirm they're accurate (Step 1 + Step 3 wording).

- [ ] **Step 12: Commit**

```bash
cd /home/mgosha/projects/aximcode/axl-sdk
git add src/backend/axl-stdio-bridge.h \
        src/backend/native/axl-backend-native.c \
        src/util/axl-shared-driver.c
git commit -m "backend: per-dispatch token gate for stdio-bridge liveness

Replace the freed-bridge-stored LoadedImage proto-match with a monotonic token
owned by a driver-resident AxlDispatchToken cell (install-if-absent at publish).
token replaces launcher_image_proto in place (layout-preserving). install stamps
current before reaping; unload reap clears current=0. Closes the handle-reuse
false-alive; preserves the v2.6.1 UAF fix."
```

---

### Task 3: Integration verification + review (no release)

**Files:** none (gate task).

- [ ] **Step 1: Full unit suite, both arches**

Run: `./test/integration/test-axl.sh --arch X64` then `--arch AARCH64`.
Expected: ratchet passes, 0 failures both arches.

- [ ] **Step 2: Full integration suite**

Run: `./scripts/run-integration.sh -j` (local parallel runner).
Expected: 0 failures; confirm the four stdio-bridge fixtures + the new liveness fixture are in the run.

- [ ] **Step 3: Lint (clang-tidy-21 == CI pin)**

Run: `./scripts/lint.sh 2>&1 | tail -20`
Expected: clean. Fix any finding in the edited files.

- [ ] **Step 4: Independent code review**

Per `feedback_code_review_before_commit`, run an integration-pass review of the diff (`git diff main` or the two feature commits) focused on: UAF safety (no cached bridge pointer; deref order), the reap ordering (§4.7), the layout-preserving field swap (offsets), and the cell lifetime (never-uninstalled singleton). Apply fixes, re-run Task 2 Step 8-9.

- [ ] **Step 5: Update memory + report to user**

Update `project_io_model_v270_2026-07-04` (mark the "fully-robust bridge liveness" follow-up DONE, unshipped) and report status. **Do NOT cut v2.7.1** — releasing is a separate, user-approved step per `docs/RELEASING.md` + `feedback_release_approval_gate`. Present the option and wait.

## Self-Review

**Spec coverage:**
- §4.1 cell (dedicated persistent handle, install-if-absent, never uninstalled) → Task 2 Steps 2, 6. ✓
- §4.2 layout-preserving field swap → Task 2 Step 1. ✓
- §4.3 stamp order → Task 2 Step 4. ✓
- §4.4 token gate → Task 2 Step 3. ✓
- §4.5 deletions → Task 2 Steps 1, 3, 4, 10. ✓
- §4.6 preserved (find_live/reap structure, exit-status path inherits) → Task 2 Step 3 (call sites), unchanged exit-status path. ✓
- §4.7 reap ordering (install stamp-before-reap; unload clears current; cell-absent reduction) → Task 2 Steps 4, 5; validated by Task 2 Step 9 reap test. ✓
- §7 test #1 (reap-path RED) → Task 1. §7 test #2 (existing guards green) → Task 2 Step 9. ✓
- §8 gates → Task 3. ✓

**Placeholder scan:** no TBD/TODO; all code shown; GUID/values exact. Two "read the neighbor first" notes (Makefile recipe macro, install function reconciliation) are deliberate — the surrounding code is the source of truth for the recipe macro name and current line numbers, which can drift.

**Type consistency:** `AxlDispatchToken.current` (uint64_t), `AxlStdioBridge.token` (uint64_t), `axl_backend_dispatch_token_ensure()` / `dispatch_cell()` used consistently across header + backend + fixture mirror. Fixture `MirrorBridge`/`MirrorToken` field order matches the header struct. GUID bytes identical in backend (`AXL_GUID`) and fixture (`EFI_GUID`) forms.
