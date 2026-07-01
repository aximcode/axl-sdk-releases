# AXL I/O Model + Shared-Driver Ergonomics — Design

**Date:** 2026-06-24
**Status:** Approved design (pre-implementation)
**Supersedes (consolidates):** `docs/AXL-Driver-Stdio-Forwarding-Design.md`,
`docs/superpowers/specs/2026-06-23-shared-driver-stdio-bridge-design.md` (the
stdin-bridge work that shipped in v2.6.0/v2.6.1 is a subset of this), and the
exploratory `docs/AXL-Subprocess-Design.md`.

## 1. Why

Two threads converged:

1. A consumer (delldiags `do.efi` / `doDriver.efi`, an axl-utils tool) split a
   tool into a thin launcher + a resident shared-driver. Making the driver
   behave like a normal app required hand-built cross-image plumbing for
   **stdin**, **exit status**, and output capture. The stdin half shipped
   (`axl_shared_driver_install_stdio_bridge`, v2.6.0); the rest is still
   consumer boilerplate.
2. Examining that plumbing exposed that axl-sdk's **I/O model has never been
   designed holistically**. `stdin`, `stdout`, `stderr`, and `stdout_raw` are
   each backed by a *different* firmware mechanism, `stderr` is a second-class
   citizen (no `2>`), and diagnostic logging pollutes redirected stdout.

The goal: a **consistent, POSIX/GLib-informed I/O model with UTF-8 as the
universal currency**, and a shared-driver layer where the SDK owns the
cross-image boilerplate so a consumer writes *app logic only*.

## 2. Empirical grounding — the redirect spike

A throwaway spike (`io-redirect-spike.c`) wrote a distinct marker to each of the
four candidate output sinks and drove it through every UEFI shell redirect
operator on **both x64 (OVMF) and aa64 (AAVMF)**. Result (identical on both):

| Operator | Sinks captured | Target | File encoding |
|---|---|---|---|
| `>` / `>>` | `gST->ConOut` + shell StdOut handle | file | UCS-2 + BOM |
| `>a` / `>>a` | `gST->ConOut` + shell StdOut handle | file | **ASCII** |
| `2>` / `2>>` | `gST->StdErr` (ConErr) + shell StdErr handle | file | UCS-2 + BOM |
| `2>a` | `gST->StdErr` (ConErr) + shell StdErr handle | file | **ASCII** |
| `>v VAR` | `gST->ConOut` + shell StdOut handle | env var | native |

**Three findings that drive the design:**

- **F1.** `>` swaps `gST->ConOut`; `2>` swaps `gST->StdErr` — *symmetrically*.
  The sink mapping is **stable across the whole operator family** (`a` / `>>`
  variants change only the file encoding, never the sink).
- **F2.** The shell owns UCS-2↔ASCII transcoding at the redirect boundary
  (`>` → UCS-2+BOM, `>a` → ASCII). **Console redirection never yields a UTF-8
  file.** A tool that needs a UTF-8 *file* must use `axl_fopen` (a file stream),
  not `tool > file`.
- **F3.** axl's diagnostic log (`[INFO] mem: no leaks detected`, every
  `axl_warning`) currently writes to `gST->ConOut`, so `tool > out.txt`
  **captures axl's own logs into the user's output file.**

## 3. Current model (the starting point)

Standard streams in `axl-stream.h`, each backed differently:

| Stream | Backed by | Honors |
|---|---|---|
| `axl_stdin` | shell **StdIn handle** (raw bytes) | `<`, `\|` (UCS-2 — needs `axl_stdin_text()`) |
| `axl_stdout` (text) | `gST->ConOut` (SimpleTextOutput) | `>` via ConOut swap |
| `axl_stdout_raw` | shell **StdOut handle** | `>`/`\|`; **NULL → fails in a driver** |
| `axl_stderr` | `gST->ConOut` — *same sink as stdout* | only `>`; **no `2>`** |

Plus a *second* input concept: `axl_console_read_key` (`ConIn` keystrokes,
`axl-console.h`) — distinct from `axl_stdin`'s pipe bytes.

Problems: every stream uses a different mechanism; `stderr` has no `2>`, no raw
variant; logs pollute stdout (F3); `stdout_raw` is broken in a resident driver.

## 4. Target I/O model

### 4.1 Principles (POSIX / GLib)

- **One currency: UTF-8.** Every public API speaks UTF-8. `fwrite(3) → write(2)`
  layering: `axl_print` (L1) → `axl_fprintf`/`axl_fwrite` (L2) → `axl_write`
  (L3). Encoding conversion happens at **exactly one boundary — layer 2** —
  using the existing `axl_stream_set_encoding`.
- **Consistency lives in the interface, not the sink.** The caller's mental
  model is uniform; *which* firmware object each std stream writes to is an
  implementation detail behind the stream vtable. This lets us keep UEFI's two
  unavoidable sink families without leaking them to callers.
- **Two sink families are real and kept separate** (like POSIX stdio-vs-termios):
  - **Shell stdio handles** (`StdIn`/`StdOut`/`StdErr`) — byte streams, the
    `< > 2> | >v` targets. "fd 0/1/2."
  - **Console protocols** (`gST->ConOut`/`StdErr`/`ConIn`) — structured
    (`OutputString(CHAR16*)`, attributes, keystrokes). "the terminal." The
    console mirror and color/cursor control hook here.

### 4.2 Standard-stream sink table (the change)

| Stream | Sink | Rationale |
|---|---|---|
| `axl_stdout` (text) | `gST->ConOut` (UTF-8→UCS-2) | F1: `>`-swapped; mirror + render |
| `axl_stderr` (text) | **`gST->StdErr` → ConOut fallback** | **F1: `2>`-swapped**; stays a console protocol |
| `axl_stdout_raw` | shell **StdOut handle** | binary pipe-out; in a driver via bridge `stdout_h` |
| `axl_stderr_raw` *(new)* | shell **StdErr handle** | symmetry; binary diagnostics under `2>` |
| `axl_stdin` (raw) | shell **StdIn handle** | `<`/`\|` raw bytes |
| `axl_stdin_text()` | sniffing wrapper over `axl_stdin` | UTF-8 text (UCS-2/BOM/ASCII auto-detect) |

The single behavioral change to text output: **`axl_stderr` writes
`gST->StdErr`** (falling back to `gST->ConOut` when null). Everything else stays;
`axl_stdout_raw` gains a driver-side bridge fallback; `axl_stderr_raw` is new.

### 4.3 Diagnostic logging → stderr (F3)

`axl_log` / `axl_warning` / leak reports currently target stdout (ConOut). They
move to **stderr (ConErr)** so they never pollute redirected stdout (`tool >
out.txt` stays clean). This is the natural payoff of giving stderr its own sink.

### 4.4 Input stays two-path (deliberate, documented)

stdin's *source* encoding is ambiguous — UCS-2 from `|`, UTF-8/ASCII from
`< file`, raw bytes from `|a`. Output has one known target (UCS-2 console) so it
auto-encodes; **input must sniff**, so it cannot have a single fixed encoding.
Therefore we keep:

- `axl_stdin` — raw bytes (binary tools, `|a`).
- `axl_stdin_text()` — sniffing wrapper → UTF-8 (the text path; the common `|`
  case). Documented as the canonical "read text from stdin" entry.
- `axl_console_read_key` — interactive keystrokes (prompts, menus); orthogonal.

"UTF-8 throughout" therefore means: **all output, and the stdin *text* path, are
UTF-8.** Raw byte I/O is encoding-agnostic by definition; console redirection is
shell-encoded (F2) — for a UTF-8 *file*, use `axl_fopen`.

### 4.5 Console mirror

`AxlConsoleMirror` currently wraps `gST->ConOut`. With stderr on `gST->StdErr`,
the mirror must **also wrap `gST->StdErr`** to mirror diagnostics/`stderr` to a
remote terminal. In scope for this work (softbmc depends on the mirror).

## 5. Shared-driver cross-image model

### 5.1 The collapse

A resident driver runs verb code in the *driver* image; some per-process state
is image-local in axl-sdk. The spike's F1 simplifies this dramatically:

| State | Cross-image mechanism | Bridge needed? |
|---|---|---|
| stdin | shell StdIn **handle** — carried by the bridge | **yes** (`stdin_h`) |
| stdout text | `gST->ConOut` — global, `>`-swapped during the launcher window | **no** |
| stderr text | `gST->StdErr` — global, `2>`-swapped during the launcher window | **no** |
| stdout raw | shell StdOut **handle** — NULL in a driver | **yes** (`stdout_h`) |
| stderr raw | shell StdErr **handle** | yes (`stderr_h`) |
| exit status | per-image pending cell | **yes** (new bridge field) |

So text stdout/stderr in a driver **just work** (global console protocols the
shell swaps); only the *handle*-backed streams (stdin, raw out/err) and exit
status need the bridge. This is far less than the original sketch.

### 5.2 The standard contract

Once stdin + exit status are SDK-owned, the cross-image vtable collapses to the
canonical `main` signature, so the SDK defines it:

```c
typedef struct { int (*run)(int argc, char **argv); } AxlSharedDriverVtable;
```

Consumers stop hand-rolling a vtable + protocol header.

### 5.3 Public API (launcher + driver)

**Driver side — one macro:**
```c
AXL_SHARED_DRIVER(name_str, init_fn, run_fn, unload_fn)
```
Generates the DriverEntry: `init_fn()` once (heavy per-boot setup; abort publish
on `AXL_ERR`), publishes a static `{.run = run_fn}` under the name's GUID; the
unload path unpublishes then calls `unload_fn()`. Composes `AXL_DRIVER` +
`axl_shared_driver_publish/unpublish` (which the consumer no longer touches).

**Launcher side — turnkey (new/simple consumers):**
```c
AXL_SHARED_DRIVER_LAUNCHER(name_str, driver_filename, embed_symbol)  /* or NONE */
```
Generates the entire `int main` → `axl_shared_driver_run(...)`:
```c
int axl_shared_driver_run(const char *name, const char *driver_filename,
                          const unsigned char *embed, size_t embed_len,
                          int argc, char **argv);
```
Resolves (warm → on-disk-by-name → embedded blob; standard locate semantics),
then dispatches (install bridge → `vt->run(argc, argv)` → apply exit status).
Returns the **driver's** status when it dispatched, or a launcher-side error
(`EFI_NOT_FOUND`, diagnostic printed) when the driver could not be located.

**Launcher side — escape hatch (custom resolution / REPL, e.g. do.efi):**
```c
int axl_shared_driver_dispatch(const AxlSharedDriverVtable *vt,
                               int argc, char **argv);
```
Brackets bridge install + `vt->run` + exit-status apply. A consumer keeping its
own resolution (thin/shell/REPL/`--reload`) calls this at each dispatch site.

**Bottom primitives (kept, documented as escape hatch):**
`axl_shared_driver_install_stdio_bridge()` (shipped) and a new
`axl_shared_driver_apply_exit_status()` — what `dispatch`/`run` are built from.

### 5.4 Exit-status reflection (the hidden "magic")

The internal `AxlStdioBridge` gains an inline cell:
```c
AxlEfiStatus pending_status;
bool         has_pending;
```
- Driver-side `axl_set_exit_status()` / `axl_exit()` become bridge-aware: when
  the calling image has *no shell params of its own* (a resident driver) and a
  **live** bridge exists (reusing the v2.6.1 liveness-checked lookup), they
  write the launcher's bridge cell. A normal app/launcher is unaffected.
- `install_stdio_bridge` resets `has_pending = false` per dispatch.
- `apply_exit_status` (called by `dispatch`/`run`) reads the cell and applies it
  to the launcher via the launcher's own `axl_set_exit_status`, so CRT0 returns
  it verbatim.

A driver verb therefore just calls `axl_set_exit_status(N)` like any app — the
consumer's `take_exit_status` vtable slot and manual apply both delete.

## 6. Consumer impact (do.efi / doDriver.efi)

- **Delete** `doDriver-protocol.h` (custom `DoVtable`), the `take_exit_status`
  slot + its driver impl, and the launcher's `install_stdio_bridge` call +
  warning + manual exit-status apply.
- **doDriver.c:** `AXL_DRIVER(...)` + manual publish → `AXL_SHARED_DRIVER("dell-diags/do",
  do_init, do_run, do_unload)`; `do err <N>` → plain `axl_set_exit_status(N)`.
- **do.c:** each dispatch site (`dispatch_once`, REPL loop) →
  `axl_shared_driver_dispatch(vt, argc, argv)`. Keeps its own thin/shell/REPL/
  `--reload` resolution.
- Its `/s` capture (via `axl_setenv`) is unchanged — it captures a *named
  scalar*, which `>v` (whole-stdout) doesn't replace.

## 7. The example (a teaching artifact)

Rewrite `sdk/examples/shared-driver-demo/` as a **thoroughly commented** example
that exercises everything with zero plumbing:

- **Driver:** `AXL_SHARED_DRIVER`; a `run()` that reads a line via
  `axl_stdin_text()`, echoes to **stdout**, notes to **stderr**, writes a **raw
  byte** to stdout (proving the raw-stdout-in-driver fallback), and sets an
  **exit status** from a verb — all plain app-style code.
- **Launcher:** the one-line `AXL_SHARED_DRIVER_LAUNCHER` macro.
- **Header comments** walk through what the SDK does behind the scenes (resolve
  → bridge → dispatch → exit-status) and embed the §4.2 per-stream behavior
  table, so a new consumer learns the model from the example alone.
- Driven by an integration fixture that asserts each stream + `%lasterror%`
  across `< > 2> |` on both arches.

## 8. Behavior changes (call out loudly)

1. **`>` no longer captures stderr** (stderr → ConErr; only `2>` captures it).
   POSIX-correct, but every existing axl app inherits it.
2. **Logs move from stdout to stderr** — scripts that scraped logs out of a
   `>`-redirected file must switch to `2>`.

SemVer: these are observable behavior changes. Ship as a clearly-documented
**MINOR** (the streams are still source-compatible; the new APIs are additive),
with a prominent CHANGELOG "Changed" entry. Target **v2.7.0**.

## 9. Implementation phases (each test-first, both arches)

- **Phase 1 — I/O model.** `axl_stderr` → `gST->StdErr` (+ConOut fallback);
  `axl_stderr_raw`; route `axl_log`/`axl_warning` to stderr; `axl_print`/
  `printerr` through the encoding-aware layer; console-mirror wraps ConErr.
  Integration: redirect matrix fixture (`> 2> >a |`) asserting separation +
  log-not-in-stdout. Unit: stderr sink selection, encoding.
- **Phase 2 — driver-side stdio bridge completion.** `axl_backend_shell_stdout`
  (and `_stderr`) bridge fallback so raw out/err work in a driver; extend the
  bridge struct with the exit-status cell; driver-side `axl_set_exit_status`
  reflection. Reuse the v2.6.1 liveness lookup.
- **Phase 3 — shared-driver ergonomics.** `AxlSharedDriverVtable`,
  `axl_shared_driver_dispatch`, `axl_shared_driver_run`, `AXL_SHARED_DRIVER`,
  `AXL_SHARED_DRIVER_LAUNCHER`, `apply_exit_status`. Integration: the
  shared-driver-demo fixture (stdin/stdout/stderr/exit-status round trip).
- **Phase 4 — docs + example.** New example; update
  `AXL-Shared-Driver-Recipe.md` (new macros + the per-stream table); fold the
  superseded design docs; CHANGELOG.

## 10. Open questions / risks

- **Console-mirror over ConErr:** confirm the mirror wrapper composes cleanly
  with `gST->StdErr` (some firmware aliases StdErr to ConOut; the mirror must
  not double-wrap). Verify in QEMU during Phase 1.
- **`gST->StdErr == NULL`** on minimal firmware → ConOut fallback path must be
  exercised (BDS / non-shell tests).
- **Encoding of `axl_print` to a `>`-redirected file:** the shell transcodes
  (F2). Confirm a `>a`-redirected `axl_print` round-trips ASCII and document
  that UTF-8 files require `axl_fopen`.
</content>
