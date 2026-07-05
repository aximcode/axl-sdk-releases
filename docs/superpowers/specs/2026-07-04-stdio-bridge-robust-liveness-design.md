# Design: fully-robust stdio-bridge liveness (v2.7.1)

**Date:** 2026-07-04
**Status:** approved (design), pending implementation plan
**Scope:** a PATCH-level robustness fix, all in `src/backend/`. Closes the
handle-reuse false-alive residual left open after v2.7.0. Requires user tag
approval before publishing (per `docs/RELEASING.md`).

Supersedes the "future work" note in the v2.7.0 `bridge_launcher_alive` comment
and the handoff `docs/handoffs/2026-07-04-stdio-bridge-robust-liveness-handoff.md`
(§6 leading direction — this spec is the validated, consumer-grounded refinement).

## 1. Problem

The shared-driver **stdio bridge** lets a resident driver read the *launcher's*
stdin/stderr and receive an exit status across the PE-image boundary. The driver
decides whether an installed bridge instance is safe to consult via
`bridge_launcher_alive()`.

That predicate has a **handle-reuse false-alive gap**. When a launcher exits
*without* uninstalling its bridge (`gBS->Exit()` / `--minimal-runtime` skip the
CRT0 atexit uninstall), its bridge stays installed with dangling handles. v2.7.0
narrowed the gap by also matching the recorded `EFI_LOADED_IMAGE_PROTOCOL*`
pointer, but that pointer **lives in the freed, recyclable bridge memory**, and
the DXE pool allocator recycles the launcher's image-handle slot and its
loaded-image-data slot on the *same* pool event — correlated, not independent —
so a relaunched same-binary launcher can spuriously match. Consequence:
**wrong stdin/stderr/exit-status *data*, never a crash** (the v2.6.1 UAF is
already fixed). Low severity, but real.

The root cause is structural: **no field stored in the (freed, recyclable)
bridge is a reliable liveness signal.** A robust gate needs a reference value in
memory that is *not* freed when a launcher exits — i.e. resident-driver memory.

## 2. Goal / non-goals

**Goal.** A liveness gate that is correct-by-construction — it accepts exactly
the bridge belonging to the dispatch currently in progress and rejects every
stale one, even under correlated pool recycling.

**Non-goals.**
- No public API change. Entirely `src/backend/`-internal.
- No new event-loop / async assumptions. The shared-driver RPC stays synchronous.
- Not trying to support forcing handle-reuse in QEMU (not reproducible; see §8).

## 3. Consumer ground truth (why the design is shaped this way)

Verified against the flagship consumer `~/work/dell/delldiags/source/src/axl-utils/`
(`do.efi` launcher + `doDriver.efi` resident driver; see memory
`reference_axl_utils_consumer`):

- **Exactly one resident shared-driver** per boot (`"dell-diags/do"`). No
  multi-driver topology → a **single global** liveness cell is unambiguous;
  a per-driver cell would be speculative complexity for a case that doesn't exist.
- **Launcher uses the escape-hatch install with its own custom vtable**
  (`do.c` calls `axl_shared_driver_install_stdio_bridge()` then
  `vt->dispatch_argv()`). The bridge machinery therefore **must stay
  driver-agnostic** — it cannot key off the driver's vtable/identity. A global
  cell fits; a driver-keyed cell fights this.
- **Dispatch is a direct synchronous in-process call** (`vt->dispatch_argv()` /
  `vt->run()`), so the launcher is on the stack — *provably alive* — for the
  entire consult. This is what makes a per-dispatch token correct.
- **Version skew is already governed**: consumer docs mandate "rebuild both
  binaries together; clear a stale older resident driver with `do -u`," and the
  tree pins the SDK via `.axl-sdk-version` + `.axl-sdk-checksums` (both halves
  link the same `libaxl.a`). Adding a field to `AxlStdioBridge` rides this
  existing rule; it is not a new hazard class.

## 4. Design

An **active per-dispatch liveness token** owned by resident-driver memory,
stamped by the launcher on every dispatch.

### 4.1 The driver-resident token cell

A new internal fixed-GUID protocol:

```c
/* src/backend/axl-stdio-bridge.h (internal) */
typedef struct {
    uint64_t current;   /* monotonic token of the dispatch in progress; 0 = none */
} AxlDispatchToken;

/* uuid 02dd6813-d275-4734-98f8-c7f60331958d */
extern const AxlGuid AXL_DISPATCH_TOKEN_GUID;
```

- The cell is a **pool allocation** (`gBS->AllocatePool`, BootServicesData),
  installed on a **dedicated persistent handle** (fresh handle via
  `axl_protocol_install(..., &new_handle)`), **not** on any image handle.
- Created **install-if-absent** by a resident driver at
  `axl_shared_driver_publish()` (driver init): `LocateProtocol` first; only
  allocate + install if none exists. Idempotent and race-free (single-threaded
  boot services).
- **Never uninstalled** — a single tiny deliberate infra allocation, image- and
  driver-independent, so it survives any launcher exit and any individual driver
  unload. A later driver load reuses the existing instance (install-if-absent).
  Rationale mirrors the fixed `AXL_STDIO_BRIDGE_GUID` identity: infra that must
  outlive the images that use it.

The install lives in the backend, called from `axl_shared_driver_publish`
(a new backend hook, e.g. `axl_backend_dispatch_token_ensure()`), so the
`src/util/axl-shared-driver.c` layer stays a thin wrapper.

### 4.2 Bridge struct change (layout-preserving)

`token` (`uint64_t`, 8 bytes) **replaces `launcher_image_proto` (`void*`, 8
bytes) at the same offset**, so `AxlStdioBridge` stays **layout-identical** to
v2.7.0 (same size, all other field offsets unchanged):

```c
typedef struct {
    AxlFileHandle  stdin_h;
    AxlFileHandle  stdout_h;
    AxlFileHandle  stderr_h;
    void          *launcher_image;   /* kept: debug/identity only, no longer the gate */
    uint64_t       token;            /* was launcher_image_proto; this dispatch's monotonic token */
    uint64_t       pending_status;
    bool           has_pending;
} AxlStdioBridge;
```

Layout stability is deliberate: it makes the one real in-practice skew window —
a **stale older resident driver** from an earlier `do` in the same boot (the
consumer docs call this out, cleared by `do -u`) — degrade to *safe silence*
rather than a field misread (see §6). The `launcher_image_proto` recording in
install is deleted (§4.5); `stdin_h`/`stdout_h`/`stderr_h` remain the first three
fields, so their offsets never move regardless of this change.

### 4.3 Stamp (launcher side, `axl_backend_stdio_bridge_install`)

Runs synchronously immediately before the launcher dispatches into the driver
(via `axl_shared_driver_dispatch`'s `install_stdio_bridge()`, or the escape-hatch
`axl_shared_driver_install_stdio_bridge()` the axl-utils launcher calls directly).

Added steps, in this **order** (the ordering matters — see §4.7):
1. `t = GetNextMonotonicCount()` — firmware-global, monotonic, unique across
   images. **Required** over a launcher-local counter (per-image counters start
   at 0 → collide across images → not robust). Guard: 0 is the reserved "no
   dispatch" sentinel, so if the counter's first value of the boot is 0,
   **re-call** to consume it and take the next value (`while (t == 0)
   GetNextMonotonicCount(&t)`). Re-calling — not fabricating `t = 1` or `t |= 1`
   — is what preserves uniqueness: it advances the shared counter so no later
   dispatch can be handed the same value. (Fabricating a value would let a real
   future raw `1` collide with an earlier synthesized `1`, resurfacing the
   leak-accumulation the reap is meant to prevent.)
2. `LocateProtocol(AXL_DISPATCH_TOKEN_GUID)` → if present, `cell->current = t`
   **before** the existing reap-at-install runs, so that reap uses the *new*
   token as its liveness reference and correctly sweeps prior leaked bridges
   (which all bear older tokens). If **absent** (no resident driver, or a
   pre-v2.7.1 driver), skip — safe: reap then finds no cell and drops every
   not-yet-installed instance, and the driver later falls back to EOF stdin.
3. Existing reap-at-install (`bridge_reap_dead()`), then set `mBridge.token = t`
   and the handle fields, then install `mBridge` — unchanged except `token`
   replaces the deleted `launcher_image_proto` recording.

The current install already calls `bridge_reap_dead()` first; the only change is
to stamp `cell->current = t` *before* that call (step 2 precedes step 3's reap).

### 4.4 Gate (driver side, `bridge_launcher_alive`)

Replace the `HandleProtocol` + proto-match body with a token match against the
driver-resident cell:

```c
static bool
bridge_launcher_alive(const AxlStdioBridge *b)
{
    if (b == NULL) return false;
    AxlDispatchToken *cell = NULL;   /* LocateProtocol(AXL_DISPATCH_TOKEN_GUID) */
    if (cell == NULL || cell->current == 0) return false;
    return b->token == cell->current;
}
```

- Only the current dispatch's bridge holds `cell->current`; a stale bridge holds
  an old/poison token that cannot equal the just-generated monotonic value unless
  it *is* the current bridge. Robust against correlated recycling because the
  reference (`cell->current`) is driver-resident, not bridge-stored.
- Reading a stale bridge's `token` field is a **value compare on mapped
  (recycled) memory** — never a deref of `stdin_h`. `stdin_h`/`stderr_h`/
  `pending_status` are only touched *after* the token matches, i.e. on the
  live bridge. This preserves the v2.6.1 UAF fix.

Signature note: the predicate now needs only the bridge pointer (drops the
`recorded_proto` param). Both call sites (`bridge_reap_dead`, `bridge_find_live`)
adjust to `bridge_launcher_alive(b)`.

### 4.5 What gets deleted

- The `launcher_image_proto` **field is repurposed in place** as `token` (§4.2);
  its `HandleProtocol` recording block in `axl_backend_stdio_bridge_install` is
  deleted (replaced by the token stamp).
- The `HandleProtocol`/`LoadedImage`/`li == recorded_proto` logic inside
  `bridge_launcher_alive`.
- The now-inaccurate "narrows, does not eliminate" comments — replaced with an
  accurate description of the token gate. **Only** claim "eliminates" for the
  correlated-recycling case now that it's earned.

### 4.6 What is preserved

- `bridge_find_live` enumerate + `bridge_reap_dead` structure (UAF safety: only
  installed-protocol memory is dereferenced; `stdin_h` only after a live match).
- Newest-live-wins in `bridge_find_live` — now redundant (the token uniquely
  identifies the current bridge) but kept as harmless belt-and-suspenders and to
  avoid perturbing the reap ordering.
- The exit-status reflection path (`axl_backend_set_exit_status`,
  `mShellStdIn == NULL` driver discriminator) — it consults via
  `bridge_find_live`, so it inherits the token gate automatically. This protects
  the `pending_status` channel for macro-based `AXL_SHARED_DRIVER_LAUNCHER`
  consumers even though the axl-utils launcher carries exit status out-of-band.

### 4.7 Reap interaction (the ordering that keeps the reap test green)

`bridge_reap_dead()` keeps its shape — enumerate installed instances,
`UninstallProtocolInterface` any where `!bridge_launcher_alive(b)` — but "dead"
now means `token != cell->current`. Its two-plus callers set `cell->current`
appropriately *first*:

- **Install path** (`axl_backend_stdio_bridge_install`): sets `cell->current = t`
  (the new dispatch's token) before reaping → prior leaked bridges (older tokens)
  are swept immediately; the about-to-be-installed bridge isn't installed yet, so
  it isn't affected. (§4.3.)
- **Driver-consult path** (`bridge_find_live`): does *not* touch `cell->current`
  — the dispatching launcher already stamped it to `t`, so reap keeps that
  launcher's bridge and drops the rest. Unchanged call.
- **Standalone / unload path** (`axl_backend_stdio_bridge_reap`, called by
  `axl_shared_driver_unload` / `do -u`): sets `cell->current = 0` before reaping
  → *every* installed bridge is dead (no token is 0) and is uninstalled,
  including a last leaked bridge whose token still equals a stale `current`.
  **This is what keeps `test-stdio-bridge-reap-qemu.sh`'s `after_unload == 0`
  green.** Without the clear, the final leaker's bridge (token == the frozen
  `current`) would spuriously survive `do -u`.

Cell-absent reduction (the reap test's actual scenario — leakers install via the
escape hatch with *no* resident driver, so the cell is never created):
`bridge_launcher_alive` returns false for every instance (`cell == NULL`), so
reap drops everything not-just-installed. This reproduces the pre-fix reap-test
semantics exactly (initial ≤ 1 after the 2nd leaker; 0 after unload).

## 5. Data flow (one dispatch)

```
driver init:   publish() -> dispatch_token_ensure() -> cell{current=0} on dedicated handle
launcher:      install_stdio_bridge():
                 t = GetNextMonotonicCount()              (guard t != 0)
                 LocateProtocol(token) -> cell->current = t   (skip if absent)
                 bridge_reap_dead()      // sweeps prior leaks: token != t
                 mBridge.token = t; set handles; install mBridge
launcher:      vt->dispatch_argv(argc, argv)      // synchronous; launcher alive
  driver:        axl_stdin/axl_stderr -> bridge_find_live()
                   reap (token != cell->current), then return the survivor
                   -> the current launcher's bridge (token == cell->current)
launcher:      (returns) atexit/exit uninstalls the bridge on a normal exit;
               a gBS->Exit leak is inert (token != any future current)
`do -u`:       axl_shared_driver_unload -> stdio_bridge_reap():
                 cell->current = 0; bridge_reap_dead()   // drops every instance
```

## 6. Failure modes & version skew (all degrade safely)

| Scenario | Behavior |
|---|---|
| Normal dispatch | Current bridge token == cell → consulted. Correct. |
| Leaked stale bridge present (gBS->Exit) | Stale token != cell->current → dead → reaped. Current bridge wins. |
| No dispatch in progress (`cell->current == 0`) | No bridge matches → EOF-fallback. Safe. |
| **Stale older (v2.7.0) resident driver + v2.7.1 launcher** (the real skew window; consumer clears it with `do -u`) | Struct is layout-identical (§4.2), so `stdin_h`/`launcher_image`/`pending_status` offsets match. New launcher stamps a token into the slot the old driver reads as `launcher_image_proto`; old driver's proto-match compares live `LoadedImage*` ≠ that token → treats the (genuinely live) bridge as dead → **EOF-fallback**. Safe *silence*, never wrong data. |
| **v2.7.1 resident driver + stale older (v2.7.0) launcher** | Old launcher writes a `void*` into the slot the new driver reads as `token` and never stamps the cell → `b->token` (a pointer value) ≠ `cell->current` (0 or a monotonic value) → dead → EOF-fallback. Safe. |
| Fully mixed `libaxl.a` snapshots (cold build) | Unsupported by design ("rebuild both / `do -u`"); consumer pins the SDK via `.axl-sdk-checksums`, so a cold pairing always links one snapshot. The layout-preserving change above covers the *resident* skew that pinning can't. |

`GetNextMonotonicCount` is already used in-tree (`src/net/axl-mbedtls-platform.c`)
and available at boot time.

## 7. Testing (bucket D — test-first)

Not QEMU-reproducible via forced handle-reuse, so verification is
**correct-by-construction + no-regression on the live path**, plus a
**deterministic leaker fixture** that does not need handle reuse:

1. **New regression (deterministic RED via the reap path, single binary, no
   handle-recycling needed):** a fixture (mirroring `AxlStdioBridge` +
   `AXL_STDIO_BRIDGE_GUID` + `AxlDispatchToken` + `AXL_DISPATCH_TOKEN_GUID`
   locally, as `stdio-bridge-reap-test.c` already mirrors the bridge GUID —
   tests use public headers only) does:
   - Install one raw bridge instance on a fresh handle with
     `launcher_image = gImageHandle` (this image, genuinely alive) and the
     8-byte slot (`launcher_image_proto` pre-fix / `token` post-fix) set to
     **this image's real `EFI_LOADED_IMAGE_PROTOCOL*`**. Pre-fix that makes the
     proto-match report the bridge *alive*; post-fix that same slot is read as
     `token` (a large pointer value).
   - Install (if absent) the `AxlDispatchToken` cell and set
     `cell->current = GetNextMonotonicCount()` — a small monotonic value
     guaranteed `!= ` the pointer-valued slot.
   - Call `axl_shared_driver_unload("<unused-name>")` (triggers the reap path).
   - Assert the bridge handle **no longer carries** `AXL_STDIO_BRIDGE_GUID`
     (`HandleProtocol` fails). Asserting on the specific handle (not a global
     count) keeps it deterministic regardless of other instances.
   - **Pre-fix**: proto-match sees a live image + matching proto → not reaped →
     `HandleProtocol` still succeeds → assertion fails → **RED**. **Post-fix**:
     the unload reap clears `cell->current = 0` (§4.7) → token gate reports dead
     → reaped → `HandleProtocol` fails → **GREEN**. Directly exercises the
     false-alive the fix closes (a bridge that fools the proto-match but is
     correctly rejected by the token gate). The fixture compiles and runs on
     *both* versions because it references only its local GUID/struct mirrors.
2. **Live-path guard (extend the existing leaker flow):** the existing
   `test-driver-stdio-qemu.sh` / `test-stdio-bridge-reap-qemu.sh` already drive
   real install + `gBS->Exit` leak + resident-driver read; they must stay green
   (they pass under both gates, so they are guards, not the RED). No new leaker
   binary is required for the RED — test #1 is self-contained.
3. **Must stay GREEN both arches** (`make ARCH=x64` / `aa64`, 0 warnings):
   `test-driver-stdio-qemu.sh` (incl. `warmpipe=1` stale-bridge UAF case,
   self-locate, cross-image `estat=1`), `test-stdio-bridge-reap-qemu.sh`,
   `test-io-redirect-qemu.sh`, `test-sd-ergo-qemu.sh`.

## 8. Gates / release

- Test-first (bucket D): regression RED → implement → GREEN → refactor while
  green. Both arches, 0 warnings.
- Independent code review before commit (`feedback_code_review_before_commit`):
  contract-first is unnecessary (no public API); integration pass required.
- Do NOT regress the v2.6.1 UAF (never cache a bridge pointer; consult live;
  keep reap + enumerate).
- Doc sync: update the `axl-stdio-bridge.h` struct comments and any
  `src/backend/README.md` prose that describes the liveness gate. `make
  check-docs` + `make check-ascii`.
- If shipped: full local suite both arches + `scripts/install.sh` staged SDK +
  `scripts/lint.sh` (clang-tidy-21) clean, then `scripts/cut-release.sh 2.7.1`
  (dry-run first). **Tag only with explicit user approval**
  (`feedback_release_approval_gate`).

## 9. File-level change list

- `src/backend/axl-stdio-bridge.h`: add `AxlDispatchToken` + `AXL_DISPATCH_TOKEN_GUID`
  + `axl_backend_dispatch_token_ensure()` decl; add `token` to `AxlStdioBridge`;
  drop `launcher_image_proto`; refresh comments.
- `src/backend/native/axl-backend-native.c`: define the GUID + cell statics;
  implement `axl_backend_dispatch_token_ensure` (install-if-absent);
  token-stamp in `axl_backend_stdio_bridge_install`; rewrite
  `bridge_launcher_alive` to the token gate + fix its two call sites; drop the
  proto recording.
- `src/util/axl-shared-driver.c`: call `axl_backend_dispatch_token_ensure()` from
  `axl_shared_driver_publish` (thin wrapper).
- `test/integration/`: new/extended leaker fixture + its `test-*-qemu.sh`.
- Docs: `src/backend/README.md` liveness prose (if present) + header comments.
