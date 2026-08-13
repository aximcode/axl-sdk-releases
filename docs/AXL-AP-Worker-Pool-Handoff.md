# Handoff — AP worker pools: two findings to fold into AxlTask/AxlCpu

**Status:** HANDOFF 2026-08-03. Nothing implemented. Written for a fresh session.
**Source:** `github.com/hardrave/NIGHTRUN`, `crates/nr-boot/src/smp.rs` (MIT). A
Rust UEFI-resident LLM runtime that runs inference across BSP + APs. Read that
file — it is ~100 lines and both findings are in it, with the reasoning.

---

## 0. What is NOT the finding

NightRun does **not** let APs make BSP-restricted calls. Its own module doc:

> *application processors are started once into the nr-tensor worker pool
> (pure compute + atomics; APs never call firmware services).*

Its AP entry point is two lines — enable SIMD, then spin in a compute loop.
Every firmware call stays on the BSP. That is the same rule `axl-task.h`
already encodes for `AxlTaskProc`. Do not go looking for an escape hatch;
there isn't one, and the spec doesn't grant one.

What is worth taking is *how they run a long-lived AP pool at all*.

---

## 1. Finding A — XCR0 is per-core, so AVX must be enabled ON each AP

NightRun's AP entry:

```rust
extern "efiapi" fn worker_entry(_arg: *mut c_void) {
    // Each AP needs its own AVX enable (XCR0 is per-core).
    crate::enable_simd_quiet();
    nr_tensor::parallel::POOL.worker_loop();
}
```

An AP whose XCR0 has not been set will `#UD` on the first AVX instruction.
The BSP enabling AVX for itself does nothing for the others.

### Where axl-sdk stands

**Already knows the fact, does not yet act on it.** `src/util/axl-cpu.c:502`
says *"CR4/XCR0 are per-CPU, so we read the live hardware state rather than
cache it"*, and `axl_cpu_enable_avx()` writes CR4/XCR0 at CPL0. Detection is
live-read, so it is correct per-core by construction.

**The hazard is latent, not live:** nothing currently dispatches SIMD work to
an AP — `grep` finds no `axl_cpu_*` or SIMD reference in
`src/backend/native/axl-backend-native-mp.c` or `src/task/*.c`. The first
caller to run vector code through `AxlTaskProc` hits it.

### What to do

Smallest correct thing: document it next to the existing Boot-Services
prohibition in `axl-task.h`, and have whatever runs a task on an AP call
`axl_cpu_enable_avx()` on that core first. Verify the current AVX-detection
path is genuinely re-entrant per-core before relying on it — read
`axl-cpu.c:382-530`, especially the CPL0 note at :382.

**Testable in QEMU?** Probably yes for the `#UD` (dispatch an AVX kernel to an
AP without enabling, expect a fault), but confirm rather than assume — this is
the class where "QEMU doesn't emulate it" bites. If it is not testable,
say so in the commit rather than claiming coverage.

---

## 2. Finding B — `StartupAllAPs` semantics diverge by arch, and a persistent
pool is not the shape MP Services was written for

This is the valuable one. Their comment, verbatim:

> The workers never return from their procedure, so the startup call can never
> "complete" — how we dispatch differs per platform:
> - **x86** (CpuDxe/MpInitLib): non-blocking with a dummy event; its blocking
>   mode **resets stragglers on timeout, which would kill the workers**.
> - **aarch64** (ArmPsciMpServicesDxe): non-blocking is **refused after
>   READY_TO_BOOT** (i.e. always, for a boot app) — *real-hardware finding*.
>   Blocking with a short timeout instead: PSCI cannot preempt an AP, so
>   `EFI_TIMEOUT` just hands control back with the workers left running.

So:

| arch | mode | timeout | success looks like |
|---|---|---|---|
| x86_64 | non-blocking + dummy event | none | `EFI_SUCCESS` |
| aarch64 | **blocking** | 120 ms | **`EFI_TIMEOUT`** |

`EFI_TIMEOUT` being the *expected, correct* outcome on aarch64 is the part a
reimplementation would get wrong — it reads like failure. They also
`core::mem::forget(mp)` to keep the protocol handle open for the image's life
rather than letting it close.

### Why this matters to axl-sdk

`AxlTask` today is fire-and-join per call. A **persistent** AP worker pool —
start once, feed work through atomics, never return — is a different shape,
and it is the shape you want if AP dispatch is ever on a hot path, because
per-call `StartupAllAPs` pays firmware overhead every time.

If AxlTask stays fire-and-join, Finding B is informational. If it grows a
persistent pool, Finding B is the map of the potholes.

### Verification hazard, stated up front

Both halves of this are **firmware-implementation** behaviour (CpuDxe vs
ArmPsciMpServicesDxe), not spec behaviour. OVMF under QEMU may reproduce
neither. Treat "it worked in QEMU" as no evidence about hardware, and see
`feedback_uefi_firmware_test_hazards` — this is exactly the register/dispatch
lifecycle class where misuse hangs rather than returning an error.

---

## 3. Decide before building

1. **Does AxlTask want a persistent AP pool at all?** If no, Finding B is a
   comment in `axl-task.h` and nothing more. Do not build a pool speculatively
   — but note Mike has said he does not want YAGNI invoked as a blanket
   objection, so argue it on cost, not on principle.
2. **Where does per-core AVX enable belong** — implicit in the task dispatcher,
   or explicit in the `AxlTaskProc` contract? Implicit is friendlier; explicit
   is honest about the cost and matches "APs never call firmware services" in
   spirit.
3. **Is any of this reachable from a resident driver**, where the BSP has
   already returned to the firmware? That is the configuration axl-sdk's
   consumers actually ship.

---

## 4. Working agreements for the new session

- **Read `CLAUDE.md` first** (project root; note it is gitignored, so it is
  local-only) and `docs/AXL-Design.md` before library changes.
- Bucket A for new public API: header + docstring first, an **independent
  contract-first review of just the header**, then failing tests, confirm RED,
  implement, refactor while green, pre-commit review. That review caught a
  double-free-class Critical on the last two APIs; it is not ceremony.
- **`./scripts/verify.sh`** runs the whole gate set concurrently (~2 min, vs
  ~4 serial) and asserts the two arch counts match. Use it instead of running
  gates by hand.
- **`./scripts/sabotage.sh`** for sabotage verification — restores with a
  `touch` (a stale object once produced a wrong result) and refuses a no-op
  sabotage.
- Ratchet baseline is **9260**, per-configuration, both arches must match.
- `--arch AARCH64` is the correct test form; `ARCH=aa64` silently runs X64.
- Gates now include `check-nul`, `check-test-registered`, a clang
  `-Wall -Wextra` compiler pass, clang-tidy over `src/`+`test/unit/`+`tools/`,
  and a **teardown leak gate** that fails a run on any leaked allocation.

## 5. Open items inherited, unrelated to the above

- **`CLAUDE.md` is gitignored** (`.gitignore:42`). Three tasks have edited its
  gate documentation; all of those edits are local-only, in no commit. Needs a
  decision.
- **`QEMU_PID` in `run-qemu.sh` is the `timeout` pid, not QEMU's.** Recommended
  NOT renaming (7 in-tree tests plus consumer repos parse it); the comment now
  says what it really is. The bad name caused a wrong diagnosis once.
- **Handoff owed to the `json-flag-redesign` worktree session**: `axl_utf8_encode`
  now exists and `append_scalar` is their adoption point; the merge was measured
  at 3 extra hunks, all take-theirs, but the private encoder's deletion merges
  *silently*, so after resolving they would call a function that no longer
  exists (one-line fix). Their branch also carries a `\xNN`-as-code-unit fix
  that main lacks.
