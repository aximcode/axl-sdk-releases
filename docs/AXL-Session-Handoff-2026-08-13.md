# Handoff — 2026-08-13: TCP queue landed, v3.2.0 shipped, C went bare-metal

> Self-contained. Everything here was measured on this tree on this date.
> `main` is at the work described below and is pushed.

---

## 0. Where to start

**Next task: verify CI is green, then pick up §6.** Nothing is half-finished in
the working tree — every change described here is committed and pushed, and the
local gates are green. The one thing in flight is a CI run validating the last
fix (§5).

Order after that: §6.1 (the §7 TCP spike, small and bounded) → §6.2 (picolibc
measurement) → §6.3 (queue steps 5-6).

---

## 1. What shipped

| | |
|---|---|
| `217c7e88` | AxlTcp send callbacks are DEFERRED (EDK2 `SIGNAL_TOKEN` port) |
| `ca0a54d7` | sync `axl_tcp_send` declines to queue; per-token delivery |
| `7214a63a` | queue handoff marked done |
| **`v3.2.0`** | **released** — `axl-sdk-releases`, all 8 assets, CI green, Docs green |
| `2a492564` | CI/release-speed design doc |
| `0d43f34` (agt) | AGT CI-cost handoff |
| `c729c6de` | `time()` + `NDEBUG` fixes (prerequisites for the flip) |
| `0bf6ed51` | **C compiles bare-metal on both arches; `include/compat/` deleted** |
| `af60cbd6` | x86_64-elf toolchain published as a release artifact |
| `47e52f84` | CI provisions the toolchains |

## 2. The TCP queue work (DONE — design §6e/§6f)

All six §6b defects are fixed. The mechanism: **retire a send by QUEUEING its
callback (`axl_defer`), never by calling it.** With no consumer code inside the
transport's call stack, the promote/signal ordering that three rounds of
patches fought over stops existing.

Consequences worth carrying:

- `axl_tcp_close` now retires the ACTIVE send too, `AXL_CANCELLED`. That also
  fixed the WS TLS ciphertext leak (one frame per teardown-mid-send).
- `tcp_close_impl` has a real re-entry guard (`sock->closed`); closing twice is
  a no-op and new sends/receives on a closed socket are refused.
- Every send owns a token, so retirement is allocation-free — the only
  `axl_malloc` is in `axl_tcp_send_async`, where a failure is the caller's to
  see.
- Delivery is scheduled PER TOKEN on its own submitter's loop. A per-socket
  schedule is wrong the moment two loops are involved, which `axl_tcp_send`'s
  ephemeral loop does routinely.
- `axl_tcp_send` (sync) REFUSES behind another caller's send rather than
  burning its 10 s timeout.

**Two review lessons, both now memories:** a per-SOCKET schedule for a
per-TOKEN event is wrong with two loops; and making teardown fire a callback it
never fired before makes previously INERT paths reachable (Ctrl-C left a token
pointing at a dead stack frame — harmless only while close stayed silent).

**Corrected in the design doc:** §3.7's claim that `AxlTcp` would be
reimplemented over POSIX sockets was withdrawn at all four sites. Newlib ships
no sockets; neither substrate doc mentions networking; and the layering
inversion is ROADMAP's deliberate, conditional revisit. `remaining` is
justified today as the live chunk cursor, not by a future partial `send()`.

## 3. v3.2.0 (SHIPPED)

Tag `v3.2.0` at `55fe5e8a`. What the cut exposed, all fixed:

- **`cut-release.sh`'s CI gate allowed 30 min**; CI's QEMU job runs the whole
  145-test suite on a 2-core runner and took ~50. It timed out on a run that
  passed, and the cut finished with `--resume`. Ceiling is 75 min now.
- **`gh auth status` exits non-zero if ANY configured account has a stale
  token** — one dead login blocked the release precondition. It asks
  `gh api user` now.
- **Doxygen version skew** — CI takes ubuntu's 1.9.8, this box has 1.13.2, and
  1.13 resolved two link targets 1.9.8 could not, so the local zero-warning
  gate was clean while the tagged Docs run failed. `scripts/build-docs.sh`
  carries the podman one-liner that reproduces CI's version.
- The CHANGELOG's `Unreleased` section was **~250 commits behind** and was
  swept by track before the cut.

## 4. The libc substrate (§4.1, §4.1b, §4.1c all DONE)

**§4.1 ANSWERED: newlib's printf reintroduces the Log→Data cycle. `AxlFormat`
stays.** Even the INTEGER-ONLY `vsniprintf` pulls `mallocr`/`freer`/`reallocr`,
the FILE machinery and `_impure_ptr`: 21.5 KB across 25 archive members against
`AxlFormat`'s 6,708 bytes and zero data. The general `vsnprintf` is 53.8 KB
across 47.

**§4.1b DONE: C compiles bare-metal on both arches; `include/compat/` is
deleted.** The whole cost was two fixes — `__assert_func` (avoided by keeping
sdefl's asserts dead with `NDEBUG`, as `compat/assert.h` did) and the `time()`
signature. AXL's own code never used the shims: across `src/` and `include/` it
includes only `stddef`, `stdint`, `stdbool`, `stdarg`. compat was always a
third-party shim.

**§4.1c DONE: the toolchain is published.**
`toolchain-x86_64-elf-14.3.0` on `aximcode/axl-sdk-releases` — 55 MB stripped
tarball, three upstream source archives, `SHA256SUMS`, `TOOLCHAIN-SOURCES.md`.
1.5 GB as built → 235 MB stripped → 55 MB packed (`cc1` alone was 326 MB of
debug info). URL + SHA256 in `axl-toolchains.conf`; `install-toolchain.sh x64`
is download-and-verify with the source build as fallback.

GPL: binaries and corresponding source in the SAME release (§6(d)), recipe in
git building unmodified upstream with no patches, three-year written offer in
the notes.

## 5. In flight, and the ONE thing to check first

**CI run `31672194387`** is the one to check. Its two build jobs are already
GREEN, which is what proves the toolchain provisioning; the integration job
(~50 min) and clang-tidy were still running when the session ended.

```sh
gh run view 31672194387 --json status,conclusion,jobs \
  -q '"\(.status) \(.conclusion)", (.jobs[] | "  \(.conclusion) \(.name)")'
```

CI took three attempts to get here, and both failures were worth the trip:

1. `31670922519` — clang-tidy red. `check-nx-compat` and `check-bss-clear`
   PRODUCE images, so the lint job compiles C and needs the cross too; and
   `install-toolchain.sh` called `sudo` unconditionally, which a root container
   with no sudo installed cannot do (`47e52f84`).
2. `31671967651` — gcc x64 and integration red **with a cache that "restored
   successfully"**. All jobs shared one cache key while each installs a
   DIFFERENT set, so whichever saved first won and the others got a hit holding
   the wrong toolchain, skipped their install on `cache-hit == 'true'`, and
   died on a compiler that was never there (`501c76dd`). A cache hit that
   satisfies the guard while omitting the payload is worse than a miss.

If clang-tidy is still red, the remaining suspects are that container's apt set
(it now installs `curl` + `xz-utils`) or its cache path. This is CI-only wiring
with no local equivalent — a dispatch-per-attempt loop.

**Not yet exercised end to end:** `install-toolchain.sh x64` replacing a real
`/opt` tree. CI exercised download+verify+extract (same code), but nobody has
watched it overwrite an existing install.

## 6. What is next, in order

### 6.1 The §7 spike (small, bounded, unblocks a real question)

`docs/AXL-Tcp-Queue-Design.md` §7: **why queue at all, when
`EFI_TCP4.Transmit` already queues?** NOT established — that multiple
outstanding tokens misbehave is spec-based caution, never measured. The spike:
submit 4 concurrent `Transmit`s on one socket under OVMF, record completion
order and status. If the firmware handles them cleanly, the one-token limit is
ours to remove and the queue's justification narrows to the §1a defect it
fixed.

### 6.2 picolibc vs AxlFormat (decides how far the substrate goes)

Design §4b, added this session. The tree does NOT use newlib's implementations
— we link `-nostdlib` and AXL defines all TWELVE standard-named symbols. Before
recommending any libc, measure picolibc's printf against `AxlFormat`'s 6,708
bytes on the §4.1 rig (probe + `-nostdlib … -lc` + read the map). picolibc's
`__thread` errno is the thing to check second, since UEFI has no TLS.

Also open there: whether to NAME the twelve-function libc seam (worth it) or
split it into a separate archive (ceremony at this size), and why "POSIX first"
is contradicted by this tree twice over.

### 6.3 Queue steps 5-6

Receive queue, and MEASURE the watermarks rather than inheriting EDK2's. Design
§6 steps 5-6.

## 7. Traps hit this session (all cost real time)

- **NEVER run `make` while `run-integration.sh` is running.** The runner
  pre-builds `AXL_TLS=1`; a plain `make` toggles the build-state signature,
  WIPES the objects and archives, and rebuilds under the suite. Symptom:
  unrelated tests failing with
  `ld: libaxl.a: error adding symbols: file format not recognized`. The results
  are worthless, not merely noisy. `/code-review` runs `make` too.
- **Editing `include/axl/` after staging re-breaks the cxx tests.**
  `test-cxx-{hosted,streams}-qemu.sh` assert staged headers match `include/axl`.
  Re-run `./scripts/install.sh --arch all --cpp` after the last header edit.
- **Overriding `CFLAGS` on the make command line drops the TLS block's
  appends**, and `-DMBEDTLS_CONFIG_FILE='<axl-mbedtls-config.h>'` must keep its
  inner quotes or `/bin/sh` reads `<...>` as a redirect. Cost a whole spike run
  that reported "0 errors" having compiled NOTHING. Always check objects exist.
- **Backticks in a `git commit -m "..."` are command-substituted by bash.**
  Mangled two commit messages this session (words silently deleted, and a
  visible `tar:` error). Use `-F <file>` for anything with backticks.
- **A clean compile is not a working image, and a clean *link* is not either.**
  The compat spikes compiled 271/271 with zero errors and then failed at the
  LINK on `__assert_func`.

## 8. Repo state

- `main` pushed; working tree clean apart from Mike's untracked notes
  (`SCRATCH.txt`, several `docs/AXL-*.md` drafts).
- Worktree `.claude/worktrees/tcp-token-queue` REMOVED (merged);
  branch `worktree-tcp-token-queue` still exists locally.
- Local gates green at HEAD: `verify.sh` ALL GREEN both arches (10393),
  integration 145/0, hosted-headers 9/9, cxx-hosted 120/0, cxx-streams 78/0.
- Toolchains required to build: `/opt/x86_64-elf-gcc-14.3.0` and
  `/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf`, both installed on
  this box. A fresh machine runs `./scripts/install-toolchain.sh all` — both
  are downloads now.
