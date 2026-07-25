# netload → full-featured network bring-up tool (design)

**Date:** 2026-07-16 · **Tool:** `tools/netload.c` · **Living doc:**
`docs/AXL-Netload-Design.md` · **Repo:** `aximcode/axl-sdk` (branch `main`,
work committed GATED / not pushed).

This spec records the design **decisions and rationale** for turning netload
from a crash-safe NIC-driver *loader/prober* into a complete network
**bring-up** tool. The user-facing design lives in `docs/AXL-Netload-Design.md`;
this file is the decision record behind that update (no content duplication —
rationale here, living design there).

## Purpose (reframed)

**The whole reason for the tool is to get an active, verified IPv4 network
connection up on a system.** "Networking is up" means *reachable* — a NIC that
leases or takes a static address but cannot reach anything is **not** up. The
flaky iDRAC/KVM console is a real, secondary constraint (it drives the
single-key menu and the mandatory short-option forms), but it is not the goal;
the working IPv4 connection is.

Two consequences shape the design:

1. **Verification is core, not cosmetic.** `--ping` / `--ping-gw` / `--resolve`
   gate success. This directly catches the RNDIS "link up, lease OK, no data
   plane" class the tool was born to diagnose.
2. **Static IP is first-class.** Many target segments have no DHCP server
   (the real 2026-07-16 case: a Realtek `RtkUsbUndiDxe` linked but never
   leased). Static config must flow through the same bring-up call no matter
   how the NIC came up.

## Scope

**In:** static IPv4 (`--ip`/`--mask`/`--gw`, CIDR + separate flags), static DNS
(`--dns`), DHCP timeout override (`--dhcp-timeout`), NIC selection
(`--mac`/`--nic`), verification (`--ping`/`--ping-gw`/`--resolve`, gating
success), saved/applied config (`--save`/`--apply`), machine output (`--json`),
retry (`--retries`), a firmware-first summary row, and a single-char short form
for **every** flag (new and existing).

**Deferred (no backing axl API today — tracked as separate library work):**
`--vlan` (needs `EFI_VLAN_CONFIG_PROTOCOL` in the manifest + a thin
`axl_net_vlan_*` API; only the GUID exists today), `--mtu` (read-only on
`AxlNetInterface`; weak EFI protocol support). `--fetch` / `--status` are
**dropped** — HTTP reachability belongs to the `fetch` tool and status to
`netinfo`; netload adds a one-line pointer rather than duplicating them.

## Verified backing APIs (all exist unless noted)

| Need | API | Header |
|---|---|---|
| static IP / mask / gw / DHCP timeout | `axl_net_bring_up(idx, ip, mask, gw, timeout, out)` | axl-net.h |
| IPv4 parse | `axl_ipv4_parse(str, oct[4])` | axl-net.h / axl-net-addr.c |
| **CIDR parse** | **`axl_ipv4_parse_cidr` — NEW (below)** | axl-net-addr.c |
| DNS setter | `axl_net_set_dns(idx, dns[4], dns2)` | axl-net.h |
| ping | `axl_net_ping(target, ms, &rtt)` | axl-net.h |
| DNS resolve | `axl_net_resolve(name, &addr)` | axl-net.h |
| gateway of a lease | `axl_net_get_dhcp_lease_by_mac(mac, &lease)` | axl-net.h |
| config method readback | `axl_net_last_config_method()` | axl-net.h |
| enumerate / link / driver | `axl_net_list_interfaces` / `_get_link_stats` / `_get_driver_info` | axl-net.h |

### New public API: `axl_ipv4_parse_cidr`

The only genuinely-new library API (the user approved "add new APIs if
necessary"). In `src/net/axl-net-addr.c`, declared in `axl-net.h`:

```c
/** Parse "A.B.C.D" or "A.B.C.D/N" (N = 0..32).
    On success writes @p octets always; writes @p mask only when a /N suffix
    is present (caller pre-seeds a default mask otherwise).
    @return AXL_OK, or AXL_ERR on malformed input / N>32 / NULL out. */
int axl_ipv4_parse_cidr(const char *str, uint8_t octets[4], uint8_t mask[4],
                        bool *had_prefix);
```

Backed by a small `prefix_to_mask(n, mask[4])`. Test-first as a **library**
unit test (`test/unit/axl-test-net-*.c` or the addr test) with exact-value
assertions — not just in the tool.

## §1 Short-option allocation (HARD CONSTRAINT — settled first)

Every flag has a collision-free single char. Framework reserves `-h`/`?`
(help), `-b` (page), `--version` — all avoided. `axl-args` `.short_name`
carries these.

| Flag | Short | Flag | Short |
|---|---|---|---|
| `--auto` | `-a` *(exists)* | `--ip A.B.C.D[/N]` | `-i` |
| `--list` | `-l` | `--mask M` | `-m` |
| `--probe DRV` | `-p` | `--gw G` | `-g` |
| `--dump` | `-u` | `--dns S[,S2]` | `-e` |
| `--clear` | `-c` | `--dhcp` (force explicit) | `-H` |
| `--dir PATH` | `-D` | `--dhcp-timeout N` | `-t` |
| `--no-deps` | `-N` | `--ping TARGET` | `-P` |
| `--connect` (firmware) | `-f` | `--ping-gw` | `-G` |
| `--debug` | `-v` | `--resolve NAME` | `-r` |
| `--save` | `-s` | `--mac XX:XX:..` | `-M` |
| `--apply` | `-y` | `--nic N` | `-n` |
| `--json` | `-j` | `--retries N` | `-R` |

Interactive menu keys (`1-9`/`a-z`/`A`/`F`/`r`/`q`) are a **separate**
namespace and unchanged. `--_mark`/`--_key` test seams stay short-less.

## §2 Static IP + NIC selection

- **Syntax:** both CIDR (`--ip 10.0.0.5/24`) and separate (`--ip 10.0.0.5
  --mask 255.255.255.0`). CIDR is fast on a bouncing console; separate flags
  are explicit. `--mask`/`--gw`/`--dns` parse via `axl_ipv4_parse`; `--dns`
  accepts `S` or `S,S2`.
- **Selection:** `--mac` and `--nic` are **mutually exclusive** (both → error).
  Each resolves to a concrete index via `axl_net_list_interfaces` (MAC match
  for `--mac`). Selection applies to `-a` (skip non-matching NICs),
  `--connect` (filter firmware NICs), and static config.
- **Static requires a selector.** `--ip` is valid only with `--mac`/`--nic`,
  or under `--probe`/interactive single pick (where "the NIC this driver
  produced" is the target — the first link-up new interface from the diff).
  **`--ip` + `-a` without a selector is a hard error** with a clear message
  ("static IP needs a target NIC: add --mac or --nic, or use --probe") —
  applying one address to every link-up NIC is wrong and would create
  duplicate-IP conflicts on multi-NIC servers.
- **One bring-up primitive.** Static flows through the SAME
  `axl_net_bring_up(idx, ip, mask, gw, timeout, &out)` in both `probe_driver`
  (staged) and `probe_firmware_stack` (firmware-first). `--dns` calls
  `axl_net_set_dns` after a successful bring-up.

## §3 Verification (gates "is it REALLY up?")

- `--ping TARGET` → `axl_net_ping`. `--ping-gw` → ping the gateway learned from
  `axl_net_get_dhcp_lease_by_mac` (DHCP) or `--gw` (static). `--resolve NAME`
  → `axl_net_resolve`.
- **Gating:** when any verify is requested and fails, the NIC is **not up**.
  New `ProbeResult PR_NO_REACH` ("up, no reach", orange row, NVRAM token
  `NOREACH`). Behavior:
  - `--probe` / interactive single pick: report FAILURE (non-zero exit) +
    diagnostic ("configured/leased but TARGET unreachable — …").
  - `-a` sweep: logged like `LINK_NO_DHCP`; the sweep **continues** to the
    next driver (a reachable NIC is still the goal).
- The existing RNDIS-specific "link up, no lease → try rndisfix" heuristic is
  preserved and now complemented by the reachability check.

## §4 Save / apply / json / retries (Tier 2)

- **`--save`:** after a win, persist a bounded NVRAM `Config` line under the
  existing netload vendor GUID: `driver|method|ip|mask|gw|dns|mac`
  (`method` ∈ `dhcp`/`static`; static fields empty for DHCP). Same
  drop-oldest / best-effort-durable discipline as `Log`/`Quarantine`; no new
  GUID. `--clear` also deletes `Config`.
- **`--apply`:** read `Config`, breadcrumb + load that one driver, `bring_up`
  with the saved method/params on the saved MAC's NIC, then run the same
  verify. **Ordering:** explicit `--apply` tries the saved config **first**
  (fast known-good repeat); a saved config consulted under `-a` runs
  **after** firmware-first (zero-stage firmware is cheaper) but **before** the
  staged sweep.
- **`--json`:** keep progress logs (the flaky-console operator watches them
  live), and **append** a single machine-readable result object at the very
  end: `{driver, method, ip, mask, gw, dns, link, ping:{target,rtt_ms,ok},
  result}`. Suppress the decorative summary tables in JSON mode (the object is
  the result); progress lines remain.
- **`--retries N`** (default 1): retry the `bring_up` wait N times on a
  link-up-no-lease NIC before moving on. `--wait` is folded into this
  (redundant with `--dhcp-timeout` + `--retries`).
- **Firmware-in-summary row** (handoff §F): `probe_firmware_stack` fills a
  `DriverReport` with an `is_firmware` flag (name = bound firmware driver or
  `(firmware)`; detail = "no firmware NIC bound" / "LEASED via X" /
  "linked, no lease"), added as `reports[0]` **always** — even on a firmware
  win — so the summary is never silent on whether the built-in path was tried.

## §5 Testing, docs, commits

- **Test-first, bucket B** (exact-string QEMU assertions,
  `axl_strcmp(buf, "…")==0`). Both arches (X64 + AARCH64) every time.
- **QEMU-exercisable coverage (new):** CIDR parse (library unit test, exact
  values incl. `/0`, `/32`, malformed, `N>32`); short-flag resolution for a
  representative set; `--ip`-without-selector error string; `--mac`/`--nic`
  mutual-exclusion error; `PR_NO_REACH`/`NOREACH` rendering in `--dump`;
  `Config` save→apply round-trip (structural, dummy driver + fixture);
  firmware-summary row appears as row 0 under `--net`.
- **Hardware-only (documented as such, NOT claimed QEMU-tested):** real DHCP
  vs static bring-up on an iDRAC NIC, real `--ping`/`--resolve` reachability,
  static IP on a no-DHCP segment.
- **Commits (GATED, no push), each: `make check-ascii` + `make check-docs` +
  independent review first:**
  1. **Tier 1** (one coherent unit): short-option retrofit on all flags +
     `axl_ipv4_parse_cidr` (lib) + `--ip`/`--mask`/`--gw`/`--dns` +
     `--dhcp-timeout` + `--mac`/`--nic` + `--ping`/`--ping-gw`/`--resolve`
     with `PR_NO_REACH` gating.
  2. `--save`/`--apply` (+ `Config` NVRAM key, ordering).
  3. `--json` + `--retries`.
  4. Firmware-in-summary row.
- **Living doc:** update `docs/AXL-Netload-Design.md` alongside each unit —
  Modes table (new flags + short forms), a new "Static config & verification"
  section, and the `Config` key in the NVRAM-state table.
- **Memory:** update `memory/project_netload_tool_2026-07-15.md` per unit.

## Invariants to preserve (from the current tool)

- `axl_driver_connect(NULL)` stays a recursive `connect -r` (guarded by the
  `--net` lease test) — firmware-first + nested deps depend on it.
- Exactly one driver name in `NetloadTrying` across any single load (crash
  culprit pins to one driver); dependency loads are separate breadcrumbed
  steps.
- Single-keypress interactive pick unchanged; NVRAM writes best-effort
  (warn + continue on read-only/full store), never a hard dependency.
- All on-screen strings ASCII (`make check-ascii`).
