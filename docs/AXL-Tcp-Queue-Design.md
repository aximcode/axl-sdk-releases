# AXL-Tcp-Queue-Design — send/receive token queues for AxlTcp

**Status: IN PROGRESS 2026-08-12** (accepted 2026-08-12, proposed 2026-08-03).
Step 1 of 6 landed on `worktree-tcp-token-queue` (`41de5751`); see §6 for the
per-step state and §6b for what review left open.

**Order of work, decided 2026-08-12:**
1. **Steps 2-4 (HERE, NEXT).** They delete the three ad-hoc mechanisms, and
   step 3 is what fixes the §1a connection drop.
2. **Cut v3.2.0** — so the release ships WITHOUT that known, reproducible
   defect. `docs/RELEASING.md`.
3. **gcc/newlib toolchain work**, which reimplements `AxlTcp` over POSIX/libc
   and settles the layering question this design does not prejudge (§3.7, §4).
4. **Steps 5-6 after that.** Step 5 ports the RECEIVE queue, and doing it
   against a transport about to be reimplemented on POSIX sockets is the part
   that would be redone.

An earlier revision of this header had the release first and paused ALL of the
queue behind it, reasoning that the POSIX move made this work throwaway. That
reasoning applies only to step 5 — steps 2-4 cost nothing extra by being done
now, and doing them first keeps a live defect out of the tag.

Decisions taken at acceptance:
- **Do this before the POSIX-layering revisit.** That revisit is deferred to the
  C++/newlib toolchain track, which carries its own layering changes; settling
  both at once avoids two independent refactors of the same boundary. §4's
  no-new-public-API constraint is what keeps the two separable, so it is now a
  hard requirement rather than a preference — if the implementation finds new
  public API unavoidable, stop and reassess (§4).
- **The two live `AXL_BUSY` misclassifications in `axl-http-response.c` stay
  unpatched** until the queue lands. Patching them directly would add the
  fourth serialisation mechanism §6 exists to avoid. See §1a.
Scope: both directions — the outbound send queue and the inbound receive queue.
Reference implementation: EDK2 `NetworkPkg/TcpDxe` socket layer (`Socket.h`,
`SockInterface.c`, `SockImpl.c`) at `tianocore/edk2` 46548b1. This design is a
**port** of that layer's token-queue mechanism into AXL naming and style, not a
fresh invention.

---

## 1. Why

`axl_tcp_send_async` is strictly one-send-in-flight. `AxlTcp` holds a single
`EFI_TCP4_IO_TOKEN tx_token` (`src/net/axl-tcp-internal.h:77`), so a second
`Transmit` would clobber the first's in-flight token. A caller that submits while
one is outstanding gets `AXL_BUSY`.

That is an AXL restriction, not a firmware one. The UEFI spec describes
`EFI_TCP4_PROTOCOL.Transmit` as *"Queue outgoing data into the transmit queue"*,
with a matching receive queue.

Because there is no queue, **every caller has invented its own serialisation**:

| # | Mechanism | Where |
|---|---|---|
| 1 | Per-connection outbound frame queue | `src/net/axl-http-ws.c` |
| 2 | `AXL_BUSY` floor before `mbedtls_ssl_write` | `axl_tls_write_async` |
| 3 | Pending-write back-pointer + resume-on-completion | `handshake_flush_async` (`0a9f81fe`) |

Three answers to one question, each locally correct. #3 shipped because its
absence was a live bug: `AXL_BUSY` collapsed into a fatal error killed healthy
connections whenever two TLS handshakes interleaved on one loop. It also had to
introduce a use-after-free hazard (a back-pointer from the write completion into
the TLS context) and then guard it — the tell that the fix works against the
grain of the substrate rather than with it.

### 1a. The same bug is still live, in two more places (measured 2026-08-12)

`0a9f81fe` fixed the `!= AXL_OK` test in `handshake_flush_async`. The identical
test survives on the response path, and it is dropping connections today:

FOUR sites in `axl-http-response.c`, every one `if (rc != AXL_OK) { …
reset_connection(conn); }` over a submit that can return `AXL_BUSY`:

| site | submit | diagnostic |
|---|---|---|
| `:361` | chunked terminator | **none — resets silently** |
| `:410` | stream pump | `stream: send_async submit failed - abrupt close` |
| `:616` | stream headers | `send_response: stream-headers submit failed` |
| `:662` | `send_response` | `send_response: submit failed - abrupt close` |

(`:322` and `:802` also test `!= AXL_OK` but are a producer error and a file
read — not submits, so not this defect.)

Measured, not inferred. `test-https.sh`'s concurrent-handshake gate fails
**1 run in 3 on an idle box** — always `23/24 OK (width 4)`. Instrumenting the
warning proves the cause end to end:

```
client: http_code:curl_rc = [000:56]        # 56 = failure receiving data
server: send_response: submit failed rc=-10 (AXL_BUSY=-10)
```

One warning per dropped request. `axl_tcp_send_in_flight` is per-socket and the
TLS handshake itself writes, so on a *fresh* connection the handshake's final
flight is often still pending when curl's immediately-following request reaches
`send_response` — which then destroys a healthy connection over the one
condition §3.2 calls retryable. `axl-http-ws.c:189` branches on `AXL_BUSY`
correctly; the response path never learned to.

This matters for prioritising the design: the ad-hoc approach is not merely
*untidy*, it is still producing new instances of one defect. Two of the three
mechanisms in the table above were added to paper over `AXL_BUSY`, and the
call sites that never got one are silently dropping connections.

It also sharpens the acceptance test in §6 step 2, in two ways.

**The gate is RED today**, so "must stay green with the resume gone" is only
meaningful once the queue lands. Reproducer for the branch doing this work:

```sh
# ~1 failure in 3
./test/integration/test-https.sh --arch X64
```

**And ~1-in-3 is a FLOOR, not the rate.** The `:361` terminator site resets with
no `axl_warning` at all, so the "one warning per dropped request" correlation
that produced the number above is blind to it. A run can lose a chunked
response there and still look clean. Two consequences for this work:

- the measured rate understates the defect, and
- the acceptance test can read green while `:361` still drops silently, so
  step 2 must assert on the SITES being gone (all four `!= AXL_OK` submit
  tests), not only on the gate going green.

**Nothing automated watches that gate.** `scripts/verify.sh` runs only
`test-axl.sh` (both arches) — `test-https.sh` runs solely under
`run-integration.sh`, which is not part of the day-to-day gate. So neither the
current red nor a future regression to it is observed by the fast loop. That is
the tree's own `check-tautology` lesson — a gate outside the gate runner cannot
fail — and it is why this defect survived in `main` long enough to be found by
a release dispatch rather than by a build.

Patching the four sites directly would mean a FOURTH serialisation mechanism —
the thing §6 says to stop and reassess over. Left unpatched deliberately.

**Success criterion: this design must let all three be deleted.** If it does not,
it is not worth doing.

## 2. Why port EDK2 rather than design fresh

EDK2's socket layer solves exactly this problem, in the same environment
(single-threaded, no preemption, event-driven, TPL-sensitive), and has been in
the field for over a decade. Two of its properties were open questions here and
are answered by the reference:

- **No copying.** `SockProcessTcpSndData` calls `NetbufFromExt`, which *wraps*
  the caller's fragments with a no-op free callback rather than copying them.
  The caller's buffer is borrowed until the token is signalled. That is exactly
  AXL's existing `axl_tcp_send_async` contract, so the port preserves it — no
  allocation-per-send on the hot path, and no redefinition of ownership. This
  was the reason a queue looked expensive; it is not.
- **No dependence on firmware completion ordering.** The socket layer keeps its
  queue *above* a single protocol-level flow and completes tokens itself, in
  FIFO order, from a byte count the protocol reports. It never relies on the
  layer below signalling multiple outstanding tokens in order.

That second point matters for portability. Reading EDK2's `TcpDxe` shows it
completes in order, but that is *implementation*, not spec — a vendor TCP stack
is free to differ. Designing on it would repeat a mistake already made in this
tree: generalising `ArmPsciMpServicesDxe`'s `EFI_TIMEOUT` behaviour into a
portable assumption (see `0a9f81fe`'s commit message). The port therefore keeps
one EFI token in flight and queues above it, which requires no such assumption.

Structural note: EDK2's socket layer sits above TcpDxe's own TCP engine, whereas
AXL sits above `EFI_TCP4`. The layering is the same shape — a token queue above
a single transport flow — so the mechanism transfers directly.

## 3. The EDK2 mechanism, as ported

### 3.1 Types

EDK2 `SOCK_TOKEN` → `AxlTcpToken`:

```c
typedef struct AxlTcpToken {
    struct AxlTcpToken *next;        /* EDK2: LIST_ENTRY TokenList */
    const void         *buf;         /* borrowed from the caller until done */
    size_t              len;
    size_t              remaining;   /* EDK2: RemainDataLen */
    AxlTcpCallback      cb;
    void               *cb_data;
    AxlCancellable     *cancel;
} AxlTcpToken;
```

EDK2 keeps **two** send lists; the port keeps both, because the split is what
makes flow control expressible:

| EDK2 | AXL | Meaning |
|---|---|---|
| `SndTokenList` | `send_queued` | accepted, not yet handed to the transport |
| `ProcessingSndTokenList` | `send_active` | handed down, awaiting completion |
| `RcvTokenList` | `recv_queued` | outstanding receive requests |
| `SndBuffer.HighWater` / `.LowWater` | `send_high_water` / `send_low_water` | byte-count flow control |

### 3.2 Send path — port of `SockSend`

1. **Reject a re-submitted token.** EDK2 calls `SockTokenExisted` and returns
   `EFI_ACCESS_DENIED` if a token with the same event is already queued. AXL's
   equivalent is rejecting a `(buf, cb_data)` pair already present, returning
   `AXL_ERR`. This catches a caller double-submitting one buffer, which under the
   borrow contract is a use-after-free waiting to happen.
2. **Flow control decision.** `free = high_water - queued_bytes`. If
   `free < low_water`, or the socket is not connected, append to `send_queued`
   and return `AXL_OK` — the send is accepted but deferred.
3. Otherwise append to `send_active` and submit to `EFI_TCP4` immediately.

**`AXL_BUSY` is not returned at all. CORRECTED 2026-08-12** — this section
previously said it "survives only as a genuine backpressure signal … the queue
is at `high_water` and the caller must wait". That was a deviation from the
reference, and it was implemented that way first.

Re-reading `SockSend` (SockInterface.c:642): EDK2 **never refuses for
capacity.** When `FreeSpace < LowWater` it buffers into `SndTokenList` and
returns `EFI_SUCCESS`; its only failure is `EFI_OUT_OF_RESOURCES` from the
token allocation. The watermarks choose submit-now vs defer — they are not a
rejection threshold.

That single deviation is what kept `AXL_BUSY` handling alive in every caller
above the transport: `axl_tls_write_async` needed a capacity floor before
encrypting, the WS layer needed a buffer-on-refusal path, and the four submit
sites in `axl-http-response.c` had to interpret a status they got wrong (§1a).
Removing the refusal removes all of it. `axl_tcp_send_async` now returns
`AXL_OK` (submitted or queued) or `AXL_ERR` (allocation failed), and nothing
else.

The cost, accepted deliberately and shared with EDK2: the queue can grow
against a peer that never drains, bounded only by callers not submitting
without limit. §7 tracks whether a depth cap is wanted on top.

### 3.3 Completion path — port of `SockDataSent`

EDK2 is called by the protocol with a byte count and walks the processing list
head-first:

```
while (count > 0):
    tok = head(send_active)
    if tok->remaining <= count:
        remove(tok); signal(tok, SUCCESS); count -= tok->remaining; free(tok)
    else:
        tok->remaining -= count; count = 0
promote_deferred()          /* EDK2: SockProcessSndToken */
```

The AXL port is simpler in one respect: one `axl_tcp_send_async` call is one
buffer and one EFI token, so a completion always retires exactly the head token
(`remaining` is either 0 or untouched). `remaining` is kept anyway — it costs one
field and it is what makes a future scatter/gather submission a local change
rather than a redesign.

After retiring, promote from `send_queued` into `send_active` while
`free >= low_water`, submitting each — the port of `SockProcessSndToken`.

### 3.4 Receive path — port of `RcvTokenList` / `SockDataRcvd`

Today `AxlTcp` has one `rx_token` and consumers re-arm from inside the completion
(`on_tls_handshake_data` returning `true`, `start_conn_recv`, and so on). Between
completion and re-arm there is a window with no outstanding receive; the firmware
buffers, so it is a latency and throughput cost rather than a correctness one.

The port lets several receive requests be queued: `recv_queued` holds them, one is
submitted at a time, and the completion retires the head and submits the next.
Consumers that re-arm by returning `true` keep working unchanged — that path
becomes "enqueue one more" rather than "arm the only one".

### 3.5 Teardown

EDK2 flushes both token lists on close, signalling each with an error status
(`SockConnFlush`). The port does the same, signalling every queued and active
token with `AXL_CANCELLED` so no caller is left waiting on a callback that will
never come. This must run *before* the owning context is freed — see the ordering
hazard in §5.

## 3.6 Cancelling a QUEUED token — port of `SockCancelToken`

Cancellation must work on a token that has not yet been submitted, not only on
the active send. EDK2 does this in `SockCancelToken` (`SockImpl.c:573`): walk
the list, `SIGNAL_TOKEN(..., EFI_ABORTED)`, `RemoveEntryList`, `FreePool` — and
it is called on `SndTokenList`, the deferred list, as well as the processing
one.

**This is load-bearing, not a nicety.** `axl_tcp_send` (the SYNC wrapper) hands
`axl_tcp_send_async` a stack `SyncResult`, an ephemeral `AxlLoop` and an
`AxlCancellable`, then frees all three when it returns. Under a queue that
accepts the send, the only thing keeping that safe is cancel-while-queued: the
wrapper's timeout fires the cancellable, the queued token is retired with
`AXL_CANCELLED`, and its callback runs while the stack frame is still alive.
Arm the cancellable only at promotion (as the first implementation did) and the
wrapper frees a context a queued token still points at — a use-after-free the
queue would have introduced. See §7.

## 3.7 Forward fit: this survives the move to POSIX/newlib

`AxlTcp` is expected to be reimplemented over POSIX sockets when the newlib
toolchain lands. That does NOT make this port throwaway work — the token queue
is the same shape every POSIX async stack uses, and the parts that look
EFI-specific are the ones that map most directly:

| here (EFI_TCP4) | there (POSIX) |
|---|---|
| one `EFI_TCP4_IO_TOKEN` in flight | one `send()` outstanding until it returns |
| completion event retires the token | writability (`poll`/`epoll` `POLLOUT`) drains it |
| `remaining` is 0-or-untouched | a PARTIAL `send()` retires part of a token — `remaining` finally earns its keep |
| `high_water` / `low_water` | identical; it is a userspace send buffer either way |

So keep the transport-facing seam NARROW — "submit one token" and "N bytes
retired" — which is what EDK2 does via `SockProcessTcpSndData`. Then swapping
`EFI_TCP4.Transmit` for `send()` is a local change inside that seam rather than
a requeue of the whole design. The chunk-chaining currently inside
`axl_tcp_send_async` belongs BELOW that seam for the same reason.

## 4. Public API impact

**`axl_tcp_send_async`'s signature does not change.** Its RETURN SET does: it
stops returning `AXL_BUSY` entirely (§3.2), leaving `AXL_OK` for accepted —
submitted or queued — and `AXL_ERR` for a failed allocation. Callers lose a
status they had to handle rather than gaining one.

**REVISED 2026-08-12.** This section previously made "no public API impact" a
hard constraint, on the reasoning that new public surface would prejudge the
parked POSIX-layering question. Two corrections from the owner:

- **We own every consumer**, so a public API change is a coordinated edit, not a
  compatibility event. "Do not add API" is therefore not the goal — *do what a
  TCP-wrapping library does*, following EDK2, is.
- The layering question is being settled by the **newlib/POSIX move** (§3.7),
  not by this queue. Avoiding public surface here does not buy the freedom the
  old text claimed it did, because that decision is happening elsewhere anyway.

What survives as a real constraint is narrower and is about lifetime, not
surface: the BEHAVIOUR change is not internal, because a caller in this library
encodes the old meaning in its control flow (§7). Any semantic change to a
return value needs its callers re-read, not just its signature preserved.

Docstrings must be updated with the behaviour. DONE: `axl-tcp.h`'s `send_async`
entry documents the queue, the borrow lifetime, the exact meaning of `AXL_ERR`
(rejected outright — and only then does no callback fire), and that the callback
is deferred; `axl_tcp_close`'s entry documents that it retires every pending
send and that closing twice is a no-op.

## 5. Risks

- **Teardown ordering.** `do_reset_connection` frees `tls_ctx` *before*
  `axl_tcp_close`. Any queue holding callbacks into a consumer context inherits
  that hazard: a token flushed after its context died dereferences freed memory.
  `0a9f81fe` handles this for one write via a severable back-pointer; the queue
  must handle it for N. Flushing on close (§3.5) is the mitigation, and the
  ordering must be asserted, not assumed.
- **Promote-vs-signal ordering (found during implementation, 2026-08-12).**
  EDK2's `SockDataSent` retires the token, signals it, *then* calls
  `SockProcessSndToken` to promote (§3.3). Reproducing that order here is
  unsafe, and the reason is a property of the reference, not a flaw in it:

  ```c
  /* SockImpl.h:23 */
  #define SIGNAL_TOKEN(Token, TokenStatus) \
    do { (Token)->Status = (TokenStatus); \
         gBS->SignalEvent ((Token)->Event); } while (0)
  ```

  `gBS->SignalEvent` QUEUES the notify at its TPL — the consumer's code does
  not run inline. So when EDK2 promotes on the next line, no application code
  has executed and the socket cannot have been destroyed underneath it.

  AXL's `on_send_complete` (`axl-tcp-async.c:640`) instead calls
  `cb(sock, status, data)` DIRECTLY, and `axl_tcp_send_async`'s own comment
  records the consequence: "old_cb could call axl_tcp_close, freeing sock".
  Promoting after that callback would touch a freed socket — a UAF the
  single-send design never had, because it had nothing left to do afterwards.

  The first cut answered this by **promoting BEFORE signalling** — arm the next
  queued token, then fire the completed one's callback. **That answer was
  wrong, and §6b is the bill:** every ordering of retire/promote/signal loses
  something while the callback runs inline, because the hazard is the inline
  call itself, not the order around it.

  **RESOLVED (§6e): port `SIGNAL_TOKEN` rather than working around it.** A
  retirement queues the callback on the loop (`axl_defer`) instead of calling
  it, which is what `gBS->SignalEvent` does, so AXL now has the property the
  reference has and can follow the reference's order. There is no inversion
  left to comment.

  One consequence survives and is worth keeping in mind: the completed send's
  callback runs while the NEXT send is already armed. That is fine for the
  transport, but any caller assuming "my callback means the socket is idle"
  would be wrong — no current caller does, and the queue's whole point is that
  they need not care.
- **Deleting the WS queue is the real test.** It carries semantics beyond
  serialisation (frame ordering, close-frame sequencing). If it cannot be
  deleted, this design has not solved the problem — it has added a fourth
  mechanism.
- **Flow-control marks are a guess.** EDK2's defaults are tuned for its own
  buffers. AXL's `high_water` / `low_water` need measuring, not copying.
- **Cancellation.** `axl_tcp_send_async` takes an `AxlCancellable`. Cancelling a
  *queued* (not yet submitted) token is new behaviour with no EDK2 analogue in
  the same shape; it must retire the token and fire `AXL_CANCELLED` without
  disturbing queue order.

## 6. Plan

Branch: `worktree-tcp-token-queue`. Status as of 2026-08-12.

1. **DONE — `41de5751`.** Port the types and the send queue; keep one EFI token
   in flight. Also ported `SockCancelToken` (§3.6), which turned out to be a
   prerequisite rather than a later step. Three new unit tests, each
   sabotage-verified. `verify.sh` ALL GREEN both arches (10387), integration
   145/0.
2. **DONE — `831e1cbf`.** Delete the `handshake_flush_async` resume and its
   back-pointer (`0a9f81fe`). The concurrent-handshake check in `test-https.sh`
   is the acceptance test for the send half: 0 fail / 6 with steps 2+3 in,
   confirmed twice.
3. **DONE — `831e1cbf`.** Delete the `axl_tls_write_async` `AXL_BUSY` floor.
   **This is the one that fixes §1a**, not step 1 — see the measurement below.
   (The seqno debug assert went with it: `23c75ee0` removed the refusal
   entirely, leaving nothing for the assert to catch.)
4. **DONE, but NOT as written — the WS queue STAYS.** See §6c. Its
   serialisation role is gone (the dead `AXL_BUSY` branch in `ws_outq_pump` is
   deleted); its lossy-backpressure POLICY is not a workaround and has no
   equivalent below it.
5. **TODO.** Port the receive queue.
6. **TODO.** Measure `high_water` / `low_water` rather than inheriting EDK2's
   numbers. The values shipped in step 1 are PLACEHOLDERS
   (`TCP_SEND_HIGH_WATER` / `_LOW_WATER` in `axl-tcp-internal.h`) and are
   explicitly not tuned.

Steps 2–4 are the point. If any of them cannot be completed, stop and reassess
rather than shipping a fourth serialisation mechanism.

### 6a. Step 1 is neutral on the §1a defect — measured, not assumed

Same-worktree A/B, 6 runs each of `test-https.sh --arch X64`, identical load,
queue disabled in place for the control:

| build | concurrent-handshake gate |
|---|---|
| queue enabled | 4 fail / 6 |
| queue disabled | 3 fail / 6 |

Noise. Step 1 neither fixes nor regresses that gate, which is what the design
predicts: the drop comes from `axl_tls_write_async` refusing on its OWN
in-flight floor, and that floor is step 3. Recorded because a single green run
appeared during the integration suite and would have read as a fix — at a
~50% failure rate one pass proves nothing.

Also worth carrying forward: the gate's failure rate is LOAD-DEPENDENT (33% on
main earlier the same day, 50% here), so compare A against B in one session
rather than against a number measured hours ago.

### 6c. Step 4 answered: the WS queue was doing TWO jobs

§1's success criterion was "this design must let all three [mechanisms] be
deleted", and §5 called the WS queue "the real test — it carries semantics
beyond serialisation. If it cannot be deleted, this design has not solved the
problem."

Reading `ws_outq_enqueue` against that: the queue is not one mechanism, it is
two, and only one of them was a workaround.

| job | verdict |
|---|---|
| Serialise sends so only one is outstanding | **Redundant now.** The transport queues; the `AXL_BUSY` branch in `ws_outq_pump` is deleted. |
| LOSSY, bounded, frame-aware backpressure | **Keep.** No equivalent below it, and no wish for one. |

The second job is a real policy, not scaffolding. `ws_outq_enqueue` rejects a
frame larger than the whole outbound budget (a multi-MB frame handed to the
transport as one send wedged the single-threaded server), and it DROPS the
oldest droppable frames until the new one fits `WS_OUT_MAX_FRAMES` /
`WS_OUT_MAX_BYTES`. For a broadcast/telemetry socket, shedding stale frames is
the correct behaviour and is what a client that cannot keep up should get.

The transport queue deliberately does the opposite: it is lossless and
unbounded (§3.2), because dropping a byte mid-stream is not a thing TCP may do.
Both are right at their own layer.

So step 4 does not "fail" the §5 test. It refines the criterion: what had to
disappear was the SERIALISATION each caller invented, and all three of those are
now gone (`handshake_flush_async`'s resume, the `axl_tls_write_async` floor, and
the WS pump's refusal branch). Keeping a frame-level drop policy above a
byte-level lossless queue is not a fourth serialisation mechanism — it is the
one place in the stack where discarding data is a deliberate feature.

One consequence worth stating: `ws_out_inflight` STAYS too. Submitting every
queued frame to the transport at once would order them correctly, but it would
also move them beyond reach of the drop policy — a frame already handed down
cannot be shed. Holding one in flight is what keeps the rest droppable.

### 6d. SUPERSEDED by §6e — close must BAR PROMOTION, not fire the active send's callback

> Kept because the two failed attempts below are the evidence for §6e, not
> because the conclusion stands. Deferring the callbacks made "close fires the
> active send's callback" implementable, and §6e does exactly that.


Review found that a send the caller merely QUEUED could be promoted into the
active slot during teardown and then lose its callback entirely — `close`
cancels the transport token without invoking `on_send`. A caller whose send was
ACCEPTED would wait forever. Real regression, introduced by the queue.

The obvious fix — have `close` fire `sock->on_send(AXL_CANCELLED)` — **does not
work, and the tree already knew.** `ws_outq_clear`'s comment named it as "the
airtight fix" and rejected it because the callback re-enters teardown
(`on_response_sent` → `reset_connection` → `axl_tcp_close`). Measured here:
`AxlTestNet` hung outright, no Results footer, starving every binary after it in
the same boot.

A second attempt — guarding `tcp_close_impl` as a whole against re-entry — hung
identically, for a different reason: close legitimately runs MORE THAN ONCE on a
socket whose teardown deferred (the graceful / loop-deferred paths finalize
later), so an early return skips the real teardown.

What works is to attack the cause rather than the symptom. `close` sets
`send_retiring` BEFORE flushing, and `tcp_send_promote` refuses while it is set.
Nothing moves into the un-callback'd slot during teardown, so every un-started
send stays on the queue and is retired with `AXL_CANCELLED` by the flush.

Consequence, stated so it is not mistaken for an oversight: **the ACTIVE send's
callback still does not fire on close.** That is the contract from before the
queue, it is what the WS comment's rejection preserved, and it is now pinned by
a test. Only the regression the queue introduced is repaired.

### 6b. The defects that made the branch unmergeable — FIXED by §6e

Found by review 2026-08-12, after steps 1-4 were gated green. Three rounds of
patches each left or created one of these, which is the signal that the shape is
wrong rather than the patches. **The root cause is one thing:** AXL invoked send
callbacks INLINE, so any callback could free the socket in the middle of a
transport operation. EDK2 never has this — `SIGNAL_TOKEN` is `gBS->SignalEvent`,
which QUEUES the notify, so no consumer code runs inside the transport's own
call stack (§5).

**Fix (2026-08-12): defer send callbacks, porting SIGNAL_TOKEN properly** —
implemented, see §6e. Retire a send by scheduling its callback on the loop
instead of calling it. That kills all of the following at the root:
promote-vs-signal ordering stops mattering, close can retire the active send
safely, and re-entrancy disappears. Defects 1, 2, 3, 5 and 6 are fixed there;
defect 4 is a separate mechanism and is tracked in §6f.

| # | Defect |
|---|---|
| 1 | **A promoted send still loses its callback.** `complete → promote → cb → cb closes`: the promoted token is ACTIVE by then, so the flush skips it and close never fires it. Caller hangs, buffer pinned, wrapper ctx leaks. Barring promotion during close only helps when close starts FIRST. |
| 2 | **Re-entrant close frees `sock` under the outer close.** `send_retiring` bars promotion only; a nested `axl_tcp_close` from a flush callback runs to `finalize_sock` → `axl_free(sock)`, then the outer keeps dereferencing and re-frees. HTTP escapes via `tearing_down`; 9p does not. |
| 3 | **`test_tcp_send_async_flush_on_close` does not pin what it claims.** It never promotes before closing, so defect 1 passes straight through it. |
| 4 | **`axl_tcp_send()` (sync) burns its full timeout** (default 10 s, blocking) when another send is outstanding: its ephemeral loop pumps only `tcp4->Poll()` while promotion is driven from a different, non-running loop. Previously it failed fast. Untested. |
| 5 | **`arm_failed` never tells anyone.** It sets `send_broken` and relies on close to retire; only the NEXT submit learns the path died, so tokens can sit for the socket's lifetime. |
| 6 | **The WS TLS ciphertext leak is NOT fixed** (one frame per teardown-mid-send). A revision of `ws_outq_clear`'s comment claimed it was; corrected. It needs defect 1 fixed first. |

Two comments asserted things that were not true and have been corrected: the 9p
one claimed `tcp_close_impl` has a re-entry guard (it does not — §6e adds one),
and the WS one claimed the leak was fixed.

### 6b-later. Smaller items — all closed by §6e

- ~~**Close re-entry guard.**~~ `tcp_close_impl` now has one: `sock->closed`,
  set at the top of teardown. A socket is torn down once.
- ~~**Promote-next-on-failure.**~~ A failed arm retires that token with
  `AXL_ERR` and the promote loop moves to the next one. Safe now that the
  callback is queued rather than called.
- ~~**The active half of close-flush is unpinned.**~~
  `test_tcp_send_async_flush_on_close` asserts both halves, and
  `test_tcp_send_close_from_send_callback` pins the promoted-then-closed case
  the old test could not reach.

### 6e. IMPLEMENTED — send callbacks are deferred (EDK2 SIGNAL_TOKEN)

Retirement is now "move the token to a done list and ask the loop to drain it",
via `axl_defer` — whose queue `axl_loop_next_event` drains at the TOP of its
next iteration, before it waits on or dispatches anything else. So the
completion dispatched in iteration N is reported in iteration N+1, with nothing
in between, and no consumer code ever runs inside the transport's call stack.

What that let the implementation delete or straighten out:

| before | now |
|---|---|
| "promote before signalling", with a comment explaining which UAF it dodges | retire, then promote; ordering is no longer load-bearing |
| `send_retiring` bars promotion during teardown | `closed` — one flag, set once, also refusing new sends/receives on a torn-down socket |
| close cancels the ACTIVE send's token and drops its callback | close retires the active send too, `AXL_CANCELLED`, like every other accepted send |
| `send_broken` parks the whole queue after one failed arm | the failed token is retired `AXL_ERR` and the next one is tried |
| cancel source per-socket, re-armed at promotion (with its own failure path) | one cancel source per token, armed at accept, dropped at retirement |
| the immediate path and the queued path arm sends separately | every send is enqueued, then `tcp_send_promote` arms it — one path |

Four structural points worth keeping:

1. **Every send owns a token, the active one included.** That is what makes
   retirement allocation-free: the only `axl_malloc` on the path is in
   `axl_tcp_send_async`, where a failure is still the caller's to see as
   `AXL_ERR`. Retiring cannot fail, which matters because a retirement that
   failed would strand a caller forever.
2. **The delivery is scheduled PER TOKEN, on the token's own loop** — not once
   per socket. A shared per-socket schedule is wrong the moment two callers use
   two loops, which is not hypothetical: `axl_tcp_send` submits from an
   ephemeral loop of its own. A shared handle latches onto whichever loop
   retired first, so the second caller's callback either runs on the first
   caller's loop (where a callback that closes the socket frees it under the
   other caller) or never runs at all if that loop has stopped. Review caught
   both; per-token scheduling removes the question. It costs nothing in the
   common case — a socket rarely has more than one callback owing.
3. **The delivery detaches its tokens before walking them** and never
   dereferences `sock` afterwards — it only passes it to callbacks. A close
   from inside a callback therefore cannot pull the walk out from under itself,
   and `finalize_sock` defers the `axl_free` (recording `free_deferred`) so the
   remaining callbacks are not handed a dangling socket.
4. **Close delivers synchronously**, unlike every other retirement, because the
   caller's loop may be freed the moment `axl_tcp_close` returns —
   `test_tcp_send_async_flush_on_close` does exactly that. It PARKS its tokens
   rather than retiring them, so the close path does not schedule a delivery it
   is about to perform itself.

Known bound, deliberately not engineered around: `AXL_DEFER_BUF_SIZE` holds
~42 entries per loop, shared with every other user of `axl_defer`. A burst
wider than that (a WS broadcast to many clients, each retiring a send in the
same iteration) fills the ring, and each such token falls back to a one-shot
timer source; if the source table is full too, the token stays parked until the
next event on that socket or its close. The callback is late, never lost, and
each miss is logged. If it ever bites, the fix is a bigger ring.

Not covered by a test, and stated rather than papered over: the arm-failure
path (defect 5). Reaching it needs the firmware to refuse a `Transmit` or the
loop to refuse a source, neither of which is reachable through the public API
without fault injection.

### 6f. FIXED — defect 4, the sync wrapper behind another send

`axl_tcp_send` (sync) builds an ephemeral loop and runs it. If another caller's
send is already active, promotion is driven by the completion source on the
OTHER loop, which is not running — so the queued token never starts and the
wrapper burned its whole timeout (10 s by default) before returning. Deferring
the callbacks does not address this one: it is a liveness problem, not an
ordering one.

**`axl_tcp_send` now refuses instead**, immediately, with `AXL_ERR` — the
behaviour it had before the queue, and what §7 means by "the sync wrapper must
opt OUT of queueing (it has no way to outlive its own call)". This is not the
transport refusing a send: `axl_tcp_send_async` still accepts every send (§3.2).
It is a synchronous shell declining a job it cannot finish inside its own call.

The alternative — have the wrapper PUMP the foreign send's completion from its
own loop, so the queue actually drains — was written out and rejected. The
mechanics do work: watch the same tx event on this loop, run `on_send_complete`
from it, and (because retirement schedules the callback on the token's OWN
loop) the foreign caller's callback correctly waits for the foreign caller's
loop rather than running inside `axl_tcp_send`. What is left is a judgement:

- It gives the synchronous shell a second job — driving another loop's
  transport completions — which is new cross-loop coupling in the one part of
  AxlTcp the tree is moving away from, and it must be armed unconditionally
  (the existing Poll tick only arms at a raised TPL).
- The gain is narrow. Two writers on one socket where one of them is
  synchronous is a shape the async API already serves properly, and §7's
  reading of the sync wrapper — "it has no way to outlive its own call" —
  points at declining the job rather than growing machinery to do it.

Revisit this if a real consumer needs it; the refusal is a return value, not a
one-way door.

`test_tcp_send_sync_behind_async` pins the refusal AND its timing: a status
assertion alone passes just as well when the timeout is burned to the last
millisecond.

## 7. Open questions

- ~~Does any consumer depend on `axl_tcp_send_async` returning `AXL_BUSY`
  today in a way that changing its meaning breaks?~~ **ANSWERED 2026-08-12:
  YES, and it is in this library, not a consumer repo.**

  `axl_tcp_send` — the SYNC wrapper in `axl-tcp-sync.c` — builds a stack
  `SyncResult r`, an ephemeral `AxlLoop`, and an `AxlCancellable`, passes
  `&r` / `loop` / `cancel` to `axl_tcp_send_async`, runs the loop, and then
  frees all three on return. It is safe today ONLY because a send already in
  flight returns `AXL_BUSY`, which its `!= AXL_OK` test turns into an
  immediate `AXL_ERR` — nothing is left queued.

  Under a queue that accepts instead, the submit returns `AXL_OK`, the wrapper
  waits on a send that is not active, times out, and frees the cancellable, the
  loop and the stack frame — leaving a queued token holding all three. The
  later promotion dereferences the freed cancellable and calls back into a dead
  stack frame. A use-after-free introduced by the queue, in the same file.

  So the port cannot simply change the meaning of the return value: the sync
  wrapper must opt OUT of queueing (it has no way to outlive its own call), or
  the queue must reject tokens whose lifetime is bounded by the caller's stack.
  Deciding which is a prerequisite for step 1, not a follow-up — the same class
  of "who owns the buffer until the callback" question §3.1 settles for data,
  applied to the callback CONTEXT.

  Other known handlers, unchanged: `axl-http-ws.c` branches on `AXL_BUSY`
  explicitly, `axl_tls_write_async` returns it upward. Both are slated for
  deletion here. Consumer repos still to be checked.
- **Why queue at all, when `EFI_TCP4` already has a transmit queue?** Raised
  2026-08-12 and worth keeping open, because §2's answer is weaker than it
  reads.

  The spec calls `Transmit` *"Queue outgoing data into the transmit queue"*
  (§1). The one-send-in-flight limit is OURS: `struct AxlTcp` holds a single
  `EFI_TCP4_IO_TOKEN tx_token`. So there were two fixes available — submit N
  EFI tokens and let the firmware queue them, or keep one token and queue
  above it. We took the second.

  §2's stated reason is that the spec does not guarantee the COMPLETION ORDER
  of multiple outstanding tokens, citing this tree's own scar from
  generalising `ArmPsciMpServicesDxe`'s `EFI_TIMEOUT` behaviour. Fair. But §2
  also leans on EDK2 as precedent, and that part does not transfer cleanly:
  EDK2's socket layer queues above *TcpDxe's internal engine*, which is not a
  queue, whereas we sit above `EFI_TCP4`, which is. The structural note in §2
  acknowledges the difference and then treats it as "the same shape".

  The strongest argument for our own queue is one §2 does not make: the
  newlib/POSIX move (§3.7). There is no firmware queue under POSIX — a
  non-blocking `send()` returns partial or `EAGAIN` and the caller queues — so
  this is the piece that survives, and `remaining` already exists for the
  partial-send case.

  What is NOT established: that multiple outstanding tokens actually misbehave.
  That is spec-based caution, never measured. The bounded spike is to submit 4
  concurrent `Transmit`s on one socket under OVMF and record completion order
  and status. Worth doing before anyone asserts the firmware queue is unusable
  — and worth doing anyway, since a "yes, it works" would simplify the POSIX
  layer's EFI sibling if that ever needs to coexist.

- Does the queue need a depth cap? It is unbounded (§3.2), as EDK2's is. A
  pathological caller queueing many tiny sends allocates one `AxlTcpToken`
  each; `TCP_SEND_QUEUE_WARN_BYTES` makes that visible but does not bound it.
- Should `AxlUdp` get the same treatment? It has the same single-token shape.
  Out of scope here; noted so the answer is deliberate.
