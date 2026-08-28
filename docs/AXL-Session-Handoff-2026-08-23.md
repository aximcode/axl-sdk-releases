# Handoff — 2026-08-23: E1 and E2a shipped, E2b parked, 4.3.2 merged, lsacpi specced

> Self-contained. Everything below was measured or checked in-session; where a
> claim is inherited rather than verified, it says so.

## 0. START HERE — state, and what is actually open

**Tree:** HEAD `e444e24b`, VERSION **4.3.2**, on `main`, tracked files clean,
**1 commit unpushed** (`e444e24b`, the lsacpi spec — deliberately held for
review). Unit **10808** both arches. `verify.sh` **ALL GREEN** as of `86a58b06`.

47 commits this session. Untracked and NOT mine: `SCRATCH.txt` and six older
`docs/AXL-*.md` — they predate this session and are Mike's.

**Nothing is half-finished.** Two phases shipped, one parked with zero code, one
spec awaiting review.

| open | where | note |
|---|---|---|
| **`lsacpi` spec awaiting Mike's review** | `docs/superpowers/specs/2026-08-23-lsacpi-design.md` | The immediate next thing. On approval → write the L1-L4 plan. Unpushed on purpose |
| **E2b — vendor ref10** | `docs/superpowers/plans/2026-08-23-ed25519-e2b-ref10.md` | Planned in full, **PARKED 2026-08-23, zero code**. Nothing on a branch |
| E4's ADX field backend | spec §6b | Now an open question, not a plan — see §5 |
| AxlSsh P1 Task 6 | `plans/2026-08-21-axl-ssh-p1-transport.md` | Blocked on E2b — **or unblockable with an ECDSA host key**, see §6 |
| Two dead guards | `axl-http-server.c`, `sdk/examples/jose-demo.c` | `axl_tls_available()`/`axl_jose_available()` return true unconditionally. Docstrings say so; guards left. Mike's call |
| `lspci` slot columns | — | Discussed, deliberately out of the lsacpi spec's scope. Revisit after lsacpi runs on real hardware |
| CMake port | ROADMAP | PROPOSED, unchanged |

---

## 1. E1 — `AXL_CHECKSUM_SHA512` (SHIPPED)

`225bd940` · `b1ef3732` · `97a23b02` · `a3c96add` · `2e29670a` (+ plan/spec).

SHA-512 as a thin adapter over `mbedtls_sha512_*`. **§3 of the Ed25519 spec
reversed**: it had chosen to hand-write SHA-512 specifically to avoid the
`AXL_TLS` toggle, and that toggle no longer exists.

**Measured, so it does not need re-deriving:** mbedTLS `sha512.o` is 2,208 B and
drags only `mbedtls_platform_zeroize` (already ours). Our `axl-digest-sha256.o`
is 1,331 B for scale. **Size does not discriminate** — what does is that
wrapping adds no cryptographic code for the security-review gate. Final measured
pull-in: **2,301 B**, recorded in spec §10.

**The defect the plan surfaced.** `axl_hmac_new`'s only guard was
`axl_checksum_type_get_length(type) != 0`, which starts *accepting* SHA-512 the
moment the enum value exists — with `HMAC_BLOCK` hardcoded 64 against SHA-512's
real 128 and `HMAC_MAX_DIGEST` 32 against its 64, `hmac_finalize` would write
`hexstr[0..128]` into a `char[65]`. The enabling mechanism was
`axl_checksum_get_digest` reporting the FULL digest length while clamping the
copy — and `axl_hmac_get_digest` had the identical defect with its docstring
describing it outright. Both fixed; `*len` is now bytes **written**.

**Task order was the safety property:** buffers widened and a fail-closed
`hmac_block_size` landed BEFORE the enum value existed, so no commit ever
carried a reachable overflow.

**Also found:** mbedTLS 3.6.3 already ships an aa64 `FEAT_SHA512` kernel, but
its detection (`getauxval` / `sysctlbyname` / SIGILL) cannot work under UEFI and
the fallback `#warning`s and silently `#undef`s it. E4 therefore *supplies
detection* via `MBEDTLS_SHA512_PROCESS_ALT`, not a kernel.

---

## 2. E2a — the `AxlPkProvider` seam (SHIPPED, 19 commits)

`12df7c0f..8e22af80`. `AxlPkKey` used to **be** an `mbedtls_pk_context`; Ed25519
cannot be one. It is now a tagged union behind an 11-member vtable, carrying
only ECDSA and RSA. `axl-pk-verify.c` went **830 → ~340 lines**; the mbedTLS work
moved to `src/net/axl-pk-mbedtls.c`.

**No behaviour changed.** The final review proved it mechanically — normalised
the old file against the two new ones and diffed the line multisets; ten helpers
and both crypto cores moved byte-identical.

### 2a. What the contract-first review caught (Task 1, 3 rounds, zero implementation)

Five Critical at header-edit cost. Two would have been very expensive:

- **C3 — a fail-OPEN hazard.** A provider table keyed on `p->alg` dereferences an
  un-linked weak provider's **NULL address**; page 0 is mapped and readable under
  UEFI, so the read returns 0, which **is** `AXL_PK_ED25519`, selecting the absent
  provider and calling a function pointer at `0x8`. Fixed by deleting `.alg` so
  `_axl_pk_provider_for` must name each symbol and test its address.
- **C4 — no DER seat at the seam.** Without the four DER members, E2b would have
  to special-case inside `axl_pk_key_load_private`, re-creating the strong
  reference that pulls ref10's 30,720-byte table into every image and destroying
  §4's entire justification.
- **C5 was measured, not asserted:** gcc does **not** warn on an omitted
  designated initializer, so "every member must be non-NULL" was a rule enforced
  by hope with a call through address 0 as the failure mode. Inverted to
  optional-and-NULL-checked (the `AxlStreamOps` precedent).

### 2b. The weak seam works for a reason nobody wrote down

GCC **folds the `&_axl_pk_provider_ed25519 != NULL` test away** — the emitted x64
is a bare `mov GOT(%rip),%rax; ret`. What actually makes it fail-closed is that
the GOT slot carries a `GLOB_DAT` relocation which `src/crt0/axl-reloc.c:99`
does **not** apply (it applies only `RELATIVE`), leaving it zero on both arches.
A reviewer also checked the linker does not relax the weak GOT load into a
`lea`, which would make the address image-base-relative and therefore **non-NULL
— fail open**. `nm` showing `w` was necessary but could never have shown any of
this.

### 2c. The number that matters for E5

**The vtable costs a verify/sign-only consumer +8,976 B `.text`** (x64 release,
`12df7c0f` → HEAD), controlled against an `axl_pk_verify()`-only build that moved
−32 B. Roughly 2.2× the reviewer's estimate. `_axl_pk_provider_mbedtls` holds
strong references to all eleven operations, so `--gc-sections` can no longer
separate keygen and the DER writers from what a consumer calls.

**That is E5/AxlSsh's floor** — a resident driver that signs and verifies with a
persisted host key and never generates one carries RSA prime generation forever.
Recorded in spec §10 and the CHANGELOG. **This is the one number worth
revisiting before E5 relies on it**; the fix would be splitting the vtable so
keygen and the DER writers move to a separately-pulled struct.

### 2d. Invariants a future reader will need

- **The tag must identify the provider that ran `key_init`.** `key_alloc` sets a
  sentinel, so between allocation and classification the tag and the arm
  disagree — a construction path that frees **before** classifying must free
  through the provider it used, never `axl_pk_key_free()` (which resolves from
  the tag). Recorded in `axl-pk-provider.h`.
- The sentinel is now **`AXL_PK_ALG_UNCLASSIFIED` (`(AxlPkAlg)-1`)**, structurally
  unresolvable forever, rather than `AXL_PK_ED25519` — whose "fails closed"
  rationale expires the moment E2b links a provider.
- **`axl-pk-provider.h` is mbedTLS-free** and must stay so; it is the header
  E2b's ref10 TU includes. The mbedTLS-typed helpers live in
  `axl-crypto-internal.h`. (An earlier ruling of mine put them in the wrong
  place; the contract review caught it.)

---

## 3. E2b — planned, PARKED, zero code

`docs/superpowers/plans/2026-08-23-ed25519-e2b-ref10.md`, six tasks.

**Parked because its only consumer is E5, and AxlSsh is itself five phases** —
finishing E2b buys the ability to *start* a large project, not a working
feature. There is **nothing on a branch**; the plan and spec are documentation
and belong on main.

The grounding is durable and does not need redoing:

| fact | value |
|---|---|
| upstream | openssh-portable, tag `V_10_5_P1`, commit `b3f7344209832eea8ece447d871ea748767c444b` |
| shape | **one amalgamated `ed25519.c`** (~202 KB), three exported symbols, everything else `static` |
| externals to shim | `crypto_hash_sha512` (E1 made this possible), `explicit_bzero`, `randombytes` (a **macro** over `arc4random_buf`) |
| licence | public domain, both files |

**The negative checks are ALREADY in the vendored code** — `sc25519_inrange`
(S ≥ L), `ge25519_unpackneg_vartime` (non-canonical), `isneutral` (small-order),
plus length and high-bits. So Task 4 proves each is *reachable through our
wrapper*, sabotaging each individually. Much better than adding them.

`crypto_sign_ed25519_open` uses NaCl's **combined signature‖message** convention
while our vtable is detached, so the adapter needs two non-aliasing scratch
buffers of `sig_len + msg_len`. Both RFC 8410 DER forms are **fixed-length**
(48 and 44 bytes), so Task 5 decodes by exact template match, not an ASN.1 walk.

---

## 4. The 4.3.2 merge

`0937f8ea`. `release-4.3.2` forked at v4.3.1 with seven rsod-decode commits;
main had moved 62 ahead. **Only `CHANGELOG.md` conflicted**, structurally — both
sides replaced the region between the preamble and `## 4.3.1`. Kept both, in
order. Verified on the merged tree: both arches, 10808 each,
`test-rsod-decode-pe-map.sh` 94/0 and `test-crashhandler.sh` 24/0 — neither had
ever run against main's 62 commits.

**It surfaced a gap:** E2a's three public functions had **no `## Unreleased`
entry** — the same defect E1's review caught and E2a's six reviews missed. Fixed
in `f4f7ed2f`. `## Unreleased` now carries Added/Changed/Fixed and **no
`### Breaking`, so the next cut is a minor.**

---

## 5. The `AXL_TLS` sweep

`86a58b06`, closed in spec §12 by `ef90c8fb`.

**The census was the load-bearing part.** Raw grep says 192 hits; `AXL_TLS_OK` /
`_ERR` / `_WANT_MORE` / `_H` are unrelated identifiers. Real count: **46 bare
references across 22 files.**

**Eight were runtime strings** telling a consumer to rebuild with a flag that
cannot be set — `axl-http-client.c` ×2, `axl-http-client-async.c`,
`axl-http-server.c`, `fetch.c`, `rfbrowse.c`, `mkfixture.c`, `jose-demo.c`. Each
now names the real cause.

**Twelve were deliberately KEPT** — seven in the Makefile plus `build-prefix.sh`,
`install.sh`, `check-libc-overlap.py` and two file comments. All past tense
explaining why code is shaped as it is ("PREFIX *once had* a THIRD input").
Deleting them loses the reason.

**Two findings beyond prose:**
- `axl-mbedtls-platform.c`'s header carried **two** stale claims — the flag, and
  a pointer to `src/mem/axl-intrinsics.c`, deleted some time ago.
- `axl_tls_available()` and `axl_jose_available()` now unconditionally
  `return true`, so two guards are **dead branches**. Docstrings say so;
  guards left in place. One is an SDK example where the check teaches a habit.

---

## 6. Parking hygiene — two documents were lying

Both fixed in `e8685d56`, and both are the "a status header contradicts its own
body" failure this project has hit repeatedly:

- The **Ed25519 spec header** said "PROPOSED. Not started." with E1 and E2a
  shipped beneath it. §11's table gained a **state column** so the header's
  pointer to it is true.
- The **AxlSsh ROADMAP entry** said "NOT started" while **five of P1's six tasks
  are shipped**, running 90 assertions across four source files
  (`axl-ssh-{buf,packet,kex}.c`, `axl-ssh-internal.h`). Each task now recorded
  with its commit.
- AxlSsh's **"the hard part is already done"** table asserted `AXL_PK_ED25519`
  exists. Spec §12 already knew it was wrong and said it would be corrected
  "when this lands" — with E2b parked, that would have waited indefinitely in
  the file CLAUDE.md calls the single source of truth.

**Worth acting on:** the corrected table notes `aes256-gcm@openssh.com` is
OpenSSH's own documented default fallback — so **P1 Task 6 could negotiate an
ECDSA host key and skip Ed25519 entirely**, unblocking AxlSsh with no vendored
crypto. That is a genuine alternative to unparking E2b.

---

## 7. `lsacpi` — specced, AWAITING REVIEW (the next thing)

`docs/superpowers/specs/2026-08-23-lsacpi-design.md`, unpushed on purpose.

Not a listing tool — a **discrepancy finder**. SMBIOS Type 9, SMBIOS Type 41,
the ACPI namespace (`_SUN`/`_UID`/`_PLD` bound by `_ADR`) and the PCIe Slot
Capabilities block all describe the same slots and **disagree on real hardware**.
`lsacpi` is the only place that can see all four.

**Grounding corrected the premise twice:**
- The expected "API updates" mostly are not gaps. `AxlSmbiosSystemSlot` already
  carries `segment_group`/`bus`/`device_function` **with sentinels**;
  `AxlSmbiosOnboardDeviceExt` the same; MCFG already decodes to ECAM windows;
  `lspci` is already ECAM-based.
- **The real collision is AML.** `axl-acpi.h:25` declares it out of scope as
  "ACPICA-sized" — right about *execution*, too broad as written. **Decision:
  the scope becomes "no AML execution"**, with a stop a reviewer can check by
  grep (a Method body is skipped by length, never entered).

**Feasibility is measured.** Against the in-tree fixture
`test/fixtures/proxmox-vm/acpi/dsdt.dat`, decompiled with `iasl`:

| | count |
|---|---:|
| `Name(_ADR, …)` | 131 |
| `Name(_SUN, …)` | **123** |
| `Method(_SUN)` | **0** |
| `_DSM` (Methods, out of reach) | 123 |
| `_PLD` | 0 — absent in a VM, real-hardware only |

The static walker reads **100% of `_SUN` and `_ADR`**, and **`iasl`,
`acpidump`, `acpixtract` are all on the build host** as independent oracles.

The parser's real requirement is **skipping** — `OperationRegion`, `Field`,
nested `Scope` stepped over by trusting PkgLength, never failed on.

**Decisions locked with Mike:** name `lsacpi` (the `lspci`/`lsusb`/`lsproto`
family); auto-hexdump for undecoded tables, not a flag; checksum column on by
default; correlation on by default; full Slot block including **Status /
Presence Detect** (the only non-build-time truth) and VSEC/DVSEC presence;
walker API is a **cursor iterator** matching the module's own idiom.

**Stated honestly in §8:** the mismatch **cannot be reproduced in a VM**, whose
four sources are generated consistently by one project. The tool's value is
unproven until it runs on real firmware. §11 names the design's weakest point —
mapping a namespace device to a PCI address when `_SEG`/`_BBN` are absent, as
they are in the fixture.

---

## 8. Traps this session paid for

- **`ARCH=aa64 ./test/integration/test-axl.sh` SILENTLY RUNS X64.** That script
  takes `--arch AARCH64` and ignores the env var. My plan had it wrong in four
  places; an implementer caught it. **Always check the banner.**
- **`TEST_APPS_ONLY=X` under `--expect-fail` gives a FALSE "detected"** — the
  filtered run's count sits below the ratchet baseline, so the harness fails on
  the ratchet, not the sabotage. Never use a filtered run as evidence.
- **A sabotage that proves nothing.** Task 3's implementer found the brief's
  sabotage targeted an unreachable arm, established it with a **control** against
  the unsabotaged tree, and supplied a valid one. An undetected sabotage has two
  meanings; they determined which.
- **My own grep missed 14 tags** — `V_[0-9]_` cannot match a two-digit major, so
  OpenSSH's latest read as 9.9p2 when it is 10.5p1. A census is only as good as
  its pattern.
- **Every implementer flagged at least one defect in my plans, and every flag was
  right** — the missing §5 mode/context, a constness inconsistency, construction
  sites routed through a tag that is not yet meaningful, a sentinel regression
  against a recorded ruling, stale snippets contradicting the settled contract.
  The dispatch prompt is not an artifact reviewers receive, which is why one
  reviewer could not verify a quote an implementer cited correctly.

---

## 9. Suggested first move next session

Ask Mike whether the `lsacpi` spec is approved. On yes → write the L1-L4 plan
(`writing-plans`), then execute subagent-driven as E1/E2a were. On changes →
revise §7/§11 first; the weakest point is the namespace-to-PCI mapping.

If he would rather not start lsacpi: **the ECDSA-host-key option in §6 unblocks
AxlSsh P1 Task 6 with no vendored crypto**, and is the cheapest path to a
working SSH transport.
