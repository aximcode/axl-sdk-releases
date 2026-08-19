# Handoff — 2026-08-18: v4.2.0 shipped, CI moved to our own runner

> Self-contained. Every number was measured on this tree on this date.
> **Supersedes `docs/AXL-Session-Handoff-2026-08-17-3.md`**, whose §6 and §7
> are both closed (§6 shipped, §7 REFUTED — see `AXL-Cxx-Unwinder-Design.md`
> §U6).
>
> Working tree clean apart from Mike's untracked `SCRATCH.txt` and several
> `docs/AXL-*.md` drafts — **do not commit those, and never `git add -A`.**

---

## 0. DONE — all three tasks shipped

All three were small, specified, and independent, and all three are now done
and verified on our own runner: **1 and 2** in `d39f871b`, **3** in the commit
that follows it. Reasoning for all three is in
`AXL-CI-Release-Speed-Design.md` §11.5 and §11.6. The task statements are kept
below with what actually happened, because two of them turned out to be subtler
than they read.

**Nothing in §0 is outstanding. Start from §1.**

1. ~~**Sequence the CI jobs.**~~ **DONE.** `.github/workflows/ci.yml` had **no
   `needs:`**, so `build`, `integration` and `lint` were all eligible at once. A
   broken build did not stop the 10-minute integration job; the run failed for a
   reason already known. `needs: build` is now on `integration` AND on `lint`.
   `fail-fast: false` is untouched, and `needs:` waits for both matrix legs, so
   x64 and aa64 still each report. Reasoning, including why `integration` is
   deliberately NOT chained behind `lint`, is in
   `AXL-CI-Release-Speed-Design.md` §11.5.

2. ~~**Trim the container apt lists to what HOST tooling actually needs.**~~
   **DONE** — `g++` and `sudo` gone, `gcc-aarch64-linux-gnu` +
   `binutils-aarch64-linux-gnu` gone (22 packages, 43 MB on every aa64 build),
   `binutils` kept only where something invokes it unprefixed, `gcc` kept
   everywhere with a comment at each site saying why. §11.5 again.

   The original statement of the task, which held up: target
   code is 100% bare-metal toolchains (`/opt/x86_64-elf-gcc-14.3.0-axl3`,
   `/opt/arm-gnu-toolchain-14.3.rel1-…`), enforced by `check-flag-parity` and
   the `CC`/`CXX` build-state signature. But `HOSTCC` (plain `gcc`) is
   genuinely required for host-side tools:

   ```
   Makefile:83     HOSTCC ?= gcc
   Makefile:2160   $(HOSTCC) -Wall -O2 -o $@ $<        # pe-set-debug
   Makefile:2178   $(HOSTCC) -Wall -O2 -DAXL_HOSTED …
   ```

   `pe-set-debug` stamps the PE debug directory on the build machine and cannot
   be built with a bare-metal cross. So `gcc` stays; **`g++` and `binutils`
   were added to the container lists without checking and are probably
   unnecessary.** Verify and trim, and leave a comment saying why `gcc`
   remains, or someone will delete it and break `pe-set-debug`.

3. ~~**A markdown-only commit runs the full QEMU suite.**~~ **DONE**, and it
   shipped NARROWER than written here. `push: [main]` had no `paths-ignore`, so
   `docs/**` and `*.md` commits paid the whole ~10-minute integration run — this
   handoff document's own commits triggered three. Free in dollars, not free in
   noise: it keeps the runner busy, cycles QEMU guests in `top` while nothing
   real is being tested, and lets a genuinely red CI hide among doc-triggered
   runs.

   The filter that shipped is one line, `paths-ignore: ['docs/**.md']`, NOT the
   two-line version sketched here:

   ```yaml
   push:
     branches: [main]
     paths-ignore:
       - '**/*.md'      # WRONG -- see below
       - 'docs/**'      # WRONG -- see below
   ```

   **The subtlety, as written:** do NOT blanket-exclude `docs/sphinx/**`.
   `build-docs.sh` is a real zero-warning gate and `check-docs` fails on a
   public-header/rst mismatch, so an `.rst` change must still be verified — and
   `docs.yml` must keep firing for docs changes regardless. Exclude prose, not
   the doc build's inputs. Getting this wrong silently disables a gate, which is
   the failure mode this tree has paid for repeatedly.

   **The subtlety it missed:** `'**/*.md'` also takes the root `README.md`, and
   `test-toolchain-variant.sh` **asserts on its content** ("README documents no
   `make CROSS=` build command"). That is an integration test, not a doc build,
   and it would have gone unrun on exactly the commits that can break it. Same
   for the 33 `src/*/README.md` that Sphinx `.. include::`s. The generalisation
   worth keeping: *prose is not a synonym for "nothing depends on it"* — a test
   can assert on documentation, and this repo has one that does. Full reasoning
   and the one residual cost (`check-nul`'s view of `docs/*.md` is delayed to
   the next non-prose push, not lost) are in
   `AXL-CI-Release-Speed-Design.md` §11.6.

Everything else below is context.

---

## 1. What shipped

**v4.2.0 is released** — tag at `08d8fbd1`, `release.yml` published 8 artifacts
(`axl-sdk.deb/.rpm`, `axl-sdk-tools-{x64,aa64}.tar.gz`,
`axl-sdk-host-tools.{deb,rpm,tar.gz}`, `SHA256SUMS`), docs site current. AGT
confirmed `v4.2.0` as its pin and asked for nothing else; its 52 commits are
unblocked.

Also landed: `axl::preorder_pruned` (the C++ pruning range AGT asked for), three
crash-handler bug fixes, the `--ci-doxygen` gate, the release stamp,
`axl_image_watch_loads`, and the §6 unwind-table flag.

---

## 2. CI now runs on our own hardware

| | hosted | self-hosted |
|---|--:|--:|
| QEMU integration | ~50 min, 1 worker | **9m20s**, 6 workers |
| billable | ~42 min | **0** |

The serialisation was never a repo defect: `run-integration.sh` picks
`nproc - 2`, which is **1** on a 2-core hosted runner and **6** on this 8-core
box. Nothing in the repo changed to get the speedup.

**Policy inverted, deliberately.** CI was dispatch-only to save Actions
minutes. That policy's bill came due: CI had last been green **2026-08-13**,
~40 commits earlier, red in three places nobody had seen — two of which a dev
box *structurally cannot* catch. CI now runs on `push: [main]` with
`cancel-in-progress: true`. Recorded in `AXL-CI-Release-Speed-Design.md` §11.

**Actions minutes were never exhausted** — 604 used in August against ~2000.
The billing endpoint MOVED (410, not a permissions error); the working one is
`gh api /organizations/aximcode/settings/billing/usage`.

### Job layout, and why it is mixed

| job | where | container |
|---|---|---|
| `build` (gcc x64/aa64) | self-hosted | `ubuntu:24.04` |
| `lint` (clang-tidy) | self-hosted | `ubuntu:26.04` |
| `docs.yml` | self-hosted | `ubuntu:24.04` |
| **`integration` (QEMU)** | self-hosted | **NONE — on the host** |

Containerized, the integration suite went **12 passed / 134 failed**: guests
produced no serial output at ~1.05 peak cores. On the host it is **160/0**
locally and **145/1** in CI. That job's coupling to the host — KVM, swtpm,
virtiofsd, tun networking — is the point of the job, so an image buys least
there. **The container failure was never root-caused.**

Consequence: a host registered for the `axl-qemu` label **must already be a
working dev box**, because that job installs nothing. Its apt and `/dev/kvm`
steps are guarded `if: runner.environment == 'github-hosted'`. Note this host
is **AlmaLinux 10 (dnf)** — it has no `apt-get` at all, so unguarded the step
would error rather than silently install.

**CI runs `--arch X64` integration only. That is a DELIBERATE prior decision of
Mike's, not a gap.** The per-arch build directories are already separate
(`out/native-x64`, `out/native-aa64`, plus `-release` variants), so nothing
blocks running both if that decision is ever revisited.

---

## 3. The runner, and how to add another host

Registered as `axl-qemu-1`, online, service enabled (survives reboot).
Procedure for a new host is in `docs/RELEASING.md` → "Registering a
self-hosted runner". Same **label** `axl-qemu`, unique **name**; GitHub sends
each job to whichever is idle.

### Docker, not podman — measured

Podman's compat socket ran container jobs after two host fixes, but the third
divergence is disqualifying:

| symptom | cause |
|---|---|
| `statfs /var/run/docker.sock: permission denied` | runner bind-mounts the socket and stats it as its own user; podman's rootful socket is `root:root` |
| still denied after `SocketGroup=docker` | `/run/podman` is itself `0700` — group on the socket is useless without directory traversal |
| `statfs …/_work/_actions: no such file` | **Docker auto-creates a missing bind-mount source; podman does not**, and the runner only populates `_actions` for a job that uses an action — so any job without a `uses:` step cannot start a container |

`docker-ce` 29.7.2 is installed (publishes for el10). Proven both ways: a
container job with a `uses:` step and one without.

### Host changes made (all persistent)

- `docker` group created; Mike added (root-equivalent socket access — the
  standard Docker trade-off)
- `docker-ce`, `docker-ce-cli`, `containerd.io`, `docker-buildx-plugin`
  installed; `docker.service` enabled
- `podman-docker` **removed**; `podman.socket` **disabled**
- `loginctl enable-linger` for Mike
- **Vestigial from the podman attempt, safe to delete:**
  `/etc/systemd/system/podman.socket.d/override.conf` and
  `/etc/tmpfiles.d/podman-docker-sock.conf`

---

## 4. Bugs fixed that a warm dev box could never see

- **A clean checkout could not link.** `$(PORTING_OBJS)` go on every link since
  P4 but were on no target's prerequisite list. Fixed by splitting the two
  roles of `LINK_CRT0`: `LINK_CRT0_CMD` for the recipe (5 sites),
  `LINK_CRT0` for prerequisites (77 targets). Appending naively double-links
  them and multiply-defines.
- **Docs broke under CI's doxygen** while the local gate read clean — a
  markdown code span straddling a line break, which 1.9.8 does not rejoin and
  1.13 does. Shipped broken docs **twice** (v3.2.0, v4.2.0). Now gated by
  `scripts/build-docs.sh --ci-doxygen`, which runs CI's version in a container.
- **`clang-tidy` was red before this session.** Its cache stored **only x64**,
  and the install step runs only on a cache *miss*, so `install-toolchain.sh
  all` never executed and `check-ctors`' aa64 half had no compiler. The step
  reported `skipped`, which reads like "not needed" rather than "wrong thing
  restored".
- **`test-pkg-deps-minimal.sh`**: CI staged `--arch x64` only; the test mounts
  that stage and builds **both** arches in a minimal container.
- **`test-toolchain-variant.sh` / `axl-cc`**: the `LIB_DIR` existence check ran
  ~300 lines before the toolchain refusal, so `AXL_TOOLCHAIN=cross` with a
  forgotten locator said "no SDK libraries" instead of naming `AXL_X64_GCC`.
  It passed locally **only because an untracked `<repo>/lib/axl/x64` leftover
  existed** — `/lib/` is gitignored with zero tracked files.
- **Four implicit host dependencies** the empty containers surfaced: `git`
  (needed *before* checkout, or the action falls back to a REST download that
  cannot do submodules), `xz-utils`, `make`, and the aa64 toolchain in `lint`.

---

## 5. Performance findings

- **The CI builds had no `-j`.** ~250 objects compiled one at a time while
  seven cores idled. Now `make -j"$(nproc)"`.
- **32 `est=` values had drifted**, badly in both directions — `cxx-exceptions`
  declared 170s and measured 28s; `console-device` declared 250s and measured
  379s. `run-integration.sh` packs longest-processing-time-first, so wrong
  estimates schedule long tests last (stranding workers) and short-declared-long
  ones into the ramp. Corrected where drift exceeded 40% AND 10s, set to
  `measured * 1.3 + 2`.
- **Measured parallelism**: ~20s ramp at 2 guests, then a steady 7–10 (≈5–6
  real guests plus `timeout` wrappers) at load ~5–6 on 8 cores. `JOBS=6` is
  correct; the *scheduling order* was the problem.
- The unit-test phase (`test-axl.sh`) is serial **by design** — one guest for
  all unit binaries under one timeout.

---

## 6. Traps this session paid for

1. **A status is not a measurement.** "docs is progressing" came from one
   `in_progress` poll; it had been stuck 18 minutes.
2. **`pgrep -f <pattern>` matches its own command line.** Reported a finished
   suite as running. Use a bracket: `pgrep -af "run-integra[t]ion"`.
3. **`nohup … &` returns 0 immediately**, and the harness reports "completed
   exit code 0" for the *wrapper*. Write a status file the real command
   creates, and read that.
4. **A trailing `grep` in a pipeline eats the exit code** — made `sabotage.sh`
   report NOT DETECTED when the test had caught it.
5. **UTC vs local.** Read `19:43Z` as evening; Mike is Central (2:43 PM).
6. **Pinning to arbitrary versions moves skew rather than removing it.**
   sphinx 7.4.7 rejected duplicate declarations 9.1.0 accepts. Pin to what the
   local gate is verified against: sphinx 9.1.0, breathe 4.36.0,
   sphinx-rtd-theme 3.1.0, myst-parser 5.0.0.
7. **A transient failure is not a systemic one.** A 95-minute `release.yml`
   hang looked like exhausted minutes; one re-run passed. Probe with one
   command before redesigning.
8. **Context estimates**: I repeatedly claimed to be out of context at 26%,
   24%, 22%, 18%. There is no counter — do not guess, and do not defer work on
   a guess.
