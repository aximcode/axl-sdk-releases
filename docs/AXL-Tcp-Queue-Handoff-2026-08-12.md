# Handoff — AxlTcp send-token queue, and everything else from 2026-08-12

> **STATUS: the send work is DONE and merged.** All six defects are fixed —
> `217c7e88` (defer the callbacks, EDK2 SIGNAL_TOKEN) and `ca0a54d7` (the sync
> send declines to queue; deliver each callback on its own submitter's loop).
> `verify.sh` ALL GREEN both arches (10393), integration 145/0 with AXL_TLS=1.
> The live record of the design and its decisions is
> `docs/AXL-Tcp-Queue-Design.md` §6e/§6f — read that, not §4 below.
>
> What is still worth reading here: **§6, the traps** (they cost real time and
> none of them are fixed by code), and **§7, the order of work** — of which
> only step 1 is done.

---

## 0. Where to start

**DONE: make send callbacks DEFERRED instead of inline** (§4, kept for the
reasoning). Remaining order: cut v3.2.0 (needs Mike's approval) → gcc/newlib
toolchain → queue steps 5-6. See §7.

---

## 1. What was wrong, and what got fixed

The session began on a different problem — a leak blocking v3.2.0 — and each fix
exposed the next thing:

| Problem | Status |
|---|---|
| `test-jose-qemu.sh` leaked 5 allocations / 352 bytes | **FIXED** `22d6e25a`. A test discarded an owned `AxlPkKey`. |
| `ci.yml` red since Jul 30, unnoticed (dispatch-only) | **FIXED** — gcc 13 rejects `-std=gnu23`; reverted to `gnu2x` (`e78aa2ce`), the SAME language mode. |
| `release.yml` had the same gcc break in 2 jobs | **FIXED** — a tag would have produced NO packages. |
| Container lint job: `git` exit 128 | **FIXED** `cd37e063` — root over a uid-1001 checkout; needed `safe.directory`. |
| `install.sh --arch x64 --cpp` demanded an AArch64 toolchain | **FIXED** `3bd537c9` — CI's integration job had NEVER passed that step. |
| CI wall-clock budget too tight | **FIXED** `316a256c` — env mismatch, not drift (50s local vs 96s CI, identical work). |
| `axl-test-loop.c` keypress flake | **FIXED** `0454967a` — phase-pinned. The symptom was never reproduced in 26 runs; fixed by construction, not by turning a red test green. |
| **§1a: ~1-in-2 concurrent-handshake connection drop** | **FIXED** `831e1cbf`. See §2. |

## 2. §1a — the real defect, and what actually fixed it

`test-https.sh`'s concurrent-handshake gate dropped **1 connection in 24 at
width 4**, failing ~50% of runs. Root cause, measured not inferred:

```
client: http_code:curl_rc = [000:56]        # 56 = failure receiving data
server: send_response: submit failed rc=-10 (AXL_BUSY=-10)
```

`axl_tls_write_async` refused whenever `axl_tcp_send_in_flight()` — usually the
handshake's own final flight, still pending when the peer's first request
arrived. Four submit sites in `axl-http-response.c` read that retryable
`AXL_BUSY` as fatal and reset a healthy connection. `0a9f81fe` had fixed the
identical `!= AXL_OK` bug in `handshake_flush_async` and missed these.

**Steps 2+3 fixed it, NOT step 1 (the queue).**

| build | gate |
|---|---|
| main / queue disabled | ~3 fail / 6 |
| step 1 only (queue) | 4 fail / 6 — **neutral, measured** |
| steps 1+2+3 | **0 fail / 6**, confirmed twice |

The rate is LOAD-DEPENDENT (33% on main, 50% in the worktree the same day) — run
any A/B in ONE session, never against an older number.

## 3. What the branch contains

Branch `worktree-tcp-token-queue`, worktree `.claude/worktrees/tcp-token-queue`.
Gates at the last commit: `verify.sh` ALL GREEN both arches (10388),
integration 145/0.

| commit | what |
|---|---|
| `41de5751` | Step 1 — EDK2 send-token queue, ported from `edk2-stable202511` (46548b1) |
| `831e1cbf` | Steps 2+3 — **the §1a fix** |
| `23c75ee0` | EDK2-exact (never refuse) + 7 review findings |
| `1092db99` | §7: "why queue when EFI_TCP4 already does?" recorded as open |
| `acc5a061` | §6b: six open defects + two false comments corrected |
| `328cbde4`, `6b930018`, `d118ad05`, `13c04d2f` | design decisions, in order |

Reference read on this host: `/home/mgosha/projects/edk2/NetworkPkg/TcpDxe/`
(`Socket.h` `SOCK_TOKEN`; `SockImpl.c` `SockProcessSndToken`, `SockDataSent`,
`SockCancelToken`, `SockConnFlush`; `SockInterface.c` `SockSend`). That checkout
is at the exact revision the design cites.

## 4. DONE — defer send callbacks (kept for the reasoning; see design §6e)

**Root cause of all six open defects (design §6b):** AXL invokes send callbacks
INLINE, so any callback can `axl_tcp_close` and free the socket mid-operation.
Every ordering is wrong under that constraint:

- promote AFTER the callback → UAF
- promote BEFORE it → a promotion failure fires a second callback, same UAF
- bar promotion during close → only helps if close starts first; in
  `complete → promote → cb → cb closes` the token is already ACTIVE and still
  loses its callback

EDK2 has none of this because `SIGNAL_TOKEN` is `gBS->SignalEvent` — it QUEUES
the notify (`SockImpl.h:23`), so no consumer code runs inside the transport's own
call stack. That is why its "signal then promote" is safe and ours is not.

**Port that properly: retire a send by SCHEDULING its callback on the loop.**
Promote-vs-signal ordering then stops mattering, close can retire the active send
safely, and re-entrancy disappears.

Things to get right, learned the hard way:

- `axl_loop_add_timeout(loop, 1, ...)` is the established deferral idiom
  (`axl-http-client-async.c:1091` — a 0 delay is rejected).
- Teardown callbacks must still fire BEFORE the loop dies.
  `test_tcp_send_async_flush_on_close` frees the loop right after
  `axl_tcp_close`, so a purely deferred flush would never run. Close must drain
  what it scheduled.
- `tcp_close_impl` runs MORE THAN ONCE on a socket whose teardown deferred
  (graceful / loop-deferred paths finalize later), so a function-scoped re-entry
  guard skips the real teardown — measured: `AxlTestNet` hung with no Results
  footer, starving every later binary in the boot.
- `test_tcp_send_async_flush_on_close` **does not pin what it claims** — it never
  promotes before closing, so defect 1 passes straight through it. Fix the test
  as part of the work.

## 5. Settled — do not re-litigate

- **Never refuse a send.** EDK2's `SockSend` buffers below low-water and returns
  `EFI_SUCCESS`; only the allocation fails. `axl_tcp_send_async` returns `AXL_OK`
  or `AXL_ERR`, never `AXL_BUSY`. This deleted the TLS floor, its seqno assert,
  three dead branches, and the WS pump's refusal branch. Accepted consequence:
  the queue is unbounded, as EDK2's is.
- **Cancel-while-queued is load-bearing** — it is what keeps `axl_tcp_send`'s
  stack-allocated `SyncResult` safe (design §3.6/§7).
- **The WS outbound queue STAYS.** It does two jobs; only serialisation was a
  workaround. Its lossy, bounded, frame-aware drop policy has no equivalent below
  it and is correct for a broadcast socket (design §6c).
- **`remaining` stays** even though EFI_TCP4 never partially retires a token — a
  partial POSIX `send()` does, and that is where AxlTcp is going (§3.7).
- **The layering revisit is DEFERRED** to the gcc/newlib track.

## 6. Traps hit this session — all cost real time

- **`verify.sh` does NOT build `AXL_TLS=1`.** It goes ALL GREEN while 16
  crypto/JOSE groups are skipped. It is not the release gate;
  `run-integration.sh` is.
- **A fresh worktree needs TWO setup steps** the suite does not do:
  `git submodule update --init --depth 1 deps/mbedtls` and
  `./scripts/install.sh --arch x64 --cpp`. `verify.sh` passes without either.
- **`sabotage.sh` restores the SOURCE, not artifacts STAGED from it.** A
  sabotaged `out/bin/axl-cc` survived its own restore and failed the NEXT run.
  `test-axl-cc-flags.sh` now guards this; sibling staged-artifact tests do not.
- **NEVER `git checkout --` to undo a sabotage.** It deleted ~300 lines of
  uncommitted work here. Recovery lived in `/tmp/axl-sabotage.*`. Commit WIP
  before any experiment that edits tracked files.
- **`pgrep -f` matches its own command line** — twice read as an orphaned QEMU.
- **Piping a gate masks its exit code.** `./scripts/verify.sh | tail` reports
  tail's status; capture the rc directly.

## 7. Order of work after the callback fix

1. ~~Merge the branch to `main`.~~ **DONE.**
2. **Cut v3.2.0** — `docs/RELEASING.md`, needs explicit approval. Held by
   DECISION, not a blocker: local gate green, CI's own breakages repaired.
3. **gcc/newlib toolchain work** — reimplements `AxlTcp` over POSIX/libc and
   settles the layering question.
4. **Queue steps 5-6** (receive queue, MEASURE the watermarks) — after the
   toolchain, since step 5 against a soon-reimplemented transport gets redone.

## 8. Open questions recorded, not answered

- **Why queue at all, when `EFI_TCP4.Transmit` already queues?** (design §7).
  The one-token limit is ours. §2's justification leans on EDK2 as precedent, but
  EDK2 queues above TcpDxe's internal engine, which is not a queue — we sit above
  `EFI_TCP4`, which is. The strongest argument for our own queue is the POSIX
  move (§3.7), which §2 never makes. NOT established: that multiple outstanding
  tokens misbehave. Bounded spike written down — 4 concurrent `Transmit`s on one
  socket under OVMF, recording completion order and status.
- Whether the unbounded queue wants a depth cap.
- `axl-macros.h:62` asserts "the project requires `-std=gnu2x` (gcc 13+)", but
  nothing enforces it and — measured — nothing needs it. Correct the comment or
  pin the dialect deliberately.
