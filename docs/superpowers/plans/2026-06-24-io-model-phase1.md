# I/O Model Phase 1 — stderr as a real stream + logs off stdout

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `axl_stderr` its own firmware sink (`gST->StdErr`) so `2>` works and `>` no longer captures stderr, route diagnostic logging to stderr, and add `axl_stderr_raw` — the foundational I/O-model fixes from the design spec.

**Architecture:** Add a ConErr-targeted backend console writer; point the `mStderr` stream at it; repoint `axl_log`'s emit path at it; add a raw stderr stream over the shell StdErr handle (with a driver bridge fallback). Text stdout/ConOut is untouched. Proven end-to-end by a QEMU redirect-matrix fixture.

**Tech Stack:** C (freestanding UEFI), AXL backend abstraction, QEMU/OVMF + AAVMF integration harness (`test/integration/common-test.sh`).

## Global Constraints

- axl-sdk ≥ current `main`; ships in **v2.7.0** (MINOR).
- Public API uses standard C types, `axl_snake_case`, UTF-8 strings; no EDK2 types leak (`docs/AXL-Coding-Style.md`).
- Both arches green: `make ARCH=x64` and `make ARCH=aa64`; QEMU on X64 **and** AARCH64.
- Exact-string assertions for output tests (`grep -c '^exact$'`), never substring (`CLAUDE.md` bucket B).
- `make check-ascii` + `make check-docs` clean before commit.
- No references to downstream consumers in code/comments (`feedback_no_consumer_names_in_code`).
- Direct commits to `main` (`feedback_direct_commits_solo`); do not tag/release (Phase 4 does).

## File Structure

- `src/backend/axl-backend.h` — declare `axl_backend_console_write_err`.
- `src/backend/native/axl-backend-native.c` — implement it (ConErr + ConOut fallback); add bridge fallback to `axl_backend_shell_stderr`.
- `src/stream/axl-stream.c` — new `console_write_err` sink fn; repoint `mStderr`; add `mStderrRaw` + `axl_stderr_raw`.
- `include/axl/axl-stream.h` — declare `axl_stderr_raw`, document the stderr sink + the redirect-encoding boundary.
- `src/log/axl-log.c` — repoint the 7 `axl_backend_console_write` emit sites to `axl_backend_console_write_err`.
- `test/integration/io-streams.c` — new tiny test tool (stdout/stderr/log/raw markers).
- `test/integration/test-io-redirect-qemu.sh` — new redirect-matrix fixture.
- `Makefile` — build target for `io-streams`.
- `src/stream/README.md`, `CHANGELOG.md` — doc updates.

---

### Task 1: Redirect-matrix fixture (RED)

**Files:**
- Create: `test/integration/io-streams.c`
- Create: `test/integration/test-io-redirect-qemu.sh`
- Modify: `Makefile` (add `io-streams` app target; add to the `.PHONY` list beside `stdio-bridge-leak`)
- Test: the fixture itself

**Interfaces:**
- Produces: `io-streams.efi` — writes `OUT:stdout` (via `axl_print`), `ERR:stderr` (via `axl_printerr`), emits a WARN log (`axl_warning("LOG:warn")`), and a raw byte marker `RAW:stderr` (via `axl_write(axl_stderr_raw, ...)` — will be added in Task 4; until then the tool omits the raw line).

- [ ] **Step 1: Write the test tool** `test/integration/io-streams.c`

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* io-streams.c — emits one marker per stream; driven under redirect
 * operators by test-io-redirect-qemu.sh to prove sink separation. */
#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;
    axl_print("OUT:stdout\n");        /* -> ConOut (stdout) */
    axl_printerr("ERR:stderr\n");     /* -> stderr sink */
    axl_warning("LOG:warn");          /* diagnostic log */
    return 0;
}
```

- [ ] **Step 2: Add the Makefile target** (place beside the stdio-bridge fixture targets; add `io-streams` to the `.PHONY:` line)

```make
# io-streams — I/O-model redirect fixture tool.
io-streams: $(PREFIX)/io-streams.efi
	@echo "  Built: $(PREFIX)/io-streams.efi"

$(PREFIX)/io-streams.efi: $(BUILDDIR)/io-streams.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/io-streams.o,$@)

$(BUILDDIR)/io-streams.o: test/integration/io-streams.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
```

- [ ] **Step 3: Write the fixture** `test/integration/test-io-redirect-qemu.sh`

```bash
#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# I/O-model redirect separation: stdout -> `>`, stderr -> `2>`, and
# NEITHER stderr NOR diagnostic logs land in a `>`-redirected stdout file.
source "$(dirname "$0")/common-test.sh"
export TEST_SKIP_RATCHET=1
while [[ $# -gt 0 ]]; do case "$1" in --arch) TEST_ARCH="$2"; shift 2;; *) exit 1;; esac; done
test_setup
declare -A _M=([X64]=x64 [AARCH64]=aa64); _a="${_M[$TEST_ARCH]:-x64}"
make -C "$PROJECT_DIR" ARCH="$_a" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} io-streams 2>&1 | tail -2
EFI="$PROJECT_DIR/out/native-$_a/io-streams.efi"
if [[ ! -f "$EFI" ]]; then echo "WARN: io-streams.efi not built; skipping."; echo "IO redirect test: SKIP"; exit 0; fi
test_add_efi "$EFI"
{
  echo "@echo -off"; echo "fs0:"; echo "cd \\"
  echo "echo R_BEGIN"
  echo "io-streams.efi > out.txt 2> err.txt"
  echo "echo TYPE_OUT"; echo "type out.txt"
  echo "echo TYPE_ERR"; echo "type err.txt"
  echo "echo R_DONE"
  echo "reset -s"
} | test_set_startup
test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 60
test_clean_log
out=$(sed -n '/TYPE_OUT/,/TYPE_ERR/p' "$TEST_CLEAN_LOG")
err=$(sed -n '/TYPE_ERR/,/R_DONE/p'  "$TEST_CLEAN_LOG")
out_has_stdout=$(grep -c 'OUT:stdout' <<<"$out" || true)
out_has_stderr=$(grep -c 'ERR:stderr' <<<"$out" || true)
out_has_log=$(grep -c 'LOG:warn'      <<<"$out" || true)
err_has_stderr=$(grep -c 'ERR:stderr' <<<"$err" || true)
err_has_log=$(grep -c 'LOG:warn'      <<<"$err" || true)
echo "Results: out{stdout=$out_has_stdout stderr=$out_has_stderr log=$out_has_log} err{stderr=$err_has_stderr log=$err_has_log}"
# GREEN: stdout file has ONLY stdout; stderr file has stderr + logs.
if [[ "$out_has_stdout" -ge 1 && "$out_has_stderr" -eq 0 && "$out_has_log" -eq 0 \
      && "$err_has_stderr" -ge 1 && "$err_has_log" -ge 1 ]]; then
  echo "IO redirect test: OK"; exit 0
else
  echo "IO redirect test: FAIL"; exit 1
fi
```

- [ ] **Step 4: chmod + run to confirm RED**

Run: `chmod +x test/integration/test-io-redirect-qemu.sh && timeout 120 ./test/integration/test-io-redirect-qemu.sh --arch X64 2>&1 | grep -E "Results:|test:"`
Expected: **FAIL** — today `ERR:stderr` and `LOG:warn` land in `out.txt` (stderr + logs currently write ConOut, captured by `>`). So `out_has_stderr>=1` and/or `out_has_log>=1` → the GREEN condition fails.

- [ ] **Step 5: Commit**

```bash
git add test/integration/io-streams.c test/integration/test-io-redirect-qemu.sh Makefile
git commit -m "test: RED redirect-matrix fixture for stderr/stdout/log separation"
```

---

### Task 2: stderr → `gST->StdErr` (ConErr)

**Files:**
- Modify: `src/backend/axl-backend.h` (declare `axl_backend_console_write_err`)
- Modify: `src/backend/native/axl-backend-native.c` (implement it, beside `axl_backend_console_write`)
- Modify: `src/stream/axl-stream.c` (add `console_write_err`; repoint `mStderr.write`)

**Interfaces:**
- Produces: `void axl_backend_console_write_err(const unsigned short *str)` — writes `gST->StdErr`, falling back to `gST->ConOut` when `StdErr` is NULL.

- [ ] **Step 1: Declare the backend fn** — in `src/backend/axl-backend.h`, immediately after the `axl_backend_console_write(...)` declaration:

```c
/**
 * @brief Write a UCS-2 string to the error console (gST->StdErr).
 *        Falls back to gST->ConOut when StdErr is NULL. NULL-safe.
 */
void
axl_backend_console_write_err(
    const unsigned short  *str  ///< UCS-2 string to output
);
```

- [ ] **Step 2: Implement it** — in `src/backend/native/axl-backend-native.c`, immediately after the `axl_backend_console_write` function:

```c
/**
 * @brief Write a UCS-2 string to the error console. NULL-safe.
 *
 * Targets gST->StdErr so the UEFI shell's `2>` redirect (which swaps
 * gST->StdErr, symmetric to `>` swapping gST->ConOut) captures it and a
 * plain `>` does not. Falls back to gST->ConOut when StdErr is absent
 * (minimal firmware / BDS).
 */
void
axl_backend_console_write_err(
    const unsigned short  *str
    )
{
    if (gST == NULL || str == NULL) {
        return;
    }
    if (gST->StdErr != NULL) {
        gST->StdErr->OutputString(gST->StdErr, (CHAR16 *)str);
    } else if (gST->ConOut != NULL) {
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)str);
    }
}
```

- [ ] **Step 3: Add a stderr sink in the stream layer** — in `src/stream/axl-stream.c`, add a `console_write_err` mirroring `console_write` but calling `axl_backend_console_write_err`. Locate the existing `console_write` static function; add directly after it:

```c
/* stderr text sink — identical transcode to console_write but emits to
   the error console so `2>` captures it (see axl_backend_console_write_err). */
static axl_ssize_t
console_write_err(void *ctx, const void *data, size_t count)
{
    (void)ctx;
    if (count == 0) {
        return 0;
    }
    unsigned short  stack_buf[CONSOLE_WRITE_STACK_UCS2];
    unsigned short *out      = stack_buf;
    size_t          out_cap  = CONSOLE_WRITE_STACK_UCS2;
    unsigned short *heap_buf = NULL;
    bool overflowed = false;
    (void)console_transcode_crlf((const uint8_t *)data, count, out, out_cap, &overflowed);
    if (overflowed) {
        size_t heap_cap = count * 2 + 1;
        heap_buf = (unsigned short *)axl_malloc(heap_cap * sizeof(unsigned short));
        if (heap_buf == NULL) {
            return -1;
        }
        (void)console_transcode_crlf((const uint8_t *)data, count, heap_buf, heap_cap, &overflowed);
        out = heap_buf;
    }
    axl_backend_console_write_err(out);
    if (heap_buf != NULL) {
        axl_free(heap_buf);
    }
    return (axl_ssize_t)count;
}
```

- [ ] **Step 4: Repoint `mStderr`** — in `src/stream/axl-stream.c`, change the `mStderr` initializer's `.write` field from `console_write` to `console_write_err`:

```c
static AxlStream mStderr = {
    .ctx    = NULL,
    .read   = NULL,
    .write  = console_write_err,
    .pread  = NULL,
    .pwrite = NULL,
    .close  = NULL,
};
```

- [ ] **Step 5: Build both arches**

Run: `make ARCH=x64 all 2>&1 | grep -iE "error|warning"; make ARCH=aa64 all 2>&1 | grep -iE "error|warning"`
Expected: no output (clean).

- [ ] **Step 6: Commit**

```bash
git add src/backend/axl-backend.h src/backend/native/axl-backend-native.c src/stream/axl-stream.c
git commit -m "stream: axl_stderr writes gST->StdErr so 2> captures it, > does not"
```

---

### Task 3: diagnostic logs → stderr

**Files:**
- Modify: `src/log/axl-log.c` (repoint the emit sites)

**Interfaces:**
- Consumes: `axl_backend_console_write_err` (Task 2).

- [ ] **Step 1: Repoint log emit** — in `src/log/axl-log.c`, replace every `axl_backend_console_write(` call in the log-line emit path with `axl_backend_console_write_err(`. There are 7 sites (the `wide` message, the level prefix, the domain, the location, the `": "`, and the trailing `"\r\n"`). Use a single replace-all:

Run: `sed -i.bak 's/axl_backend_console_write(/axl_backend_console_write_err(/g' src/log/axl-log.c && rm -f src/log/axl-log.c.bak`

- [ ] **Step 2: Verify no stray stdout writes remain in the log path**

Run: `grep -n 'axl_backend_console_write(' src/log/axl-log.c`
Expected: no matches (all now `_err`).

- [ ] **Step 3: Build both arches**

Run: `make ARCH=x64 all 2>&1 | grep -iE "error|warning"; make ARCH=aa64 all 2>&1 | grep -iE "error|warning"`
Expected: clean.

- [ ] **Step 4: Confirm the fixture goes GREEN (x64)**

Run: `timeout 120 ./test/integration/test-io-redirect-qemu.sh --arch X64 2>&1 | grep -E "Results:|test:"`
Expected: `out{stdout=1 stderr=0 log=0} err{stderr=1 log=1}` and `IO redirect test: OK`.

- [ ] **Step 5: Confirm GREEN on aa64**

Run: `timeout 200 ./test/integration/test-io-redirect-qemu.sh --arch AARCH64 2>&1 | grep -E "Results:|test:"`
Expected: `IO redirect test: OK`.

- [ ] **Step 6: Commit**

```bash
git add src/log/axl-log.c
git commit -m "log: route diagnostics to stderr so > redirection keeps stdout clean"
```

---

### Task 4: `axl_stderr_raw` (+ driver bridge fallback)

**Files:**
- Modify: `include/axl/axl-stream.h` (declare `axl_stderr_raw`)
- Modify: `src/stream/axl-stream.c` (add `console_write_err_raw`, `mStderrRaw`, `axl_stderr_raw`; init in `axl_stream_init`)
- Modify: `src/backend/native/axl-backend-native.c` (bridge fallback in `axl_backend_shell_stderr`)

**Interfaces:**
- Produces: `extern AxlStream *axl_stderr_raw;` — binary stderr over the shell StdErr handle.
- Consumes: `axl_backend_shell_stderr()` (existing; gains a bridge fallback).

- [ ] **Step 1: Declare `axl_stderr_raw`** — in `include/axl/axl-stream.h`, after the `axl_stdout_raw` extern + its doc block:

```c
/**
 * **axl_stderr_raw** — binary sibling of axl_stderr. Writes via the shell
 * StdErr handle (EFI_SHELL_PARAMETERS_PROTOCOL.StdErr), bypassing the
 * UTF-8->UCS-2 console conversion, so a tool can emit raw diagnostic bytes
 * under `2>`. Returns -1 when no shell StdErr handle is available.
 */
extern AxlStream *axl_stderr_raw;
```

- [ ] **Step 2: Add the raw stderr sink + stream** — in `src/stream/axl-stream.c`, after the existing `console_write_raw` / `mStdoutRaw`:

```c
static axl_ssize_t
console_write_err_raw(void *ctx, const void *data, size_t count)
{
    (void)ctx;
    if (data == NULL || count == 0) {
        return 0;
    }
    AxlFileHandle h = axl_backend_shell_stderr();
    if (h == NULL) {
        return -1;
    }
    size_t n = count;
    if (axl_backend_file_write(h, &n, data) != AXL_OK) {
        return -1;
    }
    return (axl_ssize_t)n;
}

static AxlStream mStderrRaw = {
    .ctx    = NULL,
    .read   = NULL,
    .write  = console_write_err_raw,
    .pread  = NULL,
    .pwrite = NULL,
    .close  = NULL,
};
```

- [ ] **Step 3: Define + init the global** — in `src/stream/axl-stream.c`, add `AxlStream *axl_stderr_raw = NULL;` beside the other stream globals, and in `axl_stream_init` add `axl_stderr_raw = &mStderrRaw;`.

- [ ] **Step 4: Bridge fallback for the driver** — in `src/backend/native/axl-backend-native.c`, change `axl_backend_shell_stderr` to consult the bridge when the image has no local StdErr handle (mirror `axl_backend_shell_stdin`'s pattern). Locate `axl_backend_shell_stderr` and replace its body:

```c
AxlFileHandle
axl_backend_shell_stderr(void)
{
    probe_shell_std_handles();
    if (mShellStdErr != NULL) {
        return (AxlFileHandle)mShellStdErr;   /* app/launcher: own params */
    }
    return bridge_lookup_stderr();            /* driver: live bridge consult */
}
```

Then add `bridge_lookup_stderr` beside the existing `bridge_lookup_stdin`, returning the live bridge's `stderr_h` (identical structure to `bridge_lookup_stdin`, but reading `->stderr_h`):

```c
static AxlFileHandle
bridge_lookup_stderr(void)
{
    AxlStdioBridge *b = bridge_find_live();   /* the helper bridge_lookup_stdin uses */
    return (b != NULL) ? b->stderr_h : NULL;
}
```

> Implementer note: v2.6.x refactored the bridge lookup — if a shared `bridge_find_live()` helper does not yet exist, extract one from `bridge_lookup_stdin` (returning the live `AxlStdioBridge *`) and have both stdin/stderr lookups call it. Confirm against the current `bridge_lookup_stdin` before writing.

- [ ] **Step 5: Build both arches**

Run: `make ARCH=x64 all 2>&1 | grep -iE "error|warning"; make ARCH=aa64 all 2>&1 | grep -iE "error|warning"`
Expected: clean.

- [ ] **Step 6: Extend the fixture with a raw-stderr assertion** — in `test/integration/io-streams.c`, add before `return 0;`:

```c
    axl_write(axl_stderr_raw, "RAW:err\n", 8);   /* binary -> shell StdErr handle */
```

and in `test/integration/test-io-redirect-qemu.sh`, add after the `err_has_log` line:

```bash
err_has_raw=$(grep -c 'RAW:err' <<<"$err" || true)
```

extend the printf and the GREEN condition to require `"$err_has_raw" -ge 1`.

- [ ] **Step 7: Run both arches GREEN**

Run: `timeout 120 ./test/integration/test-io-redirect-qemu.sh --arch X64 2>&1 | grep -E "Results:|test:"; timeout 200 ./test/integration/test-io-redirect-qemu.sh --arch AARCH64 2>&1 | grep -E "test:"`
Expected: `IO redirect test: OK` on both.

- [ ] **Step 8: Commit**

```bash
git add include/axl/axl-stream.h src/stream/axl-stream.c src/backend/native/axl-backend-native.c test/integration/io-streams.c test/integration/test-io-redirect-qemu.sh
git commit -m "stream: add axl_stderr_raw over the shell StdErr handle (+driver bridge fallback)"
```

---

### Task 5: Console mirror wraps ConErr

**Files:**
- Modify: `src/gfx/axl-console-mirror.c` (wrap `gST->StdErr` alongside `gST->ConOut`)
- Test: extend an existing console-mirror QEMU fixture, or assert via `test-io-redirect-qemu.sh` under an installed mirror.

**Interfaces:**
- Consumes: the mirror's existing ConOut-wrap install/uninstall.

- [ ] **Step 1: Read the current mirror** — `src/gfx/axl-console-mirror.c` (or wherever `AxlConsoleMirror` installs its ConOut wrapper). Identify where it saves `gST->ConOut` and swaps in its wrapper.

- [ ] **Step 2: Mirror StdErr too** — at install, if `gST->StdErr != NULL && gST->StdErr != gST->ConOut`, save it and swap in the same (or a sibling) wrapper so mirrored terminals see stderr; at uninstall, restore it. Guard the `StdErr == ConOut` alias case (some firmware) to avoid double-wrapping.

- [ ] **Step 3: Build both arches**

Run: `make ARCH=x64 all 2>&1 | grep -iE "error|warning"; make ARCH=aa64 all 2>&1 | grep -iE "error|warning"`
Expected: clean.

- [ ] **Step 4: Run the console-mirror fixture(s)**

Run: `timeout 120 ./test/integration/test-console-mirror-qemu.sh --arch X64 2>&1 | grep -E "OK|FAIL"`
Expected: OK (no regression); if the fixture can assert a stderr line is mirrored, add that assertion.

- [ ] **Step 5: Commit**

```bash
git add src/gfx/axl-console-mirror.c
git commit -m "console-mirror: also wrap gST->StdErr so stderr mirrors to remote terminals"
```

---

### Task 6: Docs + CHANGELOG

**Files:**
- Modify: `src/stream/README.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update the stream README** — in `src/stream/README.md`, add a "Standard streams and their sinks" subsection with the sink table (stdin=StdIn handle; stdout text=ConOut/`>`; stderr text=StdErr/`2>`; raw variants=shell handles) and the redirect-encoding boundary note: `>` writes UCS-2+BOM, `>a` writes ASCII, **never UTF-8 — for a UTF-8 file use `axl_fopen`**; diagnostics go to stderr.

- [ ] **Step 2: Add the CHANGELOG entry** — under a new `## Unreleased` block, add `### Changed`:

```markdown
### Changed

- **`axl_stderr` now writes the error console (`gST->StdErr`), not stdout.**
  `2>` redirection captures stderr and a plain `>` no longer does — matching
  POSIX. Diagnostic logging (`axl_log` / `axl_warning`) moved to stderr for
  the same reason, so `tool > out.txt` keeps `out.txt` free of AXL log lines.
  Scripts that scraped logs from a `>`-redirected file must use `2>`.

### Added

- **`axl_stderr_raw`** — binary stderr over the shell StdErr handle (sibling
  of `axl_stdout_raw`); works in a resident shared-driver via the stdio bridge.
```

- [ ] **Step 3: Gates**

Run: `make check-ascii 2>&1 | tail -1; make check-docs 2>&1 | tail -1`
Expected: both clean.

- [ ] **Step 4: Commit**

```bash
git add src/stream/README.md CHANGELOG.md
git commit -m "docs: stderr sink + redirect-encoding boundary; CHANGELOG for the stderr change"
```

---

## Self-Review

- **Spec coverage (§4.2, §4.3, §4.5, §8):** stderr→ConErr (Task 2), logs→stderr (Task 3), `axl_stderr_raw` + driver fallback (Task 4), mirror→ConErr (Task 5), behavior-change docs (Task 6). stdin two-path and `axl_print`-through-encoding (§4.1/§4.4) are deliberately **out of Phase 1** — stdin is unchanged and the encoding-layer routing is a non-behavioral refactor deferred to avoid regression risk here; note for a later phase if desired.
- **Placeholders:** none — every code step shows complete code; Task 4/5 carry an explicit "read current code first" note because v2.6.x refactored the bridge lookup and the mirror internals, which is a grounding instruction, not a placeholder.
- **Type consistency:** `axl_backend_console_write_err` (declared Task 2, used Task 3); `console_write_err`/`console_write_err_raw`/`mStderrRaw`/`axl_stderr_raw` consistent across Task 2/4; `bridge_lookup_stderr`/`bridge_find_live` flagged for grounding in Task 4.
- **Risk:** `gST->StdErr == gST->ConOut` aliasing on some firmware — Task 5 guards it; Task 2's fallback covers `StdErr == NULL`. Both must be exercised (the fixture runs on OVMF + AAVMF; add a BDS/no-shell note if a null-StdErr firmware is available).
</content>
