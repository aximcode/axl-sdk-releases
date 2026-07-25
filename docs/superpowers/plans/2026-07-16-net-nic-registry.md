# axl-net NIC Registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `nic_index` mean "the Nth physical NIC" identically across every `axl-net` public API, via an internal MAC-keyed registry built once per public call.

**Architecture:** A new internal module `src/net/axl-net-nic.c` owns the canonical NIC model: it dedupes SimpleNetwork child handles by MAC (first handle wins, stable firmware order) and correlates each physical NIC to its IP4Config2 handle by MAC, in one pass with two `LocateHandleBuffer` calls total. Every index-taking public API builds this snapshot on entry, bounds-checks the ordinal, and reads `registry[nic]`. The `>= count → 0` clamp is deleted everywhere; `ip4cfg_for_nic` and `ip4cfg_for_nic_index` are deleted with it.

**Tech Stack:** C (UEFI freestanding), `axl_efi_call` for protocol calls, AXL backend API (`axl_bs()`, `axl_backend_free`), QEMU/OVMF integration tests, GNU make.

**Spec:** `docs/superpowers/specs/2026-07-16-net-nic-registry-design.md` — read it before starting.

## Global Constraints

- **Both arches, always.** Every test run is `make ARCH=x64` **and** `make ARCH=aa64`. A change is not green until both are.
- **Commit but DO NOT push.** All work is GATED on `main`. No `git push` at any point.
- **Test-first.** Write the test, run it, confirm it FAILS for the expected reason, then implement. Bucket A/B (public API semantics + exact output) and bucket D (bug regression) per `CLAUDE.md`.
- **No backticks in `git commit -m`** — bash command-substitutes them. Use `git commit -F -` with a heredoc, as every commit step below does.
- **No references to Claude/AI in commit messages, code, or docs.** No `Co-Authored-By`.
- **Style:** `axl_snake_case` functions, `AxlPascalCase` types, 4-space indent, K&R braces, no space before parens. `///<` inline param docs, `@brief`/`@return` block comments. See `docs/AXL-Coding-Style.md`.
- **No public UEFI types.** `AxlNic` is internal (`src/net/axl-net-internal.h`), so `EFI_HANDLE` in it is fine. It must never appear in `include/axl/`.
- **Fix compile warnings before moving on.**
- **Never assert with `axl_strstr` for output checks** — exact `axl_strcmp(buf, "...") == 0` only.
- **No `test_check(true, "...")` tautologies.**
- **SKIP-path balancers must equal the populated-path count** per `feedback_balancer_count`.
- **Do NOT drive `set_static_ip` / `set_dns` positively from unit tests** — a valid index reconfigures live firmware. Negative-index and NULL guards only.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/net/axl-net-nic.c` | **Create.** Owns the NIC registry: build, free, AUTO resolution, ip4cfg resolution. Nothing else. | 2 |
| `src/net/axl-net-internal.h` | **Modify.** Declare `AxlNic`, `_axl_net_nics_build`, `_axl_net_nics_free`, `_axl_net_nic_resolve_ip4cfg`. | 2 |
| `src/net/axl-net-interfaces.c` | **Modify.** `list_interfaces` reads the registry; `populate_ipv4_for_mac` deleted (registry does it). `locate_sb` / `get_ip_address` untouched. | 3 |
| `src/net/axl-net-linkstats.c` | **Modify.** `get_link_stats` reads `registry[nic].link_up`. | 4 |
| `src/net/axl-net-dhcp.c` | **Modify.** `ip4cfg_for_nic` + `ip4cfg_for_nic_index` deleted; `set_dns` / `wait_ip_settled` / `get_dhcp_lease` / `set_static_ip` / `auto_init` resolve via the registry. | 5 |
| `src/net/axl-net-arp.c` | **Modify.** `arp_list` correlates the ARP-SB handle by MAC. **Gated on Task 6's spike.** | 7 |
| `include/axl/axl-net.h` | **Modify.** Rewrite the now-false index-divergence docs. | 8 |
| `src/net/README.md` | **Modify.** Prose staleness pass. | 8 |
| `tools/netload.c` | **Modify.** Delete the three redundant `seen[32][6]` display dedups. | 9 |
| `test/unit/axl-test-net.c` | **Modify.** Safe negatives + live ordinal-consistency checks in `net-diag` mode. | 1, 3, 4, 5 |
| `test/integration/test-netload-qemu.sh` | **Modify.** Add the "2 interfaces, not 6" assertion to the existing multi-NIC boot. | 3 |

## Key Facts You Need

Read these before Task 1; they are not obvious from the code.

- **A single physical NIC publishes 2–3 SimpleNetwork child handles.** This is OVMF's SNP/MNP layering, not a NIC-model quirk — it reproduces on virtio and on real Dell hardware alike. `LocateHandleBuffer(SNP)` therefore returns ~3× more handles than there are NICs, and ~3× more than `LocateHandleBuffer(IP4Config2)` returns.
- **The two enumerations diverge in BOTH order and count.** Indexing one with the other's index is the bug this whole plan exists to remove.
- **`AXL_NET_NIC_AUTO` is `(uint64_t)-1` → `SIZE_MAX`** (`include/axl/axl-net-opts.h:61`). It is currently implemented *by the clamp we are deleting*. Task 5 replaces it explicitly. Do not skip that.
- **Existing test homes:** `test/integration/test-netdiag-qemu.sh` boots `AxlTestNet net-diag` with ONE live NIC (SLIRP, lease `10.0.2.15`, gw `10.0.2.2`) — live reads go there, using the `ND_CHECK` macro. `test/integration/test-netload-qemu.sh:531` boots TWO virtio NICs where only the high-index one (`52:54:00:12:34:57`) has DHCP — the multi-NIC guard. The plain `test-axl.sh` unit boot has NO NIC, so `list_interfaces` returns 0 there; anything needing a NIC must SKIP-balance.
- **Why the reverted appendix code in `local/docs/handoff-net-nic-registry.md` must NOT be pasted back:** it zero-fills `mac[6]` and compares all 6 bytes, so two handles reporting `HwAddressSize == 0` collapse into one row and match an arbitrary IP4Config2 handle. The registry skips such handles instead. The registry also subsumes that helper entirely.

---

### Task 1: Baseline — confirm green before touching anything

No production code changes. This exists so that any later failure is attributable to our change rather than to a pre-existing break.

**Files:**
- Modify: none

- [ ] **Step 1: Build both arches**

```bash
cd /home/mgosha/projects/aximcode/axl-sdk
make ARCH=x64 tools && make ARCH=aa64 tools && make ARCH=x64 tests && make ARCH=aa64 tests
```

Expected: exit 0, no warnings.

- [ ] **Step 2: Run the multi-NIC regression, both arches**

```bash
timeout 300s ./test/integration/test-netload-qemu.sh --arch X64
timeout 300s ./test/integration/test-netload-qemu.sh --arch AARCH64
```

Expected: both print `=== PASS (X64) ===` / `=== PASS (AARCH64) ===`. Specifically confirm this line appears in each:
`PASS: multi-NIC IP4Config2 resolved by MAC -- the high-index DHCP NIC leases`

**If this line does NOT pass, STOP.** It means `c7c56b2e` is not actually working and the premise of this plan is wrong. Report to the user rather than proceeding.

- [ ] **Step 3: Run the net unit suite and net-diag, both arches**

```bash
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch AARCH64
TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch X64
TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch AARCH64
```

Expected: `net-diag Results: N passed, 0 failed` in each netdiag run; AxlTestNet prints its Results footer with 0 failures.

- [ ] **Step 4: Record the baseline test count**

Note the total unit test count from `test-axl.sh` output. Write it down — the ratchet requires the count never drops, and you will compare against it in Task 10.

No commit (nothing changed).

---

### Task 2: The registry core

**Files:**
- Create: `src/net/axl-net-nic.c`
- Modify: `src/net/axl-net-internal.h` (add declarations after the `_axl_net_bus_location` block, before the HTTP Core section)

**This task commits code only — no test.** The registry's contract test needs the
wiring to be observable through the public API, and it only goes green in Task 3.
Committing it here would leave the suite RED in history. Task 3 writes it, confirms RED
against this unwired registry, wires `list_interfaces`, and commits test+wiring together
— still strictly test-first, with every commit green.

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces (every later task depends on these exact signatures):
  - `typedef struct { uint8_t mac[6]; char name[32]; bool link_up; uint32_t mtu; bool has_ipv4; uint8_t ipv4[4]; uint8_t netmask[4]; uint8_t gateway[4]; EFI_HANDLE snp_handle; EFI_HANDLE ip4cfg_handle; } AxlNic;`
  - `int _axl_net_nics_build(AxlNic **out, size_t *count);`
  - `void _axl_net_nics_free(AxlNic *nics);`
  - `EFI_IP4_CONFIG2_PROTOCOL *_axl_net_nic_resolve_ip4cfg(const AxlNic *nics, size_t count, size_t nic_index);`

- [ ] **Step 1: Add the declarations to `src/net/axl-net-internal.h`**

Insert immediately after the `_axl_net_bus_location` declaration (ends line ~72), before the `// HTTP Core` banner:

```c
// ---------------------------------------------------------------------------
// NIC registry (axl-net-nic.c) — the canonical per-physical-NIC model.
//
// LocateHandleBuffer(SimpleNetwork) returns one handle per SNP *child*, so a
// single physical NIC commonly repeats 2-3x, and that enumeration diverges
// from the IP4Config2 one in BOTH order and count. Indexing one with the
// other's index lands config on the wrong NIC (real-HW symptom: a link-up NIC
// never leases because DHCP went to a link-down sibling). The registry is the
// single answer to "what NICs does this machine have": SNP handles deduped by
// MAC, each correlated to its IP4Config2 handle by MAC, in stable enumeration
// order — so a NIC index is a per-physical-NIC ordinal, consistent across
// every net API.
//
// Built fresh per public call and freed when that call returns: NICs appear as
// drivers load and connect, so a cached list would go stale exactly when it
// matters.
// ---------------------------------------------------------------------------

/// One physical NIC. Internal — never exposed through include/axl/.
typedef struct {
    uint8_t     mac[6];         ///< hardware address (the stable key)
    char        name[32];       ///< "eth<ordinal>"
    bool        link_up;        ///< !MediaPresentSupported || MediaPresent
    uint32_t    mtu;            ///< SNP Mode->MaxPacketSize
    bool        has_ipv4;       ///< true when ipv4/netmask are valid
    uint8_t     ipv4[4];        ///< station address (valid if has_ipv4)
    uint8_t     netmask[4];     ///< subnet mask (valid if has_ipv4)
    uint8_t     gateway[4];     ///< default gateway (valid if has_ipv4)
    EFI_HANDLE  snp_handle;     ///< first SNP child handle publishing this MAC
    EFI_HANDLE  ip4cfg_handle;  ///< IP4Config2 handle for this MAC, NULL if none
} AxlNic;

/**
 * @brief Build the canonical per-physical-NIC list.
 *
 * Stable firmware-enumeration order; the ordinal is the array position.
 * Zero NICs is success with @p *count == 0 and @p *out == NULL.
 *
 * @return AXL_OK on success (including zero NICs), AXL_ERR on NULL args or
 *     allocation failure.
 */
int
_axl_net_nics_build(
    AxlNic **out,    ///< [out] axl_malloc'd array, release with _axl_net_nics_free
    size_t  *count   ///< [out] number of physical NICs
);

/// Release an array from _axl_net_nics_build. NULL is a no-op.
void
_axl_net_nics_free(
    AxlNic *nics
);

/**
 * @brief Resolve a NIC ordinal to its IP4Config2 protocol.
 *
 * @p nic_index may be AXL_NET_NIC_AUTO (SIZE_MAX): prefers the first link-up
 * NIC with an IP4Config2, else the first NIC with one. An explicit index is
 * bounds-checked — out of range is an error, NOT a clamp to NIC 0 (that clamp
 * was the wrong-NIC bug). When a NIC has no correlated IP4Config2, falls back
 * to the single positional handle only when there is exactly one NIC and
 * exactly one IP4Config2 handle — the only case where the guess cannot be
 * wrong.
 *
 * @return the protocol, or NULL if unresolvable.
 */
EFI_IP4_CONFIG2_PROTOCOL *
_axl_net_nic_resolve_ip4cfg(
    const AxlNic *nics,       ///< registry from _axl_net_nics_build
    size_t        count,      ///< registry length
    size_t        nic_index   ///< ordinal, or AXL_NET_NIC_AUTO
);
```

- [ ] **Step 2: Create `src/net/axl-net-nic.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-nic.c
    The canonical per-physical-NIC registry. SimpleNetwork child handles
    deduped by MAC, each NIC correlated to its IP4Config2 handle by MAC, in
    stable firmware-enumeration order. See axl-net-internal.h for why.
**/

#include "../backend/axl-backend.h"
#include "axl-net-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-net-opts.h>

AXL_LOG_DOMAIN("net");

/* Read the SNP MAC into out_mac[6].
   A handle reporting HwAddressSize 0 (or < 6) cannot be keyed: a zero MAC
   compares equal to everything, so it would both collapse distinct NICs into
   one row and match an arbitrary IP4Config2 handle. Skip those handles.
   HwAddressSize > 6 (e.g. InfiniBand) is fine -- the leading 6 bytes key it. */
static bool
snp_mac(EFI_SIMPLE_NETWORK_PROTOCOL *snp, uint8_t out_mac[6])
{
    if (snp == NULL || snp->Mode == NULL || snp->Mode->HwAddressSize < 6) {
        return false;
    }
    axl_memcpy(out_mac, &snp->Mode->CurrentAddress, 6);
    return true;
}

/* SNP protocol on a handle, or NULL. */
static EFI_SIMPLE_NETWORK_PROTOCOL *
snp_on(EFI_HANDLE h)
{
    EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
    axl_efi_call(axl_bs()->HandleProtocol, 3, h,
        &gEfiSimpleNetworkProtocolGuid, (void **)&snp);
    return snp;
}

/* Fill nic->has_ipv4/ipv4/netmask/gateway from its already-resolved
   ip4cfg_handle. No-op when the NIC has no IP4Config2. */
static void
fill_ipv4(AxlNic *nic)
{
    if (nic->ip4cfg_handle == NULL) {
        return;
    }
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    axl_efi_call(axl_bs()->HandleProtocol, 3, nic->ip4cfg_handle,
        &gEfiIp4Config2ProtocolGuid, (void **)&cfg);
    if (cfg == NULL) {
        return;
    }

    /* Two-phase: the InterfaceInfo buffer carries an optional trailing route
       table whose size we don't know up front. */
    size_t info_size = 0;
    EFI_STATUS st = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &info_size, NULL);
    if (st != EFI_BUFFER_TOO_SMALL
        || info_size < sizeof(EFI_IP4_CONFIG2_INTERFACE_INFO)) {
        return;
    }
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = axl_malloc(info_size);
    if (info == NULL) {
        return;
    }
    st = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &info_size, info);
    if (EFI_ERROR(st)) {
        axl_free(info);
        return;
    }
    EFI_IPv4_ADDRESS *sa = &info->StationAddress;
    if (sa->Addr[0] != 0 || sa->Addr[1] != 0
        || sa->Addr[2] != 0 || sa->Addr[3] != 0) {
        nic->has_ipv4 = true;
        axl_memcpy(nic->ipv4,    &info->StationAddress, 4);
        axl_memcpy(nic->netmask, &info->SubnetMask,     4);
    }
    axl_free(info);

    if (nic->has_ipv4) {
        EFI_IPv4_ADDRESS gw;
        size_t gw_size = sizeof(gw);
        st = axl_efi_call(cfg->GetData, 4, cfg,
            Ip4Config2DataTypeGateway, &gw_size, &gw);
        if (!EFI_ERROR(st)) {
            axl_memcpy(nic->gateway, &gw, 4);
        }
    }
}

void
_axl_net_nics_free(AxlNic *nics)
{
    axl_free(nics);
}

int
_axl_net_nics_build(AxlNic **out, size_t *count)
{
    if (out == NULL || count == NULL) {
        return AXL_ERR;
    }
    *out = NULL;
    *count = 0;

    EFI_HANDLE *snp_handles = NULL;
    size_t      nsnp = 0;
    EFI_STATUS  st = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
        ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL, &nsnp, &snp_handles);
    if (EFI_ERROR(st) || nsnp == 0 || snp_handles == NULL) {
        return AXL_OK;   /* no NICs is success with count 0 */
    }

    /* IP4Config2 handles fetched ONCE here, not per NIC. The pre-registry code
       re-enumerated them inside a per-NIC loop, so a 5-NIC bring-up did ~10
       full sweeps. */
    EFI_HANDLE *cfg_handles = NULL;
    size_t      ncfg = 0;
    st = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
        ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL, &ncfg, &cfg_handles);
    if (EFI_ERROR(st)) {
        cfg_handles = NULL;
        ncfg = 0;
    }

    AxlNic *nics = axl_calloc(nsnp, sizeof *nics);   /* nsnp is the upper bound */
    if (nics == NULL) {
        axl_backend_free(snp_handles);
        if (cfg_handles != NULL) {
            axl_backend_free(cfg_handles);
        }
        return AXL_ERR;
    }

    size_t nnic = 0;
    for (size_t i = 0; i < nsnp; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = snp_on(snp_handles[i]);
        uint8_t mac[6];
        if (!snp_mac(snp, mac)) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < nnic; j++) {
            if (axl_memcmp(nics[j].mac, mac, 6) == 0) {
                dup = true;   /* first handle for this MAC wins */
                break;
            }
        }
        if (dup) {
            continue;
        }

        AxlNic *nic = &nics[nnic];
        axl_memcpy(nic->mac, mac, 6);
        axl_snprintf(nic->name, sizeof(nic->name), "eth%zu", nnic);
        /* A NIC whose firmware doesn't implement media detection reports
           MediaPresent=FALSE meaninglessly; treating that as down would hide
           a working NIC. One rule, registry-wide. */
        nic->link_up = (!snp->Mode->MediaPresentSupported)
                       || snp->Mode->MediaPresent;
        nic->mtu = snp->Mode->MaxPacketSize;
        nic->snp_handle = snp_handles[i];

        /* Correlate to IP4Config2 by MAC: it lives on a child handle separate
           from the bare-NIC SNP handle on some OEM firmware, so we can't just
           HandleProtocol the SNP handle. */
        for (size_t k = 0; k < ncfg; k++) {
            uint8_t cfg_mac[6];
            if (!snp_mac(snp_on(cfg_handles[k]), cfg_mac)) {
                continue;
            }
            if (axl_memcmp(cfg_mac, mac, 6) == 0) {
                nic->ip4cfg_handle = cfg_handles[k];
                break;
            }
        }
        fill_ipv4(nic);
        nnic++;
    }

    axl_backend_free(snp_handles);
    if (cfg_handles != NULL) {
        axl_backend_free(cfg_handles);
    }

    if (nnic == 0) {
        axl_free(nics);
        return AXL_OK;
    }
    *out = nics;
    *count = nnic;
    return AXL_OK;
}

EFI_IP4_CONFIG2_PROTOCOL *
_axl_net_nic_resolve_ip4cfg(const AxlNic *nics, size_t count, size_t nic_index)
{
    if (nics == NULL || count == 0) {
        return NULL;
    }

    size_t idx;
    if (nic_index == (size_t)AXL_NET_NIC_AUTO) {
        /* Prefer link-up AND configurable; else the first configurable. Today's
           AUTO is a side effect of the clamp being deleted, and it happily
           picks a link-down NIC that will never lease. */
        size_t first_cfg = count;
        idx = count;
        for (size_t i = 0; i < count; i++) {
            if (nics[i].ip4cfg_handle == NULL) {
                continue;
            }
            if (first_cfg == count) {
                first_cfg = i;
            }
            if (nics[i].link_up) {
                idx = i;
                break;
            }
        }
        if (idx == count) {
            idx = first_cfg;
        }
        if (idx == count) {
            axl_debug("nic auto-select: no NIC has an IP4Config2");
            return NULL;
        }
    } else if (nic_index >= count) {
        /* NOT a clamp to 0 -- that clamp is what sent DHCP to the wrong NIC. */
        axl_warning("nic index %zu out of range (%zu NIC%s)",
                    nic_index, count, (count == 1) ? "" : "s");
        return NULL;
    } else {
        idx = nic_index;
    }

    EFI_HANDLE handle = nics[idx].ip4cfg_handle;
    if (handle == NULL) {
        /* Uncorrelatable MAC (firmware where SNP isn't reachable from the
           IP4Config2 handle). Guess positionally ONLY when there is exactly
           one NIC and exactly one IP4Config2 handle -- the only configuration
           where the guess cannot be wrong. One IP4Config2 handle can coexist
           with several NICs (only one has the IP4 stack bound), and there we
           still can't tell which NIC it serves. */
        EFI_HANDLE *cfg_handles = NULL;
        size_t      ncfg = 0;
        EFI_STATUS  st = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
            ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL, &ncfg, &cfg_handles);
        if (EFI_ERROR(st) || cfg_handles == NULL) {
            return NULL;
        }
        if (count == 1 && ncfg == 1) {
            handle = cfg_handles[0];
        } else {
            axl_warning("nic %zu (%02x:%02x:%02x:%02x:%02x:%02x): no IP4Config2 "
                        "correlates and a positional guess is unsafe "
                        "(%zu NICs, %zu IP4Config2)",
                        idx, nics[idx].mac[0], nics[idx].mac[1],
                        nics[idx].mac[2], nics[idx].mac[3],
                        nics[idx].mac[4], nics[idx].mac[5], count, ncfg);
        }
        axl_backend_free(cfg_handles);
        if (handle == NULL) {
            return NULL;
        }
    }

    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    axl_efi_call(axl_bs()->HandleProtocol, 3, handle,
        &gEfiIp4Config2ProtocolGuid, (void **)&cfg);
    return cfg;
}
```

- [ ] **Step 3: Confirm the build picks up the new file, both arches**

```bash
make ARCH=x64 2>&1 | tail -20
make ARCH=aa64 2>&1 | tail -20
```

Expected: `src/net/axl-net-nic.c` compiles on both, zero warnings. If the build system does not auto-glob `src/net/*.c`, add the file to the net module's source list — inspect `Makefile` / `scripts/build.sh` to see which pattern is in use, and follow it.

**Note:** nothing calls the registry yet — Task 3 wires it in. Because `libaxl.a` is selectively linked, an unreferenced `axl-net-nic.o` may not link into any binary yet. That is expected and is why this task ships no test: the contract is only observable once `list_interfaces` reads it.

- [ ] **Step 4: Confirm the suite is still green (nothing should have changed)**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch X64
```

Expected: same result as the Task 1 baseline — this task adds unreferenced code, so nothing should move.

- [ ] **Step 5: Commit**

```bash
git status --short
git add src/net/axl-net-nic.c src/net/axl-net-internal.h
git commit -F - <<'EOF'
net: add the internal MAC-keyed NIC registry

LocateHandleBuffer(SimpleNetwork) returns one handle per SNP child, so a
physical NIC repeats 2-3x and that enumeration diverges from the IP4Config2
one in both order and count. Indexing one with the other's index is what sent
DHCP to a link-down sibling on real hardware.

Add the canonical model: SNP handles deduped by MAC (first wins, stable order),
each NIC correlated to its IP4Config2 handle by MAC, IP4Config2 enumerated once
per build rather than once per NIC. Internal only; no public type.

Not wired into any API yet -- that follows, one API at a time.
EOF
```

---

### Task 3: Rewire `list_interfaces`

**Files:**
- Modify: `test/unit/axl-test-net.c` (the registry contract test)
- Modify: `src/net/axl-net-interfaces.c:301-478` (delete `populate_ipv4_for_mac`, rewrite `axl_net_list_interfaces`)
- Modify: `test/integration/test-netload-qemu.sh` (add the 2-interface assertion)

**Interfaces:**
- Consumes: `_axl_net_nics_build`, `_axl_net_nics_free`, `AxlNic` (Task 2).
- Produces: `axl_net_list_interfaces` whose ordinal every later task's API must match.

- [ ] **Step 1: Write the failing test**

In `test/unit/axl-test-net.c`, find `test_ws_conn_api_validation` and add this function immediately BEFORE it. Then register it by adding `test_nic_registry_contract();` next to the existing `test_..._api_validation();` calls in the same runner that calls `test_ws_conn_api_validation`.

Tests must use public headers only (`feedback_test_public_headers`), so this exercises the registry through `axl_net_list_interfaces` rather than including the internal header.

```c
/* NIC registry contract, observed through the public API (tests never include
   src/net/axl-net-internal.h). The plain unit boot has NO NIC, so the
   populated path is SKIP-balanced against the zero-NIC path -- equal check
   counts either way, per the balancer rule. */
static void
test_nic_registry_contract(void)
{
    /* NULL count returns on the guard, at any NIC count. */
    test_check(axl_net_list_interfaces(NULL, NULL) == AXL_ERR,
               "list_interfaces: NULL count -> AXL_ERR");

    size_t n = 0;
    test_check(axl_net_list_interfaces(NULL, &n) == AXL_OK,
               "list_interfaces: count query -> AXL_OK");

    if (n == 0) {
        axl_printf("SKIP: list_interfaces MACs unique (no NIC)\n");
        axl_printf("SKIP: list_interfaces count == filled (no NIC)\n");
        return;
    }

    AxlNetInterface *ifs = axl_calloc(n, sizeof *ifs);
    if (ifs == NULL) {
        axl_printf("SKIP: list_interfaces MACs unique (alloc failed)\n");
        axl_printf("SKIP: list_interfaces count == filled (alloc failed)\n");
        return;
    }
    size_t filled = n;
    int rc = axl_net_list_interfaces(ifs, &filled);

    /* Dedup: every MAC in the listing is unique. Pre-registry this fails --
       a single NIC repeats across its SNP child handles. */
    bool unique = (rc == AXL_OK);
    for (size_t i = 0; unique && i < filled; i++) {
        for (size_t j = i + 1; j < filled; j++) {
            if (axl_memcmp(ifs[i].mac, ifs[j].mac, 6) == 0) {
                unique = false;
                break;
            }
        }
    }
    test_check(unique, "list_interfaces: every MAC is unique (one row per NIC)");

    /* The count query must agree with what the fill actually produces. */
    test_check(rc == AXL_OK && filled == n,
               "list_interfaces: count query == filled count");

    axl_free(ifs);
}
```

- [ ] **Step 2: Run the test and confirm it FAILS**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch X64
```

In the no-NIC unit boot the two SKIPs print and nothing fails — **that is not proof**. Get the real RED from the live single-NIC boot:

```bash
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
```

Expected: `list_interfaces: every MAC is unique` **FAILS** — `list_interfaces` still walks raw SNP handles, so one NIC yields duplicate MAC rows.

**If it PASSES here, STOP and report.** It means the QEMU profile is not producing SNP child dupes, so the premise of the dedup is unverified in this environment and Step 4's assertion would pass for the wrong reason.

- [ ] **Step 3: Replace the implementation**

In `src/net/axl-net-interfaces.c`, delete `populate_ipv4_for_mac` entirely (lines ~301–402 — the registry's `fill_ipv4` replaces it) and replace `axl_net_list_interfaces` (lines ~404–478) with:

```c
int
axl_net_list_interfaces(AxlNetInterface *out, size_t *count)
{
    if (count == NULL) {
        return AXL_ERR;
    }
    size_t capacity = (out != NULL) ? *count : 0;

    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK) {
        *count = 0;
        return AXL_ERR;
    }

    if (out == NULL) {
        *count = nnic;   /* count query: physical NICs, not SNP child handles */
        _axl_net_nics_free(nics);
        return AXL_OK;
    }

    size_t filled = 0;
    for (size_t i = 0; i < nnic && filled < capacity; i++) {
        AxlNetInterface *iface = &out[filled];
        axl_memset(iface, 0, sizeof(*iface));
        axl_strlcpy(iface->name, nics[i].name, sizeof(iface->name));
        axl_memcpy(iface->mac, nics[i].mac, 6);
        iface->link_up  = nics[i].link_up;
        iface->mtu      = nics[i].mtu;
        iface->has_ipv4 = nics[i].has_ipv4;
        axl_memcpy(iface->ipv4,    nics[i].ipv4,    4);
        axl_memcpy(iface->netmask, nics[i].netmask, 4);
        axl_memcpy(iface->gateway, nics[i].gateway, 4);
        filled++;
    }
    _axl_net_nics_free(nics);
    *count = filled;
    return AXL_OK;
}
```

- [ ] **Step 4: Confirm the test now PASSES**

```bash
make ARCH=x64 tests && timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
```

Expected: `list_interfaces: every MAC is unique (one row per NIC)` now PASSES — the same check that FAILED in Step 2.

- [ ] **Step 5: Add the multi-NIC count assertion**

In `test/integration/test-netload-qemu.sh`, the multi-NIC boot at line ~544 currently writes this nsh:

```bash
{ echo '@echo -off'; echo 'fs0:'; echo 'echo MARK_IDX'
  echo 'netload.efi --connect --mac 52:54:00:12:34:57'; echo 'echo MARK_IDX_DONE'
  echo 'reset -s'; } > "$IDXNSH"
```

Change it to also dump the landscape, so the same boot proves the dedup:

```bash
{ echo '@echo -off'; echo 'fs0:'; echo 'echo MARK_IDX'
  echo 'netload.efi --connect --mac 52:54:00:12:34:57'; echo 'echo MARK_IDX_DONE'
  echo 'echo MARK_IDX_LIST'; echo 'netload.efi --diag'; echo 'echo MARK_IDX_LIST_DONE'
  echo 'reset -s'; } > "$IDXNSH"
```

Then add this assertion immediately after the existing `PASS: multi-NIC IP4Config2 resolved by MAC` check (line ~560):

```bash
# Dedup guard: TWO virtio NICs must list as TWO interfaces. Pre-registry each
# physical NIC repeated across its SNP child handles, so this printed 6.
IDXN=$(isect MARK_IDX_LIST MARK_IDX_LIST_DONE | grep -acE '^ *eth[0-9]+ ' || true)
[[ "$IDXN" -eq 2 ]] \
  && echo "PASS: 2 NICs list as 2 interfaces (SNP child handles deduped)" \
  || { echo "FAIL: expected 2 interfaces, got $IDXN (SNP child dupes leaking?)"; fail=1; }
```

**Before relying on that `grep -E` pattern, run the boot once and read the actual `--diag` output.** Match the real column format rather than assuming `^ *eth[0-9]+ `. If the landscape rows are formatted differently, fix the pattern to match what is actually printed — an assertion that matches nothing silently reports 0 and fails for the wrong reason.

- [ ] **Step 6: Confirm everything passes, both arches**

```bash
make ARCH=x64 tests && make ARCH=aa64 tests
TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch X64
TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch AARCH64
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch AARCH64
timeout 300s ./test/integration/test-netload-qemu.sh --arch X64
timeout 300s ./test/integration/test-netload-qemu.sh --arch AARCH64
```

Expected: all `=== PASS ===` / `0 failed`, including `PASS: 2 NICs list as 2 interfaces` and the pre-existing `PASS: multi-NIC IP4Config2 resolved by MAC` (which must NOT regress).

- [ ] **Step 7: Commit**

```bash
git status --short
git add src/net/axl-net-interfaces.c test/unit/axl-test-net.c test/integration/test-netload-qemu.sh
git commit -F - <<'EOF'
net: list_interfaces returns one row per physical NIC

It returned one row per SimpleNetwork child handle, so a single NIC appeared
2-3x and the count query reported the raw handle count. Read the registry
instead: the index is now a per-physical-NIC ordinal.

populate_ipv4_for_mac is gone -- it re-enumerated every IP4Config2 handle once
per NIC; the registry does it once per build.

Guarded by the two-virtio-NIC boot, which must now report 2 interfaces.
EOF
```

---

### Task 4: Rewire `get_link_stats`

**Files:**
- Modify: `src/net/axl-net-linkstats.c:20-54`
- Test: `test/unit/axl-test-net.c`

**Interfaces:**
- Consumes: `_axl_net_nics_build`, `_axl_net_nics_free`, `AxlNic` (Task 2); the ordinal from Task 3.

- [ ] **Step 1: Write the failing tests**

In `test/unit/axl-test-net.c`, find the existing `get_link_stats: NULL out -> AXL_ERR` check (line ~4057). Add immediately after it:

```c
    /* Out-of-range nic must ERROR, not clamp. The `>= count -> 0` clamp is the
       wrong-NIC bug: it silently answered for NIC 0. Safe negative -- returns
       on our own bounds check before any firmware call. */
    AxlNetLinkStats ls_oob;
    test_check(axl_net_get_link_stats(SIZE_MAX - 1, &ls_oob) == AXL_ERR,
               "link_stats: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");
```

And in the `net-diag` live section, next to the existing `link-stats: returns AXL_OK` checks (line ~4848), add the cross-API invariant — the heart of the refactor:

```c
    /* Ordinal consistency: get_link_stats(i) and list_interfaces()[i] must
       describe the SAME NIC. Before the registry they indexed different
       spaces (and computed link_up by different rules), so agreement was
       coincidence at NIC 0 and wrong beyond it. */
    size_t lc_n = 0;
    if (axl_net_list_interfaces(NULL, &lc_n) == AXL_OK && lc_n > 0) {
        AxlNetInterface *lc_ifs = axl_calloc(lc_n, sizeof *lc_ifs);
        if (lc_ifs != NULL) {
            size_t lc_filled = lc_n;
            if (axl_net_list_interfaces(lc_ifs, &lc_filled) == AXL_OK) {
                bool agree = true;
                for (size_t i = 0; i < lc_filled; i++) {
                    AxlNetLinkStats st_i;
                    if (axl_net_get_link_stats(i, &st_i) != AXL_OK
                        || st_i.link_up != lc_ifs[i].link_up) {
                        agree = false;
                        break;
                    }
                }
                ND_CHECK(agree,
                    "ordinal: get_link_stats(i).link_up == list_interfaces()[i].link_up");
            }
            axl_free(lc_ifs);
        }
    }
```

- [ ] **Step 2: Run and confirm the out-of-range test FAILS**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch X64
```

Expected: FAIL on `link_stats: out-of-range nic -> AXL_ERR`. Current code returns `AXL_ERR` only because `hc == 0` in the no-NIC unit boot — so **also** confirm RED where a NIC exists:

```bash
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
```

There, current code hits `if (nic >= hc)` and returns `AXL_ERR` already, so that check may pass for the right reason even pre-change. The decisive RED is the ordinal invariant: pre-registry, `list_interfaces` uses bare `MediaPresent` while `get_link_stats` uses `!MediaPresentSupported || MediaPresent`, and the indexes address different spaces.

- [ ] **Step 3: Replace the implementation**

Replace the body of `axl_net_get_link_stats` in `src/net/axl-net-linkstats.c`:

```c
int
axl_net_get_link_stats(size_t nic, AxlNetLinkStats *out)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK || nnic == 0) {
        _axl_net_nics_free(nics);
        return AXL_ERR;
    }
    if (nic >= nnic) {
        /* Out of range errors -- it does NOT clamp to NIC 0. */
        _axl_net_nics_free(nics);
        return AXL_ERR;
    }

    /* Link state is authoritative; speed/duplex/autoneg have no portable
       SimpleNetwork source and stay 0 (see the header note). */
    out->link_up = nics[nic].link_up;
    _axl_net_nics_free(nics);
    return AXL_OK;
}
```

- [ ] **Step 4: Confirm GREEN, both arches**

```bash
make ARCH=x64 tests && make ARCH=aa64 tests
TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch X64
TEST_APPS_ONLY=AxlTestNet timeout 300s ./test/integration/test-axl.sh --arch AARCH64
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch AARCH64
```

Expected: all pass, including the ordinal invariant and `net-diag Results: N passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git status --short
git add src/net/axl-net-linkstats.c test/unit/axl-test-net.c
git commit -F - <<'EOF'
net: get_link_stats reads the NIC registry ordinal

It indexed the raw SimpleNetwork handle buffer, so its "nic 1" was a different
NIC from list_interfaces' "nic 1", and it computed link_up by a different rule
(!MediaPresentSupported || MediaPresent vs bare MediaPresent). Both now come
from the registry, which keeps the more correct rule: a NIC whose firmware
lacks media detection reports MediaPresent=FALSE meaninglessly, and calling
that "down" hides a working NIC.

Out-of-range now errors instead of clamping to NIC 0.
EOF
```

---

### Task 5: Rewire the DHCP/config APIs and replace AUTO

The largest task. Do the steps in order — AUTO must land in the same commit as the clamp deletion, because the clamp is what currently implements AUTO.

**Files:**
- Modify: `src/net/axl-net-dhcp.c` — delete `ip4cfg_for_nic` (~396-416) and `ip4cfg_for_nic_index` (~615-642) and its forward declaration (~302); rewire `set_static_ip` (~304), `set_dns` (~418), `wait_ip_settled` (~506), `get_dhcp_lease` (~710), `auto_init` (~1018)
- Test: `test/unit/axl-test-net.c`

**Interfaces:**
- Consumes: `_axl_net_nics_build`, `_axl_net_nics_free`, `_axl_net_nic_resolve_ip4cfg` (Task 2).

- [ ] **Step 1: Write the failing tests**

In `test/unit/axl-test-net.c`, add to the same validation function that holds the `link_stats` negatives:

```c
    /* The clamp regression, on every API that still carries it. Each returns
       on our own bounds check before any firmware call, so these are safe
       negatives (feedback_uefi_firmware_test_hazards) -- a VALID index here
       would reconfigure live firmware, which is why only the out-of-range and
       NULL cases are driven. SIZE_MAX-1 is used rather than SIZE_MAX: SIZE_MAX
       IS AXL_NET_NIC_AUTO and means auto-select, not out-of-range. */
    static const uint8_t oob_dns[4] = { 10, 0, 2, 3 };
    test_check(axl_net_set_dns(SIZE_MAX - 1, oob_dns, NULL) == AXL_ERR,
               "set_dns: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");

    AxlDhcpLease oob_lease;
    test_check(axl_net_get_dhcp_lease(SIZE_MAX - 1, &oob_lease) == AXL_ERR,
               "get_dhcp_lease: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");

    static const uint8_t oob_ip[4] = { 10, 0, 2, 77 };
    test_check(axl_net_wait_ip_settled(SIZE_MAX - 1, oob_ip, 1) == AXL_ERR,
               "wait_ip_settled: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");
```

Verify `axl_net_wait_ip_settled`'s exact signature in `include/axl/axl-net.h:536` before writing this call and match it; adjust the arguments if it differs.

- [ ] **Step 2: Run and confirm RED**

```bash
make ARCH=x64 tests && timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
```

Expected: these FAIL in the live single-NIC boot, because the clamp currently makes an out-of-range index silently answer for NIC 0 (`AXL_OK`). In the no-NIC unit boot they may pass for the wrong reason (no IP4Config2 at all) — **the netdiag boot is the decisive RED**. Confirm it there.

- [ ] **Step 3: Delete the two resolvers**

In `src/net/axl-net-dhcp.c`, delete:
- the forward declaration at ~302 (`static EFI_IP4_CONFIG2_PROTOCOL *ip4cfg_for_nic_index(size_t nic_index);`) and its comment
- `ip4cfg_for_nic` (~396-416) with its comment
- `ip4cfg_for_nic_index` (~615-642) with its comment

Keep `ip4cfg_for_mac` — `get_dhcp_lease_by_mac` still uses it and it is correct.

- [ ] **Step 4: Add the shared resolver helper**

Add near the top of `src/net/axl-net-dhcp.c`, after the includes and before its first use:

```c
/* Resolve a NIC ordinal (or AXL_NET_NIC_AUTO) to its IP4Config2 protocol via a
   freshly-built registry. The registry is built per call and released before
   returning: the protocol pointer stays valid (it belongs to the firmware, not
   to us), and NICs appear as drivers load, so a cached list would go stale
   exactly when it matters. Returns NULL if unresolvable -- callers must treat
   that as an error, never as "use NIC 0". */
static EFI_IP4_CONFIG2_PROTOCOL *
ip4cfg_for(size_t nic_index)
{
    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK) {
        return NULL;
    }
    EFI_IP4_CONFIG2_PROTOCOL *cfg =
        _axl_net_nic_resolve_ip4cfg(nics, nnic, nic_index);
    _axl_net_nics_free(nics);
    return cfg;
}
```

- [ ] **Step 5: Rewire `set_dns`**

```c
int
axl_net_set_dns(size_t nic_index, const uint8_t dns[4], const uint8_t *dns2)
{
    if (dns == NULL) {
        return AXL_ERR;
    }
    EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for(nic_index);
    if (cfg == NULL) {
        return AXL_ERR;
    }
    /* ... rest of the existing body unchanged (servers[], SetData, warning) ... */
}
```

Only the resolver call changes: `ip4cfg_for_nic(nic_index)` → `ip4cfg_for(nic_index)`. Leave the body below it exactly as-is.

- [ ] **Step 6: Rewire `wait_ip_settled` and `get_dhcp_lease`**

Same one-line substitution in each: `ip4cfg_for_nic(nic_index)` → `ip4cfg_for(nic_index)`. Bodies unchanged.

- [ ] **Step 7: Rewire `set_static_ip`**

Replace the resolver block (lines ~311–339). The `axl_protocol_enumerate` call and the positional fallback both go away — `ip4cfg_for` owns the fallback policy now:

```c
int
axl_net_set_static_ip(
    size_t         nic_index,
    const uint8_t  ip[4],
    const uint8_t  netmask[4],
    const uint8_t *gateway)
{
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    EFI_STATUS status;

    if (ip == NULL || netmask == NULL) {
        return AXL_ERR;
    }

    cfg = ip4cfg_for(nic_index);
    if (cfg == NULL) {
        axl_warning("set_static_ip: no IP4Config2 for nic %zu", nic_index);
        return AXL_ERR;
    }

    /* ... rest of the existing body unchanged, starting at the policy SetData ... */
}
```

Delete the now-unused `void **handles` / `size_t count` locals and the `axl_free(handles)` that followed the old block. Check the remainder of the function for any other use of `handles` before removing it.

- [ ] **Step 8: Rewire `auto_init`**

Replace the resolver block (lines ~1074–1088). Keep everything above it — the `get_ip_address` short-circuit, `drivers_up`, and the IP4Config2-free DHCP4-SB/PXE ladder — exactly as-is. The `cfg_handles` enumeration above it is still needed for the "no IP4Config2 at all" branch, so leave that too; only the resolution and its positional fallback change:

```c
    /* Resolve the NIC's IP4Config2 through the registry: by MAC for an explicit
       ordinal, or the AUTO rule (first link-up NIC with an IP4Config2, else the
       first with one). No positional clamp -- that clamp sent DHCP to a
       link-down sibling on real hardware. */
    EFI_IP4_CONFIG2_PROTOCOL *ip4cfg = ip4cfg_for(nic_index);

    axl_free(cfg_handles);

    if (ip4cfg == NULL) {
        return AXL_ERR;
    }
```

- [ ] **Step 9: Build both arches, zero warnings**

```bash
make ARCH=x64 && make ARCH=aa64
```

Expected: exit 0, no warnings. Unused-variable warnings here mean a leftover local from Steps 7–8 — remove it rather than casting it away.

- [ ] **Step 10: Confirm GREEN, both arches**

```bash
make ARCH=x64 tools && make ARCH=aa64 tools && make ARCH=x64 tests && make ARCH=aa64 tests
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch AARCH64
timeout 300s ./test/integration/test-netload-qemu.sh --arch X64
timeout 300s ./test/integration/test-netload-qemu.sh --arch AARCH64
```

Expected: all pass. **The decisive one is `PASS: multi-NIC IP4Config2 resolved by MAC -- the high-index DHCP NIC leases`** — that boot is the functional guard that deleting `ip4cfg_for_nic_index` did not regress `c7c56b2e`. If it fails, the registry's MAC correlation is not equivalent to the deleted code. Do not paper over it; debug the correlation.

Also confirm netload's AUTO paths still behave: `--connect` and `-a` default to `AXL_NET_NIC_AUTO`, so the whole netload suite exercises the new AUTO rule.

- [ ] **Step 11: Commit**

```bash
git status --short
git add src/net/axl-net-dhcp.c test/unit/axl-test-net.c
git commit -F - <<'EOF'
net: resolve every DHCP/config API through the NIC registry

c7c56b2e fixed auto_init and set_static_ip by resolving IP4Config2 by MAC, but
set_dns, wait_ip_settled and get_dhcp_lease still went through ip4cfg_for_nic
-- the positional index with the same `>= count -> 0` clamp, i.e. the same
wrong-NIC bug on three more entry points. Both resolvers are gone; all five
share one registry-backed path.

Out-of-range now errors instead of answering for NIC 0. The uncorrelatable-MAC
fallback is narrowed to exactly one NIC and exactly one IP4Config2 handle --
the only case where a positional guess cannot be wrong.

AXL_NET_NIC_AUTO becomes explicit rather than a side effect of the clamp it
used to fall through to: first link-up NIC with an IP4Config2, else the first
with one. Today's AUTO picks a link-down NIC that will never lease, which is
the same symptom by another route.
EOF
```

---

### Task 6: Spike — can the ARP service binding be MAC-correlated?

A throwaway investigation, not shipped code. `axl_net_arp_list`'s alignment assumes SNP is reachable from the ARP service-binding handle. That is **unproven**. Find out before writing Task 7.

**Files:**
- Modify: `test/unit/axl-test-net.c` (temporary probe, reverted at the end of this task)

- [ ] **Step 1: Add a temporary probe to `net-diag` mode**

Add to the `net-diag` section of `test/unit/axl-test-net.c`, temporarily:

```c
    /* SPIKE (temporary): can an ARP service-binding handle be MAC-correlated? */
    {
        EFI_HANDLE *ah = NULL;
        size_t      an = 0;
        EFI_STATUS  ast = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
            ByProtocol, &gEfiArpServiceBindingProtocolGuid, NULL, &an, &ah);
        axl_printf("SPIKE: arp-sb handles=%zu st=%llx\r\n",
                   an, (unsigned long long)ast);
        for (size_t i = 0; !EFI_ERROR(ast) && i < an; i++) {
            EFI_SIMPLE_NETWORK_PROTOCOL *s = NULL;
            axl_efi_call(axl_bs()->HandleProtocol, 3, ah[i],
                &gEfiSimpleNetworkProtocolGuid, (void **)&s);
            if (s != NULL && s->Mode != NULL && s->Mode->HwAddressSize >= 6) {
                uint8_t *m = (uint8_t *)&s->Mode->CurrentAddress;
                axl_printf("SPIKE: arp-sb[%zu] SNP MAC %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                           i, m[0], m[1], m[2], m[3], m[4], m[5]);
            } else {
                axl_printf("SPIKE: arp-sb[%zu] NO reachable SNP\r\n", i);
            }
        }
        if (ah != NULL) {
            axl_backend_free(ah);
        }
    }
```

This spike needs internal UEFI symbols that public headers do not expose. If `test/unit/axl-test-net.c` cannot reach `axl_bs()` / the GUIDs (tests use public headers only), put the probe in a scratch tool under the scratchpad directory instead of the test file, and build it against the library directly.

- [ ] **Step 2: Run it and read the output**

```bash
make ARCH=x64 tests && timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64 2>&1 | grep SPIKE
```

- [ ] **Step 3: Decide, and report to the user**

- **Every ARP-SB handle shows a reachable SNP MAC** → correlation holds. Proceed to Task 7.
- **Any handle shows `NO reachable SNP`** → correlation does NOT hold. **STOP.** Do not ship a silent fallback. Report the output to the user; the honest alternative is changing `arp_list` to take a `const uint8_t mac[6]` instead of an index, which is a public signature change the user must approve. Skip Task 7 until they decide.

- [ ] **Step 4: Revert the spike**

```bash
git checkout -- test/unit/axl-test-net.c
```

Confirm nothing from the spike remains: `git status --short` should not list the test file. No commit — spikes are throwaway (`feedback_spike_solutions`).

---

### Task 7: Rewire `arp_list` (GATED on Task 6)

**Do not start this task if Task 6 Step 3 said STOP.**

**Files:**
- Modify: `src/net/axl-net-arp.c:17-45`
- Test: `test/unit/axl-test-net.c`

**Interfaces:**
- Consumes: `_axl_net_nics_build`, `_axl_net_nics_free`, `AxlNic` (Task 2).

- [ ] **Step 1: Write the failing test**

Next to the existing `arp_list: NULL count -> AXL_ERR` check in `test/unit/axl-test-net.c`:

```c
    /* Out-of-range nic must ERROR, not answer for a different NIC. Safe
       negative: returns on our own bounds check before CreateChild. */
    AxlArpEntry ae_oob[2];
    size_t      ae_oob_n = 0;
    test_check(axl_net_arp_list(SIZE_MAX - 1, ae_oob, 2, &ae_oob_n) == AXL_ERR,
               "arp_list: out-of-range nic -> AXL_ERR");
```

- [ ] **Step 2: Run and confirm RED in the live boot**

```bash
make ARCH=x64 tests && timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
```

The current code checks `nic >= hc` against the ARP-SB handle count, so this may already pass. If it does, say so plainly — the value of Task 7 is then the ordinal alignment, not this negative. Do not add a tautology to manufacture a RED.

- [ ] **Step 3: Replace the handle-selection block**

In `src/net/axl-net-arp.c`, replace lines 25–45 (from `EFI_HANDLE *handles = NULL;` through the `HandleProtocol` that yields `sb`) with:

```c
    /* Resolve the ordinal to a MAC through the registry, then find the ARP
       service binding carrying that MAC. The raw ARP-SB handle index is its
       own enumeration -- unrelated to the SNP and IP4Config2 ones -- so
       indexing it with a NIC ordinal named a different NIC. */
    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK || nnic == 0) {
        _axl_net_nics_free(nics);
        return AXL_ERR;
    }
    if (nic >= nnic) {
        _axl_net_nics_free(nics);
        return AXL_ERR;
    }
    uint8_t want_mac[6];
    axl_memcpy(want_mac, nics[nic].mac, 6);
    _axl_net_nics_free(nics);

    EFI_HANDLE *handles = NULL;
    size_t      hc      = 0;
    EFI_STATUS  st      = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                                       ByProtocol,
                                       &gEfiArpServiceBindingProtocolGuid,
                                       NULL, &hc, &handles);
    if (EFI_ERROR(st) || hc == 0 || handles == NULL) {
        return AXL_ERR;
    }

    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    for (size_t i = 0; i < hc; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
        axl_efi_call(axl_bs()->HandleProtocol, 3, handles[i],
                     &gEfiSimpleNetworkProtocolGuid, (void **)&snp);
        if (snp == NULL || snp->Mode == NULL || snp->Mode->HwAddressSize < 6) {
            continue;
        }
        if (axl_memcmp(&snp->Mode->CurrentAddress, want_mac, 6) != 0) {
            continue;
        }
        axl_efi_call(axl_bs()->HandleProtocol, 3, handles[i],
                     &gEfiArpServiceBindingProtocolGuid, (void **)&sb);
        if (sb != NULL) {
            break;
        }
    }
    axl_backend_free(handles);
    if (sb == NULL) {
        axl_warning("arp_list: no ARP service binding for nic %zu", nic);
        return AXL_ERR;
    }
```

Everything below (`CreateChild`, `Configure`, `Find`, cleanup) stays exactly as-is.

- [ ] **Step 4: Confirm GREEN, both arches**

```bash
make ARCH=x64 tests && make ARCH=aa64 tests
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch X64
timeout 300s ./test/integration/test-netdiag-qemu.sh --arch AARCH64
```

Expected: `net-diag Results: N passed, 0 failed` both arches. The pre-existing `arp-list: gateway 10.0.2.2 resolved in the cache` check must NOT regress — if it does, the MAC-correlated handle is not the one whose ARP cache carries the gateway, and Task 6's conclusion was wrong.

- [ ] **Step 5: Commit**

```bash
git status --short
git add src/net/axl-net-arp.c test/unit/axl-test-net.c
git commit -F - <<'EOF'
net: arp_list resolves the NIC ordinal by MAC

It indexed the ARP service-binding handle buffer -- a third enumeration,
unrelated to the SimpleNetwork and IP4Config2 ones -- so its "nic 1" named a
different NIC from every other net API's. Resolve the ordinal to a MAC through
the registry and find the ARP service binding carrying it.
EOF
```

---

### Task 8: Documentation

The index-divergence warnings become false the moment the registry lands. A doc that describes the old bug as current behavior is worse than no doc.

**Files:**
- Modify: `include/axl/axl-net.h` — the `AxlNetInterface` block (~780-807), `get_link_stats` (~198-215), `get_dhcp_lease` `@warning` (~580-595)
- Modify: `src/net/README.md`

- [ ] **Step 1: Rewrite the `get_dhcp_lease` warning**

The existing `@warning` (~580) says `nic_index` indexes the IP4Config2 handle buffer and is not the same space as `list_interfaces`. That is now false. Replace it with:

```c
 * @p nic_index is a per-physical-NIC ordinal — the same index space as
 * @c axl_net_list_interfaces and every other net API. Out of range returns
 * AXL_ERR (it does not fall back to the first NIC).
 *
 * @note The ordinal is stable within a boot's topology, but a NIC appearing
 *     (a driver loading and connecting) renumbers later ordinals. Callers
 *     holding a reference to a *specific* NIC across such an event should use
 *     @c axl_net_get_dhcp_lease_by_mac — a MAC never moves.
```

- [ ] **Step 2: Rewrite the `AxlNetInterface` block**

Add to the `axl_net_list_interfaces` doc comment:

```c
 * One row per PHYSICAL NIC. A single NIC publishes several SimpleNetwork
 * child handles; they are deduped by MAC, so the row index is a
 * per-physical-NIC ordinal shared with every other net API taking a
 * @c nic_index.
```

And on the `link_up` field of `AxlNetInterface`:

```c
    bool     link_up;       ///< link state (a NIC whose firmware lacks media
                            ///< detection counts as up)
```

- [ ] **Step 3: Fix the `get_link_stats` doc**

Its `@p nic` doc (~215) says "SimpleNetwork interface index (0 = first)". Replace with `///< NIC ordinal (0 = first physical NIC)` and drop any surviving text about SimpleNetwork handle indexing.

- [ ] **Step 4: Re-read `src/net/README.md` for stale prose**

Read the whole file. Any prose describing NIC indexing, SNP-vs-IP4Config2 divergence, or duplicate interface rows is now false — fix it. This is the trap a script cannot catch: `make check-docs` verifies structural coverage, not truth.

- [ ] **Step 5: Run the doc gates**

```bash
make check-docs && make check-ascii
```

Expected: both pass. If `build-docs.sh` is available, run `./scripts/build-docs.sh` and fix any `\ref` / directive warnings introduced above.

- [ ] **Step 6: Commit**

```bash
git status --short
git add include/axl/axl-net.h src/net/README.md
git commit -F - <<'EOF'
docs: net index docs describe the ordinal, not the old divergence

The AxlNetInterface block, the get_link_stats note and the get_dhcp_lease
warning all documented the SNP-vs-IP4Config2 index divergence as current
behavior. The registry removed it. Documenting a bug that no longer exists is
worse than not documenting it -- it sends callers to work around nothing.

Keep recommending the _by_mac variants where they still earn it: the ordinal is
stable within a boot's topology, but a NIC appearing renumbers later ordinals.
A MAC never moves.
EOF
```

---

### Task 9: Drop netload's redundant display dedups

**Files:**
- Modify: `tools/netload.c:491-495` (`print_net_landscape`), `:1294-1298` (`probe_firmware_stack`), `:1444-1449` (`probe_driver`)

- [ ] **Step 1: Remove the three `seen[32][6]` blocks**

Each site declares `uint8_t seen[32][6];` plus an `nseen` counter and skips a row whose MAC was already recorded. `list_interfaces` now returns one row per NIC, so each is dead weight.

At each of the three sites: delete the `seen` array, the `nseen` counter, the dup-check, and the `if (nseen < 32) { axl_memcpy(...) }` record step; keep the loop body that does the real work.

**Do NOT touch line ~2222** — that `seen[NETLOAD_MAX_TRIED][NETLOAD_NAME_MAX]` dedups driver *names*, not MACs. It is unrelated and still needed.

**Keep the `--_hmap` seam.** It dumps SNP order vs IP4Config2 order and stays a useful diagnostic.

- [ ] **Step 2: Build both arches, zero warnings**

```bash
make ARCH=x64 tools && make ARCH=aa64 tools
```

Expected: exit 0, no warnings. An unused-variable warning means a leftover counter.

- [ ] **Step 3: Confirm netload's full suite still passes, both arches**

```bash
timeout 300s ./test/integration/test-netload-qemu.sh --arch X64
timeout 300s ./test/integration/test-netload-qemu.sh --arch AARCH64
```

Expected: `=== PASS ===` both, including `PASS: 2 NICs list as 2 interfaces` — which now proves the library dedup alone carries it, with netload's own dedup gone.

- [ ] **Step 4: Commit**

```bash
git status --short
git add tools/netload.c
git commit -F - <<'EOF'
netload: drop the display-side MAC dedup

048d46fa deduped interface listings by MAC in three display paths, working
around list_interfaces returning one row per SNP child handle. The library
returns one row per physical NIC now, so the workaround is dead weight.

The --_hmap seam stays -- dumping SNP order against IP4Config2 order is still
a useful diagnostic. The driver-NAME dedup in the sweep is untouched; it
dedups something else.
EOF
```

---

### Task 10: Full-suite verification

**Files:**
- Modify: none (verification only)

- [ ] **Step 1: Clean build, both arches**

```bash
cd /home/mgosha/projects/aximcode/axl-sdk
make clean && make ARCH=x64 && make ARCH=x64 tools && make ARCH=x64 tests
make clean && make ARCH=aa64 && make ARCH=aa64 tools && make ARCH=aa64 tests
```

Expected: exit 0, zero warnings on both.

- [ ] **Step 2: Full unit suite, both arches**

```bash
timeout 900s ./test/integration/test-axl.sh --arch X64
timeout 900s ./test/integration/test-axl.sh --arch AARCH64
```

Expected: 0 failures, and the total count **at or above** the Task 1 Step 4 baseline. The ratchet fails the run if the count dropped. If it dropped, find the tests that stopped running — a SKIP-path imbalance is the usual cause (`feedback_balancer_count`).

- [ ] **Step 3: Full integration suite, both arches**

```bash
timeout 900s ./test/integration/test-netdiag-qemu.sh --arch X64
timeout 900s ./test/integration/test-netdiag-qemu.sh --arch AARCH64
timeout 900s ./test/integration/test-netload-qemu.sh --arch X64
timeout 900s ./test/integration/test-netload-qemu.sh --arch AARCH64
```

Expected: all `=== PASS ===` / `0 failed`.

- [ ] **Step 4: Confirm nothing was pushed**

```bash
git status --short
git log --oneline origin/main..HEAD
```

Expected: the working tree is clean of unintended files, and the log lists our commits as unpushed. **Do not push.**

- [ ] **Step 5: Report honestly**

Summarize for the user: what was tested and passed (exact commands + results), and what was **not** covered. State plainly that **real-hardware validation on the Dell R6725 did not happen** — the multi-NIC coverage is QEMU/OVMF only. Do not claim HW coverage. If Task 6 concluded STOP, say that `arp_list` is unchanged and why.

---

## Review Checkpoints

Per `feedback_code_review_before_commit`, this plan has three review points:

1. **Contract-first — SKIPPED.** The header/contract was designed with the user in the brainstorming session (spec `2026-07-16-net-nic-registry-design.md`). That WAS the early review; don't double-pay.
2. **Mid-point — after Task 5.** The registry core plus every DHCP/config API is the foundation the rest layers onto. Review before Tasks 7–9 build on it.
3. **Integration — before the final report.** Full review of the complete diff after Task 9.

## Definition of Done

- Every index-taking net API resolves through the registry; `ip4cfg_for_nic` and `ip4cfg_for_nic_index` no longer exist.
- No `>= count → 0` clamp anywhere in `src/net/`. Verify: `grep -rn "< count) ? \|>= count" src/net/` returns nothing of that shape.
- `AXL_NET_NIC_AUTO` resolves explicitly, not via a clamp.
- Both arches green: full unit suite at/above baseline count, netdiag, netload.
- The multi-NIC boot still proves the high-index NIC leases — `c7c56b2e` is not regressed.
- Docs describe the ordinal; no surviving prose about index divergence.
- All commits GATED. Nothing pushed.
