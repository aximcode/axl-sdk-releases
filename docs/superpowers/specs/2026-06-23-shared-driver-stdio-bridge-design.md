# Design: bridge the launcher's stdio across the shared-driver boundary

**Date:** 2026-06-23
**Status:** approved (brainstorm), pending implementation plan
**Origin:** handoff `docs/AXL-Driver-Stdio-Forwarding-Design.md` (delldiags `do.efi`
consumer). This spec refines that handoff against the actual substrate.

## Problem (confirmed against code)

A resident shared-driver's `axl_stdin` is EOF: `console_read`
(`src/stream/axl-stream.c`) resolves the handle via
`axl_backend_shell_stdin()` → the static `mShellStdIn`, which
`probe_shell_std_handles()` fills from `EFI_SHELL_PARAMETERS_PROTOCOL.StdIn`
**on `gImageHandle`**. In the resident driver image there are no shell params
(it was loaded as a driver, not launched by the shell) → `mShellStdIn == NULL`
→ `axl_stdin` is EOF. Piped (`echo x | tool v`), `<`-redirected, and
interactive input all live on the *launcher's* `StdIn`, which never crosses the
`vt->dispatch_argv(argc, argv)` boundary.

## Goal

A driver verb that does `axl_readline(axl_stdin)` transparently reads the
launcher's piped / `<`-redirected / interactive input, **with no per-tool
code** on either side. StdOut/StdErr bridging is **test-gated** (see Output).

## Decisions (settled in brainstorming)

| # | Decision | Choice |
|---|---|---|
| 1 | Mechanism | SDK-owned **stdio-bridge protocol**: launcher captures its shell `{StdIn,StdOut,StdErr}` and installs it on a fixed GUID; the driver's backend getters fall back to it when the local image has no shell params. |
| 2 | Where the launcher captures | Inside `axl_shared_locate` (already called per launcher invocation) — zero new launcher call sites. |
| 3 | Where the driver adopts | Inside the backend getters (`axl_backend_shell_stdin/stdout`) — zero driver code. |
| 4 | Scope | **StdIn is the core deliverable.** StdOut/StdErr bridging only if the QEMU `>`-redirect test shows driver output isn't already captured (see Output). |
| 5 | ABI | No change to the consumer's vtable or the existing shared-driver API signatures (the bridge is internal to `axl_shared_locate` + the backend). |

## Architecture

### The bridge protocol (SDK-internal)

A protocol installed on a fixed AXL GUID whose interface is:

```c
typedef struct {
    AxlFileHandle stdin_h;    /* SHELL_FILE_HANDLE: launcher's StdIn  */
    AxlFileHandle stdout_h;   /* launcher's StdOut */
    AxlFileHandle stderr_h;   /* launcher's StdErr (may be NULL) */
    void         *launcher_image;  /* EFI_HANDLE of the launcher, for liveness */
} AxlStdioBridge;
```

The handles are firmware-owned `SHELL_FILE_HANDLE` pointers, valid in the shared
UEFI address space and usable from the driver image *while the launcher
invocation is live*.

### Launcher side — `axl_shared_locate`

Each launcher invocation (the launcher's `int main` runs fresh per shell
command) already calls `axl_shared_locate(...)` to find/ensure the driver.
Extend it to, **after** the driver is resident:
1. Read the launcher's own `EFI_SHELL_PARAMETERS_PROTOCOL.{StdIn,StdOut,StdErr}`
   off `gImageHandle` (cheap — 3 pointers).
2. Install/refresh the `AxlStdioBridge` protocol (singleton on the fixed GUID;
   reinstall to update the handles each invocation — `axl_protocol_install`,
   or uninstall+install if already present).
3. Register an `axl_atexit` handler that **uninstalls** the bridge when the
   launcher exits. This is load-bearing: the launcher's `StdIn` handle dies
   when the launcher returns; a stale bridge would feed a freed handle to a
   later *direct* driver call.

If the launcher has no shell params itself (non-Shell-2.0 launch), it installs
nothing — the driver falls through to its existing EOF behavior (no regression).

### Driver side — backend getters

`probe_shell_std_handles()` currently caches once. Restructure the getters so
the **bridge path is never cached** (the launcher's handles change per
invocation):

```
axl_backend_shell_stdin():
    probe_shell_std_handles();        // local image's own params (cached)
    if (mShellStdIn != NULL) return mShellStdIn;   // app/launcher: as today
    return bridge_stdin();            // driver: LIVE lookup of AxlStdioBridge
```

`bridge_stdin()` does a fresh `axl_protocol_find_guid(AXL_STDIO_BRIDGE_GUID)`
each call (no caching) and returns its `stdin_h` (or NULL if absent). The
find is an in-memory handle-DB scan — cheap; it only runs on the no-local-params
(driver) path. Optionally validate `launcher_image` is still a live handle
before returning, as defense against a missed uninstall.

The same pattern applies to `axl_backend_shell_stdout()` if Output bridging is
needed.

### Output side — TEST FIRST, then decide

`axl_stdout`'s `console_write` targets **`ConOut`** (`axl_backend_console_write`),
not shell `StdOut`. A normal app's `tool > file` works because the shell swaps
`gST->ConOut` during the app's run — and the driver dispatch executes **inside
the launcher's shell-execution window**, so driver output to `ConOut` **may
already honor `>`** with no change. The implementation plan must:
1. First add the `>`-redirect fixture case and observe whether driver output is
   already captured.
2. Only if NOT captured: bridge `StdOut` (and add `mShellStdErr`/`stderr_h`) via
   the same getter-fallback. Otherwise document that output redirect already
   works and leave the output path untouched.

`axl_stdout_raw` already writes to `axl_backend_shell_stdout()` (the shell
StdOut handle), so it is bridged for free once the getter fallback exists —
note this in the report.

## Lifetime / safety (load-bearing)

- **Never cache the bridge handle in the driver.** Live-resolve per getter call.
- **Launcher uninstalls the bridge on exit** (`axl_atexit`). Closes the
  freed-handle window for later direct driver calls.
- A direct (non-launcher) driver call with no bridge installed → getter returns
  NULL → `axl_stdin` is EOF, exactly as today (no regression).
- Synchronous dispatch only (no event loop, one call at a time) — no
  re-entrancy concern.

## Public API

Minimal. The bridge is internal; the only new *public* surface (if any) is an
optional explicit pair for consumers who call the driver through a raw vtable fn
and want to opt in without `axl_shared_locate`:

- (Internal) `AXL_STDIO_BRIDGE_GUID`, `AxlStdioBridge`, the install/uninstall in
  `axl-shared-driver.c`, the backend getter fallback.
- (Public, optional, only if needed) `axl_shared_stdio_bridge_install(void)` /
  `_uninstall(void)` — exposed only if a consumer needs to bridge outside the
  `axl_shared_locate` path. Default design needs no new public symbol.

## Testing

New fixture under `test/` (a minimal launcher + driver, modeled on
`sdk/examples/shared-driver-demo/`): a driver verb `echo` that reads one
`axl_readline(axl_stdin)` line and writes it to `axl_print`. A
`test/integration/test-driver-stdio-qemu.sh` drives it via `startup.nsh`:

- `echo hello | fixture echo`  → asserts output `hello` (**pipe**)
- `fixture echo < in.txt`        → asserts first line of `in.txt` (**`<`**)
- `fixture emit > out.txt` then read back → asserts driver output captured
  (**`>`** — this is the Output probe; its result decides whether StdOut
  bridging is implemented)
- a **direct** (non-launcher) build of the same driver logic, or a no-bridge
  invocation, still EOFs cleanly (**no regression**)
- interactive: a `run-qemu --interactive` note (manual; can't assert in CI).

Exact-string assertions (`axl_strcmp` / anchored grep). Bridge-absent path
covered. Runs under the existing QEMU harness.

## Out of scope

- Bridging anything beyond `{StdIn, StdOut, StdErr}` (no cwd, no env — those
  have their own backend paths).
- The async/event-loop service driver (`axl-service.h`) — this is the
  synchronous shared-driver path only.
- Changing the consumer vtable ABI.

## Risks / watch-items

- **Stale launcher handle after exit** — mitigated by atexit uninstall +
  optional `launcher_image` liveness check; pin with the no-regression test.
- **Output may already work** — resolve empirically before writing any
  StdOut-bridge code (don't build it speculatively).
- **Per-call bridge lookup cost** — acceptable (in-memory handle-DB scan, driver
  path only); revisit only if a profile shows it hot.
