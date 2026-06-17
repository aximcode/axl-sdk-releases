# AXL Async HTTP Client — Implementation Tracker

**Status:** In progress. **Created:** 2026-06-16. Implements **Items 2–3** of
[AXL-Loop-Reentrancy-Plan.md](AXL-Loop-Reentrancy-Plan.md) (deferred-responses /
async-API coverage), now un-deferred by a concrete consumer: SoftBMC's alert
webhook does a sync `axl_http_post` from a timer callback, which nests an
ephemeral loop and trips the re-entrancy warning (`8a827765`).

This is the *implementation tracker* (live phase status, design decisions,
resume pointers). The *strategy / why* lives in the Loop-Reentrancy-Plan; the
*model* in [AXL-Concurrency.md](AXL-Concurrency.md). Expected to span multiple
sessions — update the phase table as each lands.

**Ground rules:** TDD + independent review before each commit; both arches
(x64 + aa64); `make check-ascii` / `check-docs` / `scripts/lint.sh` gate;
**no pushes** without the user's explicit OK; SoftBMC + axl-sdk commits are
**release-coupled** (consumers build only against the new SDK).

---

## 1. The decision (already made with the user)

- **Option A:** build the async cores; reimplement the sync `axl_http_get/post`
  + `axl_net_resolve` as thin wrappers over them (ephemeral loop + async + run
  + harvest). One I/O implementation, not two. Both sync **and** async stay
  public.
- **Scope (user-chosen, broader):** also refactor sync `axl_udp_send/recv` to
  wrap the existing `axl_udp_*_async` (DRY/consistency).
- **Ask 1 (release-critical): keep the sync-wait-in-callback path WARN-ONLY**
  (`axl-wait.c`, `8a827765`). Do NOT flip to hard `AXL_BUSY` until consumers
  are converted. Already satisfied (it proceeds; the flip is a later phase).

## 2. Locked contract (designed + independently reviewed + revised)

```c
// <axl/axl-net.h>
typedef void (*AxlNetResolveDoneFn)(const AxlIPv4Address *addr, AxlStatus st, void *user);
int axl_net_resolve_async(const char *hostname, AxlLoop *loop,
                          AxlCancellable *cancel, AxlNetResolveDoneFn cb, void *user);

// <axl/axl-http-client.h>
typedef void (*AxlHttpClientDoneFn)(AxlHttpClientResponse *resp, AxlStatus st, void *user);
int axl_http_get_async (AxlHttpClient *c, AxlLoop *loop, const char *url,
                        AxlCancellable *cancel, AxlHttpClientDoneFn cb, void *user);
int axl_http_post_async(AxlHttpClient *c, AxlLoop *loop, const char *url,
                        const void *body, size_t size, const char *content_type,
                        AxlCancellable *cancel, AxlHttpClientDoneFn cb, void *user);
```

Contract rules (from the contract-first review):
- **Returns AXL_OK ⇒ `cb` WILL fire later; any error ⇒ `cb` does NOT fire.**
- **One in-flight request per client** → a second call returns **`AXL_BUSY`**
  (cb does not fire). Use separate clients for concurrency.
- **`cb` owns `resp`** on success (frees via `axl_http_client_response_free`);
  `resp == NULL` on failure. A non-2xx status is success (AXL_OK + resp).
- **Fire-and-forget** (`cb == NULL`): runs to completion, response freed
  internally — discards ALL post-initiation outcomes (errors included).
- **Do NOT `axl_http_client_free` while in flight** — cancel or await `cb`.
- `body` is **borrowed until `cb` fires** (not copied). Contiguous body only
  (no async streaming peer — use sync `axl_http_request_streaming`).
- https requires `axl_tls_init()` once at startup (strippable-TLS contract).
- `cancel` is the `AxlCancellable` from the rest of the async SDK (NULL = none).
- The async core **takes the client** (config + keep-alive) — this is what lets
  the sync API be a thin wrapper (Option A). [Diverges from SoftBMC's proposed
  client-less shape; SoftBMC keeps a long-lived client, which it already does.]

## 3. Design notes

- **State-machine discipline (Samba `tevent_req` lens, borrowed conceptually,
  NOT the framework):** one heap-allocated request-state struct per
  `axl_http_*_async`; each I/O step launches the next `_async` op and its
  completion callback advances the machine; **exactly one completion path**
  frees the state + invokes the user `cb`; cancel checked at each transition.
  This is the proven composable-async shape. We deliberately do NOT adopt
  tevent/talloc or build a generic `AxlAsyncReq`/promise layer — AXL already has
  `AxlLoop` + `AxlCancellable` + the `_async` primitives, and the design ethos
  rejects speculative abstraction (`AXL-Concurrency.md`, the AxlFuture note).
  If a *second* composite async op appears, extract a shared helper then.
- **Reuse (from the building-block map):** request build via
  `http_build_request_line` (axl-http-core.c); parsers
  `axl_http_parse_status_line` / `_parse_headers` / `_find_header_end` are
  incremental (chunk-fed) — drive them from the async recv callback;
  `read_chunked_body` logic for chunked framing. TLS:
  `axl_tls_handshake_async` (stage recv → handshake, ret 0/1/-1),
  `axl_tls_write_async`, `axl_tls_stage_data`.
- **Redirects:** the async machine re-enters (resolve→connect→…) on 301/302/307
  up to the client's `max.redirects`, mirroring sync `do_request` recursion.
- **Timeout:** a loop timeout bounded by the client's `timeout.ms` (resolve uses
  a fixed 5 s, no client to inherit from).
- **Phase-2 refinements (vs the original building-block map):**
  - **Connect by resolved address.** `axl_tcp_connect_async_via_ex` resolves the
    host *synchronously* (nested `axl_loop_run`), which would defeat the
    nest-free goal. Split it: the new internal `axl_tcp_connect_addr_async`
    takes a pre-resolved `AxlIPv4Address`; the machine does
    `axl_net_resolve_async` → connect-to-addr (also gives the async DNS path
    real end-to-end coverage). The `_via_ex` form now just resolves+delegates.
  - **Async TLS through the ops vtable.** The async machine lives in the
    *always-linked* client, so it must NOT statically reference `axl_tls_*`
    (that re-pins mbedTLS into plain-HTTP consumers, undoing `e0222d65`). The
    `AxlHttpClientTlsOps` vtable gained `handshake_async` + `write_async`;
    `test-tls-strippable.sh` confirms mbedTLS still strips.
  - **Deferred first hop + single completion.** `http_async_start` schedules the
    first transition on a 1 ms timer, so a call returning `AXL_OK` never fires
    the callback re-entrantly even when the first step fails. One `req_finish`
    frees state + fires the callback exactly once; keep-alive success leaves the
    connection open, every other outcome drops it. Stale reused keep-alive
    connections replay once (`req_transport_fail`), mirroring the sync client.

## 4. Phase tracker

| # | Phase | Status | Commit |
|---|---|---|---|
| 0 | Ask 1 — keep sync-wait WARN-only (no flip) | ✅ confirmed (no change needed) | — |
| — | Contract + contract-first review + fixes | ✅ done (headers only) | (uncommitted) |
| 1 | `axl_net_resolve_async` + sync `axl_net_resolve` wraps it | ✅ done | `f465e2d5` |
| 2 | `axl_http_{get,post}_async` core state machine | ✅ done | `1ca5e1aa` |
| 3 | Sync `axl_http_get/post/put/delete/request` → wrap the async core (Option A) | ✅ done | `b391d63b` |

> **Phase 3 done.** The five contiguous-body sync entry points are now thin
> ephemeral-loop wrappers over the async core (`_axl_http_request_sync`); the
> streaming path (`axl_http_request_streaming`) keeps its own `do_request` I/O
> (no async streaming peer). Both share one overflow-safe request builder
> (`_axl_http_build_request`, bounded `ReqBuf`), which **retired the pre-existing
> stack-overflow** in `do_request`'s old `req_len += axl_snprintf(req_buf +
> req_len, sizeof - req_len, ...)` builder (the same class the phase-2 review
> caught in the async path). The sync wrapper drives a Poll tick over the
> client's current socket so it still progresses at a raised TPL, and clears the
> kept-alive socket's `async_loop` / recv source-ids before freeing the
> ephemeral loop. Review-driven: `timeout.ms` is now an IDLE (per-phase,
> re-armed-on-progress) bound, NOT a whole-op ceiling — preserving the old sync
> client's per-op timeout guarantee for slow-but-progressing transfers.
| 4 | Sync `axl_udp_send`/`sendrecv` → wrap `axl_udp_*_async` | ✅ done | `159fefed` |
| 5 | CI wiring + lint/ascii + **Sphinx docs** + review (axl-sdk, in-repo) | ✅ done | (across 2–4) |
| 5 | CI wiring + lint/ascii + **Sphinx docs** + review + commit (axl-sdk) | ⬜ | — |
| 6 | SoftBMC: webhook→`axl_http_post_async`, syslog→`axl_udp_send_async` (cross-repo, release-coupled) | ✅ done (SoftBMC repo) | — |
| 7 | flip sync-wait WARN → hard `AXL_BUSY` | ✅ evaluated + **rejected** (warn-only is final) | — |

**Phases 1–6 are COMPLETE.** All async cores + sync rewraps landed, both arches
green (6938/6938 unit + the http/redfish/https/udp/sntp/netdiag integration
suites), Sphinx renders the new APIs (`build-docs` exit 0, zero Sphinx
warnings), `test-http-async-qemu.sh` ci-wired. **SoftBMC's consumer migration is
complete and committed** (separate repo, built against this SDK working tree) —
so the release coupling that held the axl-sdk commits is now satisfied.
**Remaining:** push the axl-sdk commits + cut a release (a large v2.0.0 — the
breaking `AxlSourceId` change — packaging everything since v1.8.0, so it needs a
`git log v1.8.0..HEAD` ↔ CHANGELOG sweep first), then **phase 7** (the
WARN→AXL_BUSY flip) in a FOLLOW-UP release once any other sync-net-in-callback
consumers are surveyed/converted. **axl-sdk `main` is 12 commits ahead of
`origin/main`, UNPUSHED — no push without explicit OK.**

Verification gate per phase: TDD red→green, both arches, review before commit.

**Coverage note (phase 1):** the unit test pins the IPv4-literal path
(deterministic, no network) + the deferred-callback / single-completion
contract. The DNS4 *hostname* async path (open child → HostNameToIp → event +
poll tick → harvest → teardown) is NOT exercised by automated tests — it needs
a real DNS server (SLIRP's is host-dependent / flaky). It mirrors the
well-exercised sync DNS4 calls; the teardown ordering (Configure(NULL) as the
synchronous abort, no nested drain) was review-validated. Exercise it on real
hardware / via a hostname-URL fetch when convenient; the phase-2 HTTP async
path can drive it end-to-end if pointed at a hostname.

## 5. SoftBMC coordination (cross-repo, release-coupled)

- Handoff: `../softbmc/docs/axl-sdk-async-http-handoff.md`.
- Migration (phase 6): `alert_webhook_send` → `axl_http_post_async(client,
  loop, …)` with a cb that frees the response (or logs failures — fire-and-
  forget discards them). `syslog` path → `axl_udp_send_async` (already exists).
  Verify `tests/alert-check.sh` 23/23 under `mirror=no` AND `mirror=yes`, with
  **no `[WARN] wait: synchronous wait …`** lines in `/api/logs`.
- **Release note:** the SoftBMC commit and the axl-sdk async APIs are coupled —
  SoftBMC builds only against an SDK that has them. Do not release/ build
  SoftBMC against a pre-async SDK.

## 6. Resume pointers

- Building-block map (signatures + reuse): see the session that created this doc
  (async TCP ops in `axl-tcp-async.c`, sync-wrapper pattern in `axl-tcp-sync.c`,
  sync DNS in `axl-net-resolve.c`, sync HTTP in `axl-http-client.c`,
  TLS async in `axl-tls.c`).
- Cross-refs: `AXL-Loop-Reentrancy-Plan.md` (Items 2–3), `AXL-Concurrency.md`,
  `src/event/axl-wait.c` (the warn site, keep warn-only).
