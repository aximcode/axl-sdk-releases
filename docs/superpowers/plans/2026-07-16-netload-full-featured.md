# netload Full-Featured Bring-Up Tool — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `tools/netload.c` from a NIC-driver loader/prober into a complete tool for getting an **active, verified IPv4 connection** up — static IP, verification gating, NIC selection, save/apply, JSON, retries — with a single-char short form for every flag.

**Architecture:** All new behavior flows through the existing `axl_net_bring_up(idx, static_ipv4, mask, gw, timeout, out)` primitive already called by `probe_driver` and `probe_firmware_stack`; today they pass DHCP-only NULLs. We parse CLI config into those arguments, add a verify step (`axl_net_ping`/`_resolve`) that gates success, add NIC selection that resolves a concrete interface index, and add a bounded NVRAM `Config` key for save/apply. One genuinely-new library API — `axl_ipv4_parse_cidr` — is added test-first. Everything else reuses existing axl-net APIs.

**Tech Stack:** C (axl-sdk / UEFI), `axl-args` (`.short_name`), `axl-nvstore`, `axl-net`, QEMU integration test (`test-netload-qemu.sh`), library unit test (`axl-test-net.c`), GCC cross-build both arches.

## Global Constraints

- Work on branch `main`; **commit each unit GATED, do NOT push**.
- **Both arches every time:** `make ARCH=x64 tools` and `ARCH=aa64 tools`; run `./test/integration/test-netload-qemu.sh --arch X64` and `--arch AARCH64`.
- **Test-first.** Library CIDR API = bucket B strict (`axl_strcmp`/exact value `test_check`). netload = integration bucket (`grep -aqE` exact-token patterns in `test-netload-qemu.sh`, headless via `--_key`/`--_mark` seams — live probe is HW-only).
- All on-screen strings **ASCII** (`make check-ascii`). Public headers get Doxygen (`make check-docs`).
- `axl_snake_case` fns, `AxlPascalCase` types, `AXL_SCREAMING_CASE` macros; 4-space indent, K&R, multi-line signatures. No `(void)`-cast on discarded calls. No empty lines with whitespace.
- Framework-reserved shorts (`-h`/`?`, `-b`/`--page`, `--version`) must NOT be reused.
- **Invariants:** `axl_driver_connect(NULL)` stays recursive `connect -r`; exactly one driver in `NetloadTrying` per load; single-keypress menu preserved; NVRAM writes best-effort (warn+continue).
- Independent code review before every commit (`feedback_code_review_before_commit`); update `docs/AXL-Netload-Design.md` + `memory/project_netload_tool_2026-07-15.md` per unit.
- Short-option map (authoritative — from the spec §1):

  | Flag | Short | Flag | Short |
  |---|---|---|---|
  | `--auto` | `-a` *(exists)* | `--ip` | `-i` |
  | `--list` | `-l` | `--mask` | `-m` |
  | `--probe` | `-p` | `--gw` | `-g` |
  | `--dump` | `-u` | `--dns` | `-e` |
  | `--clear` | `-c` | `--dhcp` | `-H` |
  | `--dir` | `-D` | `--dhcp-timeout` | `-t` |
  | `--no-deps` | `-N` | `--ping` | `-P` |
  | `--connect` | `-f` | `--ping-gw` | `-G` |
  | `--debug` | `-v` | `--resolve` | `-r` |
  | `--save` | `-s` | `--mac` | `-M` |
  | `--apply` | `-y` | `--nic` | `-n` |
  | `--json` | `-j` | `--retries` | `-R` |

---

## File Structure

- `src/net/axl-net-addr.c` — add `axl_ipv4_parse_cidr` + static `prefix_to_mask`.
- `include/axl/axl-net.h` — declare `axl_ipv4_parse_cidr` (Doxygen).
- `test/unit/axl-test-net.c` — add `test_ipv4_parse_cidr()` next to `test_ipv4_parse_format()` (line ~4092), call it from the runner.
- `tools/netload.c` — all tool changes (flags, static/selection, verify, save/apply, json, retries, firmware row).
- `test/integration/test-netload-qemu.sh` — new headless assertions.
- `docs/AXL-Netload-Design.md` — Modes table, new "Static config & verification" section, `Config` NVRAM row.
- `memory/project_netload_tool_2026-07-15.md` — running history per unit.

---

## COMMIT 1 — Library: `axl_ipv4_parse_cidr`

### Task 1: CIDR parser (library, test-first)

**Files:**
- Modify: `include/axl/axl-net.h` (declare, after `axl_ipv4_parse`, ~line 694)
- Modify: `src/net/axl-net-addr.c` (implement, after `axl_ipv4_parse`)
- Test: `test/unit/axl-test-net.c` (add `test_ipv4_parse_cidr`, call in runner)

**Interfaces:**
- Produces: `int axl_ipv4_parse_cidr(const char *str, uint8_t octets[4], uint8_t mask[4], bool *had_prefix)` — parses `"A.B.C.D"` or `"A.B.C.D/N"` (N 0..32). Always writes `octets` on AXL_OK; writes `mask` (and `*had_prefix=true`) only when a `/N` is present, else `*had_prefix=false` and `mask` untouched. `AXL_ERR` on malformed / N>32 / NULL `str`/`octets`. `had_prefix` may be NULL.

- [ ] **Step 1: Write the failing test.** In `test/unit/axl-test-net.c`, add directly after `test_ipv4_parse_format(void)` (ends ~line 4134):

```c
static void
test_ipv4_parse_cidr(void)
{
    uint8_t oct[4], mask[4];
    bool    hp;

    /* Bare address: octets set, had_prefix false, mask untouched. */
    axl_memset(mask, 0xAB, 4);
    test_check(axl_ipv4_parse_cidr("10.0.0.5", oct, mask, &hp) == AXL_OK,
               "cidr: bare parses");
    test_check(oct[0] == 10 && oct[1] == 0 && oct[2] == 0 && oct[3] == 5,
               "cidr: bare octets");
    test_check(hp == false, "cidr: bare has no prefix");
    test_check(mask[0] == 0xAB, "cidr: bare leaves mask untouched");

    /* /24 -> 255.255.255.0 */
    test_check(axl_ipv4_parse_cidr("192.168.1.1/24", oct, mask, &hp) == AXL_OK,
               "cidr: /24 parses");
    test_check(hp == true, "cidr: /24 has prefix");
    test_check(mask[0] == 255 && mask[1] == 255 && mask[2] == 255 && mask[3] == 0,
               "cidr: /24 mask");

    /* /0 -> 0.0.0.0, /32 -> 255.255.255.255, /1 -> 128.0.0.0 */
    test_check(axl_ipv4_parse_cidr("1.2.3.4/0", oct, mask, &hp) == AXL_OK
               && mask[0] == 0 && mask[3] == 0, "cidr: /0 mask all-zero");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/32", oct, mask, &hp) == AXL_OK
               && mask[0] == 255 && mask[3] == 255, "cidr: /32 mask all-ones");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/1", oct, mask, &hp) == AXL_OK
               && mask[0] == 128 && mask[1] == 0, "cidr: /1 mask 128.0.0.0");

    /* Rejections. */
    test_check(axl_ipv4_parse_cidr("1.2.3.4/33", oct, mask, &hp) == AXL_ERR,
               "cidr: /33 rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/", oct, mask, &hp) == AXL_ERR,
               "cidr: trailing slash rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/x", oct, mask, &hp) == AXL_ERR,
               "cidr: non-numeric prefix rejected");
    test_check(axl_ipv4_parse_cidr("256.0.0.1/24", oct, mask, &hp) == AXL_ERR,
               "cidr: bad octet rejected");
    test_check(axl_ipv4_parse_cidr(NULL, oct, mask, &hp) == AXL_ERR,
               "cidr: NULL str rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/24", NULL, mask, &hp) == AXL_ERR,
               "cidr: NULL octets rejected");

    /* had_prefix may be NULL. */
    test_check(axl_ipv4_parse_cidr("8.8.8.8/8", oct, mask, NULL) == AXL_OK
               && mask[0] == 255 && mask[1] == 0, "cidr: NULL had_prefix ok");
}
```

Register it: find the call to `test_ipv4_parse_format();` in the runner (it is invoked near the other addr tests) and add `test_ipv4_parse_cidr();` on the next line.

- [ ] **Step 2: Run to verify it fails (link error — function undefined).**

```bash
make ARCH=x64 tests >/dev/null 2>&1; echo "expect: undefined reference to axl_ipv4_parse_cidr"
```

Expected: build FAILS with an undefined-reference to `axl_ipv4_parse_cidr`.

- [ ] **Step 3: Declare in the header.** In `include/axl/axl-net.h`, immediately after the `axl_ipv4_parse(...)` declaration (~line 696):

```c
/**
 * @brief Parse a dotted-decimal IPv4 address, optionally with a `/N` CIDR
 *        prefix.
 *
 * Accepts `"A.B.C.D"` or `"A.B.C.D/N"` with @a N in 0..32. On success @p octets
 * is always written; @p mask is written (and @p had_prefix set true) only when
 * a `/N` suffix is present — a bare address leaves @p mask untouched so the
 * caller's default mask survives.
 *
 * @return AXL_OK on success; AXL_ERR on malformed input, @a N > 32, or NULL
 *     @p str / @p octets.
 */
int
axl_ipv4_parse_cidr(
    const char *str,        ///< IPv4 string, optionally `A.B.C.D/N`
    uint8_t     octets[4],  ///< [out] the four octets (always written on AXL_OK)
    uint8_t     mask[4],    ///< [out] derived netmask (written only when `/N` present)
    bool       *had_prefix  ///< [out] true iff a `/N` was present (NULL to ignore)
);
```

- [ ] **Step 4: Implement.** In `src/net/axl-net-addr.c`, after `axl_ipv4_parse`:

```c
/* Turn a CIDR prefix length (0..32) into a big-endian netmask. */
static void
prefix_to_mask(unsigned int n, uint8_t mask[4])
{
    uint32_t bits = n == 0 ? 0u : (0xFFFFFFFFu << (32u - n));
    mask[0] = (uint8_t)(bits >> 24);
    mask[1] = (uint8_t)(bits >> 16);
    mask[2] = (uint8_t)(bits >> 8);
    mask[3] = (uint8_t)(bits);
}

int
axl_ipv4_parse_cidr(const char *str, uint8_t octets[4], uint8_t mask[4],
                    bool *had_prefix)
{
    if (str == NULL || octets == NULL) { return AXL_ERR; }
    if (had_prefix != NULL) { *had_prefix = false; }

    const char *slash = axl_strchr(str, '/');
    if (slash == NULL) {
        return axl_ipv4_parse(str, octets);   /* bare address */
    }

    /* Split "addr/prefix" into a bounded local copy, parse each half. */
    size_t addr_len = (size_t)(slash - str);
    char addr[16];
    if (addr_len >= sizeof addr) { return AXL_ERR; }
    axl_memcpy(addr, str, addr_len);
    addr[addr_len] = '\0';
    if (axl_ipv4_parse(addr, octets) != AXL_OK) { return AXL_ERR; }

    unsigned int n = 0;
    int consumed = 0;
    /* %u%n; reject empty ("/"), trailing garbage ("/24x"), and N>32. */
    if (axl_sscanf(slash + 1, "%u%n", &n, &consumed) != 1
        || slash[1 + consumed] != '\0' || n > 32) {
        return AXL_ERR;
    }
    if (mask != NULL) { prefix_to_mask(n, mask); }
    if (had_prefix != NULL) { *had_prefix = true; }
    return AXL_OK;
}
```

- [ ] **Step 5: Run both arches, confirm GREEN + ratchet holds.**

```bash
make ARCH=x64 tests >/dev/null 2>&1 && ./test/integration/test-netload-qemu.sh --arch X64 >/dev/null 2>&1; \
TEST_APPS_ONLY=AxlTestNet ./test/integration/test-axl.sh --arch X64 2>&1 | grep -iE "cidr:|Results|FAIL"
make ARCH=aa64 tests >/dev/null 2>&1; TEST_APPS_ONLY=AxlTestNet ./test/integration/test-axl.sh --arch AARCH64 2>&1 | grep -iE "cidr:|Results|FAIL"
```

Expected: every `cidr:` check PASSes, Results footer shows 0 failures, count ratchets **up**.

- [ ] **Step 6: Refactor while green** — re-read the impl + test for duplication / wrong-reason passes; ensure `axl_strchr`/`axl_sscanf`/`axl_memcpy` are the dogfooded calls (not raw libc).

- [ ] **Step 7: Docs + review.** `make check-docs` (the `.. doxygenfile:: axl-net.h` already renders the new fn — no rst change needed; confirm no warning). Independent contract+impl review of the header + impl + test.

- [ ] **Step 8: Commit (GATED).**

```bash
git status && git add include/axl/axl-net.h src/net/axl-net-addr.c test/unit/axl-test-net.c
git commit -m "net: add axl_ipv4_parse_cidr (A.B.C.D[/N] -> octets + mask)

New public helper backing netload's --ip CIDR syntax. Bare address leaves
the caller's mask untouched; /N (0..32) derives a big-endian netmask.
Rejects N>32, empty/garbage prefixes, bad octets. Test-first, exact-value
assertions, both arches."
```

---

## COMMIT 2 — netload Tier 1: short-opts + static IP + selection + verify

This is one coherent unit. Build it in these sub-steps, then run the full netload test once per arch and commit once.

### Task 2: Retrofit short forms + add new flag descriptors

**Files:** Modify `tools/netload.c` (the `flags[]` array, lines 57-79).

**Interfaces:**
- Produces: new args readable via `axl_args_get_string(a, "ip"|"mask"|"gw"|"dns"|"mac"|"resolve"|"ping"|"nic"|"dhcp-timeout"|"retries")` and `axl_args_get_bool(a, "dhcp"|"ping-gw"|"save"|"apply"|"json")`. There is **no `AXL_ARG_INT`** (only BOOL/STRING); `--nic`/`--dhcp-timeout`/`--retries` are `AXL_ARG_STRING` parsed via the tool's `parse_uint` (Task 3) — the built-in `axl_args_get_uint` returns 0-on-miss and can't distinguish unset from malformed (`--retries abc` must error, not silently be 0).

- [ ] **Step 1: Add `.short_name` to every existing descriptor and add the new ones.** Replace the `flags[]` array body so each existing flag carries its short from the map and the new flags are present. Example additions (place before the `_mark`/`_key` seams, keeping those last):

```c
    { .name = "list",  .short_name = 'l', .type = AXL_ARG_BOOL,
      .help = "List discovered drivers (tags quarantined ones [crashed]) and exit" },
    { .name = "probe", .short_name = 'p', .type = AXL_ARG_STRING,
      .help = "Run the full load/link/DHCP probe on one named driver and exit" },
    { .name = "dump",  .short_name = 'u', .type = AXL_ARG_BOOL,
      .help = "Print the NVRAM quarantine + result log and exit" },
    { .name = "clear", .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Clear all netload NVRAM state and exit" },
    { .name = "dir",   .short_name = 'D', .type = AXL_ARG_STRING,
      .help = "Override the driver directory (default: <boot-vol>:\\drivers\\<arch>)" },
    { .name = "no-deps", .short_name = 'N', .type = AXL_ARG_BOOL,
      .help = "Probe each driver standalone; do not auto-load dependency drivers" },
    { .name = "connect", .short_name = 'f', .type = AXL_ARG_BOOL,
      .help = "Connect the firmware's own NIC drivers and try DHCP; no staging" },
    { .name = "debug", .short_name = 'v', .type = AXL_ARG_BOOL, .help = "Verbose (DEBUG) logging" },
    /* --- IP configuration --- */
    { .name = "ip",   .short_name = 'i', .type = AXL_ARG_STRING,
      .help = "Static IPv4, dotted-decimal, optional /N CIDR (needs --mac/--nic or --probe)" },
    { .name = "mask", .short_name = 'm', .type = AXL_ARG_STRING,
      .help = "Netmask for --ip when no /N given (default 255.255.255.0)" },
    { .name = "gw",   .short_name = 'g', .type = AXL_ARG_STRING,
      .help = "Default gateway for the static path" },
    { .name = "dns",  .short_name = 'e', .type = AXL_ARG_STRING,
      .help = "DNS server(s): S or S,S2" },
    { .name = "dhcp", .short_name = 'H', .type = AXL_ARG_BOOL,
      .help = "Force DHCP (the default; overrides a saved static config)" },
    { .name = "dhcp-timeout", .short_name = 't', .type = AXL_ARG_STRING,
      .help = "DHCP wait in seconds (default 15)" },
    /* --- NIC selection --- */
    { .name = "mac", .short_name = 'M', .type = AXL_ARG_STRING,
      .help = "Target one NIC by MAC (xx:xx:xx:xx:xx:xx)" },
    { .name = "nic", .short_name = 'n', .type = AXL_ARG_STRING,
      .help = "Target one NIC by enumeration index" },
    /* --- Verification (gates success) --- */
    { .name = "ping", .short_name = 'P', .type = AXL_ARG_STRING,
      .help = "After bring-up, ICMP a target; failure means not up" },
    { .name = "ping-gw", .short_name = 'G', .type = AXL_ARG_BOOL,
      .help = "After bring-up, ping the gateway (learned or --gw)" },
    { .name = "resolve", .short_name = 'r', .type = AXL_ARG_STRING,
      .help = "After bring-up, resolve a DNS name; failure means not up" },
    /* --- Tier 2 (present now, wired in later commits) --- */
    { .name = "save",  .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Persist the winning config to NVRAM for --apply" },
    { .name = "apply", .short_name = 'y', .type = AXL_ARG_BOOL,
      .help = "Re-apply the saved config, skipping the sweep" },
    { .name = "json",  .short_name = 'j', .type = AXL_ARG_BOOL,
      .help = "Append a machine-readable JSON result object" },
    { .name = "retries", .short_name = 'R', .type = AXL_ARG_STRING,
      .help = "Retry a link-up-no-lease NIC N times (default 1)" },
    { .name = "auto",  .short_name = 'a', .type = AXL_ARG_BOOL,
      .help = "Try every driver until one gets a DHCP lease" },
```

Keep `auto` present (it already had `-a`); ensure it stays in the array exactly once. Leave `_mark`/`_key` unchanged and last.

- [ ] **Step 2: Build both arches, confirm the tool still runs (no test yet).**

```bash
make ARCH=x64 tools >/dev/null 2>&1 && make ARCH=aa64 tools >/dev/null 2>&1 && echo BUILD_OK
```

Expected: `BUILD_OK`, zero warnings.

### Task 3: Config struct + parse/validate helper

**Files:** Modify `tools/netload.c` (new types + a parse helper near the top, after the `NetloadDep` types ~line 50).

**Interfaces:**
- Produces:
  - `typedef struct { bool have; uint8_t ip[4], mask[4], gw[4]; bool have_gw; uint8_t dns[2][4]; size_t ndns; } NetloadStatic;`
  - `typedef struct { NetloadStatic st; bool have_sel; bool sel_by_mac; uint8_t sel_mac[6]; size_t sel_nic; uint32_t dhcp_timeout; uint32_t retries; const char *ping; bool ping_gw; const char *resolve; bool want_json; } NetloadCfg;`
  - `static int netload_cfg_parse(AxlArgs *a, NetloadCfg *c);` → AXL_OK, or AXL_ERR after printing a specific error (mutually-exclusive `--mac`/`--nic`; `--ip` without a selector *and* under `-a`; malformed IP/mask/gw/dns/timeout/retries).

- [ ] **Step 1: Add the failing integration assertions first** (bucket = integration). In `test-netload-qemu.sh`, in the `startup.nsh` heredoc, add error-path probes (these need no NIC — pure validation, exit before any bring-up). Add after the existing `MARK_KEYQ` block:

```bash
  echo 'echo MARK_ERR_IP_NOSEL'; echo "netload.efi --ip 10.0.0.5 -a --dir fs0:\\drivers\\$NAT"
  echo 'echo MARK_ERR_MACNIC';   echo "netload.efi --mac 00:11:22:33:44:55 --nic 0 --ip 10.0.0.5"
  echo 'echo MARK_ERR_BADIP';    echo "netload.efi --nic 0 --ip 999.0.0.1"
  echo 'echo MARK_HELP_SHORT';   echo 'netload.efi -h'
```

And in the assertion section:

```bash
sect MARK_ERR_IP_NOSEL MARK_ERR_MACNIC | grep -aqiE "static IP needs a target NIC|--mac|--nic" \
  && echo "PASS: --ip without selector under -a errors" || { echo "FAIL: ip-nosel guard"; fail=1; }
sect MARK_ERR_MACNIC MARK_ERR_BADIP | grep -aqiE "mutually exclusive|--mac and --nic" \
  && echo "PASS: --mac + --nic mutually exclusive" || { echo "FAIL: mac/nic exclusive"; fail=1; }
sect MARK_ERR_BADIP MARK_HELP_SHORT | grep -aqiE "invalid.*ip|could not parse|999" \
  && echo "PASS: malformed --ip rejected" || { echo "FAIL: bad ip"; fail=1; }
sect MARK_HELP_SHORT MARK_DONE | grep -aqiE "netload" \
  && echo "PASS: -h short help runs" || { echo "FAIL: -h"; fail=1; }
```

(Ensure a trailing `echo MARK_DONE` sentinel exists after the last command — reuse the existing one; if the new blocks are appended after it, move `MARK_DONE` to the end.)

- [ ] **Step 2: Run the test, confirm the new assertions FAIL (RED).**

```bash
make ARCH=x64 tools >/dev/null 2>&1 && ./test/integration/test-netload-qemu.sh --arch X64 2>&1 | grep -E "FAIL: (ip-nosel|mac/nic|bad ip|-h)"
```

Expected: those four print FAIL (guards not implemented yet). `-h` may already PASS (framework provides it) — acceptable.

- [ ] **Step 3: Implement the config types + `netload_cfg_parse`.** Add near the other typedefs:

```c
typedef struct {
    bool    have;
    uint8_t ip[4];
    uint8_t mask[4];
    bool    have_gw;
    uint8_t gw[4];
    uint8_t dns[2][4];
    size_t  ndns;
} NetloadStatic;

typedef struct {
    NetloadStatic st;
    bool          have_sel;
    bool          sel_by_mac;
    uint8_t       sel_mac[6];
    size_t        sel_nic;
    uint32_t      dhcp_timeout;   /* seconds; 0 -> tool default 15 */
    uint32_t      retries;        /* >=1 */
    const char   *ping;           /* NULL = none */
    bool          ping_gw;
    const char   *resolve;        /* NULL = none */
    bool          want_json;
} NetloadCfg;
```

And the parser (place after `mac_str`):

```c
/* Parse a "xx:xx:xx:xx:xx:xx" MAC into 6 bytes. Returns AXL_OK/AXL_ERR. */
static int
parse_mac(const char *s, uint8_t out[6])
{
    unsigned int b[6];
    int consumed = 0;
    if (s == NULL) { return AXL_ERR; }
    if (axl_sscanf(s, "%x:%x:%x:%x:%x:%x%n",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &consumed) != 6
        || s[consumed] != '\0') {
        return AXL_ERR;
    }
    for (size_t i = 0; i < 6; i++) {
        if (b[i] > 0xFF) { return AXL_ERR; }
        out[i] = (uint8_t)b[i];
    }
    return AXL_OK;
}

/* Parse a bounded non-negative integer arg (NULL/"" -> AXL_ERR). */
static int
parse_uint(const char *s, uint32_t *out)
{
    if (s == NULL || s[0] == '\0') { return AXL_ERR; }
    unsigned int v = 0;
    int consumed = 0;
    if (axl_sscanf(s, "%u%n", &v, &consumed) != 1 || s[consumed] != '\0') {
        return AXL_ERR;
    }
    *out = v;
    return AXL_OK;
}

/* Gather and validate all config-bearing flags. Prints a specific error and
   returns AXL_ERR on any conflict / malformed value; AXL_OK fills @c. */
static int
netload_cfg_parse(AxlArgs *a, NetloadCfg *c)
{
    axl_memset(c, 0, sizeof *c);
    c->retries = 1;

    const char *mac = axl_args_get_string(a, "mac");
    const char *nic = axl_args_get_string(a, "nic");
    if (mac && mac[0] && nic && nic[0]) {
        axl_printf("netload: --mac and --nic are mutually exclusive\n");
        return AXL_ERR;
    }
    if (mac && mac[0]) {
        if (parse_mac(mac, c->sel_mac) != AXL_OK) {
            axl_printf("netload: invalid --mac '%s' (want xx:xx:xx:xx:xx:xx)\n", mac);
            return AXL_ERR;
        }
        c->have_sel = true; c->sel_by_mac = true;
    } else if (nic && nic[0]) {
        uint32_t idx = 0;
        if (parse_uint(nic, &idx) != AXL_OK) {
            axl_printf("netload: invalid --nic '%s' (want an index)\n", nic);
            return AXL_ERR;
        }
        c->have_sel = true; c->sel_nic = idx;
    }

    const char *ip = axl_args_get_string(a, "ip");
    if (ip && ip[0]) {
        bool hp = false;
        axl_memset(c->st.mask, 0, 4);
        axl_ipv4_parse("255.255.255.0", c->st.mask);   /* default mask */
        if (axl_ipv4_parse_cidr(ip, c->st.ip, c->st.mask, &hp) != AXL_OK) {
            axl_printf("netload: invalid --ip '%s'\n", ip);
            return AXL_ERR;
        }
        const char *mask = axl_args_get_string(a, "mask");
        if (mask && mask[0]) {
            if (axl_ipv4_parse(mask, c->st.mask) != AXL_OK) {
                axl_printf("netload: invalid --mask '%s'\n", mask);
                return AXL_ERR;
            }
        }
        const char *gw = axl_args_get_string(a, "gw");
        if (gw && gw[0]) {
            if (axl_ipv4_parse(gw, c->st.gw) != AXL_OK) {
                axl_printf("netload: invalid --gw '%s'\n", gw);
                return AXL_ERR;
            }
            c->st.have_gw = true;
        }
        c->st.have = true;
    }

    /* DNS (usable on both static and DHCP paths). */
    const char *dns = axl_args_get_string(a, "dns");
    if (dns && dns[0]) {
        char tmp[64];
        axl_strlcpy(tmp, dns, sizeof tmp);
        char *comma = axl_strchr(tmp, ',');
        if (comma) { *comma = '\0'; }
        if (axl_ipv4_parse(tmp, c->st.dns[0]) != AXL_OK) {
            axl_printf("netload: invalid --dns '%s'\n", dns);
            return AXL_ERR;
        }
        c->st.ndns = 1;
        if (comma && comma[1]) {
            if (axl_ipv4_parse(comma + 1, c->st.dns[1]) != AXL_OK) {
                axl_printf("netload: invalid secondary --dns '%s'\n", comma + 1);
                return AXL_ERR;
            }
            c->st.ndns = 2;
        }
    }

    const char *dt = axl_args_get_string(a, "dhcp-timeout");
    if (dt && dt[0] && parse_uint(dt, &c->dhcp_timeout) != AXL_OK) {
        axl_printf("netload: invalid --dhcp-timeout '%s'\n", dt);
        return AXL_ERR;
    }
    const char *rt = axl_args_get_string(a, "retries");
    if (rt && rt[0]) {
        if (parse_uint(rt, &c->retries) != AXL_OK || c->retries == 0) {
            axl_printf("netload: invalid --retries '%s' (want >= 1)\n", rt);
            return AXL_ERR;
        }
    }

    c->ping     = axl_args_get_string(a, "ping");
    c->ping_gw  = axl_args_get_bool(a, "ping-gw");
    c->resolve  = axl_args_get_string(a, "resolve");
    c->want_json = axl_args_get_bool(a, "json");

    /* Static IP needs a target NIC; forbid it under a blind -a sweep. */
    if (c->st.have && !c->have_sel && axl_args_get_bool(a, "auto")) {
        axl_printf("netload: static IP needs a target NIC: add --mac or --nic, "
                   "or use --probe\n");
        return AXL_ERR;
    }
    return AXL_OK;
}
```

Call it early in `run_netload` (after `nv_init()`), before dispatching, and return 1 on AXL_ERR:

```c
    NetloadCfg cfg;
    if (netload_cfg_parse(a, &cfg) != AXL_OK) {
        return 1;
    }
```

- [ ] **Step 4: Run test both arches, confirm the four guard assertions GREEN.**

```bash
make ARCH=x64 tools >/dev/null 2>&1 && ./test/integration/test-netload-qemu.sh --arch X64 2>&1 | grep -E "PASS: (--ip without|--mac \+|malformed|-h)"
make ARCH=aa64 tools >/dev/null 2>&1 && ./test/integration/test-netload-qemu.sh --arch AARCH64 2>&1 | tail -5
```

Expected: all four PASS; overall suite PASS both arches.

### Task 4: Thread selection + static config + verify through the probe path

**Files:** Modify `tools/netload.c` — `probe_driver`, `probe_firmware_stack`, and their callers, to take the `NetloadCfg`.

**Interfaces:**
- Consumes: `NetloadCfg` from Task 3, `axl_net_bring_up`, `axl_net_set_dns`, `axl_net_ping`, `axl_net_resolve`, `axl_net_get_dhcp_lease_by_mac`, `axl_net_list_interfaces`.
- Produces:
  - New enum value `PR_NO_REACH` in `ProbeResult` (after `PR_LINK_NO_DHCP`).
  - `static int resolve_selected_index(const NetloadCfg *c, size_t *out_idx);` — maps `--mac`/`--nic` to a concrete `axl_net_list_interfaces` index; AXL_ERR if not found.
  - `static ProbeResult bring_up_and_verify(size_t nic_index, const NetloadCfg *c, const char *drv_name, DriverReport *rep);` — the shared "configure (static or DHCP w/ timeout+retries) → set DNS → verify" core used by both probe paths. Returns PR_OK / PR_NO_REACH / PR_LINK_NO_DHCP.
  - `probe_driver` / `probe_firmware_stack` gain a `const NetloadCfg *c` parameter.

- [ ] **Step 1: Add the token + label + color for `PR_NO_REACH`.** Extend `ProbeResult`, `outcome_label`, `outcome_color`, `log_token_label` (token `NOREACH` → "up, no reach", `AXL_CONSOLE_FG_YELLOW`), and `print_row_detail`. (Follow the existing `PR_LINK_NO_DHCP` cases verbatim, substituting the reach wording.)

- [ ] **Step 2: Add a failing assertion for the `NOREACH` render in `--dump`.** In `test-netload-qemu.sh`, seed a NOREACH log line via a new seam OR assert the label mapping through `--dump` after a crafted log. Simplest: extend `log_token_label` coverage by adding to the existing dump test a check that an unknown-safe path still works; but to test `NOREACH` specifically, add a hidden seam `--_log <TOKEN> <name>` that appends one log line, then `--dump`. Add the seam descriptor and handler (mirrors `--_mark`), then:

```bash
  echo 'echo MARK_NOREACH_SEED'; echo 'netload.efi --clear'
  echo 'netload.efi --_log NOREACH Rtk.efi'
  echo 'echo MARK_NOREACH_DUMP'; echo 'netload.efi --dump'
```

```bash
sect MARK_NOREACH_DUMP MARK_DONE | grep -aqiE "up, no reach|Rtk\.efi" \
  && echo "PASS: NOREACH renders in --dump" || { echo "FAIL: noreach dump"; fail=1; }
```

Run, confirm FAIL (seam + token not present yet).

- [ ] **Step 3: Implement `resolve_selected_index` + `bring_up_and_verify` + the `--_log` seam.**

```c
/* Map --mac / --nic to a concrete list_interfaces index. AXL_ERR if the
   selector matches no present interface. */
static int
resolve_selected_index(const NetloadCfg *c, size_t *out_idx)
{
    if (!c->have_sel) { return AXL_ERR; }
    if (!c->sel_by_mac) {
        *out_idx = c->sel_nic;   /* by index: trust it; bring_up validates range */
        return AXL_OK;
    }
    size_t count = 0;
    axl_net_list_interfaces(NULL, &count);
    if (count == 0) { return AXL_ERR; }
    AxlNetInterface *ifs = axl_calloc(count, sizeof *ifs);
    if (ifs == NULL) { return AXL_ERR; }
    axl_net_list_interfaces(ifs, &count);
    int rc = AXL_ERR;
    for (size_t i = 0; i < count; i++) {
        if (axl_memcmp(ifs[i].mac, c->sel_mac, 6) == 0) { *out_idx = i; rc = AXL_OK; break; }
    }
    axl_free(ifs);
    return rc;
}

/* Configure @nic_index (static if cfg.st.have, else DHCP with the configured
   timeout, retried cfg.retries times), program DNS, then run any requested
   verification. Verification failure downgrades a configured NIC to
   PR_NO_REACH. Fills @rep's ip/have_ip on a successful configure. */
static ProbeResult
bring_up_and_verify(size_t nic_index, const NetloadCfg *c, const char *drv_name,
                    DriverReport *rep)
{
    uint32_t timeout = c->dhcp_timeout ? c->dhcp_timeout : 15;
    const uint8_t *ip   = c->st.have ? c->st.ip   : NULL;
    const uint8_t *mask = c->st.have ? c->st.mask : NULL;
    const uint8_t *gw   = (c->st.have && c->st.have_gw) ? c->st.gw : NULL;

    AxlIPv4Address got = {0};
    int rc = AXL_ERR;
    for (uint32_t attempt = 0; attempt < c->retries && rc != AXL_OK; attempt++) {
        if (attempt > 0) {
            axl_printf("  retry %u/%u ...\n", attempt + 1, c->retries);
        }
        rc = axl_net_bring_up(nic_index, ip, mask, gw, timeout, &got);
    }
    if (rc != AXL_OK) {
        return PR_LINK_NO_DHCP;   /* configured link but no address */
    }
    if (rep != NULL) {
        rep->have_ip = true;
        axl_memcpy(rep->ip, got.addr, 4);
    }
    if (c->st.ndns > 0) {
        axl_net_set_dns(nic_index, c->st.dns[0], c->st.ndns > 1 ? c->st.dns[1] : NULL);
    }

    /* Verification: any requested check that fails => not really up. */
    bool verify_requested = (c->ping && c->ping[0]) || c->ping_gw || (c->resolve && c->resolve[0]);
    if (!verify_requested) {
        axl_printf("  [ok] address up: %u.%u.%u.%u\n",
                   got.addr[0], got.addr[1], got.addr[2], got.addr[3]);
        return PR_OK;
    }
    bool reach_ok = true;
    if (c->ping && c->ping[0]) {
        AxlIPv4Address tgt = {0};
        size_t rtt = 0;
        if (axl_ipv4_parse(c->ping, tgt.addr) != AXL_OK
            || axl_net_ping(&tgt, 2000, &rtt) != AXL_OK) {
            axl_printf("  ping %s FAILED -- configured but unreachable\n", c->ping);
            reach_ok = false;
        } else {
            axl_printf("  [ok] ping %s: %zu ms\n", c->ping, rtt);
        }
    }
    if (c->ping_gw) {
        uint8_t gwip[4] = {0};
        bool have = false;
        if (gw != NULL) { axl_memcpy(gwip, gw, 4); have = true; }
        else {
            size_t count = 0;   /* look up gateway from the lease by MAC */
            axl_net_list_interfaces(NULL, &count);
            AxlNetInterface *ifs = count ? axl_calloc(count, sizeof *ifs) : NULL;
            if (ifs) {
                axl_net_list_interfaces(ifs, &count);
                if (nic_index < count) {
                    AxlDhcpLease lease = {0};
                    if (axl_net_get_dhcp_lease_by_mac(ifs[nic_index].mac, &lease) == AXL_OK) {
                        axl_memcpy(gwip, lease.router, 4); have = true;
                    }
                }
                axl_free(ifs);
            }
        }
        if (!have) {
            axl_printf("  --ping-gw: no gateway known -- skipping\n");
        } else {
            AxlIPv4Address tgt = {0};
            size_t rtt = 0;
            axl_memcpy(tgt.addr, gwip, 4);
            if (axl_net_ping(&tgt, 2000, &rtt) != AXL_OK) {
                axl_printf("  ping gateway %u.%u.%u.%u FAILED\n",
                           gwip[0], gwip[1], gwip[2], gwip[3]);
                reach_ok = false;
            } else {
                axl_printf("  [ok] ping gateway: %zu ms\n", rtt);
            }
        }
    }
    if (c->resolve && c->resolve[0]) {
        AxlIPv4Address ra = {0};
        if (axl_net_resolve(c->resolve, &ra) != AXL_OK) {
            axl_printf("  resolve %s FAILED\n", c->resolve);
            reach_ok = false;
        } else {
            axl_printf("  [ok] resolve %s -> %u.%u.%u.%u\n", c->resolve,
                       ra.addr[0], ra.addr[1], ra.addr[2], ra.addr[3]);
        }
    }
    if (!reach_ok) {
        log_append_fmt("NOREACH %s", drv_name);
        return PR_NO_REACH;
    }
    return PR_OK;
}
```

Add the `--_log` seam descriptor (grouped with the other seams) and, in `run_netload`, a handler that appends one line and exits:

```c
    const char *lg = axl_args_get_string(a, "_log");
    if (lg != NULL && lg[0] != '\0') {           /* test seam: seed one log line */
        const char *nm = axl_args_get_string(a, "_logname");
        char line[128];
        axl_snprintf(line, sizeof line, "%s %s", lg, nm && nm[0] ? nm : "X.efi");
        log_append(line);
        return 0;
    }
```

(Declare `_log`/`_logname` as string seams. The lease struct is `AxlDhcpLease` and the gateway field is `router[4]` — confirmed against `axl-net.h` L563-570.)

- [ ] **Step 4: Replace the inline DHCP-only `axl_net_bring_up(...)` calls in `probe_driver` and `probe_firmware_stack` with `bring_up_and_verify(i, c, name/drv, rep)`**, and add the `const NetloadCfg *c` parameter to both, threading it from `cmd_auto` / `cmd_interactive` / `--probe` / `--connect`. In the `-a` sweep, when `c->have_sel`, skip a candidate whose produced NIC's MAC/index doesn't match the selector (compare `ifs[i]` MAC to `c->sel_mac`, or index to `c->sel_nic`). Map the new return: `PR_NO_REACH` behaves like `PR_LINK_NO_DHCP` in `cmd_auto` (continue), and makes `--probe` return non-zero.

- [ ] **Step 5: Run both arches; confirm the NOREACH dump PASSes and nothing regressed.**

```bash
make ARCH=x64 tools >/dev/null 2>&1 && ./test/integration/test-netload-qemu.sh --arch X64 2>&1 | tail -8
make ARCH=aa64 tools >/dev/null 2>&1 && ./test/integration/test-netload-qemu.sh --arch AARCH64 2>&1 | tail -8
```

Expected: full suite PASS both arches, including `PASS: NOREACH renders in --dump`.

- [ ] **Step 6: Refactor while green** — dedupe the two `list_interfaces` count/alloc/read blocks into a small helper if it reads cleanly; ensure `bring_up_and_verify` is the single bring-up site.

### Task 5: Docs, review, commit Tier 1

- [ ] **Step 1:** Update `docs/AXL-Netload-Design.md`: add the new flags + short forms to the Modes section, add a "Static config & verification" section (static needs a selector, verify gates success, `PR_NO_REACH`), and note `--dhcp-timeout`/`--retries`.
- [ ] **Step 2:** `make check-ascii && make check-docs` → both clean.
- [ ] **Step 3:** Independent code review (contract skipped — designed with the user; integration review required per `feedback_code_review_before_commit`). Apply fixes.
- [ ] **Step 4:** Update `memory/project_netload_tool_2026-07-15.md` (Tier 1 landed).
- [ ] **Step 5: Commit (GATED).**

```bash
git status && git add tools/netload.c test/integration/test-netload-qemu.sh docs/AXL-Netload-Design.md
git commit -m "netload: static IP + verify + NIC selection + short flags (Tier 1)

Every flag now has a single-char short form (bounce-console ergonomics).
--ip[/N]/--mask/--gw/--dns bring a NIC up statically through the same
axl_net_bring_up primitive as DHCP; --mac/--nic select one NIC (mutually
exclusive); static requires a selector (hard error under a blind -a).
--dhcp-timeout/--retries tune the DHCP wait. --ping/--ping-gw/--resolve
verify reachability and gate success (new PR_NO_REACH outcome). Both arches."
```

---

## COMMIT 3 — `--save` / `--apply`

### Task 6: Config NVRAM key + save on win + apply-first ordering

**Files:** Modify `tools/netload.c`; add assertions to `test-netload-qemu.sh`.

**Interfaces:**
- Produces: `static void config_save(const char *driver, const NetloadCfg *c, const uint8_t nic_mac[6]);`, `static bool config_load(char *driver, size_t dcap, NetloadCfg *c, uint8_t nic_mac[6]);`, `static int cmd_apply(const char *dir, const NetloadCfg *c);`. NVRAM key `"Config"` = `driver|method|ip|mask|gw|dns|mac` (method `dhcp`/`static`).

- [ ] **Step 1: Failing assertions** — save via a seam, apply reads it back. Add a `--_saveconf` seam that writes a synthetic Config line (so no real win is needed headless), then assert `--dump` shows it and `--clear` removes it:

```bash
  echo 'echo MARK_SAVE'; echo 'netload.efi --clear'
  echo 'netload.efi --_saveconf Rtk.efi|static|10.0.0.5|255.255.255.0|10.0.0.1||60:7d:09:57:f8:bf'
  echo 'echo MARK_SAVE_DUMP'; echo 'netload.efi --dump'
  echo 'echo MARK_SAVE_CLR'; echo 'netload.efi --clear'
  echo 'echo MARK_SAVE_GONE'; echo 'netload.efi --dump'
```

```bash
sect MARK_SAVE_DUMP MARK_SAVE_CLR | grep -aqiE "Rtk\.efi.*static|saved config|10\.0\.0\.5" \
  && echo "PASS: saved Config shown in --dump" || { echo "FAIL: config dump"; fail=1; }
sect MARK_SAVE_GONE MARK_DONE | grep -aqiE "10\.0\.0\.5" \
  && { echo "FAIL: Config survived --clear"; fail=1; } || echo "PASS: --clear removes Config"
```

Run, confirm RED.

- [ ] **Step 2: Implement** `config_save`/`config_load`/`cmd_apply`, the `--_saveconf` seam, `Config` deletion in `cmd_clear`, and a "saved config" line in `cmd_dump`. Serialize with `|` fields (empty when absent); parse with a bounded tokenizer. On a real win in `cmd_auto`/`cmd_interactive`/`--probe`, if `--save`, call `config_save(winner, &cfg, winner_mac)`. Wire ordering in `run_netload`: explicit `--apply` → `cmd_apply` first; under `-a`, after `probe_firmware_stack` returns non-OK and before the staged loop, if a Config exists try `cmd_apply` (best-effort).
- [ ] **Step 3:** Run both arches → GREEN. Refactor while green.
- [ ] **Step 4:** Docs (`Config` row in the NVRAM-state table + `--save`/`--apply` in Modes), `make check-ascii`/`check-docs`, review, memory, commit GATED:

```bash
git commit -m "netload: --save/--apply working-config to NVRAM (Config key)

Persist the winning driver+method+static params+MAC to a bounded Config
NVRAM var; --apply re-applies it (driver load + bring_up + verify),
skipping the sweep. Explicit --apply tries saved first; under -a it runs
after firmware-first, before the staged sweep. --clear drops Config."
```

---

## COMMIT 4 — `--json` + `--retries` surfacing

### Task 7: JSON result object + retries visibility

**Files:** Modify `tools/netload.c`; add assertions to `test-netload-qemu.sh`.

**Interfaces:**
- Produces: `static void print_json_result(const DriverReport *r, const NetloadCfg *c);` — one line: `{"driver":"..","method":"dhcp|static","ip":"a.b.c.d","mask":"..","gw":"..","link":true,"result":"leased|noreach|no-lease|none"}` plus a `ping` sub-object when a ping ran. `--retries` was already parsed (Task 3) and consumed in `bring_up_and_verify` (Task 4) — this task only surfaces it in JSON and confirms the retry log line.

- [ ] **Step 1: Failing assertion** — `--json` on the `--_key`/`--probe` seam path emits a `{...}` line. Add:

```bash
  echo 'echo MARK_JSON'; echo "netload.efi --probe Aaa.efi --json --dir fs0:\\drivers\\$NAT"
```

```bash
sect MARK_JSON MARK_DONE | grep -aqE '\{"driver":"Aaa\.efi"' \
  && echo "PASS: --json emits a result object" || { echo "FAIL: json object"; fail=1; }
sect MARK_JSON MARK_DONE | grep -aqiE "loading Aaa\.efi" \
  && echo "PASS: --json keeps progress logs" || { echo "FAIL: json progress"; fail=1; }
```

Run, confirm RED (progress line may already pass; the `{` line fails).

- [ ] **Step 2: Implement** `print_json_result`, called at the end of `probe_driver` (for `--probe`) and after the sweep/interactive summary when `cfg.want_json`. In JSON mode, suppress the decorative `print_summary` tables but keep progress logs (guard the `print_summary` call on `!cfg.want_json`; still persist the SWEEP NVRAM line). Escape nothing exotic — driver names are ASCII basenames; emit numeric/bool literally.
- [ ] **Step 3:** Run both arches → GREEN. Refactor while green.
- [ ] **Step 4:** Docs (`--json`/`--retries` in Modes + an example object), gates, review, memory, commit GATED:

```bash
git commit -m "netload: --json machine result + --retries surfacing

--json keeps live progress logs and appends a single JSON result object
(driver, method, ip/mask/gw, link, ping, result); decorative summary
tables suppressed in JSON mode. --retries N (parsed in Tier 1) retries a
link-up-no-lease bring-up; count reflected in the result."
```

---

## COMMIT 5 — Firmware-first summary row

### Task 8: `probe_firmware_stack` fills reports[0] always

**Files:** Modify `tools/netload.c`; add assertion to `test-netload-qemu.sh`.

**Interfaces:**
- Consumes: `DriverReport`, `probe_firmware_stack`.
- Produces: `DriverReport` gains `bool is_firmware;`. `probe_firmware_stack` takes `DriverReport *rep` and fills it (name = bound firmware driver or `"(firmware)"`, `is_firmware=true`, detail via result: PR_NO_NIC→"no firmware NIC bound", PR_OK→leased, PR_LINK_NO_DHCP→linked-no-lease). `cmd_auto` inserts it as `reports[0]` before the staged rows (even on a firmware win, so the summary prints).

- [ ] **Step 1: Failing assertion** — under `--net` (the second QEMU boot in the test), `-a`'s summary shows a firmware row. In the `--net` boot's nsh/assertions (the firmware-first regression section), add:

```bash
sect MARK_NET_AUTO MARK_DONE | grep -aqiE "\(firmware\)|firmware.*no firmware NIC bound|firmware.*link" \
  && echo "PASS: firmware path appears in the summary" || { echo "FAIL: firmware summary row"; fail=1; }
```

(If there is no `-a` invocation in the `--net` boot yet, add `netload.efi -a` there guarded so a real SLIRP lease is fine — the row must appear whether it leased or not.) Run, confirm RED.

- [ ] **Step 2: Implement.** Add `is_firmware` to `DriverReport`; give `probe_firmware_stack` a `DriverReport *rep` out-param and fill it on every path; in `cmd_auto`, always seed `reports[0]` from the firmware probe (shift staged rows to start at index 1), and on a firmware win still call `print_summary` before returning 0. Extend `outcome_label`/`print_row_detail` to render the firmware row distinctly (prefix `firmware:` or a `[fw]` tag).
- [ ] **Step 3:** Run both arches (including the `--net` boot) → GREEN. Refactor while green.
- [ ] **Step 4:** Docs (Findings-report section: firmware row always present), gates, review, memory, commit GATED:

```bash
git commit -m "netload: put the firmware-first result in the summary (reports[0])

probe_firmware_stack now fills a DriverReport (is_firmware, name = bound
driver or (firmware), detail per outcome), always inserted as summary row
0 -- even on a firmware win. Operators can now see whether the built-in
drivers were tried and what they found, instead of it scrolling off."
```

---

## Self-Review

**Spec coverage:**
- §1 short-opts → Task 2 (map applied verbatim). ✓
- §2 static + selection (CIDR, mask/gw/dns, mutual-exclusion, selector-required) → Tasks 1, 3, 4. ✓
- §3 verify + `PR_NO_REACH` gating → Task 4. ✓
- §4 save/apply (ordering, Config key) → Task 6; json (keep progress + append) → Task 7; retries → Tasks 3/4/7; firmware row → Task 8. ✓
- New API `axl_ipv4_parse_cidr` → Task 1 (library, test-first). ✓
- Deferred VLAN/MTU, dropped fetch/status → not in plan (correct). ✓
- Both-arch testing, ASCII/docs gates, GATED commits, memory updates → every task. ✓

**Placeholder scan:** No TBD/TODO; every code step shows full code. The two former verify-against-header notes are now resolved inline (lease struct `AxlDhcpLease`.`router`; no `AXL_ARG_INT`, use STRING + `parse_uint`).

**Type consistency:** `NetloadCfg`/`NetloadStatic` field names used identically across Tasks 3/4/6/7. `PR_NO_REACH` + token `NOREACH` consistent (Task 4 defines, Tasks 6/7 reuse). `bring_up_and_verify`/`resolve_selected_index`/`config_save`/`config_load`/`print_json_result` signatures stable across the tasks that call them.

**Known confirm-at-impl items (call out, don't guess):**
1. ~~Lease gateway field~~ — RESOLVED: `AxlDhcpLease.router[4]` (axl-net.h L563-570).
2. ~~`AXL_ARG_INT`~~ — RESOLVED: doesn't exist; STRING + `parse_uint`.
3. The `--net` boot already having (or needing) an `-a` invocation for Task 8's assertion — verify in `test-netload-qemu.sh` when reaching Task 8.
