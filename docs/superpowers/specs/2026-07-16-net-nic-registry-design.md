# axl-net NIC registry — one ordinal for every net API

**Date:** 2026-07-16
**Status:** approved (design)
**Supersedes:** the Level-1 / Level-2 split in `local/docs/handoff-net-nic-registry.md`

## Problem

`nic_index` means four different things across `axl-net`'s public API. A caller who
reads a row from `axl_net_list_interfaces` and passes its index to any other net
function is, on a multi-NIC box, talking about a different NIC.

| API | What `nic` indexes today |
|---|---|
| `axl_net_list_interfaces` | raw SimpleNetwork handle buffer — one row per SNP *child*, so a physical NIC repeats 2–3× |
| `axl_net_get_link_stats` | raw SimpleNetwork handle buffer |
| `axl_net_arp_list` | ARP service-binding handle buffer — a third, unrelated space |
| `axl_net_set_dns`, `axl_net_wait_ip_settled`, `axl_net_get_dhcp_lease` | positional IP4Config2 index, with a `>= count → 0` clamp |
| `axl_net_auto_init`, `axl_net_set_static_ip` | MAC-resolved (fixed in `c7c56b2e`), but the MAC comes from `list_interfaces`, so it inherits the SNP-dupe ordinal |
| `axl_net_get_driver_info`, `axl_net_get_dhcp_lease_by_mac` | MAC — correct, and the model for the rest |

Commit `c7c56b2e` fixed exactly one instance of this on real hardware: a Realtek USB
NIC on a Dell R6725 linked up but never leased, because the SNP-derived index clamped
to IP4Config2 handle 0 — a link-down Broadcom sibling. **The same clamp still lives in
`ip4cfg_for_nic`**, serving `set_dns` / `wait_ip_settled` / `get_dhcp_lease`.

There is a cost problem alongside the correctness one. `ip4cfg_for_nic_index` calls
`list_interfaces` twice, and `list_interfaces` re-enumerates IP4Config2 once per NIC
inside `populate_ipv4_for_mac`. A bring-up on the 5-NIC Dell performs roughly ten full
IP4Config2 sweeps to answer a question that needs two.

## Goal

`nic_index` means "the Nth physical NIC", identically, everywhere. One build pass owns
the NIC model; every API queries it.

## Non-goals

- No new public NIC type. The registry is internal.
- No caching across calls. No `axl_net_nics_refresh()`.
- No unrelated refactoring of `axl-net-interfaces.c`'s other residents
  (`axl_net_locate_sb`, `axl_net_get_ip_address`) beyond moving the registry out.

## Design

### The registry

New file `src/net/axl-net-nic.c`, declared in `src/net/axl-net-internal.h`. It gets its
own file because no current file owns "what NICs does this machine have";
`axl-net-interfaces.c` is already 478 lines doing three unrelated jobs.

```c
typedef struct {
    uint8_t     mac[6];
    char        name[32];        /* "eth<ordinal>" */
    bool        link_up;
    uint32_t    mtu;
    bool        has_ipv4;
    uint8_t     ipv4[4], netmask[4], gateway[4];
    EFI_HANDLE  snp_handle;      /* first SNP child publishing this MAC */
    EFI_HANDLE  ip4cfg_handle;   /* NULL when none correlates */
} AxlNic;

/* Build the canonical per-physical-NIC list, in stable firmware-enumeration
   order. On AXL_OK, *out is an array of *count records the caller releases
   with _axl_net_nics_free (NULL when *count is 0). Zero NICs is success. */
int  _axl_net_nics_build(AxlNic **out, size_t *count);
void _axl_net_nics_free(AxlNic *nics);
```

### The build pass

One pass, two enumerations total:

1. `LocateHandleBuffer(SimpleNetwork)` — once.
2. `LocateHandleBuffer(IP4Config2)` — once.
3. Walk the SNP handles in firmware order. For each, read `Mode->CurrentAddress`.
   Skip the handle if that MAC was already recorded — **first handle wins**, preserving
   stable enumeration order so the ordinal is deterministic across calls.
4. For each surviving NIC, scan the already-fetched IP4Config2 list for a MAC match to
   set `ip4cfg_handle`, and fill `ipv4`/`netmask`/`gateway` from it.
5. Ordinal = array position. `name` = `"eth<ordinal>"`.

**MAC guard.** A handle whose `Mode->HwAddressSize` is 0 or less than 6 is skipped, not
keyed. A zero MAC compares equal to everything: keying on it would both collapse two
unkeyable NICs into one row and match an arbitrary IP4Config2 handle. `ip4cfg_for_mac`
already guards this; the registry must too. (The reverted Level-1 appendix code in the
handoff did *not* — it zero-filled `mac[6]` and compared all six bytes. Do not restore
it; the registry replaces it wholesale.)

**Link state.** The registry stores one `link_up`, computed as
`!Mode->MediaPresentSupported || Mode->MediaPresent`. Today `get_link_stats` uses this
rule and `list_interfaces` uses bare `MediaPresent` — two meanings of "link up" in one
library. The chosen rule is the correct one: a NIC whose firmware does not implement
media detection reports `MediaPresent = FALSE` meaninglessly, and treating that as
"down" hides working NICs. **This can flip `list_interfaces` rows from down to up**, and
netload gates on link state, so the new rule gets an exact-string test.

### Lifetime

Each **public** entry point builds the registry once on entry and passes it to every
internal helper; the snapshot dies when the call returns. Always fresh at the public
boundary, so there is no invalidation problem when drivers load or connect mid-session —
which they genuinely do, throughout netload's whole workflow. Within one call, the
repeated sweeps collapse: a bring-up goes from ~10 IP4Config2 enumerations to **2** —
one in the registry build, plus the one `auto_init` keeps to gate the IP4Config2-free
ladder (see the AUTO section on why that one cannot be folded in).

A process-lifetime cache was rejected: a stale NIC list after a driver loads is the same
class of bug this work exists to remove.

### Rewiring

Every index-taking API becomes: build registry → bounds-check `nic < count` → use
`registry[nic]`.

| API | Change |
|---|---|
| `list_interfaces` | rows from the registry; count-query returns the unique NIC count |
| `get_link_stats` | `registry[nic].link_up` |
| `arp_list` | ordinal → MAC → ARP-SB handle by MAC correlation |
| `set_dns`, `wait_ip_settled`, `get_dhcp_lease` | `registry[nic].ip4cfg_handle`; `ip4cfg_for_nic` deleted |
| `auto_init`, `set_static_ip` | same; `ip4cfg_for_nic_index` deleted |
| `get_driver_info(mac)`, `get_dhcp_lease_by_mac` | unchanged — already MAC-keyed |

`axl_net_arp_list` correlation is unproven: the ARP service binding may sit on a handle
where SNP is not reachable. **Verify in QEMU before committing to it.** If correlation
does not hold, stop and report rather than shipping a silent fallback — taking a MAC
argument instead of an index is the honest alternative, but that is a signature change
to decide with the user, not unilaterally.

### AXL_NET_NIC_AUTO

`AXL_NET_NIC_AUTO` is `(uint64_t)-1` → `SIZE_MAX`. Today `auto_init(SIZE_MAX)` resolves
by-MAC (fails: out of range), falls through to the positional fallback, and `SIZE_MAX >=
count` hits the `→ 0` clamp — so **the clamp being deleted is what currently implements
AUTO**. Deleting it without replacing AUTO would break auto-select on exactly the
multi-NIC boxes this work targets, and netload defaults to AUTO.

AUTO becomes an explicit resolution step in the registry:

> Prefer the first NIC that is **both link-up and has a non-NULL `ip4cfg_handle`**. If no
> NIC is link-up, use the first NIC with a non-NULL `ip4cfg_handle`. If **no** NIC has
> one, fall through to the same narrow positional fallback an explicit index gets (see
> the error contract below): the `nic_count == 1 && ip4cfg_count == 1` guard is what makes
> that guess unwrongable, and it does not care how the index was spelled. Otherwise
> `AXL_ERR`.

This matches what `axl-net-opts.h` already promises ("auto-detect first usable NIC") and
is strictly better than today: on the Dell, today's AUTO picks a link-down Broadcom —
the same wrong-NIC symptom by another route. Behavior change: AUTO may select a
different NIC than before, in the caller's favor.

**Correction (found in the mid-point review of Task 5).** An earlier draft of this
section had AUTO deliberately skip the fallback, justified by "the IP4Config2-free ladder
in `auto_init` still runs before this point." That justification was **wrong**: the ladder
only runs when `cfg_count == 0`. On uncorrelatable-MAC firmware with one NIC and one
IP4Config2 handle, `cfg_count == 1`, so the ladder never fires and AUTO simply failed —
regressing a path that works today via the clamp, while `auto_init(0)` on the same box
kept working. AUTO and an explicit index now get the identical guard.

**Do NOT "simplify" `auto_init`'s `cfg_count == 0` check** into "no NIC has an
`ip4cfg_handle`" to save the second IP4Config2 enumeration. The predicates are not
equivalent: `cfg_count == 0` means no IP4Config2 protocol exists at all, while "no NIC has
a handle" is also true when handles exist but fail to correlate. Swapping them sends
uncorrelated-MAC firmware down the DHCP4-SB/PXE ladder wrongly.

### Error contract

The `>= count → 0` clamp is deleted everywhere. An out-of-range index returns `AXL_ERR`
instead of silently configuring NIC 0. This is a public behavior change, and the point:
the clamp *is* the `c7c56b2e` defect.

**Uncorrelatable-MAC fallback.** When no IP4Config2 handle carries a NIC's MAC (firmware
where SNP is not reachable from the IP4Config2 handle), fall back to the positional
handle **only when `nic_count == 1` and `ip4cfg_count == 1`**. This applies to an
explicit index; `AXL_NET_NIC_AUTO` resolves through the AUTO rule above, not the
fallback. That is the only
configuration in which the guess cannot be wrong. Anything else returns `AXL_ERR` and
logs the uncorrelated MAC.

Note this is deliberately tighter than "exactly one IP4Config2 handle". A box can have
one IP4Config2 handle and three NICs — only one has the IP4 stack bound. If correlation
failed there, we still cannot know which NIC that handle serves, so guessing reproduces
the original bug at smaller scale.

### Consumer cleanup

Remove netload's display-side MAC dedup from `048d46fa` — the `seen[32][6]` loops in
`print_net_landscape`, `probe_firmware_stack`, and `probe_driver` are redundant once
`list_interfaces` dedupes. Keep the `--_hmap` seam; it stays a useful diagnostic.

### Documentation

`include/axl/axl-net.h`: the `AxlNetInterface` block, the `get_link_stats` note, and the
`get_dhcp_lease` `@warning` about SNP-vs-IP4Config2 index divergence all become false the
moment the registry lands. Rewrite them to state the ordinal contract. Keep recommending
the `_by_mac` variants for callers that need stability across driver-load events — the
ordinal is consistent within a boot's topology, but a NIC appearing shifts later
ordinals; a MAC never moves.

`src/net/README.md`: re-read for prose staleness per the doc-sync rule.

## Testing

Test-first. Bucket A/B for the new contract, bucket D for the clamp: tests land first,
RED confirmed, then the registry.

**Baseline first.** Confirm `test-netload-qemu.sh` is green on both arches *before*
touching anything, so a later failure is attributable.

**Unit** (`test/unit/axl-test-net.c`, ratcheted, both arches):

- *Clamp regression (the bucket-D guards).* Out-of-range `nic` returns `AXL_ERR` from
  `get_link_stats`, `set_dns`, `wait_ip_settled`, `get_dhcp_lease`, `arp_list`. These are
  safe negatives per the firmware-hazard rule: they return on our own validation before
  any firmware call.
- *Ordinal consistency.* For every `i < count`, `get_link_stats(i).link_up` agrees with
  `list_interfaces()[i].link_up`. This is the heart of the refactor and holds at any NIC
  count. SKIP-balanced at zero NICs.
- *Dedup.* Every MAC in `list_interfaces` output is unique; the count-query equals the
  filled count.
- *Link rule.* The `!MediaPresentSupported || MediaPresent` result is pinned exactly.

**Not tested in unit:** `set_static_ip` / `set_dns` driven positively. A valid index
there reconfigures live firmware. Negative-index and NULL guards only.

**Integration** (`test-netload-qemu.sh`, both arches): the existing 2-virtio-NIC
regression boot at line 531 asserts the high-index DHCP NIC leases, with a dead-socket
netdev on NIC A making a false pass impossible. It is the functional guard that deleting
`ip4cfg_for_nic_index` did not regress `c7c56b2e`. Add an assertion to that boot that the
landscape reports **2** interfaces, not 6.

**Real hardware.** The Dell R6725 validation of `c7c56b2e` is *not* a blocker: the QEMU
multi-NIC boot reproduces the bug and proves the fix. It remains a backlog nice-to-have.
Nothing here claims HW coverage.

## Risks

- **`arp_list` MAC correlation may not hold.** Verify in QEMU; stop and report if not.
- **`link_up` rule change can flip netload output.** Pinned by exact-string test; watch
  the netload integration assertions on the first green run.
- **Ordinal renumbering is a real semantic change** for any consumer passing an index
  between net calls. That is the intended fix, but consumers reading a *specific* NIC
  should move to the `_by_mac` variants.
- **AUTO may pick a different NIC than before.** Note netload does **not** default to AUTO
  — an earlier draft claimed it did, but `tools/netload.c` contains no reference to the
  sentinel and iterates concrete indices instead. AUTO's real consumers are
  `axl_net_init_static` and direct API callers, so netload's suite is *not* AUTO evidence.
- **`src/net/axl-net-opts.c:249` hardcodes `AUTO -> 0`** before calling the per-NIC
  setters, so `axl_net_init_static` never reaches the registry's link-up-preferring AUTO
  rule — it unconditionally picks NIC 0 (a link-down Broadcom on the Dell). Left in place
  deliberately: out of scope for this work. Removing it is safe only *after* the AUTO
  fallback fix above, which is now landed. Worth a follow-up.
