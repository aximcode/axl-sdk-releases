# Axl9p Phase 5 — `9p` launcher tool + resident serve/mount drivers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the `9p` first-class SDK tool — a thin `AxlArgs` verb-tree launcher over the Phase 1-4 library — with `serve` and `mount` running as resident embedded-DXE drivers via `AxlService`, so a UEFI box can export a volume to a Linux `mount -t 9p` peer, or mount a remote 9P share as an `fsN:` volume, from one command in the Shell.

**Architecture:** Three foreground sources plus a shared header. `tools/9p.c` owns the verb tree and the one-shot verbs (`ls`/`get`/`put`) which are plain synchronous `Axl9pClient` calls. `tools/9p-serve-svc.c` and `tools/9p-mount-svc.c` are each **dual-compiled** (the `service-demo.c` pattern): once with `-DAXL_SERVICE_BUILD_DRIVER` to produce `9p-serve-dxe.efi` / `9p-mount-dxe.efi` (subsystem 11, `AXL_SERVICE_DRIVER` emits DriverEntry), and once without it into the `9p.efi` app, which carries the driver images as `.incbin` blobs and launches them with `axl_service_start_embedded`. Options cross the launcher→driver boundary as `AxlConfigDesc`-described LoadOptions; the driver's `setup()` brings the NIC up and calls the library, `teardown()` reverses it.

**Tech Stack:** C (public `<axl.h>` only — the tool is an SDK consumer and must not reach into `src/`), `AxlArgs` verb tree, `AxlService` + `AXL_SERVICE_DRIVER` + `AXL_EMBED_DECLARE`, `AxlNetOpts`, `Axl9pClient` / `Axl9pServer`, GNU make (`EMBED_BLOB`), QEMU integration harness (`common-test.sh`) with the existing host `p9-server.py` (guest-connects-out) and `p9-client.py` (guest-listens) fixtures.

## Global Constraints

- **Public headers only in `tools/`.** `#include <axl.h>`; never `src/9p/axl-9p-internal.h`. The tool is the dogfooding proof that the public API is sufficient. (spec §8)
- **No new library API.** Phase 5 is launcher + build + docs, plus the one library **behavior** fix in Task 4 (which adds no new function, type or field to any public header). If a verb cannot be implemented over the existing public API, that is a finding to report, not a licence to extend `include/axl/`. (settled scope; see "Deviations" below)
- **Both arches mandatory** for every test run: `./test/integration/<script> ` and `./test/integration/<script> --arch AARCH64`. Cross-compile with `make ARCH=aa64`.
- **Exact-string assertions.** Guest output lines are matched with `grep -Fxq` (whole line) in the harness. Never a substring match. A tool line whose exact text is asserted must be printed with a literal format string, not assembled from a value that could silently change shape.
- **No `(void)`-cast on discarded call returns** (`feedback_no_void_cast_on_calls`). Must-check returns get **checked**, not cast away. `(void)param;` for genuinely unused parameters stays.
- **Style:** `docs/AXL-Coding-Style.md` — `axl_snake_case` functions, `AxlPascalCase` types, 4-space indent, K&R braces, multi-line function signatures, file order = header comment / includes / macros / types / globals / static prototypes / implementations.
- **ASCII gate:** `make check-ascii` must pass (no non-ASCII in sources). Run before every commit.
- **Docs gate:** `make check-docs` must pass. No new public header ships in this phase, so this is a regression guard, not new work.
- **9P default port is 564.** `--port 0` is not "any port" anywhere in this tool; a 0 reaches `axl_9p_server_listen` as "use 564".
- Service identity is derived from `AxlService.name` via `axl_guid_v5`. The two services MUST have distinct names: `"9p-serve"` and `"9p-mount"`. Two services sharing a name would resolve to one GUID and `serve-stop` would unload the mount.

## Deviations from the design spec (§8), decided up front

Record these in the design doc in Task 5 — do not quietly reword the spec.

1. **`--listen-ip` (serve) and `--source-ip` (mount) are NOT implemented.** Spec §8 lists both. The public API accepts neither: `axl_9p_server_listen(s, port)` has no bind address, and `axl_9p_connect(host, port, uname, aname, out)` has no source-IP parameter (the underlying `axl_tcp_connect_via` does, but `axl_9p_connect` does not expose it). Implementing them means new library API, which is out of Phase 5's settled scope. `--nic` IS implemented on every networked verb and is the working interface-selection knob.
2. **`9p mount` never supervises.** Residency is the whole point — the verb returns to the Shell prompt with `fsN:` live. `9p serve` supervises by default (a server you can Ctrl-C, like every other server) with `--detach` to background it.
3. **A `status` verb is added** (not in spec §8). It reports both services in one call; `axl_service_is_running` makes it ~10 lines and it is what the integration harness uses to prove the driver is actually resident.
4. **`9p` is excluded from the busybox multiplexer** (`TOOL_NAMES`), per spec §8. It is a first-class tool with its own Makefile recipe (like `mkrd` / `fbcon`) because it links two embedded driver blobs.
5. **The `EXDEV` client fallback is IN scope** (Task 4), by explicit project-owner decision on 2026-07-22. Phase 4 recorded it as a documented gap and a Phase-5-or-v2 candidate; `9p mount` is what makes it user-visible (a cross-directory `mv` from the Shell over a mount of an AXL server fails today), so it ships with the verb that exposes it.

## File Structure

**This layout mirrors `axl-webfs`, the architectural template named in spec §3 — verified against the repo, not its summary.** The correspondence is exact and worth keeping exact, because the next person to read either project should recognize the other:

| axl-webfs | this plan | responsibility |
|---|---|---|
| `src/app/main.c` | `tools/9p.c` | verb tree + `main()`, nothing else |
| `src/app/cmd-serve.c` | `tools/9p-cmd-serve.c` | `serve` / `serve-stop` handlers, flags, `AXL_EMBED_DECLARE`, deploy |
| `src/app/cmd-mount.c` | `tools/9p-cmd-mount.c` | `mount` / `umount` handlers, flags, `AXL_EMBED_DECLARE`, deploy |
| `src/serve/webfs-serve.h` | `tools/9p-serve-svc.h` | opts struct + `extern` descriptor/descs/opts |
| `src/serve/webfs-serve.c` | `tools/9p-serve-svc.c` | dual-compiled service |
| `src/mount/webfs-mount.{h,c}` | `tools/9p-mount-svc.{h,c}` | ditto for mount |
| *(no analog — webfs has no one-shot verbs)* | `tools/9p-cmd-file.c` | `ls` / `get` / `put` handlers |
| *(no analog — webfs duplicates these rows)* | `tools/9p-common.{h,c}` | the DRY seam (below) |

`tools/` is flat in this repo (`fbcon.c` / `fbcon-drv.c` / `fbcon-marker.h`), so the directory structure becomes a filename prefix. Same division of responsibility, local naming convention.

- `tools/9p.c` — **Create.** `main()` + the `AxlArgsNode verbs[]` tree + the `status` handler. No verb logic beyond `status` (which is three lines over a shared helper). Mirrors `main.c`, which likewise keeps only its small `list-nics` handler inline.
- `tools/9p-common.h` — **Create.** The DRY seam between the two services and the two cmd files: `AXL_9P_PORT_DEFAULT` / `AXL_9P_PORT_DEFAULT_STR` (564, needed by both services *and* the `host[:port]` splitter), the `AXL_9P_NET_CFG_DESCS(T)` and `AXL_9P_NET_ARG_FLAGS` macros, and `axl9p_report_service`.
- `tools/9p-common.c` — **Create.** `axl9p_report_service` — the one body that is genuinely identical at both call sites.
- `tools/9p-cmd-file.c` — **Create.** The three one-shot handlers, their flags/positionals, and the two helpers only they use (`split_host_port`, `open_session`). Single TU, so those two stay `static`.
- `tools/9p-cmd-serve.c` — **Create.** `serve` / `serve-stop` handlers, `axl9p_serve_flags`, the `serve` positional, `AXL_EMBED_DECLARE(serve9p_dxe)`, and the `AxlServiceDeploy` it builds. Reaches `g_serve9p_opts` / `serve9p_service` via `tools/9p-serve-svc.h`.
- `tools/9p-cmd-mount.c` — **Create.** Same for `mount` / `umount`.
- `tools/9p-serve-svc.h` — **Create.** `Serve9pOpts` + `extern` declarations of `g_serve9p_opts`, `serve9p_descs`, `serve9p_service`. Documents the cross-binary ABI rule (both images built from one tree, identical flags but for the `-D` toggle), as `webfs-serve.h` does.
- `tools/9p-serve-svc.c` — **Create.** Dual-compiled. `Serve9pOpts g_serve9p_opts`, `serve9p_descs[]` and `const AxlService serve9p_service` are defined **unconditionally** so the launcher-side compile carries the descriptor the cmd file needs; `serve_setup` / `serve_teardown`, the `.setup` / `.teardown` initializer members, and `AXL_SERVICE_DRIVER` are gated on `AXL_SERVICE_BUILD_DRIVER` so the launcher does not drag in `Axl9pServer`. This gating pattern is `webfs-serve.c:24-1133` — follow it precisely, including gating the initializer *members*, not just the functions.
- `tools/9p-mount-svc.h` / `.c` — **Create.** Same shape for `Mount9pOpts` / `mount9p_service`.

**The DRY seam, and its limits.** `axl-webfs` duplicates its per-service descriptor rows and its start/stop verb bodies between `cmd-serve.c` and `cmd-mount.c`. Two things there are worth factoring rather than copying:

- **The `nic` / `port` descriptor rows**, which appear in two `AxlConfigDesc` tables and two `AxlArgDesc` tables and are pure duplication that silently drifts. `AXL_9P_NET_CFG_DESCS(T)` takes the opts type as a parameter because each `offsetof` needs its own struct; `AXL_9P_NET_ARG_FLAGS` needs no parameter.
- **`axl9p_report_service`**, called identically from `status` for both services.

Two things are **not** worth factoring, and the plan deliberately leaves them duplicated: the `start` and `stop` verb bodies. They differ in every message they print and in what they report on success (`serve` prints root and port; `mount` prints the discovered volume name), so a shared helper would take three or four message strings as parameters and save nothing. Task 3 Step 9 re-examines this with both bodies actually written — that is the honest moment to decide, not now.
- `Makefile` — **Modify.** Twin-compile + `EMBED_BLOB` for both drivers, the `9p.efi` link rule, `9p` phony target, add to `tools:` and `clean-tools`, add to `.PHONY`.
- `test/integration/test-9p-tool-qemu.sh` — **Create.** Guest-connects-out topology (host `p9-server.py`): the one-shot verbs (Task 1) and `mount` / `umount` (Task 3).
- `test/integration/test-9p-tool-serve-qemu.sh` — **Create.** Guest-listens topology (host `p9-client.py` over a QEMU port forward): `serve` / `status` / `serve-stop` (Task 2).
- `src/9p/axl-9p-internal.h` — **Modify (Task 4).** One `uint32_t last_errno` field on `struct Axl9pClient` so a caller can branch on *which* `Rlerror` came back, not just that one did.
- `src/9p/axl-9p-client.c` — **Modify (Task 4).** Record the errno in `axl_9p_transact`; add the `EXDEV` copy-then-unlink fallback to `axl_9p_rename`.
- `include/axl/axl-9p.h` — **Modify (Task 4).** `axl_9p_rename`'s docstring: the contract changes, so the doc changes in the same commit.
- `test/integration/p9-server.py` — **Modify (Task 4).** Answer `Rlerror(EXDEV)` on a cross-directory `Trename`, matching what AXL's own server does — the fixture is currently more permissive than the real server, which is why this gap survived Phase 2.
- `test/unit/axl-test-net.c` — **Modify (Task 4).** A cross-directory rename case in the `9p-client` mode.
- `src/9p/README.md` — **Modify.** Replace the "There is no standalone `9p` tool/launcher yet (Phase 5)" paragraph with a Tool section.
- `docs/superpowers/specs/2026-07-19-axl-9p-design.md` — **Modify.** Phase 5 marked DONE + the deviations above.
- `docs/ROADMAP.md` — **Modify.** Add the Axl9p tracker entry (the module has no entry at all today).
- `README.md` — **Modify.** One row in the tools table.
- `devkit.conf` — **Modify.** A `desc:` line and a `binary` line.

---

## Task 1: `9p.efi` skeleton + the one-shot verbs (`ls` / `get` / `put`)

A runnable `9p.efi` with a working verb tree and the three synchronous client verbs, proven against the existing host `p9-server.py` fixture. No drivers yet — this task's binary links no blobs.

**Files:**
- Create: `tools/9p.c`
- Create: `tools/9p-common.h`
- Create: `tools/9p-cmd-file.c`
- Create: `test/integration/test-9p-tool-qemu.sh`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `<axl.h>` public API — `axl_args_run`, `axl_net_init`, `axl_9p_connect`, `axl_9p_disconnect`, `axl_9p_read_file`, `axl_9p_write_file`, `axl_9p_list`, `axl_bytes_get_data`, `axl_bytes_unref`, `axl_array_len`, `axl_array_get`, `axl_array_free`, `axl_file_get_contents`, `axl_file_set_contents`, `axl_str_to_u16`, `axl_strchr`, `axl_strlcpy`, `axl_fs_entry_is_dir`.
- Produces (for Tasks 2 and 3):
  - `tools/9p.c`'s `verbs[]` tree — later tasks add entries pointing at handlers declared in the per-service headers.
  - `tools/9p-common.h`'s `AXL_9P_PORT_DEFAULT` (`564`) and `AXL_9P_PORT_DEFAULT_STR` (`"564"`), used by both services' descriptor defaults.
  - `extern const AxlArgDesc axl9p_file_flags[];` and the three `extern` positional arrays plus `axl9p_ls_handler` / `axl9p_get_handler` / `axl9p_put_handler`, declared in `tools/9p-common.h` so `tools/9p.c` can build the tree without seeing their bodies.

**Do NOT create `tools/9p-common.c` in this task.** `9p-common.h` here is header-only (two macros' worth of constants plus declarations). The shared *code* — `axl9p_report_service` and the net-descriptor macros — arrives in Task 2, when a second call site actually exists. Factoring a helper with one caller is the speculative generality this project explicitly pushes back on.

- [ ] **Step 1: Write the failing integration test**

Create `test/integration/test-9p-tool-qemu.sh`. Mode 0755 (`chmod +x`).

```bash
#!/bin/bash
# test-meta: arch=both needs= est=70 local-only=0
# test-9p-tool-qemu.sh -- the `9p` TOOL against a host 9P server. Same
# guest-connects-out topology as test-9p-qemu.sh (p9-server.py on the host,
# reached at 10.0.2.2:<port> through QEMU user-net), but the guest runs the
# shipped tool from the Shell instead of a purpose-built selftest app -- so
# what is under test is the launcher: argv parsing, host[:port] splitting,
# network bring-up, and the printed contract a human reads.
#
# Covers the one-shot verbs: `ls` (entry lines, exact), `get` to stdout and
# `get` to a file, and `put` from a staged file with a `get` read-back that
# proves the bytes reached the server.
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1) -- integration, not a
# unit count.
#
# Usage: ./test/integration/test-9p-tool-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all 9p 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/tools/9p.efi"

# The source file for `9p put`, staged with EXACT bytes -- written from the
# host rather than with the Shell's `echo >a`, which appends CRLF and would
# make the read-back assertion depend on line-ending trivia.
printf 'put-ok-9p' > "$TEST_STAGING/putsrc.txt"

P9_PORT=$(test_port 0)
P9_PID=0
python3 "$(dirname "$0")/p9-server.py" "$P9_PORT" &
P9_PID=$!
trap 'test_cleanup; [[ $P9_PID -gt 0 ]] && kill $P9_PID 2>/dev/null || true' EXIT

# Interpolated heredoc (unquoted) so $P9_PORT lands in the startup script.
cat << NSHEOF | test_set_startup
@echo -off
fs0:
cd \\

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo TOOL-LS
9p.efi ls 10.0.2.2:${P9_PORT} /
echo TOOL-GET-STDOUT
9p.efi get 10.0.2.2:${P9_PORT} /hello.txt
echo TOOL-GET-FILE
9p.efi get 10.0.2.2:${P9_PORT} /hello.txt fs0:\\got.txt
echo TOOL-PUT
9p.efi put fs0:\\putsrc.txt 10.0.2.2:${P9_PORT} /puttest.txt
echo TOOL-PUT-RB
9p.efi get 10.0.2.2:${P9_PORT} /puttest.txt
echo TOOL-DONE
reset -s
NSHEOF

test_build_image

echo "=== 9p tool one-shot verbs ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID; 9P server PID: $P9_PID on host :$P9_PORT"

if ! test_wait_for "TOOL-DONE" 90; then
    echo "FAIL: the tool run did not finish within 90 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

# Whole-line match against the guest's serial output. A substring match would
# let a wrong size or a wrong name hide inside a right-looking line.
assert_line() {
    if grep -Fxq "$1" "$TEST_CLEAN_LOG"; then
        pass "$1"
    else
        fail "$1"
    fi
}

echo ""
echo "  --- one-shot verbs ---"

# p9-server.py's root: hello.txt (14 B), dir/ (a dir), readonly.txt (18 B).
assert_line "f 14 hello.txt"
assert_line "d 0 dir"
assert_line "f 18 readonly.txt"

# `get` with no outfile streams the bytes to stdout.
assert_line "hello from 9p"

# `get` with an outfile reports what it wrote, and nothing else.
assert_line "9p: wrote 14 bytes to fs0:\\got.txt"

# `put` reports what it sent; the read-back proves the bytes reached the
# server rather than only the tool's own opinion of success.
assert_line "9p: put 9 bytes to /puttest.txt"
assert_line "put-ok-9p"

echo ""
printf "9p tool one-shot verbs: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 7 ]] && exit 0 || exit 1
```

- [ ] **Step 2: Run it to confirm RED**

```bash
chmod +x test/integration/test-9p-tool-qemu.sh
./test/integration/test-9p-tool-qemu.sh
```

Expected: FAIL at `make ... 9p` / `Not found: .../tools/9p.efi` — there is no `9p` target and no `tools/9p.c`. That is the RED we want; the harness must fail because the tool is absent, not because the assertions are wrong.

- [ ] **Step 3a: Write `tools/9p-common.h`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-common.h - shared surface of the `9p` tool's translation units.
 *
 * The tool is split the way axl-webfs splits: 9p.c is the verb tree,
 * 9p-cmd-*.c are the verb handlers, and 9p-{serve,mount}-svc.c are the
 * dual-compiled services (each also linked into its own DXE driver
 * image). This header carries what more than one of those TUs needs.
 */

#ifndef AXL_TOOLS_9P_COMMON_H
#define AXL_TOOLS_9P_COMMON_H

#include <axl.h>

/* The 9P well-known port. Needed by the host[:port] splitter and, as a
   string, by both services' AxlConfigDesc defaults - which take a
   compile-time const char *, so the two forms are kept adjacent to make a
   mismatch obvious. */
#define AXL_9P_PORT_DEFAULT      564
#define AXL_9P_PORT_DEFAULT_STR  "564"

// ---------------------------------------------------------------------------
// One-shot verbs - implemented in 9p-cmd-file.c
// ---------------------------------------------------------------------------

/// `--nic`, shared by all three one-shot verbs.
extern const AxlArgDesc axl9p_file_flags[];

extern const AxlArgDesc axl9p_ls_positional[];
extern const AxlArgDesc axl9p_get_positional[];
extern const AxlArgDesc axl9p_put_positional[];

/// @brief `9p ls <host>[:port] [path]` - list a remote directory.
/// @return 0 on success, 1 on failure.
int
axl9p_ls_handler(
    AxlArgs *a   ///< parsed `ls` verb arguments
);

/// @brief `9p get <host>[:port] <path> [outfile]` - read a remote file.
/// @return 0 on success, 1 on failure.
int
axl9p_get_handler(
    AxlArgs *a   ///< parsed `get` verb arguments
);

/// @brief `9p put <infile> <host>[:port] <path>` - write a remote file.
/// @return 0 on success, 1 on failure.
int
axl9p_put_handler(
    AxlArgs *a   ///< parsed `put` verb arguments
);

#endif /* AXL_TOOLS_9P_COMMON_H */
```

- [ ] **Step 3b: Write `tools/9p-cmd-file.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-cmd-file.c - the `9p` one-shot verbs: ls, get, put.
 *
 * Each opens a synchronous Axl9pClient session, does one thing, and
 * disconnects - no driver, no residency, nothing left behind. That is the
 * whole difference from serve/mount, which deploy resident DXE drivers
 * (9p-cmd-serve.c, 9p-cmd-mount.c).
 *
 * Server addresses are written host[:port]; the port defaults to the 9P
 * well-known 564.
 */

#include <axl.h>

#include "9p-common.h"

AXL_LOG_DOMAIN("9p");

#define P9_HOST_MAX 128

// ---------------------------------------------------------------------------
// Session helpers - used only by this file's three verbs
// ---------------------------------------------------------------------------

/* Split "host" or "host:port" into its parts. IPv4 / hostname only (9P over
   IPv6 is not offered by the library), so the FIRST colon is the separator -
   there is no bracketed-literal form to disambiguate. */
static bool
split_host_port(
    const char *spec,
    char       *host,
    size_t      host_cap,
    uint16_t   *port
)
{
    const char *colon;
    size_t      host_len;

    if (spec == NULL || spec[0] == '\0') {
        return false;
    }
    *port = AXL_9P_PORT_DEFAULT;
    colon = axl_strchr(spec, ':');
    if (colon == NULL) {
        return axl_strlcpy(host, spec, host_cap) < host_cap;
    }
    if (axl_str_to_u16(colon + 1, 10, port, NULL) != AXL_OK || *port == 0) {
        return false;
    }
    host_len = (size_t)(colon - spec);
    if (host_len == 0 || host_len >= host_cap) {
        return false;
    }
    axl_memcpy(host, spec, host_len);
    host[host_len] = '\0';
    return true;
}

/* Bring the NIC up and open a session. Returns NULL after reporting the
   failure itself, so every verb's error path is one `if`. */
static Axl9pClient *
open_session(
    AxlArgs    *a,
    const char *spec
)
{
    char         host[P9_HOST_MAX];
    uint16_t     port = 0;
    Axl9pClient *c    = NULL;

    if (!split_host_port(spec, host, sizeof(host), &port)) {
        axl_printerr("9p: bad server address '%s' (want host or host:port)\n",
                     spec != NULL ? spec : "");
        return NULL;
    }
    if (axl_net_init(axl_args_get_uint(a, "nic"), 10) != AXL_OK) {
        axl_printerr("9p: could not bring a NIC online\n");
        return NULL;
    }
    if (axl_9p_connect(host, port, "", NULL, &c) != AXL_OK) {
        axl_printerr("9p: connect to %s:%u failed\n", host, (unsigned)port);
        return NULL;
    }
    return c;
}

// ---------------------------------------------------------------------------
// Verbs
// ---------------------------------------------------------------------------

int
axl9p_ls_handler(
    AxlArgs *a
)
{
    Axl9pClient *c;
    AxlArray    *entries = NULL;
    const char  *path    = axl_args_get_string(a, "path");
    size_t       i;
    int          rc      = 0;

    c = open_session(a, axl_args_get_string(a, "server"));
    if (c == NULL) {
        return 1;
    }
    if (axl_9p_list(c, path, &entries) != AXL_OK) {
        axl_printerr("9p: cannot list %s\n", path);
        axl_9p_disconnect(c);
        return 1;
    }
    for (i = 0; i < axl_array_len(entries); i++) {
        const AxlFsEntry *e = (const AxlFsEntry *)axl_array_get(entries, i);
        axl_printf("%c %llu %s\n",
                   axl_fs_entry_is_dir(e) ? 'd' : 'f',
                   (unsigned long long)e->size, e->name);
    }
    axl_array_free(entries);
    axl_9p_disconnect(c);
    return rc;
}

int
axl9p_get_handler(
    AxlArgs *a
)
{
    Axl9pClient *c;
    AxlBytes    *data    = NULL;
    const char  *path    = axl_args_get_string(a, "path");
    const char  *outfile = axl_args_get_string(a, "outfile");
    const void  *buf;
    size_t       len     = 0;
    int          rc      = 0;

    c = open_session(a, axl_args_get_string(a, "server"));
    if (c == NULL) {
        return 1;
    }
    if (axl_9p_read_file(c, path, &data) != AXL_OK) {
        axl_printerr("9p: cannot read %s\n", path);
        axl_9p_disconnect(c);
        return 1;
    }
    buf = axl_bytes_get_data(data, &len);
    if (outfile != NULL && outfile[0] != '\0') {
        if (axl_file_set_contents(outfile, buf, len) == AXL_OK) {
            axl_printf("9p: wrote %llu bytes to %s\n",
                       (unsigned long long)len, outfile);
        } else {
            axl_printerr("9p: cannot write %s\n", outfile);
            rc = 1;
        }
    } else {
        axl_printf("%.*s", (int)len, (const char *)buf);
    }
    axl_bytes_unref(data);
    axl_9p_disconnect(c);
    return rc;
}

int
axl9p_put_handler(
    AxlArgs *a
)
{
    Axl9pClient *c;
    const char  *infile = axl_args_get_string(a, "infile");
    const char  *path   = axl_args_get_string(a, "path");
    void        *buf    = NULL;
    size_t       len    = 0;
    int          rc     = 0;

    if (axl_file_get_contents(infile, &buf, &len) != AXL_OK) {
        axl_printerr("9p: cannot read %s\n", infile);
        return 1;
    }
    c = open_session(a, axl_args_get_string(a, "server"));
    if (c == NULL) {
        axl_free(buf);
        return 1;
    }
    if (axl_9p_write_file(c, path, buf, len) == AXL_OK) {
        axl_printf("9p: put %llu bytes to %s\n",
                   (unsigned long long)len, path);
    } else {
        axl_printerr("9p: cannot write %s on the server\n", path);
        rc = 1;
    }
    axl_free(buf);
    axl_9p_disconnect(c);
    return rc;
}

// ---------------------------------------------------------------------------
// Flags and positionals - non-static; 9p.c builds the verb tree from them
// ---------------------------------------------------------------------------

const AxlArgDesc axl9p_file_flags[] = {
    { .name = "nic", .short_name = 'n', .type = AXL_ARG_U64,
      .default_value = AXL_NET_NIC_AUTO_STR,
      .help = "NIC ordinal to bring up (default: first usable)" },
    { 0 }
};

const AxlArgDesc axl9p_ls_positional[] = {
    { .name = "server", .type = AXL_ARG_STRING, .required = true,
      .help = "server address as host or host:port" },
    { .name = "path",   .type = AXL_ARG_STRING, .default_value = "/",
      .help = "directory on the server" },
    { 0 }
};

const AxlArgDesc axl9p_get_positional[] = {
    { .name = "server",  .type = AXL_ARG_STRING, .required = true,
      .help = "server address as host or host:port" },
    { .name = "path",    .type = AXL_ARG_STRING, .required = true,
      .help = "file on the server" },
    { .name = "outfile", .type = AXL_ARG_STRING,
      .help = "local destination (default: write to stdout)" },
    { 0 }
};

const AxlArgDesc axl9p_put_positional[] = {
    { .name = "infile", .type = AXL_ARG_STRING, .required = true,
      .help = "local file to send" },
    { .name = "server", .type = AXL_ARG_STRING, .required = true,
      .help = "server address as host or host:port" },
    { .name = "path",   .type = AXL_ARG_STRING, .required = true,
      .help = "destination path on the server" },
    { 0 }
};
```

- [ ] **Step 3c: Write `tools/9p.c`**

The verb tree and nothing else — the shape of `axl-webfs`'s `src/app/main.c`.

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p.c - 9P2000.L client and server for the UEFI Shell: the verb tree.
 *
 * A thin launcher over <axl/axl-9p.h>. Two shapes of verb:
 *
 *   One-shot   ls / get / put - a synchronous Axl9pClient session per
 *              invocation. Handlers in 9p-cmd-file.c.
 *
 *   Resident   serve / mount - deploy an embedded DXE driver via
 *              AxlService so the export (or the fsN: volume) outlives the
 *              command. Handlers in 9p-cmd-serve.c / 9p-cmd-mount.c; the
 *              services themselves in 9p-{serve,mount}-svc.c, each also
 *              compiled into its own driver image.
 *
 * Same file split as axl-webfs (src/app/main.c + cmd-*.c + the
 * dual-compiled service sources), which is this tool's template.
 */

#include <axl.h>

#include "9p-common.h"

static const AxlArgsNode verbs[] = {
    { .name = "ls",  .help = "List a directory on a 9P server",
      .flags = axl9p_file_flags, .positionals = axl9p_ls_positional,
      .handler = axl9p_ls_handler },
    { .name = "get", .help = "Read a file from a 9P server",
      .flags = axl9p_file_flags, .positionals = axl9p_get_positional,
      .handler = axl9p_get_handler },
    { .name = "put", .help = "Write a local file to a 9P server",
      .flags = axl9p_file_flags, .positionals = axl9p_put_positional,
      .handler = axl9p_put_handler },
    { 0 }
};

AXL_TOOL_MAIN(9p)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "9p",
        .help        = "9P2000.L client and server",
        .help_prolog =
            "Speaks 9P2000.L over TCP. The one-shot verbs (ls/get/put) open "
            "a session, act, and exit. Server addresses are host[:port]; the "
            "port defaults to 564.",
        .help_epilog =
            "Examples:\n"
            "  9p ls 10.0.0.5 /\n"
            "  9p get 10.0.0.5:5640 /hello.txt fs0:\\hello.txt\n"
            "  9p put fs0:\\log.txt 10.0.0.5 /log.txt",
        .verbs       = verbs,
    });
}
```

**Notes for the implementer:**
- The port constants live in `tools/9p-common.h`, deliberately **not** in `<axl/axl-9p.h>` — no new public library API this phase. Check whether `<axl/axl-9p.h>` already exports an equivalent (`grep -n DEFAULT_PORT include/axl/axl-9p.h`); if it does, use it and drop the local definitions rather than shadowing them.
- `AXL_TOOL_MAIN(9p)` token-pastes `_axl_tool_body_` with `9p`. The result is a valid identifier and GCC accepts it, but **verify by compiling**. If it does not compile, drop the macro and spell the entry point out:
  ```c
  int
  main(int argc, char **argv)
  {
      if (axl_version_handle("9p", argc, argv)) {
          return 0;
      }
      return run_9p(argc, argv);
  }
  ```
  with the body moved into `static int run_9p(int argc, char **argv)`. Report which you did.
- Verify the `AxlArgDesc` field names (`required`, `default_value`, `short_name`, `type`, `help`) against `include/axl/axl-args.h` before writing — copy the shape from `tools/tar.c` if they differ.
- **`AXL_ARG_U64` may not exist.** `AXL_ARG_U16` and `AXL_ARG_U32` are both known to (axl-webfs's `cmd-serve.c` uses each), and `tools/netload.c:123` uses `AXL_ARG_U32` with `.base = 10`. Check `include/axl/axl-args.h`. If there is no 64-bit arg type, **report it as a finding rather than downgrading to `AXL_ARG_U32`** — `AXL_NET_NIC_AUTO` is `UINT64_MAX`, so a 32-bit parse of `AXL_NET_NIC_AUTO_STR` either fails or truncates to 4294967295, which is a real NIC ordinal request, not "auto". A wrong choice here silently breaks NIC auto-selection on every verb.
  For reference, axl-webfs dodges this by making `--nic` default-less and mapping "unset" to the sentinel by hand:
  ```c
  opts.net.nic_index = axl_args_get_string(a, "nic") != NULL
                     ? axl_args_get_uint(a, "nic") : AXL_NET_NIC_AUTO;
  ```
  That is an acceptable fallback if no 64-bit type exists — but say so in the report, and use the same idiom in all three places (`9p-cmd-file.c`, `9p-cmd-serve.c`, `9p-cmd-mount.c`) rather than mixing conventions.

- [ ] **Step 4: Add the Makefile rule**

In `Makefile`, immediately after the `mkrd` special rule (the `$(BUILDDIR)/mkrd.o:` recipe, around line 2200), insert:

```make
# ===================================================================
# 9p -- 9P2000.L client/server launcher. A first-class tool with its
# own recipe (not in TOOL_NAMES) because it links two embedded DXE
# driver blobs -- 9p-serve-dxe and 9p-mount-dxe -- and so is excluded
# from the busybox multiplexer, which builds one .o per TOOL_NAMES
# entry from a single .c.
# ===================================================================
9p: $(PREFIX)/tools/9p.efi
	@echo "  Built: $(PREFIX)/tools/9p.efi"

# Launcher objects. Tasks 2 and 3 add 9p-cmd-serve.o / 9p-cmd-mount.o and
# the two dual-compiled service objects to this list.
NINEP_APP_OBJS = $(BUILDDIR)/9p.o $(BUILDDIR)/9p-cmd-file.o

$(PREFIX)/tools/9p.efi: $(NINEP_APP_OBJS) $(LINK_CRT0) \
                        $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_APP,$(NINEP_APP_OBJS),$@)

$(BUILDDIR)/9p.o: tools/9p.c tools/9p-common.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-cmd-file.o: tools/9p-cmd-file.c tools/9p-common.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
```

`NINEP_APP_OBJS` is a variable rather than an inline list precisely because Tasks 2 and 3 extend it — a plain list would be edited in two places (prerequisites and recipe) each time, which is how a link line and its dependencies drift apart.

Add `9p` to the `.PHONY:` line (around line 640), after `fbcon`. Add `$(PREFIX)/tools/9p.efi` to the `tools:` prerequisite list and mention it in the following `@echo`. Add `rm -f $(BUILDDIR)/9p*.o` to `clean-tools`.

- [ ] **Step 5: Build both arches and fix warnings**

```bash
make ARCH=x64 all 9p 2>&1 | tail -20
make ARCH=aa64 all 9p 2>&1 | tail -20
make check-ascii
```

Expected: both link cleanly, zero warnings. Fix any warning before moving on (project rule).

- [ ] **Step 6: Run the test on both arches to confirm GREEN**

```bash
timeout 300 ./test/integration/test-9p-tool-qemu.sh
timeout 300 ./test/integration/test-9p-tool-qemu.sh --arch AARCH64
```

Expected on each: `9p tool one-shot verbs: 7 passed, 0 failed (<ARCH>)`, exit 0.

If an `ls` size assertion fails, read the actual line out of the serial log and check it against `p9-server.py`'s tree (lines 64-69) before touching the assertion — the fixture's sizes are the oracle, and an assertion edited to match wrong output is exactly the failure this project's exact-string rule exists to prevent.

- [ ] **Step 7: Commit**

```bash
git status
git add tools/9p.c tools/9p-common.h tools/9p-cmd-file.c Makefile \
        test/integration/test-9p-tool-qemu.sh
git commit -m "9p tool: the one-shot verbs, over the public client API"
```

---

## Task 2: `serve` — the resident 9P server driver

`9p serve <root>` deploys `9p-serve-dxe.efi` as a resident driver that brings a NIC online, builds an `Axl9pServer` over `<root>` on the driver loop, and listens. `9p serve-stop` unloads it. `9p status` reports both services.

**Files:**
- Create: `tools/9p-serve-svc.h`
- Create: `tools/9p-serve-svc.c`
- Create: `tools/9p-cmd-serve.c`
- Create: `tools/9p-common.c`
- Modify: `tools/9p-common.h`
- Modify: `tools/9p.c`
- Modify: `Makefile`
- Create: `test/integration/test-9p-tool-serve-qemu.sh`

**Interfaces:**
- Consumes: Task 1's `verbs[]` tree, `AXL_9P_PORT_DEFAULT_STR`, and the `--nic` convention (including whichever `AXL_ARG_*` type Task 1 established for it — match it, do not re-decide).
- Produces:
  - In `tools/9p-serve-svc.h`: `typedef struct { AxlNetOpts net; const char *root; bool ro; } Serve9pOpts;` plus `extern Serve9pOpts g_serve9p_opts;`, `extern const AxlConfigDesc serve9p_descs[];`, `extern const AxlService serve9p_service;`.
  - In `tools/9p-common.h`: `AXL_9P_NET_CFG_DESCS(T)`, `AXL_9P_NET_ARG_FLAGS`, and `bool axl9p_report_service(const char *label, const AxlService *svc);` (returns the running state it printed). Task 3 consumes all three.
  - In `tools/9p-common.h`, for the tree: `extern const AxlArgDesc axl9p_serve_flags[];`, `extern const AxlArgDesc axl9p_serve_positional[];`, `int axl9p_serve_handler(AxlArgs *a);`, `int axl9p_serve_stop_handler(AxlArgs *a);`. Both handlers return a shell exit code (0/1), not an `AXL_*`.

- [ ] **Step 1: Write the failing integration test**

Create `test/integration/test-9p-tool-serve-qemu.sh`, mode 0755.

```bash
#!/bin/bash
# test-meta: arch=both needs= est=90 local-only=0
# test-9p-tool-serve-qemu.sh -- `9p serve` as a RESIDENT driver. The guest
# Shell runs `9p.efi serve fs0:\9pexport --detach`, which deploys the
# embedded 9p-serve-dxe.efi; the driver brings the NIC up, exports the
# staged tree and listens. The host then drives it with p9-client.py -- the
# same wire client Phase 4 is gated by -- over a QEMU port forward.
#
# What this test owns that test-9p-server-qemu.sh does not: the LAUNCHER.
# That the opts crossed the LoadOptions boundary (root, port, ro), that the
# driver stayed resident after the launching app exited, that `status` sees
# it, and that `serve-stop` actually tears the listener down rather than
# just unpublishing a marker. p9-client.py is run WITHOUT --ro-port, so only
# its functional suite runs (~2 s) -- the adversarial suite is Phase 4's
# gate and re-running it here would buy nothing for ~35 s.
#
# The export tree is staged from the HOST into the FAT image, so the guest
# needs no seeding step and the bytes are exact. It mirrors the tree
# p9-client.py's functional suite expects:
#   9pexport/hello.txt      "hello from 9p server\n"   (21 bytes)
#   9pexport/sub/inner.txt  "inner\n"                  (6 bytes)
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1).
#
# Usage: ./test/integration/test-9p-tool-serve-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=5640

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all 9p 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/tools/9p.efi"

mkdir -p "$TEST_STAGING/9pexport/sub"
printf 'hello from 9p server\n' > "$TEST_STAGING/9pexport/hello.txt"
printf 'inner\n'                > "$TEST_STAGING/9pexport/sub/inner.txt"

# Interpolated heredoc (unquoted) so $GUEST_PORT lands in the startup script.
cat << NSHEOF | test_set_startup
@echo -off
fs0:
cd \\

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo TOOL-SERVE
9p.efi serve fs0:\\9pexport --port ${GUEST_PORT} --detach
echo TOOL-STATUS-1
9p.efi status
echo TOOL-SERVE-READY
NSHEOF

test_build_image

echo "=== 9p serve resident driver ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, host :$HOST_PORT -> guest :$GUEST_PORT"

if ! test_wait_for "TOOL-SERVE-READY" 120; then
    echo "FAIL: `9p serve` did not reach the ready marker within 120 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

CLIENT_OUT="$TEST_TMPDIR/p9-client-tool.out"
CLIENT_RC=0
timeout 90 python3 "$(dirname "$0")/p9-client.py" 127.0.0.1 "$HOST_PORT" \
    > "$CLIENT_OUT" 2>&1 || CLIENT_RC=$?

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

assert_guest_line() {
    if grep -Fxq "$1" "$TEST_CLEAN_LOG"; then
        pass "guest: $1"
    else
        fail "guest: $1"
    fi
}

assert_client_line() {
    if grep -Fxq "$1" "$CLIENT_OUT"; then
        pass "client: $1"
    else
        fail "client: $1  [$(grep -F "DIAG [$1]" "$CLIENT_OUT" | head -1)]"
    fi
}

echo ""
echo "  --- launcher + residency ---"

assert_guest_line "9p: serving fs0:\\9pexport on port ${GUEST_PORT}"
assert_guest_line "9p-serve: running"

echo ""
echo "  --- the resident export answers a real 9P client ---"

assert_client_line "VERSION msize=8192 version=9P2000.L"
assert_client_line "ATTACH root isdir=1"
assert_client_line "GETATTR hello.txt size=21"
assert_client_line "READ hello.txt = hello from 9p server"
assert_client_line "WALK sub/inner.txt read = ok"

[[ $CLIENT_RC -eq 0 ]] \
    && pass "p9-client functional suite exited 0" \
    || fail "p9-client functional suite exited $CLIENT_RC ($(tail -2 "$CLIENT_OUT" | tr '\n' ' '))"

# --- serve-stop must actually close the listener ------------------------
echo ""
echo "  --- serve-stop ---"

test_send_shell_line "9p.efi serve-stop"
test_send_shell_line "9p.efi status"
test_send_shell_line "echo TOOL-STOPPED"

if ! test_wait_for "TOOL-STOPPED" 60; then
    fail "the guest never acknowledged serve-stop"
else
    test_clean_log
    assert_guest_line "9p-serve: stopped"

    STOP_OUT="$TEST_TMPDIR/p9-client-afterstop.out"
    STOP_RC=0
    timeout 30 python3 "$(dirname "$0")/p9-client.py" 127.0.0.1 "$HOST_PORT" \
        > "$STOP_OUT" 2>&1 || STOP_RC=$?
    if [[ $STOP_RC -ne 0 ]] && grep -q "CLIENT ERROR" "$STOP_OUT"; then
        pass "after serve-stop the port no longer serves 9P"
    else
        fail "after serve-stop a client still got served (rc=$STOP_RC)"
    fi
fi

echo ""
printf "9p serve resident driver: %d passed, %d failed (%s)\n" \
    "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- client output ---"; tail -30 "$CLIENT_OUT"
    echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 10 ]] && exit 0 || exit 1
```

**Harness notes for the implementer:**
- `test_send_shell_line` may not exist. Check `test/integration/common-test.sh` for how another test drives the guest Shell *after* boot (grep for `test_send`, `monitor`, `sendkey`, or a serial-write helper). **If there is no such helper, do not invent one in this task** — restructure the startup script to do the whole sequence unattended instead, with a `stall` long enough for the host client run to land in between:
  ```
  9p.efi serve fs0:\9pexport --port 5640 --detach
  9p.efi status
  echo TOOL-SERVE-READY
  stall 45000000
  9p.efi serve-stop
  9p.efi status
  echo TOOL-STOPPED
  reset -s
  ```
  and have the host run its client inside that 45 s window (the same "budget, not a hang mask" shape `test-9p-server-qemu.sh` uses). Report which shape you used.
- `test_add_port_forward` emits a whole `-device`/`-netdev` pair — do not also call `test_add_network`.
- `p9-client.py`'s functional suite writes and creates inside the export, so the export must be writable. It is: the FAT image is mounted read-write.
- The exact `assert_client_line` strings are copied from `test-9p-server-qemu.sh`'s `ASSERTIONS` array. If one of them has drifted, take the current text from that file rather than from this plan.

- [ ] **Step 2: Run it to confirm RED**

```bash
chmod +x test/integration/test-9p-tool-serve-qemu.sh
timeout 400 ./test/integration/test-9p-tool-serve-qemu.sh
```

Expected: the guest prints an unknown-verb error for `serve` and the run fails on `TOOL-SERVE-READY` or on the `9p: serving ...` assertion. Confirm the failure is "the verb does not exist", not a harness bug.

- [ ] **Step 3a: Extend `tools/9p-common.h` with the DRY seam**

Two consumers now exist for each of these (Task 3 is the second for the macros; `status` is the second for the reporter), which is what makes factoring them right rather than speculative. Add:

```c
#include <axl/axl-net-opts.h>
#include <stddef.h>

/* The `nic` and `port` rows of a service's AxlConfigDesc table.
 *
 * Both services take the same two networking options, and both tables are
 * hand-authored rather than composed with axl_config_descs_net(): that
 * helper writes into a RUNTIME accumulator, while AxlService.opts_descs
 * must be a static table - AXL_SERVICE_DRIVER's DriverEntry reads it with
 * no consumer hook to run a builder first. The KEYS and the AUTO sentinel
 * are kept identical to what the helper emits, so the CLI vocabulary does
 * not fork from every other networked AXL tool.
 *
 * @p T is the consumer's opts type; each table needs its own offsetof, so
 * the type cannot be hidden inside the macro. */
#define AXL_9P_NET_CFG_DESCS(T)                                            \
    { "nic",  AXL_CFG_UINT, AXL_NET_NIC_AUTO_STR,                          \
      "NIC ordinal to bring up (default: first usable)",                   \
      offsetof(T, net.nic_index), sizeof(uint64_t) },                      \
    { "port", AXL_CFG_UINT, AXL_9P_PORT_DEFAULT_STR,                       \
      "TCP port",                                                          \
      offsetof(T, net.port),      sizeof(uint16_t) }

/// The matching `--nic` / `--port` rows of a verb's AxlArgDesc table.
#define AXL_9P_NET_ARG_FLAGS                                               \
    { .name = "nic",  .short_name = 'n', .type = AXL_ARG_U64,              \
      .default_value = AXL_NET_NIC_AUTO_STR,                               \
      .help = "NIC ordinal to bring up (default: first usable)" },         \
    { .name = "port", .short_name = 'p', .type = AXL_ARG_U64,              \
      .default_value = AXL_9P_PORT_DEFAULT_STR,                            \
      .help = "TCP port" }

/**
 * @brief Print "<label>: running" or "<label>: stopped" for a service.
 * @return true if the service is running.
 */
bool
axl9p_report_service(
    const char       *label,   ///< name printed before the colon
    const AxlService *svc      ///< service whose residency to query
);

// ---------------------------------------------------------------------------
// serve - handlers in 9p-cmd-serve.c
// ---------------------------------------------------------------------------

extern const AxlArgDesc axl9p_serve_flags[];
extern const AxlArgDesc axl9p_serve_positional[];

/// @brief `9p serve [root]` - deploy the resident 9P server driver.
/// @return 0 on success (or already serving), 1 on failure.
int
axl9p_serve_handler(
    AxlArgs *a   ///< parsed `serve` verb arguments
);

/// @brief `9p serve-stop` - unload the resident 9P server driver.
/// @return 0 on success or when it was not running, 1 on failure.
int
axl9p_serve_stop_handler(
    AxlArgs *a   ///< parsed `serve-stop` verb arguments (unused)
);
```

If the `AXL_ARG_U64` check in Task 1 forced the axl-webfs "default-less `--nic` + manual sentinel" idiom, `AXL_9P_NET_ARG_FLAGS` must encode *that* idiom instead — the macro exists to keep the two verbs identical, so it has to carry whatever Task 1 settled on.

- [ ] **Step 3b: Write `tools/9p-common.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-common.c - helpers shared by the `9p` tool's translation units.
 */

#include <axl.h>

#include "9p-common.h"

bool
axl9p_report_service(
    const char       *label,
    const AxlService *svc
)
{
    const AxlServiceDeploy deploy = { .service = svc };
    bool                   running;

    /* axl_service_is_running only reads deploy->service - the identity it
       looks up is the GUID derived from svc->name - so the blob fields a
       launch would need are correctly absent here. */
    running = axl_service_is_running(&deploy);
    axl_printf("%s: %s\n", label, running ? "running" : "stopped");
    return running;
}
```

- [ ] **Step 3c: Write `tools/9p-serve-svc.h`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-serve-svc.h - the `9p serve` service descriptor, linked into BOTH
 * binaries.
 *
 * 9p-serve-svc.c defines Serve9pOpts, serve9p_descs and serve9p_service
 * unconditionally, but the impl (setup, teardown, the Axl9pServer it
 * builds) is gated on AXL_SERVICE_BUILD_DRIVER, so the launcher build
 * carries only the descriptor. The launcher reads svc->opts_descs and
 * svc->user for axl_service_start_embedded's LoadOptions serialization;
 * it never invokes setup/teardown, which run on the driver side.
 *
 * Cross-binary ABI rule: build both images from the same source tree with
 * identical compile flags except the -DAXL_SERVICE_BUILD_DRIVER toggle,
 * per axl-sdk's AxlService contract.
 */

#ifndef AXL_TOOLS_9P_SERVE_SVC_H
#define AXL_TOOLS_9P_SERVE_SVC_H

#include <axl.h>
#include <axl/axl-net-opts.h>

/// Configuration for the serve service.
///
/// Every field is populated by AxlConfig auto-apply from @ref
/// serve9p_descs - from CLI args in the launcher, from LoadOptions in the
/// driver. @ref net is the canonical AxlNetOpts sub-struct; serve uses its
/// `nic_index` and `port` (the listen port). `local_ip` is unused: the
/// library's axl_9p_server_listen takes no bind address, which is why the
/// tool ships no --listen-ip.
///
/// @ref root is `const char *` per AxlConfig's AXL_CFG_STRING contract -
/// auto-apply assigns a borrowed pointer, not a copy, so an inline char[]
/// would silently fail to populate.
typedef struct {
    AxlNetOpts  net;    ///< nic_index + port (listen); local_ip unused
    const char *root;   ///< AxlFs subtree to export
    bool        ro;     ///< export read-only (mutating ops answer EROFS)
} Serve9pOpts;

/* Defined in 9p-serve-svc.c, linked into both binaries. */
extern Serve9pOpts         g_serve9p_opts;
extern const AxlConfigDesc serve9p_descs[];
extern const AxlService    serve9p_service;

#endif /* AXL_TOOLS_9P_SERVE_SVC_H */
```

- [ ] **Step 3d: Write `tools/9p-serve-svc.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-serve-svc.c - the `9p serve` service, dual-compiled.
 *
 * With -DAXL_SERVICE_BUILD_DRIVER this file IS 9p-serve-dxe.efi: a
 * resident DXE driver whose setup() brings a NIC online, builds an
 * Axl9pServer over the requested root on the driver's loop, and listens.
 * Without it, the same file contributes only the descriptor + descs +
 * opts that 9p-cmd-serve.c hands to axl_service_start_embedded - so the
 * launcher does not link Axl9pServer at all.
 *
 * The descriptor is the cross-binary ABI: one definition, two images.
 * See 9p-serve-svc.h.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-serve-svc.h"

AXL_LOG_DOMAIN("9p-serve");

Serve9pOpts g_serve9p_opts;

const AxlConfigDesc serve9p_descs[] = {
    AXL_9P_NET_CFG_DESCS(Serve9pOpts),
    { "root", AXL_CFG_STRING, "fs0:\\",
      "AxlFs subtree to export",
      offsetof(Serve9pOpts, root), sizeof(const char *) },
    { "ro",   AXL_CFG_BOOL,   "false",
      "Export read-only (every mutating request answers EROFS)",
      offsetof(Serve9pOpts, ro),   sizeof(bool) },
    { 0 }
};

#ifdef AXL_SERVICE_BUILD_DRIVER

static Axl9pServer *g_server;

static int
serve_setup(
    AxlLoop *loop,
    void    *user
)
{
    Serve9pOpts *o = (Serve9pOpts *)user;

    if (o->root == NULL || o->root[0] == '\0') {
        axl_error("no export root");
        return AXL_ERR;
    }
    if (axl_net_init_from_opts(&o->net, 10) != AXL_OK) {
        axl_error("could not bring a NIC online");
        return AXL_ERR;
    }
    if (axl_9p_server_new(loop, o->root, o->ro, &g_server) != AXL_OK) {
        axl_error("cannot export %s", o->root);
        return AXL_ERR;
    }
    if (axl_9p_server_listen(g_server, o->net.port) != AXL_OK) {
        axl_error("cannot listen on port %u", (unsigned)o->net.port);
        axl_9p_server_free(g_server);
        g_server = NULL;
        return AXL_ERR;
    }
    axl_info("exporting %s on port %u%s", o->root, (unsigned)o->net.port,
             o->ro ? " (read-only)" : "");
    return AXL_OK;
}

static int
serve_teardown(
    void *user
)
{
    (void)user;
    axl_9p_server_free(g_server);
    g_server = NULL;
    return AXL_OK;
}

#endif /* AXL_SERVICE_BUILD_DRIVER */

/* Defined in both images. The setup/teardown MEMBERS are gated, not just
   their functions - a launcher-side reference to serve_setup would drag
   Axl9pServer into 9p.efi, which is the whole point of the split. Same
   shape as axl-webfs's webfs-serve.c:1120-1127. */
const AxlService serve9p_service = {
    .name           = "9p-serve",
    .opts_descs     = serve9p_descs,
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = serve_setup,
    .teardown       = serve_teardown,
#endif
    .user           = &g_serve9p_opts,
    .driver_tick_ms = 20,
};

#ifdef AXL_SERVICE_BUILD_DRIVER
AXL_SERVICE_DRIVER(serve9p_service);
#endif
```

**`.setup` is documented as required and must not be NULL** — but it is only ever *called* on the driver side, and the launcher-side `AxlService` exists purely so `axl_service_start_embedded` can read `.name`, `.opts_descs` and `.user`. Verify this holds by reading `axl_service_start_embedded` and `axl_service_is_running` in `src/service/axl-service.c`: if either dereferences `.setup`, the gating must move (keep the functions, gate only their bodies). axl-webfs ships the gated-member form, so it is expected to be fine — **confirm it rather than assuming, and say which you found.**

- [ ] **Step 3e: Write `tools/9p-cmd-serve.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-cmd-serve.c - the `serve` and `serve-stop` verbs.
 *
 * serve: fills g_serve9p_opts from AxlArgs (the launcher and the driver
 * agree on the field layout via serve9p_descs, so this manual fill matches
 * what AXL_SERVICE_DRIVER decodes from LoadOptions on the other side),
 * then hands an AxlServiceDeploy to axl_service_start_embedded, which
 * loads the embedded 9p-serve-dxe.efi. The export runs as a resident DXE
 * driver until `serve-stop` (or `unload -n 9p-serve-dxe.efi`).
 *
 * serve-stop: axl_service_stop resolves the running image by the
 * service's name-derived GUID and unloads it. Idempotent.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-serve-svc.h"

/* Embedded 9p-serve-dxe.efi blob - spliced in by the Makefile's
   EMBED_BLOB(serve9p_dxe, ...). */
AXL_EMBED_DECLARE(serve9p_dxe);

const AxlArgDesc axl9p_serve_flags[] = {
    AXL_9P_NET_ARG_FLAGS,
    { .name = "ro",                        .type = AXL_ARG_BOOL,
      .help = "Export read-only" },
    { .name = "detach", .short_name = 'd', .type = AXL_ARG_BOOL,
      .help = "Start the driver and return to the shell" },
    { 0 }
};

const AxlArgDesc axl9p_serve_positional[] = {
    { .name = "root", .type = AXL_ARG_STRING, .default_value = "fs0:\\",
      .help = "AxlFs subtree to export" },
    { 0 }
};

static AxlServiceDeploy
serve_deploy(void)
{
    AxlServiceDeploy d = {
        .service         = &serve9p_service,
        .driver_blob     = AXL_EMBED_DATA(serve9p_dxe),
        .driver_blob_len = AXL_EMBED_SIZE(serve9p_dxe),
        .driver_name     = "9p-serve-dxe.efi",
    };
    return d;
}

int
axl9p_serve_handler(
    AxlArgs *a
)
{
    AxlServiceDeploy d = serve_deploy();

    /* Populate the shared opts struct BEFORE start_embedded serializes it.
       axl_args_get_string's pointers stay valid until this handler returns,
       and the serialize pass happens inside the call below - so the copy
       must not be deferred past it. */
    g_serve9p_opts.net.nic_index = axl_args_get_uint(a, "nic");
    g_serve9p_opts.net.port      = (uint16_t)axl_args_get_uint(a, "port");
    g_serve9p_opts.net.local_ip  = "";
    g_serve9p_opts.root          = axl_args_get_string(a, "root");
    g_serve9p_opts.ro            = axl_args_get_bool(a, "ro");

    if (axl_service_is_running(&d)) {
        axl_printf("9p: already serving\n");
        return 0;
    }
    if (axl_service_start_embedded(&d) != AXL_OK) {
        axl_printerr("9p: could not start the serve driver\n");
        return 1;
    }
    axl_printf("9p: serving %s on port %u\n", g_serve9p_opts.root,
               (unsigned)g_serve9p_opts.net.port);
    if (axl_args_get_bool(a, "detach")) {
        return 0;
    }
    return axl_service_supervise(&d) == AXL_OK ? 0 : 1;
}

int
axl9p_serve_stop_handler(
    AxlArgs *a
)
{
    (void)a;

    /* axl_service_stop only reads deploy->service - the name-derived GUID
       is what it looks up - so the blob fields a launch needs are
       deliberately absent here. */
    const AxlServiceDeploy d = { .service = &serve9p_service };

    if (!axl_service_is_running(&d)) {
        axl_printf("9p: not serving\n");
        return 0;
    }
    if (axl_service_stop(&d) != AXL_OK) {
        axl_printerr("9p: could not stop the serve driver\n");
        return 1;
    }
    axl_printf("9p: stopped serving\n");
    return 0;
}
```

**Notes for the implementer:**
- Verify `AxlConfigDesc`'s field order and names against `include/axl/axl-config.h` before writing the table — the positional initializers follow `sdk/examples/service-demo-custom.c:63-71` (`key, type, default_value, description, offset, field_size`).
- `offsetof(Serve9pOpts, net.nic_index)` (a nested member) is valid C and is the point of the hand-authored table.
- `axl_service_supervise` blocks on the default loop until Ctrl-C. Confirm its return convention (`AXL_OK`) in `include/axl/axl-service.h:530` before relying on the comparison above.
- The `root` **positional default** (`"fs0:\\"`) and `serve9p_descs`'s `root` **descriptor default** must agree — the launcher's parsed value is what crosses into LoadOptions, so a disagreement would mean `9p serve` with no argument exports one tree while the descriptor documents another.
- `axl_service_stop` takes a `const AxlServiceDeploy *`; if its signature is non-const, drop the `const` rather than casting it away.

- [ ] **Step 4: Add the verbs to `tools/9p.c`**

`9p.c` stays the verb tree. Add `#include "9p-serve-svc.h"` (for `serve9p_service`, which `status` queries), the `status` handler, and three `verbs[]` entries:

```c
static int
verb_status(
    AxlArgs *a
)
{
    (void)a;
    axl9p_report_service("9p-serve", &serve9p_service);
    return 0;
}
```

and in `verbs[]`, before the `{ 0 }` terminator:

```c
    { .name = "serve",      .help = "Export a local subtree over 9P (resident)",
      .flags = axl9p_serve_flags, .positionals = axl9p_serve_positional,
      .handler = axl9p_serve_handler },
    { .name = "serve-stop", .help = "Unload the resident 9P server",
      .handler = axl9p_serve_stop_handler },
    { .name = "status",     .help = "Report the resident services",
      .handler = verb_status },
```

`status` reports only `serve` in this task. Task 3 adds the `9p-mount:` line once there is a mount service whose state it can actually query — printing a hardcoded `9p-mount: stopped` here and asserting it in the harness would be a test that cannot fail, which this project bans outright.

`axl9p_report_service`'s return value is deliberately discarded here: `status` prints, it does not branch. That is a plain `bool` return on a helper this file owns, not a must-check API — do **not** add a `(void)` cast (project rule).

`axl9p_serve_positional` gives `root` a default of `fs0:\`, so `9p serve` with no argument exports the boot volume. That matches `serve9p_descs`'s default for the same key — the two defaults must agree, since the launcher's parsed value is what crosses into LoadOptions.

- [ ] **Step 5: Add the twin-compile + embed Makefile rules**

In `Makefile`, replace the `9p` block added in Task 1 with:

```make
# ===================================================================
# 9p -- 9P2000.L client/server launcher. A first-class tool with its
# own recipe (not in TOOL_NAMES) because it links two embedded DXE
# driver blobs, which the busybox multiplexer's one-.o-per-tool rule
# cannot express.
#
# 9p-serve-svc.c and 9p-mount-svc.c are each compiled TWICE, the
# service-demo pattern: with -DAXL_SERVICE_BUILD_DRIVER into a driver
# image (subsystem 11), and without it into the launcher. The driver
# .efi files are BUILDDIR intermediates -- they ship only inside
# 9p.efi, so nothing stages them separately.
# ===================================================================
9p: $(PREFIX)/tools/9p.efi
	@echo "  Built: $(PREFIX)/tools/9p.efi (launcher + embedded serve/mount drivers)"

NINEP_HDRS = tools/9p-common.h tools/9p-serve-svc.h

NINEP_APP_OBJS = $(BUILDDIR)/9p.o $(BUILDDIR)/9p-common.o \
                 $(BUILDDIR)/9p-cmd-file.o $(BUILDDIR)/9p-cmd-serve.o \
                 $(BUILDDIR)/9p-serve-app.o

$(BUILDDIR)/9p-serve-dxe.efi: $(BUILDDIR)/9p-serve-dxe.o $(PREFIX)/lib/libaxl.a | $(BUILDDIR)
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/9p-serve-dxe.o,$@)

$(BUILDDIR)/9p-serve-dxe.o: tools/9p-serve-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

$(BUILDDIR)/9p-serve-app.o: tools/9p-serve-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-cmd-serve.o: tools/9p-cmd-serve.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-common.o: tools/9p-common.c tools/9p-common.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Embed symbol axl_embedded_serve9p_dxe matches AXL_EMBED_DECLARE(serve9p_dxe).
# The blob NAME deliberately does not lead with a digit: AXL_EMBED_DECLARE
# token-pastes it onto axl_embedded_, and a leading digit makes that paste a
# preprocessing-number rather than plainly an identifier.
$(eval $(call EMBED_BLOB,serve9p_dxe,$(BUILDDIR)/9p-serve-dxe.efi))

$(PREFIX)/tools/9p.efi: $(NINEP_APP_OBJS) $(BLOB_OBJ_serve9p_dxe) \
                        $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_APP,$(NINEP_APP_OBJS) $(BLOB_OBJ_serve9p_dxe),$@)

$(BUILDDIR)/9p.o: tools/9p.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-cmd-file.o: tools/9p-cmd-file.c tools/9p-common.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
```

Note `9p-serve-svc.c` builds to **two** objects from one source — that is the dual compile, and the only difference is the `-D`. Extend `clean-tools` with `rm -f $(BUILDDIR)/9p*.o $(BUILDDIR)/9p-serve-dxe.efi`.

- [ ] **Step 6: Build both arches**

```bash
make ARCH=x64 all 9p 2>&1 | tail -20
make ARCH=aa64 all 9p 2>&1 | tail -20
make check-ascii
```

Expected: clean, zero warnings, and `out/native-x64/tools/9p.efi` noticeably larger than after Task 1 (it now carries the driver blob).

- [ ] **Step 7: Run both new tests on both arches to confirm GREEN**

```bash
timeout 400 ./test/integration/test-9p-tool-serve-qemu.sh
timeout 400 ./test/integration/test-9p-tool-serve-qemu.sh --arch AARCH64
timeout 300 ./test/integration/test-9p-tool-qemu.sh
timeout 300 ./test/integration/test-9p-tool-qemu.sh --arch AARCH64
```

Expected: `9p serve resident driver: 10 passed, 0 failed` on each arch, and Task 1's `7 passed, 0 failed` still green on each arch.

**If `serve` fails inside the driver**, the driver's `axl_error` lines go to the same serial console — read them. The three most likely causes, in order: (1) DHCP has not finished when `setup()` runs, so `axl_net_init_from_opts` fails — the startup script's `ifconfig -s eth0 dhcp` + `stall 3000000` must precede the `9p serve` line; (2) the LoadOptions round-trip dropped `root` (check the `axl_info("exporting %s ...")` line reports the path you passed, not `fs0:\`); (3) `9p-serve-dxe.efi` failed to load at all, which `axl_service_start_embedded` reports.

- [ ] **Step 8: Commit**

```bash
git status
git add tools/9p-serve-svc.h tools/9p-serve-svc.c tools/9p-cmd-serve.c \
        tools/9p-common.h tools/9p-common.c tools/9p.c Makefile \
        test/integration/test-9p-tool-serve-qemu.sh
git commit -m "9p serve: the export runs as a resident driver, not a foreground app"
```

---

## Task 3: `mount` — the resident 9P mount driver

`9p mount <host>` deploys `9p-mount-dxe.efi`, which connects to the remote server and publishes it as an `fsN:` volume that outlives the command. `9p umount` unloads it.

**Files:**
- Create: `tools/9p-mount-svc.h`
- Create: `tools/9p-mount-svc.c`
- Create: `tools/9p-cmd-mount.c`
- Modify: `tools/9p-common.h`
- Modify: `tools/9p.c`
- Modify: `Makefile`
- Modify: `test/integration/test-9p-tool-qemu.sh`

**Interfaces:**
- Consumes: Task 2's shape exactly — the same header/service/cmd three-way split, the same `AXL_9P_NET_CFG_DESCS(T)` / `AXL_9P_NET_ARG_FLAGS` macros, the same gated-member `AxlService` initializer, the same populate-then-start ordering, and `axl9p_report_service`.
- Produces:
  - In `tools/9p-mount-svc.h`: `typedef struct { AxlNetOpts net; const char *host; const char *aname; bool ro; } Mount9pOpts;` plus `extern Mount9pOpts g_mount9p_opts;`, `extern const AxlConfigDesc mount9p_descs[];`, `extern const AxlService mount9p_service;`.
  - In `tools/9p-common.h`: `extern const AxlArgDesc axl9p_mount_flags[];`, `extern const AxlArgDesc axl9p_mount_positional[];`, `int axl9p_mount_handler(AxlArgs *a);`, `int axl9p_umount_handler(AxlArgs *a);`.

**Symmetry is a requirement, not a preference.** Anywhere this task's shape diverges from Task 2's without a stated reason, that divergence is a defect — the two services exist side by side and the next reader will compare them.

- [ ] **Step 1: Extend the failing test**

In `test/integration/test-9p-tool-qemu.sh`, replace the startup heredoc's tail (from `echo TOOL-PUT-RB` onward) with:

```
echo TOOL-PUT-RB
9p.efi get 10.0.2.2:${P9_PORT} /puttest.txt
echo TOOL-MOUNT
9p.efi mount 10.0.2.2 --port ${P9_PORT}
echo TOOL-MOUNT-STATUS
9p.efi status
echo TOOL-MOUNT-READ
type fs1:\\hello.txt
echo TOOL-UMOUNT
9p.efi umount
9p.efi status
echo TOOL-DONE
reset -s
```

and append these assertions before the summary block, raising the expected pass count from 7 to 12:

```bash
echo ""
echo "  --- mount / umount (resident) ---"

assert_line "9p: mounted 10.0.2.2 as fs1:"
assert_line "9p-mount: running"
assert_line "hello from 9p"
assert_line "9p: unmounted"
assert_line "9p-mount: stopped"
```

and change the final gate to `[[ $FAIL -eq 0 && $PASS -eq 12 ]]`.

**`fs1:` is a guess and must be verified, not assumed.** `axl_9p_mount` publishes through `axl_fs_provider_publish`, which assigns the next free `fsN:` — with only the boot volume present that is `fs1:`, but confirm it from the first run's serial log. If the guest reports a different mapping, use *that* in both the `type` line and the assertion. **Do not** replace the assertion with a wildcard: the whole point of printing the mapping is that a human can then type it.

The tool must therefore print the assigned name. `axl_9p_mount` returns an opaque token, not a name — so the mount driver has to discover it. Use the same technique as `test/integration/9p-mount-selftest.c:88-118`: enumerate volumes with `axl_volume_enumerate` after publishing and identify the new one. **Preferred, if the API allows it: capture the volume list before publishing and report the name that appeared.** That is unambiguous and does not depend on probing for a file that may not exist on an arbitrary export. If no public API supports the before/after diff, report that as a finding and fall back to printing the mount without a name (`9p: mounted 10.0.2.2` + `9p: run `map -r` to see the new volume`), adjusting the assertions to match — but say so explicitly in the task report rather than quietly weakening the test.

- [ ] **Step 2: Run it to confirm RED**

```bash
timeout 300 ./test/integration/test-9p-tool-qemu.sh
```

Expected: the five new assertions fail (`mount` is not a verb yet) and the run reports `7 passed, 5 failed`.

- [ ] **Step 3a: Write `tools/9p-mount-svc.c`**

(Write `tools/9p-mount-svc.h` from Step 3b first — this file includes it.)

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-mount-svc.c - the `9p mount` service, dual-compiled.
 *
 * With -DAXL_SERVICE_BUILD_DRIVER this file IS 9p-mount-dxe.efi: a
 * resident DXE driver whose setup() brings a NIC online, connects to the
 * remote 9P server and publishes it as a UEFI fsN: volume. Residency is
 * the point - the driver owns the connection, so the volume outlives the
 * `9p mount` command and the Shell can read it at the prompt.
 *
 * The provider's callbacks run synchronously in the CALLER's context (a
 * Shell `type`, another app's axl_file_get_contents), not on the driver
 * loop - the loop only exists because AxlService gives every driver one.
 * Hence there is no supervise loop and no tick work to do.
 *
 * Coherence note: the volume this publishes is CLOSE-TO-OPEN consistent,
 * the guarantee the rest of AXL's file layer promises - a freshly opened
 * handle sees current contents. Two images reading one tree are not
 * coherent WHILE open, by contract.
 *
 * The descriptor is the cross-binary ABI: one definition, two images.
 * See 9p-mount-svc.h.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-mount-svc.h"

AXL_LOG_DOMAIN("9p-mount");

Mount9pOpts g_mount9p_opts;

const AxlConfigDesc mount9p_descs[] = {
    AXL_9P_NET_CFG_DESCS(Mount9pOpts),
    { "host",  AXL_CFG_STRING, "",
      "Remote 9P server address",
      offsetof(Mount9pOpts, host),  sizeof(const char *) },
    { "aname", AXL_CFG_STRING, "/",
      "Exported tree to attach",
      offsetof(Mount9pOpts, aname), sizeof(const char *) },
    { "ro",    AXL_CFG_BOOL,   "false",
      "Publish the volume read-only",
      offsetof(Mount9pOpts, ro),    sizeof(bool) },
    { 0 }
};

#ifdef AXL_SERVICE_BUILD_DRIVER

static Axl9pClient *g_client;
static void        *g_volume;

static int
mount_setup(
    AxlLoop *loop,
    void    *user
)
{
    Mount9pOpts *o = (Mount9pOpts *)user;

    (void)loop;   /* the provider is synchronous; nothing runs on the loop */

    if (o->host == NULL || o->host[0] == '\0') {
        axl_error("no server address");
        return AXL_ERR;
    }
    if (axl_net_init_from_opts(&o->net, 10) != AXL_OK) {
        axl_error("could not bring a NIC online");
        return AXL_ERR;
    }
    if (axl_9p_connect(o->host, o->net.port, "", o->aname, &g_client)
        != AXL_OK) {
        axl_error("cannot connect to %s:%u", o->host, (unsigned)o->net.port);
        return AXL_ERR;
    }
    if (axl_9p_mount(g_client, o->ro, &g_volume) != AXL_OK) {
        axl_error("cannot publish the volume");
        axl_9p_disconnect(g_client);
        g_client = NULL;
        return AXL_ERR;
    }
    axl_info("mounted %s:%u", o->host, (unsigned)o->net.port);
    return AXL_OK;
}

static int
mount_teardown(
    void *user
)
{
    (void)user;

    /* Unmount BEFORE disconnecting: axl_9p_mount borrows the client, and
       the volume's still-open handles are clunked over that connection. */
    if (axl_9p_unmount(g_volume) != AXL_OK) {
        axl_warning("unmount reported a failure; disconnecting anyway");
    }
    g_volume = NULL;
    axl_9p_disconnect(g_client);
    g_client = NULL;
    return AXL_OK;
}

#endif /* AXL_SERVICE_BUILD_DRIVER */

const AxlService mount9p_service = {
    .name           = "9p-mount",
    .opts_descs     = mount9p_descs,
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = mount_setup,
    .teardown       = mount_teardown,
#endif
    .user           = &g_mount9p_opts,
    .driver_tick_ms = 200,
};

#ifdef AXL_SERVICE_BUILD_DRIVER
AXL_SERVICE_DRIVER(mount9p_service);
#endif
```

- [ ] **Step 3b: Write `tools/9p-mount-svc.h`**

Mirror `tools/9p-serve-svc.h` exactly — same header comment structure (dual-compile explanation + the cross-binary ABI rule), the `Mount9pOpts` struct with per-field `///<` comments, and the three `extern` declarations. Note in the struct's doc comment that `net.local_ip` is unused here for the same reason as serve, but a *different* underlying one: `axl_9p_connect` takes no source-IP parameter even though `axl_tcp_connect_via` does, which is why the tool ships no `--source-ip`.

- [ ] **Step 3c: Write `tools/9p-cmd-mount.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-cmd-mount.c - the `mount` and `umount` verbs.
 *
 * mount: fills g_mount9p_opts from AxlArgs and hands an AxlServiceDeploy
 * to axl_service_start_embedded, which loads the embedded
 * 9p-mount-dxe.efi. The driver connects and publishes the volume, then
 * stays resident so the fsN: outlives this command - which is the entire
 * point of the verb.
 *
 * umount: axl_service_stop unloads the driver image, whose teardown
 * unmounts the volume and disconnects. Idempotent.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-mount-svc.h"

/* Embedded 9p-mount-dxe.efi blob - spliced in by the Makefile's
   EMBED_BLOB(mount9p_dxe, ...). */
AXL_EMBED_DECLARE(mount9p_dxe);

const AxlArgDesc axl9p_mount_flags[] = {
    AXL_9P_NET_ARG_FLAGS,
    { .name = "aname", .type = AXL_ARG_STRING, .default_value = "/",
      .help = "Exported tree to attach" },
    { .name = "ro",    .type = AXL_ARG_BOOL,
      .help = "Publish the volume read-only" },
    { 0 }
};

const AxlArgDesc axl9p_mount_positional[] = {
    { .name = "host", .type = AXL_ARG_STRING, .required = true,
      .help = "remote 9P server address" },
    { 0 }
};

static AxlServiceDeploy
mount_deploy(void)
{
    AxlServiceDeploy d = {
        .service         = &mount9p_service,
        .driver_blob     = AXL_EMBED_DATA(mount9p_dxe),
        .driver_blob_len = AXL_EMBED_SIZE(mount9p_dxe),
        .driver_name     = "9p-mount-dxe.efi",
    };
    return d;
}

int
axl9p_mount_handler(
    AxlArgs *a
)
{
    AxlServiceDeploy d = mount_deploy();

    g_mount9p_opts.net.nic_index = axl_args_get_uint(a, "nic");
    g_mount9p_opts.net.port      = (uint16_t)axl_args_get_uint(a, "port");
    g_mount9p_opts.net.local_ip  = "";
    g_mount9p_opts.host          = axl_args_get_string(a, "host");
    g_mount9p_opts.aname         = axl_args_get_string(a, "aname");
    g_mount9p_opts.ro            = axl_args_get_bool(a, "ro");

    if (axl_service_is_running(&d)) {
        axl_printf("9p: already mounted\n");
        return 0;
    }
    if (axl_service_start_embedded(&d) != AXL_OK) {
        axl_printerr("9p: could not start the mount driver\n");
        return 1;
    }
    axl_printf("9p: mounted %s\n", g_mount9p_opts.host);
    return 0;
}

int
axl9p_umount_handler(
    AxlArgs *a
)
{
    (void)a;

    const AxlServiceDeploy d = { .service = &mount9p_service };

    if (!axl_service_is_running(&d)) {
        axl_printf("9p: not mounted\n");
        return 0;
    }
    if (axl_service_stop(&d) != AXL_OK) {
        axl_printerr("9p: could not unmount\n");
        return 1;
    }
    axl_printf("9p: unmounted\n");
    return 0;
}
```

**The `9p: mounted 10.0.2.2 as fs1:` line is not printed by the code above** — `axl9p_mount_handler` prints only the host, because the *launcher* cannot see the volume name (the driver published it in another image). Resolve this in Step 4 before running the test.

- [ ] **Step 4: Report the published volume name**

The name is discoverable from either side; pick the one the API actually supports and say which in the task report.

**Option A (preferred) — the launcher diffs the volume list.** `axl_volume_enumerate` is a public API and volumes are global, so the launcher can snapshot before `axl_service_start_embedded` and again after, then report the name that appeared:

```c
static bool
volume_appeared(
    const AxlVolume *before,
    size_t           n_before,
    const AxlVolume *after,
    size_t           n_after,
    char            *out,
    size_t           out_cap
)
{
    for (size_t i = 0; i < n_after; i++) {
        bool seen = false;
        for (size_t j = 0; j < n_before; j++) {
            if (axl_strcmp(after[i].name, before[j].name) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            return axl_strlcpy(out, after[i].name, out_cap) < out_cap;
        }
    }
    return false;
}
```

used in `axl9p_mount_handler` as:

```c
    AxlVolume before[32];
    AxlVolume after[32];
    size_t    n_before = 0;
    size_t    n_after  = 0;
    char      name[16];

    axl_volume_enumerate(before, 32, &n_before);
    if (axl_service_start_embedded(&d) != AXL_OK) {
        axl_printerr("9p: could not start the mount driver\n");
        return 1;
    }
    axl_volume_enumerate(after, 32, &n_after);
    if (volume_appeared(before, n_before, after, n_after, name, sizeof(name))) {
        axl_printf("9p: mounted %s as %s:\n", g_mount9p_opts.host, name);
    } else {
        axl_printf("9p: mounted %s (no new volume appeared)\n",
                   g_mount9p_opts.host);
    }
```

Check `axl_volume_enumerate`'s signature and the `AxlVolume` field name in `include/axl/axl-fs.h` first — `test/integration/9p-mount-selftest.c:90-93` is a working call site. Also check its return value: if it is must-check, **check it** (project rule — no `(void)` cast).

**Option B (fallback, only if A cannot work)** — the driver logs the name via `axl_info` and the harness asserts the driver's line instead of the launcher's. Weaker (it asserts a log line, not the tool's user-facing contract), so use it only with an explicit note.

`9p: mounted %s (no new volume appeared)` is a real branch, not defensive padding: a second mount of the same server, or a firmware with no free `fsN:` slot, reaches it. Do not delete it.

- [ ] **Step 5: Wire the verbs and extend `status`**

Add to `tools/9p-common.h`, mirroring the serve block Task 2 added:

```c
// ---------------------------------------------------------------------------
// mount - handlers in 9p-cmd-mount.c
// ---------------------------------------------------------------------------

extern const AxlArgDesc axl9p_mount_flags[];
extern const AxlArgDesc axl9p_mount_positional[];

/// @brief `9p mount <host>` - deploy the resident 9P mount driver.
/// @return 0 on success (or already mounted), 1 on failure.
int
axl9p_mount_handler(
    AxlArgs *a   ///< parsed `mount` verb arguments
);

/// @brief `9p umount` - unload the mount driver, tearing the volume down.
/// @return 0 on success or when it was not mounted, 1 on failure.
int
axl9p_umount_handler(
    AxlArgs *a   ///< parsed `umount` verb arguments (unused)
);
```

In `tools/9p.c`, add `#include "9p-mount-svc.h"` and two `verbs[]` entries:

```c
    { .name = "mount",  .help = "Mount a remote 9P export as fsN: (resident)",
      .flags = axl9p_mount_flags, .positionals = axl9p_mount_positional,
      .handler = axl9p_mount_handler },
    { .name = "umount", .help = "Unmount the resident 9P volume",
      .handler = axl9p_umount_handler },
```

**Add the mount line to `verb_status`**, below the existing serve line:

```c
    axl9p_report_service("9p-mount", &mount9p_service);
```

Task 2 deliberately left this out — there was no mount service to query, and printing a constant would have made the harness assertion untestable. Now it reports real state, and this task's harness asserts both `running` and `stopped` across a mount/umount cycle, so both branches are exercised.

- [ ] **Step 6: Extend the Makefile**

Mirror the serve rules:

```make
NINEP_HDRS += tools/9p-mount-svc.h

NINEP_APP_OBJS += $(BUILDDIR)/9p-cmd-mount.o $(BUILDDIR)/9p-mount-app.o

$(BUILDDIR)/9p-mount-dxe.efi: $(BUILDDIR)/9p-mount-dxe.o $(PREFIX)/lib/libaxl.a | $(BUILDDIR)
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/9p-mount-dxe.o,$@)

$(BUILDDIR)/9p-mount-dxe.o: tools/9p-mount-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

$(BUILDDIR)/9p-mount-app.o: tools/9p-mount-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-cmd-mount.o: tools/9p-cmd-mount.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(eval $(call EMBED_BLOB,mount9p_dxe,$(BUILDDIR)/9p-mount-dxe.efi))
```

and extend the link rule to carry the second blob:

```make
$(PREFIX)/tools/9p.efi: $(NINEP_APP_OBJS) \
                        $(BLOB_OBJ_serve9p_dxe) $(BLOB_OBJ_mount9p_dxe) \
                        $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_APP,$(NINEP_APP_OBJS) \
	                    $(BLOB_OBJ_serve9p_dxe) $(BLOB_OBJ_mount9p_dxe),$@)
```

`+=` on `NINEP_HDRS` / `NINEP_APP_OBJS` requires those assignments to appear *after* Task 2's `=` definitions and *before* the link rule — make expands recursively-assigned variables at use time, but ordering still matters for `+=` against a simply-expanded (`:=`) variable. Task 2 used `=`, so this works; if you change either to `:=`, this breaks. Extend `clean-tools` with `$(BUILDDIR)/9p-mount-dxe.efi`.

- [ ] **Step 7: Build both arches**

```bash
make ARCH=x64 all 9p 2>&1 | tail -20
make ARCH=aa64 all 9p 2>&1 | tail -20
make check-ascii
```

- [ ] **Step 8: Run both tests on both arches to confirm GREEN**

```bash
timeout 400 ./test/integration/test-9p-tool-qemu.sh
timeout 400 ./test/integration/test-9p-tool-qemu.sh --arch AARCH64
timeout 400 ./test/integration/test-9p-tool-serve-qemu.sh
timeout 400 ./test/integration/test-9p-tool-serve-qemu.sh --arch AARCH64
```

Expected: `12 passed, 0 failed` (tool) and `10 passed, 0 failed` (serve) on each arch.

- [ ] **Step 9: Refactor while green**

Now that both services exist, look for what only becomes visible with two of them side by side, and clean it up with the tests still passing (re-run after each change):
- **`serve_deploy()` / `mount_deploy()`** are structurally identical bar three values. The plan deliberately left them duplicated (see the File Structure section); now that both are written, decide with the code in front of you. Extract only if the result reads better than the pair — a helper taking the service, the blob, the blob length and the filename is the same four values in a different order, and would be worse. **Report the decision either way; "left duplicated, here is why" is a valid outcome.**
- Same question for the two `start` bodies and the two `stop` bodies. The stop bodies are the stronger candidate: they differ only in two message strings.
- The `axl_args_get_*` → opts blocks share their first three lines (`nic` / `port` / `local_ip`). A macro over those three is plausible; weigh it against how much it obscures a plain assignment sequence.
- Confirm `AXL_9P_NET_CFG_DESCS` / `AXL_9P_NET_ARG_FLAGS` are actually used by BOTH services. A macro with one caller should have stayed inline.
- Check for a leftover `(void)` cast on a *call* return (as opposed to an unused parameter) — the project forbids the former.
- Check the two `-svc.h` headers and the two `-svc.c` files really are symmetric. Any asymmetry with no stated reason is a defect.

- [ ] **Step 10: Commit**

```bash
git status
git add tools/9p-mount-svc.h tools/9p-mount-svc.c tools/9p-cmd-mount.c \
        tools/9p-common.h tools/9p.c Makefile \
        test/integration/test-9p-tool-qemu.sh
git commit -m "9p mount: the remote export stays mounted after the command exits"
```

---

## Task 4: `axl_9p_rename` — the `EXDEV` copy-then-unlink fallback

A cross-directory rename against a server that answers `Rlerror(EXDEV)` currently returns `AXL_ERR` from `axl_9p_rename`. AXL's own Phase 4 server answers exactly that (deliberately — `axl_file_move`'s whole-file copy is unbounded synchronous I/O on the server's single loop), so AXL's client cannot `mv` across directories on AXL's own server. Every POSIX client degrades to copy-then-unlink; ours will too.

This is a **bug fix**, so it follows the strict test-first cadence: regression test first, confirm RED against current code, fix, confirm GREEN, refactor while green.

**Files:**
- Modify: `test/integration/p9-server.py`
- Modify: `test/unit/axl-test-net.c`
- Modify: `test/integration/test-9p-qemu.sh`
- Modify: `src/9p/axl-9p-internal.h`
- Modify: `src/9p/axl-9p-client.c`
- Modify: `include/axl/axl-9p.h`
- Modify: `src/9p/README.md`

**Interfaces:**
- Consumes: `axl_9p_transact` (internal, `src/9p/axl-9p-internal.h:140`), `axl_9p_client_walk` / `_clunk` / `_getattr`, `axl_9p_read_file`, `axl_9p_write_file`, `axl_9p_remove`.
- Produces: no new public symbol. `struct Axl9pClient` gains `uint32_t last_errno` (internal only). `axl_9p_rename`'s *behavior* and docstring change.

- [ ] **Step 1: Make the host fixture answer EXDEV (infra first)**

`test/integration/p9-server.py`'s `TRENAME` handler (line ~259) currently accepts any rename. AXL's server refuses a cross-directory one. Make the fixture match, so the client is tested against the behavior it will actually meet:

```python
        elif mtype == TRENAME:              # fid[4] dfid[4] name[s]
            fid, dfid = struct.unpack_from("<II", body, 0)
            name, _ = get_string(body, 8)
            node = fids.get(fid)
            newparent = fids.get(dfid)
            if node is None or newparent is None:
                rlerror(sock, tag, EBADF)
                continue
            # A cross-directory rename is refused with EXDEV, exactly as AXL's
            # own 9P server does (an in-server move would be an unbounded
            # synchronous whole-file copy). Without this the fixture is more
            # permissive than any server the client meets in production, and
            # the client's EXDEV fallback would never be exercised.
            oldparent = parent_of(node)
            if oldparent is None or oldparent.path != newparent.path:
                rlerror(sock, tag, EXDEV)
                continue
            <existing same-directory rename body, unchanged>
```

Add `EXDEV = 18` next to the other errno constants. `parent_of(node)` may not exist — the tree is `NODES: dict[int, Node]` with `children: dict[str, int]`, so a helper that scans for the node holding this child is a few lines; write it if absent. **Read the actual handler before editing** — the snippet above is the shape, not a verbatim patch, and the surrounding variable names must be taken from the file.

Run the existing suite to confirm the infra change alone does not break it:

```bash
timeout 300 ./test/integration/test-9p-qemu.sh
```

Expected: still `16 passed, 0 failed`. The existing `RENAME-RB` case renames `/ren-src.txt` → `/ren-dst.txt`, both in the root, so it is a same-directory rename and stays green. **If it goes red, the fixture edit is wrong — fix it before continuing, do not proceed with a broken baseline.**

- [ ] **Step 2: Write the failing regression test**

In `test/unit/axl-test-net.c`, immediately after the existing rename block (around line 4917, before `axl_9p_disconnect(c)`), add:

```c
    /* Cross-directory rename: the server answers Rlerror(EXDEV) rather than
       moving the bytes itself (an unbounded synchronous copy on its loop), so
       the CLIENT must degrade to copy-then-unlink the way every POSIX client
       does. Proves three things at once: the destination has the source's
       bytes, the source is gone, and the call reported success only because
       both actually happened. */
    axl_9p_write_file(c, "/dir/xdev-src.txt", "xdev-payload", 12);
    if (axl_9p_rename(c, "/dir/xdev-src.txt", "/xdev-dst.txt") == AXL_OK) {
        AxlBytes *xb = NULL;
        if (axl_9p_read_file(c, "/xdev-dst.txt", &xb) == AXL_OK) {
            size_t         n = 0;
            const uint8_t *d = axl_bytes_get_data(xb, &n);
            axl_printf("XDEV-RB: %.*s\n", (int)n, (const char *)d);
            axl_bytes_unref(xb);
        } else {
            axl_printf("XDEV-FAIL: destination unreadable\n");
        }
        AxlBytes *sb = NULL;
        if (axl_9p_read_file(c, "/dir/xdev-src.txt", &sb) != AXL_OK) {
            axl_printf("XDEV-SRC-GONE\n");
        } else {
            axl_printf("XDEV-FAIL: source survived the move\n");
            axl_bytes_unref(sb);
        }
    } else {
        axl_printf("XDEV-FAIL: rename returned an error\n");
    }
```

In `test/integration/test-9p-qemu.sh`, after the existing `RENAME-RB` assertion block (line ~152), add:

```bash
grep -Fxq "XDEV-RB: xdev-payload" "$TEST_CLEAN_LOG" \
    && pass "cross-directory rename falls back to copy-then-unlink" \
    || fail "EXDEV fallback ($(grep 'XDEV-' "$TEST_CLEAN_LOG" | head -1))"

grep -Fxq "XDEV-SRC-GONE" "$TEST_CLEAN_LOG" \
    && pass "the EXDEV fallback removes the source" \
    || fail "EXDEV source cleanup ($(grep 'XDEV-' "$TEST_CLEAN_LOG" | head -1))"
```

and raise the final gate from `$PASS -eq 16` to `$PASS -eq 18`.

Note these two use `grep -Fxq` (whole line) while the surrounding assertions use `grep -q`. That is deliberate and correct — a substring match on `XDEV-RB:` would pass on a truncated payload.

- [ ] **Step 3: Run it to confirm RED**

```bash
timeout 300 ./test/integration/test-9p-qemu.sh
```

Expected: `16 passed, 2 failed`, with the serial log showing `XDEV-FAIL: rename returned an error`. That exact line is the confirmation that the test exercises the path we think it does — if instead you see `XDEV-FAIL: destination unreadable` or no `XDEV-` line at all, stop and find out why before implementing.

- [ ] **Step 4: Record the errno**

In `src/9p/axl-9p-internal.h`, add to `struct Axl9pClient` (after `size_t rlen;`):

```c
    uint32_t last_errno;   ///< errno from the most recent Rlerror; 0 if the
                           ///< last transact did not fail with one
```

In `src/9p/axl-9p-client.c`'s `axl_9p_transact`, clear it alongside `c->rlen` and set it on the Rlerror path:

```c
    c->rlen       = 0;
    c->last_errno = 0;
```

```c
    if (type == AXL_9P_RLERROR) {
        Axl9pReader er;
        axl_9p_r_init(&er, c->rbuf + 7, size - 7);
        uint32_t ecode = axl_9p_r_u32(&er);
        c->last_errno  = ecode;
        axl_warning("9p: server error errno=%u", ecode);
        return AXL_ERR;
    }
```

Clearing at the top matters: a caller that checks `last_errno` after a *transport* failure (send/recv error, no reply at all) must not read a stale errno from three requests ago and take a fallback branch that makes no sense.

- [ ] **Step 5: Implement the fallback in `axl_9p_rename`**

Add above `axl_9p_rename` in `src/9p/axl-9p-client.c`:

```c
#define AXL_9P_EXDEV              18u                  /* Linux EXDEV */
#define AXL_9P_XDEV_COPY_MAX      (32u * 1024u * 1024u)

/* Copy-then-unlink for a cross-directory rename the server refused with
   EXDEV. This is what a POSIX client does, and the server's refusal is
   deliberate (a server-side move is an unbounded synchronous whole-file copy
   on its single loop), so the cost belongs on the client, where it blocks
   only its own caller.
 *
 * Bounded, and only for regular files:
 *   - a DIRECTORY is refused. Copy-then-unlink on a directory is a recursive
 *     tree walk with its own partial-failure semantics, which rename() does
 *     not have; answering AXL_ERR is honest, silently moving half a tree is
 *     not.
 *   - AXL_9P_XDEV_COPY_MAX caps it because the whole file is materialized in
 *     UEFI heap. A larger move is a copy the caller should make deliberately.
 *
 * Failure semantics, stated because the compound operation can fail halfway:
 * if the unlink fails after the copy succeeded, the destination is LEFT IN
 * PLACE and AXL_ERR is returned - the copy happened, the move did not, and
 * the caller is told so rather than being handed a success for a source that
 * still exists. */
static int
axl_9p_rename_xdev(
    Axl9pClient *c,
    const char  *from,
    const char  *to
)
{
    uint32_t  fid   = 0;
    uint64_t  size  = 0;
    uint64_t  mtime = 0;
    uint32_t  mode  = 0;
    AxlBytes *data  = NULL;

    if (axl_9p_client_walk(c, from, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = axl_9p_client_getattr(c, fid, &size, &mtime, &mode);
    axl_9p_client_clunk(c, fid);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }
    if ((mode & 0xF000u) == 0x4000u) {   /* S_IFDIR */
        axl_warning("9p: cross-directory rename of a directory is not "
                    "supported (server answered EXDEV)");
        return AXL_ERR;
    }
    if (size > AXL_9P_XDEV_COPY_MAX) {
        axl_warning("9p: cross-directory rename of %llu bytes exceeds the "
                    "%u-byte copy limit",
                    (unsigned long long)size,
                    (unsigned)AXL_9P_XDEV_COPY_MAX);
        return AXL_ERR;
    }
    if (axl_9p_read_file(c, from, &data) != AXL_OK) {
        return AXL_ERR;
    }
    size_t      len = 0;
    const void *buf = axl_bytes_get_data(data, &len);
    rc = axl_9p_write_file(c, to, buf, len);
    axl_bytes_unref(data);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_9p_remove(c, from) != AXL_OK) {
        axl_warning("9p: cross-directory rename copied %s to %s but could "
                    "not remove the source; both now exist", from, to);
        return AXL_ERR;
    }
    return AXL_OK;
}
```

and change the tail of `axl_9p_rename` from:

```c
    int rc = (w.overflow) ? AXL_ERR
           : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RRENAME);
    axl_9p_client_clunk(c, dfid);
    axl_9p_client_clunk(c, fid);
    return rc;
```

to:

```c
    int rc = (w.overflow) ? AXL_ERR
           : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RRENAME);
    axl_9p_client_clunk(c, dfid);
    axl_9p_client_clunk(c, fid);
    /* Clunk BOTH fids before the fallback: it walks the same paths again and
       a server with a bounded fid table should not be made to hold four fids
       for one logical move. */
    if (rc != AXL_OK && c->last_errno == AXL_9P_EXDEV) {
        return axl_9p_rename_xdev(c, from, to);
    }
    return rc;
```

**Verify `axl_9p_client_getattr`'s signature and its `mode` semantics before using it** (`src/9p/axl-9p-client.c:251`) — the `S_IFDIR` test above assumes `mode` is a POSIX `st_mode`. `test/integration/p9-server.py:200` sets `0o040755` for directories, and `src/9p/axl-9p-mount.c` already branches dir-vs-file on this field; copy whatever predicate it uses rather than open-coding a second one. **If the mount code has a named helper for this, use it — do not add a second dir-test.**

- [ ] **Step 6: Update the public docstring**

`include/axl/axl-9p.h`'s `axl_9p_rename` block comment currently says the move happens on the server. It must now state the fallback, its bounds, and its failure mode — the docstring IS the contract, and a docstring that hid a copy-then-unlink behind the word "rename" would be exactly the class of overclaim this branch spent Phase 4 closing. Cover: (1) a same-directory rename is a single server-side `Trename`; (2) a cross-directory rename against a server answering `EXDEV` degrades to copy-then-unlink on the client; (3) directories and files over 32 MiB are refused with `AXL_ERR` rather than copied; (4) if the copy succeeded but the source could not be removed, both paths exist and `AXL_ERR` is returned.

Update `src/9p/README.md`'s write-path bullet for `axl_9p_rename` to match, and remove or rewrite whatever it currently says about the gap (the Phase 4 text describing this as an unfixed limitation is now wrong).

- [ ] **Step 7: Run to confirm GREEN, both arches**

```bash
timeout 300 ./test/integration/test-9p-qemu.sh
timeout 300 ./test/integration/test-9p-qemu.sh --arch AARCH64
make check-ascii && make check-docs
```

Expected: `18 passed, 0 failed` on each arch.

Note `test-9p-qemu.sh` currently carries `# test-meta: arch=x64`. Change it to `arch=both` — this task adds client behavior that must be proven on both arches, and running it manually on aa64 while CI only ever runs x64 would be a coverage claim the harness does not back.

- [ ] **Step 8: Confirm the server side still refuses, and that nothing else regressed**

```bash
timeout 400 ./test/integration/test-9p-server-qemu.sh
timeout 400 ./test/integration/test-9p-server-qemu.sh --arch AARCH64
```

The server's `EXDEV` behavior is unchanged and its suite must be byte-identical in outcome. If a case moved, the fallback leaked into the server path.

- [ ] **Step 9: Refactor while green**

Re-run the two suites after each change:
- Is `AXL_9P_EXDEV` duplicating a `P9_EXDEV` the server already defines (`src/9p/axl-9p-server-ns-ops.c:477`)? If the server's lives somewhere shareable, share it; if it is file-local, hoist it to `src/9p/axl-9p-internal.h` so there is one definition of 18.
- Does the dir-test duplicate one in `axl-9p-mount.c`? If so, one of them should go.
- Is `last_errno` clear-on-entry actually covered by a test? If nothing exercises "transport failure then a stale-errno read", say so in the task report rather than assuming.

- [ ] **Step 10: Commit**

```bash
git status
git add src/9p/axl-9p-client.c src/9p/axl-9p-internal.h include/axl/axl-9p.h \
        src/9p/README.md test/unit/axl-test-net.c \
        test/integration/p9-server.py test/integration/test-9p-qemu.sh
git commit -m "9p client: a cross-directory rename copies-then-unlinks instead of failing"
```

---

## Task 5: Documentation, staging, and the phase record

No code behavior changes. This is the doc-sync step the project treats as part of the change, not an afterthought.

**Files:**
- Modify: `src/9p/README.md`
- Modify: `README.md`
- Modify: `devkit.conf`
- Modify: `docs/superpowers/specs/2026-07-19-axl-9p-design.md`
- Modify: `docs/ROADMAP.md`

- [ ] **Step 1: Rewrite the `src/9p/README.md` tool paragraph**

Delete:

```
There is no standalone `9p` tool/launcher yet (Phase 5); see the design
doc's phasing.
```

and add a `## Tool` section at the end of the file documenting the verb tree exactly as shipped. It must cover: the three one-shot verbs with their `host[:port]` argument shape; that `serve` and `mount` are resident embedded-DXE drivers deployed via `AxlService` and stopped with `serve-stop` / `umount`; that `serve` supervises unless `--detach` while `mount` always returns; `status`; and — explicitly — that `--listen-ip` / `--source-ip` from the design spec are **not** implemented because the library API has no bind-address parameter, with `--nic` as the interface selector that does work.

Task 4 already rewrote this file's write-path bullet for `axl_9p_rename`. Do **not** restate the `EXDEV` behavior in the Tool section — cross-reference it, and make sure no sentence anywhere in the file still describes the fallback as a gap.

Re-read the whole README for staleness while you are in it, not just the paragraph you came for. Prose that says "client only" or "no tool yet" anywhere else is now wrong.

- [ ] **Step 2: Add the `README.md` tool table row**

After the `mkrd` row (`README.md:298`), matching the surrounding style:

```markdown
| `9p`       | 9P2000.L client and server. `9p ls/get/put <host>[:port] <path>` for one-shot access; `9p serve <root>` exports a subtree so a Linux host can `mount -t 9p -otrans=tcp`; `9p mount <host>` publishes a remote export as a local `fsN:` volume. `serve` and `mount` run as resident drivers — stop them with `serve-stop` / `umount`. |
```

- [ ] **Step 3: Add the `devkit.conf` entries**

A `desc:` line with the others:

```
desc: 9p             9P2000.L client and server (ls/get/put, serve, mount)
```

and a `binary` line with the others:

```
binary  out/native-${arch}/tools/9p.efi          ${arch}/9p.efi
```

Note the destination name: every other tool is CamelCased (`MkRd.efi`), but `9p` has no natural CamelCase form and the protocol is universally lowercase. Ship it as `9p.efi` and say so in the commit message. **Verify uefi-devkit's orchestrator tolerates a leading digit in the alias it generates from this name** — if it cannot, `Ninep.efi` is the fallback; report which you used.

- [ ] **Step 4: Mark Phase 5 DONE in the design spec**

In `docs/superpowers/specs/2026-07-19-axl-9p-design.md` §12, expand item 5 the way items 3 and 4 were expanded — what shipped, plus a **"Where reality deviated from this spec"** block recording the five deviations from this plan's header section (no `--listen-ip`/`--source-ip`; `mount` never supervises; a `status` verb was added; `9p` is excluded from busybox; the `EXDEV` fallback was pulled in from the phase-4 deferral list). Follow the existing convention: record the deviation, do not quietly reword the spec text.

Phase 4's §12 item 4 ends with the sentence "Documented in `src/9p/README.md`; a candidate fix for Phase 5 or v2" about the missing `EXDEV` fallback. **That sentence is now stale** — amend it to point at Phase 5 as where it landed, rather than leaving a spec that describes a shipped fix as pending.

- [ ] **Step 5: Add the Axl9p entry to `docs/ROADMAP.md`**

`docs/ROADMAP.md` is the project's single source of truth for phase state and currently has **no Axl9p entry at all** — only an incidental mention at line 179. Add one in the same shape as the neighboring module trackers (`AxlScsi` / `AxlSmart` at lines 132-133 are the format to copy), with all five phases marked `[x]` and one line each on what each phase delivered.

- [ ] **Step 6: Verify the doc gates**

```bash
make check-ascii
make check-docs
./scripts/build-docs.sh 2>&1 | tail -20
```

`make check-docs` should pass (no new public header this phase). `build-docs.sh` must produce no new warnings — `docs/sphinx/modules/9p.rst` includes `src/9p/README.md`, so a malformed markdown table or a bad `\ref` in Step 1 surfaces here.

`make check-dogfood` is known to fail on `src/fv/axl-fv.c:338,347` — pre-existing on `main`, unrelated. Run it anyway and confirm those are the ONLY findings; a new one is yours.

- [ ] **Step 7: Commit**

```bash
git status
git add src/9p/README.md README.md devkit.conf \
        docs/superpowers/specs/2026-07-19-axl-9p-design.md docs/ROADMAP.md
git commit -m "9p: document the tool and close out Phase 5"
```

---

## Final gate (before `superpowers:finishing-a-development-branch`)

- [ ] **Full unit suite, both arches, no regression**

```bash
./test/integration/test-axl.sh
./test/integration/test-axl.sh --arch AARCH64
```

Expected: the branch's current count (8259 at `d5b6c06c`) or higher, 0 failures, on BOTH arches. This phase adds no `test_check` assertions to any `AxlTest*` binary — Task 4's client case prints markers from `AxlTestNet`'s `9p-client` mode, which the integration harness grades, not the unit counter — so an unchanged count is the correct outcome. A *drop* means something regressed.

Note: a subset run (`TEST_APPS_ONLY=X`) prints an "expected at least N" ratchet FAIL that is not a real failure. Judge by the `=== Results:` footer.

- [ ] **All four 9P integration suites, both arches**

```bash
for s in test-9p-qemu.sh test-9p-server-qemu.sh \
         test-9p-tool-qemu.sh test-9p-tool-serve-qemu.sh; do
    timeout 400 ./test/integration/$s || echo "FAILED: $s x64"
    timeout 400 ./test/integration/$s --arch AARCH64 || echo "FAILED: $s aa64"
done
```

`test-9p-qemu.sh` must report **18 passed** (16 pre-existing + Task 4's two), and `test-9p-server-qemu.sh` its current count, unchanged. Task 4 is the only library change in the phase and it touches the client's rename path alone — movement anywhere else is a real finding.

- [ ] **A clean-tree build, to catch a stale-object false green**

```bash
make clean && make ARCH=x64 all tools 9p && make ARCH=aa64 all tools 9p
```

- [ ] **Independent pre-commit review of the whole branch**

Per `feedback_code_review_before_commit`, an integration pass over the full Phase 5 diff before finishing. Specific things to point the reviewer at:
- The cross-binary opts ABI: does every `AxlConfigDesc` `field_size` match its struct member? A `uint16_t port` described as `sizeof(uint64_t)` writes 6 bytes past the field, and nothing in the round-trip would necessarily catch it.
- Teardown ordering in `mount_teardown` (unmount before disconnect) and `serve_teardown`.
- Every `axl_*` call whose return is discarded: is it a must-check?
- Whether any assertion in the two new harnesses can pass for the wrong reason — particularly anything asserting a *constant* the code prints unconditionally.
- Whether the `9p-serve` / `9p-mount` service names can collide with any other AxlService name in the ecosystem (they derive GUIDs).
- Task 4's compound failure paths: is there any input for which `axl_9p_rename` returns `AXL_OK` while the source still exists, or while the destination holds partial bytes? That is the exact defect class this branch closed ten instances of.
- Task 4's `last_errno`: every `axl_9p_transact` exit path must leave it either freshly set or cleared. A path that returns `AXL_ERR` without touching it is a stale-errno bug waiting for the next caller that branches on it.

- [ ] **Update `.superpowers/sdd/progress.md`** with a `# ==== Phase 5 (tool) ====` section recording each task's commit range and review outcome, matching the existing Phase 3 / Phase 4 sections' format.

---

## Self-review notes

**Spec coverage** — spec §8's verb table maps to tasks as: `ls`/`get`/`put` → Task 1; `serve`/`serve-stop` → Task 2; `mount`/`umount` → Task 3; the phase-4 `EXDEV` deferral → Task 4; `TOOL_NAMES`/staging/README/Sphinx (spec §12 item 5) → Task 5. `--listen-ip` / `--source-ip` are the only uncovered items, deliberately, and are recorded as deviations in the plan header and again in Task 5 Step 4.

**Known soft spots** — flagged inline rather than hidden: (1) `AXL_TOOL_MAIN(9p)`'s digit-leading token paste; (2) whether a post-boot Shell-input helper exists in `common-test.sh`, with a fully specified fallback; (3) the published `fsN:` name, with an explicit instruction to verify rather than assume; (4) whether `AXL_ARG_U64` exists, with an instruction to report rather than truncate the AUTO sentinel.
