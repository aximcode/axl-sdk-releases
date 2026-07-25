# netload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `netload` UEFI tool that lists staged NIC drivers, lets you pick one with a single keypress (or `-a` to try them all), loads it, checks link, and runs DHCP for 15 s — recording a crash-surviving NVRAM breadcrumb so a driver that hangs the box is identified and quarantined across reboots.

**Architecture:** Single-file axl-sdk tool (`tools/netload.c`, `AXL_TOOL_MAIN`) that is pure glue over existing APIs: `axl_driver_load/_start/_connect/_unload`, `axl_net_list_interfaces/_get_link_stats/_get_driver_info/_bring_up`, `axl_console_read_key`, `axl_nvstore_*`, `axl_dir_*`, `axl_app_boot_path`. Persistence is NVRAM-only (the devkit FAT is read-only); all verbose output goes to the console (ASCII).

**Tech Stack:** C (gnu2x, freestanding UEFI), axl-sdk public API, QEMU integration test harness (`test/integration/*-qemu.sh`, `scripts/run-qemu.sh`).

**Spec:** `docs/AXL-Netload-Design.md` (read it first).

## Global Constraints

- **On-screen strings are ASCII only** (UEFI text console + `make check-ascii`). No `→`/`✓`/`—`; use `->`, `[ok]`, `--`.
- **No filesystem writes** — the devkit FAT is read-only. Persistence is NVRAM (`axl_nvstore_*`) only; reads of `drivers\<arch>\` are fine.
- **`SetVariable` is best-effort** — if NVRAM write fails, warn on-screen and continue; never block the probe.
- **Every tool routes through `AXL_TOOL_MAIN`** (gives `--version`/`-h`/`-b` + the version stamp; `test-tool-version-qemu.sh` will check it).
- **Test-first, batched** per `CLAUDE.md`: extend `test/integration/test-netload-qemu.sh`, confirm RED, implement, confirm GREEN. Real driver-load/link/DHCP is **HW-validated by the user**, documented as such — never claimed QEMU-tested.
- **DRY** — reuse `axl_net_*` accessors (the same ones `netinfo` uses); do not re-enumerate SNP by hand.
- Arch subdir is compile-time: `#if defined(__aarch64__)` → `"aa64"`, `#elif defined(__x86_64__)` → `"x64"`.

## File Structure

- **Create `tools/netload.c`** — the whole tool. Sections in this order (per `AXL-Coding-Style.md`): file header; includes; macros (arch, NVRAM ns/keys, caps); types (outcome enum); the pure NVRAM-list helpers; NVRAM breadcrumb/quarantine/log functions; driver discovery; the per-driver probe; auto + interactive drivers; arg table + `AXL_TOOL_MAIN`.
- **Modify `Makefile`** — add `netload` to `TOOL_NAMES` (line ~2055). `BUILD_TOOL` + the busybox aggregate pick it up automatically.
- **Modify `devkit.conf`** — a `desc:` line and a `binary` staging line.
- **Create `test/integration/test-netload-qemu.sh`** — the integration test; grows one assertion block per task.
- **Modify `docs/AXL-Netload-Design.md`** — only if the design shifts during build (keep it truthful).

---

### Task 1: Tool scaffold + build wiring + version stamp

**Files:**
- Create: `tools/netload.c`
- Modify: `Makefile:2055` (`TOOL_NAMES`)
- Modify: `devkit.conf`
- Test: `test/integration/test-netload-qemu.sh` (create)

**Interfaces:**
- Produces: the `netload.efi` tool; `AXL_TOOL_MAIN(netload)` entry; an `AxlArgs`-based handler `run_netload(AxlArgs *a)`. Flags established here: `-a/--auto` (bool), `--dump` (bool), `--clear` (bool), `--dir <path>` (string), `--debug` (bool), and the hidden test seam `--_mark <name>` (string).

- [ ] **Step 1: Write the failing test** — create `test/integration/test-netload-qemu.sh`:

```bash
#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# test-netload-qemu.sh — netload tool: scaffold, NVRAM state machine, driver
# discovery, and the crash-recovery quarantine flow. Real driver-load/link/DHCP
# is HW-validated, not covered here (no NIC drivers in QEMU).
set -euo pipefail
ARCH="X64"
while [[ $# -gt 0 ]]; do case "$1" in --arch) ARCH="$2"; shift 2;; *) echo "bad arg $1"; exit 2;; esac; done
case "$ARCH" in X64) NAT=x64;; AARCH64) NAT=aa64;; *) echo "bad arch"; exit 2;; esac
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS="$DIR/out/native-$NAT/tools"
make -C "$DIR" ARCH="$NAT" tools >/dev/null 2>&1 || true
[[ -x "$TOOLS/netload.efi" ]] || { echo "ERROR: netload.efi not built"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"; LOG="$TMP/serial.log"
{ echo '@echo -off'; echo 'fs0:'; echo 'echo MARK_HELP'; echo 'netload.efi --help'; echo 'echo MARK_DONE'; echo 'reset -s'; } > "$NSH"
"$DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout 60 --nsh "$NSH" "$TOOLS/netload.efi" > "$LOG" 2>&1 || true
fail=0
sect() { sed -n "/$1/,/$2/p" "$LOG"; }
sect MARK_HELP MARK_DONE | grep -aqiE "netload" && echo "PASS: help runs" || { echo "FAIL: help"; fail=1; }
[[ "$fail" -eq 0 ]] && { echo "=== PASS ($ARCH) ==="; exit 0; } || { echo "=== FAIL ($ARCH) ==="; exit 1; }
```

- [ ] **Step 2: Run it, verify RED**

Run: `chmod +x test/integration/test-netload-qemu.sh && ./test/integration/test-netload-qemu.sh`
Expected: FAIL — "netload.efi not built".

- [ ] **Step 3: Create `tools/netload.c` scaffold**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file netload.c
    Interactive NIC-driver loader + link/DHCP probe with crash-culprit NVRAM
    breadcrumb. See docs/AXL-Netload-Design.md.
**/

#include <axl.h>

AXL_LOG_DOMAIN("netload");

#if defined(__aarch64__)
#  define NETLOAD_ARCH "aa64"
#elif defined(__x86_64__)
#  define NETLOAD_ARCH "x64"
#else
#  error "unsupported arch"
#endif

static const AxlArgDesc flags[] = {
    { .name = "auto",  .short_name = 'a', .type = AXL_ARG_BOOL,
      .help = "Try every driver until one gets a DHCP lease" },
    { .name = "dump",  .type = AXL_ARG_BOOL,
      .help = "Print the NVRAM quarantine + result log and exit" },
    { .name = "clear", .type = AXL_ARG_BOOL,
      .help = "Clear all netload NVRAM state and exit" },
    { .name = "dir",   .type = AXL_ARG_STR,
      .help = "Override the driver directory (default: <boot-vol>:\\drivers\\<arch>)" },
    { .name = "debug", .type = AXL_ARG_BOOL, .help = "Verbose (DEBUG) logging" },
    { .name = "_mark", .type = AXL_ARG_STR, .hidden = true,
      .help = "TEST SEAM: set the crash breadcrumb to <name> and exit" },
    {0}
};

static int
run_netload(AxlArgs *a)
{
    if (axl_args_get_bool(a, "debug")) {
        axl_log_set_level(AXL_LOG_DEBUG);
    }
    axl_printf("netload %s (arch %s)\n", axl_version(), NETLOAD_ARCH);
    return 0;
}

AXL_TOOL_MAIN(netload)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name    = "netload",
        .help    = "Load a staged NIC driver, check link, and try DHCP; "
                   "records a crash-surviving NVRAM breadcrumb to find a driver "
                   "that hangs the box.",
        .flags   = flags,
        .handler = run_netload,
    });
}
```

> If `AxlArgDesc` has no `.hidden` field, drop it from the `_mark` entry (still works; it just shows in help). Verify against `include/axl/axl-args.h` while implementing.

- [ ] **Step 4: Register the tool** — in `Makefile`, append `netload` to `TOOL_NAMES` (line ~2055):

```make
TOOL_NAMES = hexdump fetch find grep sed cat sysinfo netinfo mkrd rfbrowse ipmi dmidecode memspd lspci lsusb mkfixture rndisfix timetest i2c clip paste tar nvme ata scsi smart fwtool axbench kbtune netload
```

- [ ] **Step 5: Stage in the devkit** — in `devkit.conf`, add a `desc:` line near the other tool descs and a `binary` line near the other tool binaries:

```
desc: NetLoad        Load a NIC driver, check link, try DHCP (crash-culprit breadcrumb)
```
```
binary  out/native-${arch}/tools/netload.efi   ${arch}/NetLoad.efi
```

- [ ] **Step 6: Build + run test, verify GREEN**

Run: `make ARCH=x64 tools -j"$(nproc)" && ./test/integration/test-netload-qemu.sh`
Expected: `PASS: help runs` / `=== PASS (X64) ===`.

- [ ] **Step 7: Commit**

```bash
git add tools/netload.c Makefile devkit.conf test/integration/test-netload-qemu.sh
git commit -m "netload: tool scaffold + build/devkit wiring"
```

---

### Task 2: NVRAM state machine (breadcrumb / quarantine / log) + `--dump` / `--clear` / crash-recovery

**Files:**
- Modify: `tools/netload.c`
- Test: `test/integration/test-netload-qemu.sh`

**Interfaces:**
- Produces (all `static` in `netload.c`):
  - `void nv_init(void)` — register the namespace once.
  - `bool nv_set_trying(const char *name)` / `bool nv_get_trying(char *out, size_t cap)` / `void nv_clear_trying(void)` — the breadcrumb.
  - `bool q_contains(const char *list, const char *name)`, `void q_append(char *list, size_t cap, const char *name)` — **pure** quarantine-list ops on a `\n`-separated string (dedupe; drop-oldest when full).
  - `void quarantine_add(const char *name)` / `bool is_quarantined(const char *name)` — NVRAM-backed.
  - `void log_append(const char *line)` — bounded ring in NVRAM (drop oldest past `NETLOAD_LOG_MAX`).
  - `int recover_crash(void)` — on startup: if the breadcrumb is set, quarantine it, log a CRASH line, clear it; returns 1 if a crash was recovered.
  - `int cmd_dump(void)`, `int cmd_clear(void)`.
- Consumes: `axl_nvstore_register_namespace`, `axl_nvstore_set/get/delete`, `AXL_NV_PERSISTENT`, `AXL_NV_BOOT`, `AXL_GUID`.

- [ ] **Step 1: Add the failing test block** — append before the PASS/FAIL footer of `test-netload-qemu.sh`, and extend the nsh generation to include these commands (replace the `{ echo ... } > "$NSH"` block):

```bash
{
  echo '@echo -off'; echo 'fs0:'
  echo 'echo MARK_HELP';   echo 'netload.efi --help'
  echo 'echo MARK_CLR';    echo 'netload.efi --clear'
  echo 'echo MARK_MARK';   echo 'netload.efi --_mark BadDrv.efi'
  echo 'echo MARK_RECOVER';echo 'netload.efi --dump'
  echo 'echo MARK_DONE';   echo 'reset -s'
} > "$NSH"
```

Then add assertions:

```bash
# --_mark seeds the breadcrumb; the NEXT run's --dump must detect the dangling
# breadcrumb, quarantine BadDrv.efi, and report it (crash recovery).
sect MARK_RECOVER MARK_DONE | grep -aqiE "CRASH.*BadDrv\.efi|quarantin.*BadDrv\.efi" \
  && echo "PASS: crash breadcrumb -> quarantine" || { echo "FAIL: crash recovery"; fail=1; }
sect MARK_RECOVER MARK_DONE | grep -aqiE "BadDrv\.efi" \
  && echo "PASS: quarantine listed in --dump" || { echo "FAIL: dump quarantine"; fail=1; }
```

- [ ] **Step 2: Run, verify RED**

Run: `./test/integration/test-netload-qemu.sh`
Expected: FAIL on "crash recovery" (no `--_mark`/`--dump` behavior yet).

- [ ] **Step 3: Implement the NVRAM module** — add to `netload.c` (macros near the top, functions after the arg table area; keep the file ordering per style):

```c
#define NETLOAD_NS       "netload"
#define NETLOAD_NAME_MAX 64        /* driver basename bound */
#define NETLOAD_Q_MAX    1024      /* quarantine var bound (bytes) */
#define NETLOAD_LOG_MAX  2048      /* result-log var bound (bytes) */
#define NETLOAD_NV_FLAGS (AXL_NV_PERSISTENT | AXL_NV_BOOT)

/* Vendor GUID for netload's NVRAM namespace (generated once, keep stable). */
static const AxlGuid NETLOAD_GUID =
    AXL_GUID(0x6e65746c, 0x6f61, 0x64ff, 0x9a, 0x21, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55);

static void
nv_init(void)
{
    axl_nvstore_register_namespace(NETLOAD_NS, &NETLOAD_GUID);
}

static bool
nv_set_trying(const char *name)
{
    if (axl_nvstore_set(NETLOAD_NS, "Trying", name, axl_strlen(name) + 1,
                        NETLOAD_NV_FLAGS) != AXL_OK) {
        axl_warning("could not persist breadcrumb (read-only/full NVRAM?)");
        return false;
    }
    return true;
}

static bool
nv_get_trying(char *out, size_t cap)
{
    size_t sz = cap;
    if (axl_nvstore_get(NETLOAD_NS, "Trying", out, &sz) != AXL_OK || sz == 0) {
        return false;
    }
    out[cap - 1] = '\0';
    return out[0] != '\0';
}

static void
nv_clear_trying(void)
{
    axl_nvstore_delete(NETLOAD_NS, "Trying");
}

/* Pure: is @name one of the '\n'-separated entries in @list? */
static bool
q_contains(const char *list, const char *name)
{
    size_t nlen = axl_strlen(name);
    const char *p = list;
    while (*p != '\0') {
        const char *nl = axl_strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : axl_strlen(p);
        if (seg == nlen && axl_memcmp(p, name, nlen) == 0) {
            return true;
        }
        p = nl ? nl + 1 : p + seg;
    }
    return false;
}

/* Pure: append "@name\n" to @list (in @cap bytes), dedup; drop oldest line if
   it would overflow. */
static void
q_append(char *list, size_t cap, const char *name)
{
    if (q_contains(list, name)) {
        return;
    }
    size_t need = axl_strlen(name) + 1;   /* name + '\n' */
    size_t have = axl_strlen(list);
    while (have + need + 1 > cap && have > 0) {   /* drop oldest (first line) */
        char *nl = axl_strchr(list, '\n');
        if (nl == NULL) { list[0] = '\0'; have = 0; break; }
        size_t rest = axl_strlen(nl + 1);
        axl_memmove(list, nl + 1, rest + 1);
        have = rest;
    }
    axl_snprintf(list + have, cap - have, "%s\n", name);
}

static void
quarantine_add(const char *name)
{
    char q[NETLOAD_Q_MAX] = {0};
    size_t sz = sizeof q;
    axl_nvstore_get(NETLOAD_NS, "Quarantine", q, &sz);   /* empty if absent */
    q[sizeof q - 1] = '\0';
    q_append(q, sizeof q, name);
    axl_nvstore_set(NETLOAD_NS, "Quarantine", q, axl_strlen(q) + 1, NETLOAD_NV_FLAGS);
}

static bool
is_quarantined(const char *name)
{
    char q[NETLOAD_Q_MAX] = {0};
    size_t sz = sizeof q;
    if (axl_nvstore_get(NETLOAD_NS, "Quarantine", q, &sz) != AXL_OK) {
        return false;
    }
    q[sizeof q - 1] = '\0';
    return q_contains(q, name);
}

static void
log_append(const char *line)
{
    char lg[NETLOAD_LOG_MAX] = {0};
    size_t sz = sizeof lg;
    axl_nvstore_get(NETLOAD_NS, "Log", lg, &sz);
    lg[sizeof lg - 1] = '\0';
    q_append(lg, sizeof lg, line);   /* same ring semantics: append + drop-oldest */
    axl_nvstore_set(NETLOAD_NS, "Log", lg, axl_strlen(lg) + 1, NETLOAD_NV_FLAGS);
}

static int
recover_crash(void)
{
    char name[NETLOAD_NAME_MAX] = {0};
    if (!nv_get_trying(name, sizeof name)) {
        return 0;
    }
    axl_printf("!! last run CRASHED while loading %s -- quarantining it\n", name);
    quarantine_add(name);
    char line[128];
    axl_snprintf(line, sizeof line, "CRASH %s", name);
    log_append(line);
    nv_clear_trying();
    return 1;
}

static int
cmd_dump(void)
{
    char buf[NETLOAD_LOG_MAX] = {0};
    size_t sz = sizeof buf;
    axl_printf("=== netload quarantine ===\n");
    sz = sizeof buf; buf[0] = '\0';
    if (axl_nvstore_get(NETLOAD_NS, "Quarantine", buf, &sz) == AXL_OK && buf[0]) {
        axl_printf("%s", buf);
    } else {
        axl_printf("(empty)\n");
    }
    axl_printf("=== netload result log ===\n");
    sz = sizeof buf; buf[0] = '\0';
    if (axl_nvstore_get(NETLOAD_NS, "Log", buf, &sz) == AXL_OK && buf[0]) {
        axl_printf("%s", buf);
    } else {
        axl_printf("(empty)\n");
    }
    return 0;
}

static int
cmd_clear(void)
{
    axl_nvstore_delete(NETLOAD_NS, "Trying");
    axl_nvstore_delete(NETLOAD_NS, "Quarantine");
    axl_nvstore_delete(NETLOAD_NS, "Log");
    axl_printf("netload: NVRAM state cleared\n");
    return 0;
}
```

- [ ] **Step 4: Wire the commands into `run_netload`** — replace the body:

```c
static int
run_netload(AxlArgs *a)
{
    if (axl_args_get_bool(a, "debug")) {
        axl_log_set_level(AXL_LOG_DEBUG);
    }
    nv_init();

    const char *mark = axl_args_get_str(a, "_mark");
    if (mark != NULL && mark[0] != '\0') {   /* test seam: simulate a crash */
        nv_set_trying(mark);
        axl_printf("netload: breadcrumb set to %s\n", mark);
        return 0;
    }
    if (axl_args_get_bool(a, "clear")) {
        return cmd_clear();
    }

    recover_crash();   /* every real run first heals a prior crash */

    if (axl_args_get_bool(a, "dump")) {
        return cmd_dump();
    }
    axl_printf("netload %s (arch %s)\n", axl_version(), NETLOAD_ARCH);
    return 0;
}
```

Verify exact names while implementing: `axl_args_get_str`, `axl_strchr`, `axl_memmove`, `axl_memcmp`, `axl_nvstore_delete`, and the `AxlGuid`/`AXL_GUID` field order (`include/axl/axl-macros.h`). Adjust if a helper differs.

- [ ] **Step 5: Build + run, verify GREEN**

Run: `make ARCH=x64 tools -j"$(nproc)" && ./test/integration/test-netload-qemu.sh`
Expected: PASS on crash-recovery + dump-quarantine. (`--_mark` seeds the breadcrumb in one boot; the design's real flow sets it inside the probe.)

- [ ] **Step 6: Refactor while green** — collapse `quarantine`/`log` get→append→set into one `nv_list_append(key, cap, line)` helper (both do the same). Re-run the test.

- [ ] **Step 7: Commit**

```bash
git add tools/netload.c test/integration/test-netload-qemu.sh
git commit -m "netload: NVRAM breadcrumb/quarantine/log + crash-recovery + --dump/--clear"
```

---

### Task 3: Driver discovery (resolve boot-volume dir + scan `*.efi`)

**Files:**
- Modify: `tools/netload.c`
- Test: `test/integration/test-netload-qemu.sh`

**Interfaces:**
- Produces:
  - `int resolve_driver_dir(AxlArgs *a, char *out, size_t cap)` — `--dir` override, else `axl_app_boot_path("\\drivers\\" NETLOAD_ARCH, out, cap)`, else fallback `axl_app_boot_path("\\drivers", ...)`.
  - `size_t scan_drivers(const char *dir, char names[][NETLOAD_NAME_MAX], size_t max)` — fill sorted `*.efi` basenames, return count.
- Consumes: `axl_app_boot_path`, `axl_dir_open/_read/_close`, `AxlFsEntry.name`, `axl_str_ends_with` (or manual suffix check), `axl_sort` (or insertion order + a simple sort).

- [ ] **Step 1: Add the failing test block** — stage a fake drivers dir and assert the listing. Insert BEFORE the run-qemu call (build a second FAT dir the image exposes). The devkit run-qemu stages the tool as `fs0:`; add extra files via `--extra`. Put dummy drivers under a dir the tool scans by pointing `--dir` at a staged path. Simplest deterministic approach — stage dummy `.efi` files and pass `--dir`:

```bash
# stage 3 dummy "drivers" (any .efi; they are only listed, not loaded, in this task)
mkdir -p "$TMP/drv"
cp "$TOOLS/hexdump.efi" "$TMP/drv/Aaa.efi"
cp "$TOOLS/hexdump.efi" "$TMP/drv/Bbb.efi"
cp "$TOOLS/hexdump.efi" "$TMP/drv/Ccc.efi"
```

Add to the nsh a `--list` invocation (a new debug command that prints the discovered set) pointed at a driver dir on the image. Since `--dir` must name an on-image path, stage the dummies into the image driver tree instead: pass them via `--extra` into `fs0:\drivers\<arch>\`. Confirm `run-qemu.sh --extra` placement while implementing; if it flattens to `fs0:\`, add a `--list --dir fs0:\` and match `.efi` names. Assertion:

```bash
sect MARK_LIST MARK_DONE | grep -aqE "Aaa\.efi" && sect MARK_LIST MARK_DONE | grep -aqE "Ccc\.efi" \
  && echo "PASS: driver scan lists staged .efi" || { echo "FAIL: driver scan"; fail=1; }
```

And nsh: `echo MARK_LIST; netload.efi --list --dir fs0:\drivers\<arch>` (or the resolved default if `--extra` lands them there).

- [ ] **Step 2: Run, verify RED** — FAIL: no `--list`, no scan.

- [ ] **Step 3: Implement discovery + a `--list` command**

```c
static bool
ends_with_efi(const char *s)
{
    size_t n = axl_strlen(s);
    return n >= 4 && (s[n-4]=='.') &&
           (s[n-3]=='e'||s[n-3]=='E') && (s[n-2]=='f'||s[n-2]=='F') &&
           (s[n-1]=='i'||s[n-1]=='I');
}

static int
resolve_driver_dir(AxlArgs *a, char *out, size_t cap)
{
    const char *override = axl_args_get_str(a, "dir");
    if (override != NULL && override[0] != '\0') {
        axl_strlcpy(out, override, cap);
        return AXL_OK;
    }
    if (axl_app_boot_path("\\drivers\\" NETLOAD_ARCH, out, cap) == AXL_OK) {
        return AXL_OK;
    }
    return axl_app_boot_path("\\drivers", out, cap);
}

static int
cmp_name(const void *x, const void *y)
{
    return axl_strcmp((const char *)x, (const char *)y);
}

static size_t
scan_drivers(const char *dir, char names[][NETLOAD_NAME_MAX], size_t max)
{
    AxlDir *d = axl_dir_open(dir);
    if (d == NULL) {
        return 0;
    }
    size_t n = 0;
    AxlFsEntry e;
    while (n < max && axl_dir_read(d, &e)) {
        if (e.name[0] == '.' || !ends_with_efi(e.name)) {
            continue;
        }
        axl_strlcpy(names[n], e.name, NETLOAD_NAME_MAX);
        n++;
    }
    axl_dir_close(d);
    axl_sort(names, n, NETLOAD_NAME_MAX, cmp_name);   /* verify axl_sort signature */
    return n;
}
```

Add a `--list` flag (bool) to `flags[]`, and in `run_netload` after `recover_crash()`:

```c
    char dir[256];
    if (resolve_driver_dir(a, dir, sizeof dir) != AXL_OK) {
        axl_printf("netload: could not resolve a driver directory\n");
        return 1;
    }
    char names[64][NETLOAD_NAME_MAX];
    size_t nd = scan_drivers(dir, names, 64);
    if (axl_args_get_bool(a, "list")) {
        axl_printf("=== drivers in %s ===\n", dir);
        for (size_t i = 0; i < nd; i++) {
            axl_printf("  %2zu) %s%s\n", i + 1, names[i],
                       is_quarantined(names[i]) ? "  [crashed]" : "");
        }
        if (nd == 0) { axl_printf("  (none)\n"); }
        return 0;
    }
```

- [ ] **Step 4: Build + run, verify GREEN** — `make ARCH=x64 tools && ./test/integration/test-netload-qemu.sh` → PASS driver scan. If `--extra` placement differs, adjust the nsh `--dir` to the actual staged path (inspect `$LOG`).

- [ ] **Step 5: Commit**

```bash
git add tools/netload.c test/integration/test-netload-qemu.sh
git commit -m "netload: driver discovery (boot-volume drivers/<arch>) + --list"
```

---

### Task 4: Per-driver probe (breadcrumb -> load -> connect -> interface diff -> link -> DHCP -> outcome)

**Files:**
- Modify: `tools/netload.c`
- Test: `test/integration/test-netload-qemu.sh`

**Interfaces:**
- Produces:
  - `typedef enum { PR_OK, PR_LINK_NO_DHCP, PR_NO_NIC, PR_LOAD_FAIL } ProbeResult;`
  - `size_t snapshot_macs(uint8_t macs[][6], size_t max)` — fill current interface MACs, return count.
  - `ProbeResult probe_driver(const char *dir, const char *name)` — the full crash-safe sequence for one driver; logs verbosely; returns the outcome. On `PR_OK` leaves the driver bound (no unload).
- Consumes: `axl_driver_load/_start/_connect/_unload`, `axl_net_list_interfaces`, `AxlNetInterface`, `axl_net_get_link_stats`, `AxlNetLinkStats`, `axl_net_get_driver_info`, `AxlNetDriverInfo`, `axl_net_bring_up`, `AxlIPv4Address`, `axl_path_join`/`axl_snprintf` for `<dir>\<name>`.

- [ ] **Step 1: Add the failing test block** — drive one dummy "driver" through the probe. A dummy `.efi` (a copy of `hexdump.efi`) is a valid PE that `axl_driver_load` can load but `axl_driver_start` will return non-driver / no binding — so the outcome is `PR_LOAD_FAIL` or `PR_NO_NIC`, and — critically — the **breadcrumb is set before the load and cleared after** (no crash). Assert the sequence via the log:

```bash
# nsh: clear, then probe a single dummy driver by name via a --probe seam
#   echo MARK_PROBE; netload.efi --clear ; netload.efi --probe Aaa.efi --dir fs0:\drivers\<arch>
sect MARK_PROBE MARK_DONE | grep -aqiE "loading Aaa\.efi" \
  && echo "PASS: probe logs the driver before load" || { echo "FAIL: probe log"; fail=1; }
# after a survived (non-crashing) load, the breadcrumb must be cleared:
sect MARK_AFTER MARK_DONE | grep -aqiE "\(empty\)" \
  && echo "PASS: breadcrumb cleared after survived load" || { echo "FAIL: breadcrumb clear"; fail=1; }
```
(nsh: after `--probe`, run `echo MARK_AFTER; netload.efi --dump` and confirm the quarantine is still empty — the dummy didn't crash, so it must NOT be quarantined.)

- [ ] **Step 2: Run, verify RED** — FAIL: no `--probe`.

- [ ] **Step 3: Implement `probe_driver` + a `--probe <name>` seam**

```c
typedef enum { PR_OK, PR_LINK_NO_DHCP, PR_NO_NIC, PR_LOAD_FAIL } ProbeResult;

static size_t
snapshot_macs(uint8_t macs[][6], size_t max)
{
    size_t count = 0;
    if (axl_net_list_interfaces(NULL, &count) != AXL_OK || count == 0) {
        return 0;
    }
    AxlNetInterface *ifs = axl_calloc(count, sizeof *ifs);
    if (ifs == NULL) { return 0; }
    axl_net_list_interfaces(ifs, &count);
    size_t n = count < max ? count : max;
    for (size_t i = 0; i < n; i++) {
        axl_memcpy(macs[i], ifs[i].mac, 6);
    }
    axl_free(ifs);
    return n;
}

static bool
mac_in(uint8_t macs[][6], size_t n, const uint8_t *m)
{
    for (size_t i = 0; i < n; i++) {
        if (axl_memcmp(macs[i], m, 6) == 0) { return true; }
    }
    return false;
}

static ProbeResult
probe_driver(const char *dir, const char *name)
{
    char path[300];
    axl_snprintf(path, sizeof path, "%s\\%s", dir, name);

    /* Snapshot interfaces BEFORE, so the diff after connect is this driver's. */
    uint8_t before[16][6];
    size_t nbefore = snapshot_macs(before, 16);

    axl_printf("> loading %s ...\n", name);
    nv_set_trying(name);                 /* durable breadcrumb BEFORE the risky load */

    AxlDriverHandle drv = NULL;
    int rc = axl_driver_load(path, &drv);
    if (rc == AXL_OK) { rc = axl_driver_start(drv); }
    if (rc == AXL_OK) { axl_driver_connect(NULL); }   /* broad connect (== connect -r) */

    nv_clear_trying();                   /* survived -> breadcrumb no longer needed */

    if (rc != AXL_OK) {
        axl_printf("  [load failed rc=%d]\n", rc);
        log_append_fmt("LOADFAIL %s", name);
        if (drv) { axl_driver_unload(drv); }
        return PR_LOAD_FAIL;
    }
    axl_printf("  [ok] loaded\n");

    /* Interface diff -> the NIC(s) this driver produced. */
    size_t count = 0;
    axl_net_list_interfaces(NULL, &count);
    AxlNetInterface *ifs = count ? axl_calloc(count, sizeof *ifs) : NULL;
    if (ifs) { axl_net_list_interfaces(ifs, &count); }

    ProbeResult result = PR_NO_NIC;
    for (size_t i = 0; i < count; i++) {
        if (mac_in(before, nbefore, ifs[i].mac)) { continue; }   /* pre-existing */

        AxlNetLinkStats ls = {0};
        axl_net_get_link_stats(i, &ls);
        AxlNetDriverInfo di = {0};
        bool haved = (axl_net_get_driver_info(ifs[i].mac, &di) == AXL_OK);
        axl_printf("  NIC %s  link=%s  layer=%s driver=%s\n",
                   /* MAC */ "", ls.link_up ? "UP" : "DOWN",
                   haved ? di.layer : "-", haved ? di.driver : "-");
        /* (fill the MAC print with a formatted %02x:.. string — see netinfo.c) */

        if (!ls.link_up) {
            if (result == PR_NO_NIC) { result = PR_NO_NIC; }
            continue;
        }
        AxlIPv4Address ip = {0};
        if (axl_net_bring_up(i, NULL, NULL, NULL, 15, &ip) == AXL_OK) {
            axl_printf("  [ok] DHCP lease acquired -- networking is UP\n");
            log_append_fmt("OK %s", name);
            axl_free(ifs);
            return PR_OK;                 /* WIN: stop; leave driver bound */
        }
        axl_printf("  link is up but no DHCP lease in 15s -- the data plane may be\n"
                   "  stalled (e.g. the EDK2 UsbRndis packet-filter bug). Try "
                   "'rndisfix', then re-probe.\n");
        result = PR_LINK_NO_DHCP;
    }
    axl_free(ifs);

    if (result == PR_NO_NIC) {
        axl_printf("  loaded, but no NIC came up (needs a companion driver, or no "
                   "matching hardware)\n");
        log_append_fmt("NONIC %s", name);
    } else {
        log_append_fmt("LINK_NO_DHCP %s", name);
    }
    if (drv) { axl_driver_unload(drv); }   /* not a winner -> free it for the next */
    return result;
}
```

Add a small `log_append_fmt(fmt, ...)` varargs wrapper over `axl_vsnprintf` + `log_append`. Add a `--probe` (str) flag; in `run_netload`, after discovery:

```c
    const char *one = axl_args_get_str(a, "probe");
    if (one != NULL && one[0] != '\0') {
        probe_driver(dir, one);
        return 0;
    }
```

- [ ] **Step 4: Build + run, verify GREEN** — the dummy driver survives load (no crash), breadcrumb clears, quarantine stays empty. Adjust the exact log strings the test greps to match what you print.

- [ ] **Step 5: Refactor while green** — extract the MAC-to-string format into `mac_str(buf, mac)` (shared with the menu later); re-run.

- [ ] **Step 6: Commit**

```bash
git add tools/netload.c test/integration/test-netload-qemu.sh
git commit -m "netload: per-driver probe (breadcrumb/load/connect/diff/link/DHCP)"
```

---

### Task 5: Auto mode — sweep, skip quarantined, stop on DHCP lease

**Files:**
- Modify: `tools/netload.c`
- Test: `test/integration/test-netload-qemu.sh`

**Interfaces:**
- Produces: `int cmd_auto(const char *dir, char names[][NETLOAD_NAME_MAX], size_t nd)` — iterate; skip `is_quarantined`; `probe_driver` each; **return 0 immediately on `PR_OK`**; else continue; print a summary if none won.
- Consumes: `probe_driver`, `is_quarantined`.

- [ ] **Step 1: Add the failing test block** — quarantine one dummy, run `-a`, assert it is skipped and the sweep visits the others:

```bash
# nsh: clear; --_mark Bbb.efi (seed a crash); netload --dump (recovers -> Bbb quarantined);
#      netload -a --dir fs0:\drivers\<arch>
sect MARK_AUTO MARK_DONE | grep -aqiE "skip.*Bbb\.efi|Bbb\.efi.*\[crashed\]" \
  && echo "PASS: auto skips quarantined" || { echo "FAIL: auto skip"; fail=1; }
sect MARK_AUTO MARK_DONE | grep -aqiE "loading Aaa\.efi" \
  && echo "PASS: auto probes non-quarantined" || { echo "FAIL: auto probe"; fail=1; }
```

- [ ] **Step 2: Run, verify RED** — FAIL: `-a` not implemented.

- [ ] **Step 3: Implement `cmd_auto` + wire `-a`**

```c
static int
cmd_auto(const char *dir, char names[][NETLOAD_NAME_MAX], size_t nd)
{
    axl_printf("=== netload auto sweep: %zu driver(s) in %s ===\n", nd, dir);
    for (size_t i = 0; i < nd; i++) {
        if (is_quarantined(names[i])) {
            axl_printf("-- skipping %s [crashed a prior run]\n", names[i]);
            continue;
        }
        if (probe_driver(dir, names[i]) == PR_OK) {
            axl_printf("=== netload: %s brought networking UP ===\n", names[i]);
            return 0;                     /* WIN: stop the sweep */
        }
    }
    axl_printf("=== netload: no driver acquired a DHCP lease ===\n");
    return 1;
}
```

In `run_netload`, after discovery / before the interactive path:

```c
    if (axl_args_get_bool(a, "auto")) {
        return cmd_auto(dir, names, nd);
    }
```

- [ ] **Step 4: Build + run, verify GREEN** — quarantined dummy skipped, others probed, sweep ends (no lease from dummies).

- [ ] **Step 5: Commit**

```bash
git add tools/netload.c test/integration/test-netload-qemu.sh
git commit -m "netload: auto sweep (-a) — skip quarantined, stop on DHCP lease"
```

---

### Task 6: Interactive single-key menu

**Files:**
- Modify: `tools/netload.c`
- Test: `test/integration/test-netload-qemu.sh`

**Interfaces:**
- Produces:
  - `int menu_index_for_key(AxlKey k, size_t nd)` — pure: `'1'..'9'`→0..8, `'a'..'z'`→9..34, clamp to `< nd`, else -1. Returns the driver index, or sentinels `MENU_AUTO`/`MENU_REDRAW`/`MENU_QUIT`.
  - `int cmd_interactive(const char *dir, char names[][NETLOAD_NAME_MAX], size_t nd)` — draw menu, read one key (`axl_console_read_key`), dispatch, loop.
- Consumes: `axl_console_read_key`, `AxlKey`, `probe_driver`, `cmd_auto`.

- [ ] **Step 1: Add the failing test block** — headless key injection is unreliable, so test the **pure mapping** through a `--_key <char>` seam that prints the resolved action:

```bash
# netload.efi --_key 2  (with 3 drivers) -> "select 2) Bbb.efi"
sect MARK_KEY MARK_DONE | grep -aqiE "select.*Bbb\.efi" \
  && echo "PASS: key '2' maps to driver 2" || { echo "FAIL: key map"; fail=1; }
# netload.efi --_key q -> "quit"
sect MARK_KEYQ MARK_DONE | grep -aqiE "quit" \
  && echo "PASS: key 'q' -> quit" || { echo "FAIL: key quit"; fail=1; }
```

- [ ] **Step 2: Run, verify RED**.

- [ ] **Step 3: Implement the menu + mapping (+ `--_key` seam)**

```c
#define MENU_AUTO   (-2)
#define MENU_REDRAW (-3)
#define MENU_QUIT   (-4)
#define MENU_NONE   (-1)

static int
menu_index_for_key(AxlKey k, size_t nd)
{
    uint16_t c = k.unicode_char;
    if (c == 'q' || c == 'Q') { return MENU_QUIT; }
    if (c == 'A')             { return MENU_AUTO; }
    if (c == 'r' || c == 'R') { return MENU_REDRAW; }
    if (c >= '1' && c <= '9') { size_t i = (size_t)(c - '1');       return i < nd ? (int)i : MENU_NONE; }
    if (c >= 'a' && c <= 'z') { size_t i = 9 + (size_t)(c - 'a');   return i < nd ? (int)i : MENU_NONE; }
    return MENU_NONE;
}

static void
draw_menu(const char *dir, char names[][NETLOAD_NAME_MAX], size_t nd)
{
    axl_printf("\n=== netload: pick a NIC driver (%s) ===\n", dir);
    for (size_t i = 0; i < nd; i++) {
        char label = i < 9 ? (char)('1' + i) : (char)('a' + (i - 9));
        axl_printf("  %c) %s%s\n", label, names[i],
                   is_quarantined(names[i]) ? "  [crashed]" : "");
    }
    axl_printf("  A) try all   r) redraw   q) quit\n> ");
}

static int
cmd_interactive(const char *dir, char names[][NETLOAD_NAME_MAX], size_t nd)
{
    if (nd == 0) { axl_printf("netload: no drivers in %s\n", dir); return 1; }
    for (;;) {
        draw_menu(dir, names, nd);
        AxlKey k = {0};
        if (axl_console_read_key(UINT64_MAX, &k) != AXL_OK) { return 1; }
        int idx = menu_index_for_key(k, nd);
        if (idx == MENU_QUIT)   { axl_printf("quit\n"); return 0; }
        if (idx == MENU_REDRAW) { continue; }
        if (idx == MENU_AUTO)   { if (cmd_auto(dir, names, nd) == 0) { return 0; } continue; }
        if (idx == MENU_NONE)   { axl_printf("\n(no such option)\n"); continue; }
        if (is_quarantined(names[idx])) {
            axl_printf("\n%s crashed a prior run. Press 'y' to retry it.\n", names[idx]);
            AxlKey y = {0}; axl_console_read_key(UINT64_MAX, &y);
            if (y.unicode_char != 'y' && y.unicode_char != 'Y') { continue; }
        }
        axl_printf("\nselect %d) %s\n", idx + 1, names[idx]);
        if (probe_driver(dir, names[idx]) == PR_OK) { return 0; }
    }
}
```

Add `--_key` (str) seam; in `run_netload` before the interactive default:

```c
    const char *tk = axl_args_get_str(a, "_key");
    if (tk != NULL && tk[0] != '\0') {
        AxlKey k = { .unicode_char = (uint16_t)tk[0] };
        int idx = menu_index_for_key(k, nd);
        if (idx == MENU_QUIT) { axl_printf("quit\n"); }
        else if (idx >= 0)    { axl_printf("select %d) %s\n", idx + 1, names[idx]); }
        else                  { axl_printf("(no such option)\n"); }
        return 0;
    }
    return cmd_interactive(dir, names, nd);
```

- [ ] **Step 4: Build + run, verify GREEN**.

- [ ] **Step 5: Commit**

```bash
git add tools/netload.c test/integration/test-netload-qemu.sh
git commit -m "netload: interactive single-key menu"
```

---

### Task 7: Docs, help polish, full-suite green

**Files:**
- Modify: `tools/netload.c` (help text, ASCII sweep)
- Modify: `docs/AXL-Netload-Design.md` (mark status implemented; note the `--_mark/--_key/--probe/--list` test seams)
- Test: run the whole gate

- [ ] **Step 1: ASCII + help audit** — ensure every `axl_printf` string is ASCII (no unicode); the help lists the **user-facing** flags `-a/--auto`, `--list`, `--probe <driver>`, `--dump`, `--clear`, `--dir <path>`, `--debug`, plus a one-line usage. The `--_mark`/`--_key` seams stay `.hidden` (test-only). Run `make check-ascii`.

- [ ] **Step 2: Verbose logging pass** — confirm every probe step prints the driver name + a timestamp (`axl_time` — reuse whatever `netinfo`/tools use), and interface rows print MAC/link/speed/MTU like `netinfo` (`mac_str` helper).

- [ ] **Step 3: Update the design doc status** to "implemented". Note that `--list` and `--probe` are user-facing commands; only `--_mark`/`--_key` are hidden test seams.

- [ ] **Step 4: Full gate**

Run:
```bash
make ARCH=x64 tools -j"$(nproc)" && make ARCH=aa64 tools -j"$(nproc)"
./test/integration/test-netload-qemu.sh --arch X64
./test/integration/test-netload-qemu.sh --arch AARCH64
make check-ascii && make check-dogfood && make check-docs
./test/integration/test-tool-version-qemu.sh --arch X64   # netload joins the stamp check
```
Expected: all PASS; `netload` appears in the version-stamp test's "all N tools" line.

- [ ] **Step 5: Commit**

```bash
git add tools/netload.c docs/AXL-Netload-Design.md
git commit -m "netload: docs + help/ASCII polish; both-arch green"
```

---

## Notes for the implementer (verify these against the headers as you go)

- **`axl_args` field/getter names** (`include/axl/axl-args.h`): `AxlArgDesc{.name,.short_name,.type,.help}`, `AXL_ARG_BOOL/STR`, `axl_args_get_bool/_get_str`, `AxlArgsNode{.name,.help,.flags,.handler}`, `axl_args_run`. If `.hidden` doesn't exist, the `_mark/_key/probe/list` seams simply show in `--help` (harmless).
- **`AXL_GUID` field order** — copy the pattern from an existing `AXL_GUID(...)` use (`grep -rn "AXL_GUID(" src/ | head`).
- **`axl_sort` signature** — `grep -n "axl_sort" include/axl/axl-sort.h`; adjust the `scan_drivers` call. If absent, a small insertion sort over the fixed array is fine.
- **`axl_net_bring_up`** returns `AXL_OK` when a lease is acquired within the timeout; that is the single "win" signal.
- **`axl_dir_read`** returns `bool` and fills `AxlFsEntry` (`.name[256]`); `.` / `..` are skipped by the `e.name[0]=='.'` guard.
- **`--extra` staging in `run-qemu.sh`** — confirm where extra files land in the QEMU FAT so the test's `--dir` path is right (`grep -n "extra" scripts/run-qemu.sh`). Fall back to `--dir fs0:\...` matching the observed layout in `$LOG`.
- **Real-hardware validation (out of QEMU):** the actual crashing-driver breadcrumb, link detection, 15 s DHCP, and the link-up->rndisfix hint against a real iDRAC RNDIS NIC are validated by the user on the Dell box, and documented as HW-validated — never claimed QEMU-tested.
