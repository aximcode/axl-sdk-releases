/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cpu.h
 *
 * Typed CPU exception handling. Backend-neutral abstraction over
 * UEFI's `EFI_CPU_ARCH_PROTOCOL.RegisterInterruptHandler` and the
 * arch-tagged-union `EFI_SYSTEM_CONTEXT` that comes with it.
 *
 * Consumers register a callback for a typed `AxlCpuExceptionKind`;
 * the callback receives a layout-stable `AxlCpuException` with the
 * full register snapshot translated from the architecture-specific
 * `EFI_SYSTEM_CONTEXT_*` arm. No consumer code needs to spell
 * `EFI_*` to monitor CPU exceptions.
 *
 * Availability is gated by `EFI_CPU_ARCH_PROTOCOL` — present on
 * conformant DXE firmwares, absent on some embedded / pre-DXE
 * contexts. `axl_cpu_register_exception` returns @c AXL_ERR with a
 * warning to log domain @c "cpu" if the protocol can't be located;
 * consumers handle that as "monitoring unavailable on this
 * firmware" rather than silently going un-monitored.
 *
 * @code
 * static void on_crash(const AxlCpuException *exc, void *user) {
 *     (void)user;
 *     axl_printf("CRASH at 0x%lx, kind=%d\n",
 *                (unsigned long)exc->instruction_ptr, exc->kind);
 *     for (;;) {}  // halt
 * }
 *
 * axl_cpu_register_exception(AXL_CPU_EXCEPTION_GP_FAULT, on_crash, NULL);
 * @endcode
 */

#ifndef AXL_CPU_H
#define AXL_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Exception kinds (cross-arch with documented availability)
// ---------------------------------------------------------------------------

/**
 * @brief Kinds of CPU exception a consumer can register a handler
 *     for.
 *
 * Per-kind availability differs by architecture; trying to
 * register an unavailable kind returns @c AXL_ERR.
 */
typedef enum {
    /* x86-64 exception kinds — register on x64 only. */
    AXL_CPU_EXCEPTION_DIVIDE_ERROR    = 1,   ///< x64 \#DE
    AXL_CPU_EXCEPTION_DEBUG           = 2,   ///< x64 \#DB
    AXL_CPU_EXCEPTION_OVERFLOW        = 3,   ///< x64 \#OF
    AXL_CPU_EXCEPTION_BOUND           = 4,   ///< x64 \#BR
    AXL_CPU_EXCEPTION_INVALID_OPCODE  = 5,   ///< x64 \#UD
    AXL_CPU_EXCEPTION_DEVICE_NA       = 6,   ///< x64 \#NM
    AXL_CPU_EXCEPTION_DOUBLE_FAULT    = 7,   ///< x64 \#DF
    AXL_CPU_EXCEPTION_SEGMENT_NP      = 8,   ///< x64 \#NP
    AXL_CPU_EXCEPTION_STACK_FAULT     = 9,   ///< x64 \#SS
    AXL_CPU_EXCEPTION_GP_FAULT        = 10,  ///< x64 \#GP
    AXL_CPU_EXCEPTION_PAGE_FAULT      = 11,  ///< x64 \#PF
    AXL_CPU_EXCEPTION_FP_ERROR        = 12,  ///< x64 \#MF
    AXL_CPU_EXCEPTION_ALIGNMENT_CHECK = 13,  ///< x64 \#AC
    AXL_CPU_EXCEPTION_SIMD            = 14,  ///< x64 \#XM

    /* AArch64 exception kinds — register on aa64 only.
       aa64 collapses x64-style traps into broad classes; the
       SYNCHRONOUS umbrella covers what x64 splits across \#UD /
       \#GP / \#PF / \#AC. Consumers that want finer detail on aa64
       inspect `ESR_EL1.EC` inside the callback. */
    AXL_CPU_EXCEPTION_SYNCHRONOUS     = 15,  ///< aa64 synchronous-exception umbrella
    AXL_CPU_EXCEPTION_SERROR          = 16,  ///< aa64 SError

    AXL_CPU_EXCEPTION_KIND_MAX        = 17,  ///< exclusive upper bound
} AxlCpuExceptionKind;

// ---------------------------------------------------------------------------
// Architecture tag (for the register-snapshot union below)
// ---------------------------------------------------------------------------

/// Architecture the register-snapshot union carries.
typedef enum {
    AXL_CPU_ARCH_UNKNOWN = 0,  ///< arch not determined (no snapshot available)
    AXL_CPU_ARCH_X64     = 1,  ///< x86-64 (regs.x64 populated)
    AXL_CPU_ARCH_AA64    = 2   ///< AArch64 (regs.aa64 populated)
} AxlCpuArch;

// ---------------------------------------------------------------------------
// Exception context
// ---------------------------------------------------------------------------

/// Current `AxlCpuException.version` value emitted by the SDK.
/// Bumped when the struct gains a new arm or extends an existing
/// arm in a way that consumers care about. Pre-1.0 SDK: layout
/// can move; consumers should test `exc->version >= N` before
/// reading fields added in version N.
#define AXL_CPU_EXCEPTION_VERSION  1

/**
 * @brief Architecture-neutral CPU exception context delivered to
 *     consumer callbacks.
 *
 * The base fields (`fault_address` through `error_code`) are
 * arch-neutral and always populated. The register snapshot lives
 * in the `regs` union; consumers branch on `arch` to pick the
 * correct arm.
 *
 * **ABI growth.** `struct_size` is the byte count the SDK wrote
 * for this instance (`sizeof(AxlCpuException)` at the SDK's build
 * time); `version` is `AXL_CPU_EXCEPTION_VERSION`. Consumers
 * targeting a forward range of SDKs guard reads of late-added
 * fields with `if (exc->struct_size >= offsetof(AxlCpuException,
 * new_field) + sizeof(exc->new_field))` or `if (exc->version >=
 * <since-version>)`. Existing fields never move; the union arms
 * grow append-only.
 *
 * **Field semantics:**
 *  - `fault_address` — meaningful for memory-access faults
 *    (x64 \#PF = CR2; aa64 sync = FAR_EL1). Zero for kinds where
 *    no fault address applies (\#DE, \#GP without memory, \#UD, ...).
 *  - `error_code` — exception-specific:
 *    x64 \#PF / \#GP / \#DF / \#NP / \#SS / \#AC carry an error code
 *    pushed by the CPU; for other kinds, 0.
 *    aa64 carries `ESR_EL1` here so consumers can recover
 *    finer-grained classification on synchronous exceptions
 *    (EC field) without a separate accessor.
 */
typedef struct {
    uint32_t            struct_size;      ///< sizeof(AxlCpuException) as written by the SDK
    uint32_t            version;          ///< AXL_CPU_EXCEPTION_VERSION at emit time
    AxlCpuExceptionKind kind;
    AxlCpuArch          arch;             ///< AXL_CPU_ARCH_X64 / _AA64 (UNKNOWN if none)
    uint64_t            fault_address;    ///< memory-fault address, or 0 if N/A
    uint64_t            instruction_ptr;  ///< RIP / ELR
    uint64_t            stack_ptr;        ///< RSP / SP
    uint64_t            frame_ptr;        ///< RBP / X29
    uint64_t            error_code;       ///< exception-specific; aa64 carries ESR_EL1
    union {
        struct {
            uint64_t rax, rbx, rcx, rdx;
            uint64_t rsi, rdi, rbp, rsp;
            uint64_t r8,  r9,  r10, r11;
            uint64_t r12, r13, r14, r15;
            uint64_t rip, rflags;
            uint64_t cr2;                 ///< raw CR2 — meaningful only when kind == PAGE_FAULT
        } x64;
        struct {
            uint64_t x[31];   ///< X0..X28, X29 (=FP), X30 (=LR)
            uint64_t sp;
            uint64_t elr;
            uint64_t spsr;
            uint64_t esr;                 ///< raw ESR_EL1 (also in `error_code`)
            uint64_t far;                 ///< raw FAR_EL1 (also in `fault_address`)
        } aa64;
    } regs;
} AxlCpuException;

// ---------------------------------------------------------------------------
// Callback + registration
// ---------------------------------------------------------------------------

/**
 * @brief CPU-exception callback signature.
 *
 * Runs in firmware exception context — heap allocation, console
 * I/O beyond `axl_printf`, and most SDK calls that allocate are
 * unsafe. The callback typically captures register state into a
 * pre-allocated buffer and either halts (`for (;;) {}`) or
 * resumes via the firmware's exception-return mechanism. The SDK
 * does not return from the callback to user code.
 */
typedef void (*AxlCpuExceptionFn)(
    const AxlCpuException *exc,
    void                  *user
) AXL_CB_NOEXCEPT;

/**
 * @brief Register an exception handler for @p kind.
 *
 * Internally locates `EFI_CPU_ARCH_PROTOCOL` (cached after first
 * call), maps @p kind onto the arch-specific `EFI_EXCEPTION_TYPE`,
 * and registers a thunk that translates `EFI_SYSTEM_CONTEXT` into
 * `AxlCpuException` before invoking @p cb.
 *
 * A second `axl_cpu_register_exception` call for the same @p kind
 * replaces the previous handler.
 *
 * @return AXL_OK on success; AXL_ERR if @p cb is NULL, @p kind is
 *     out of range, @p kind is not available on the current arch,
 *     or `EFI_CPU_ARCH_PROTOCOL` is not published.
 */
int
axl_cpu_register_exception(
    AxlCpuExceptionKind kind,
    AxlCpuExceptionFn   cb,
    void               *user
);

/**
 * @brief Unregister a previously-installed handler.
 *
 * Safe to call on a kind that was never registered (no-op).
 *
 * @return AXL_OK on success, AXL_ERR if @p kind is out of range
 *     or `EFI_CPU_ARCH_PROTOCOL` is not published.
 */
int
axl_cpu_unregister_exception(
    AxlCpuExceptionKind kind
);

// ---------------------------------------------------------------------------
// Instruction-set feature detection + SIMD dispatch
// ---------------------------------------------------------------------------

/**
 * @brief Detected CPU instruction-set features.
 *
 * Filled once from `CPUID` (x86) on first query and cached. Fields
 * for the other architecture are always `false` — read `neon` on
 * aarch64, the x86 fields on x86. These report what the CPU *can
 * execute*; for AVX, "can execute" still requires a one-time state
 * enable (see `axl_cpu_enable_avx`) before the YMM registers are
 * usable without a \#UD fault.
 *
 * **Describes the machine, not the calling core.** Detection runs once,
 * on whichever processor asks first, and the result is shared. On a
 * hybrid part (performance + efficiency cores with different ISAs) the
 * core you are running on may not have everything listed here. Code
 * dispatched to an AP should therefore gate vector work on
 * `axl_cpu_enable_avx` / `axl_cpu_enable_avx512`, which answer for the
 * core that calls them. Querying from an AP is safe — publication is
 * synchronised — but the answer is still the machine's, not the core's.
 *
 * Most consumers want `axl_cpu_simd_tier` (a single ordered value
 * for kernel dispatch) rather than these individual bits.
 */
typedef struct {
    /* --- x86: SIMD (all false on non-x86) --- */
    bool sse2;    ///< SSE2 (always present on x86-64; firmware enables XMM state)
    bool sse3;    ///< SSE3
    bool ssse3;   ///< SSSE3 (PSHUFB — byte shuffles)
    bool sse41;   ///< SSE4.1 (PMOVZX / PBLENDVB / ROUNDPS — no state enable needed)
    bool sse42;   ///< SSE4.2 (also CRC32 instruction)
    bool fma;     ///< FMA3 fused multiply-add
    bool xsave;   ///< XSAVE/XGETBV/XSETBV present (precondition for enabling AVX)
    bool avx;     ///< AVX (256-bit) — usable only after axl_cpu_enable_avx()
    bool avx2;    ///< AVX2 (256-bit integer) — usable only after enable
    bool avx512f;   ///< AVX-512 Foundation — usable only after axl_cpu_enable_avx512()
    bool avx512dq;  ///< AVX-512 Doubleword/Quadword
    bool avx512bw;  ///< AVX-512 Byte/Word
    bool avx512vl;  ///< AVX-512 Vector Length extensions
    bool avx512cd;  ///< AVX-512 Conflict Detection
    bool avx512vnni; ///< AVX-512 Vector Neural Network Instructions
    /* --- x86: crypto / hashing --- */
    bool aes;        ///< AES-NI (AESENC/AESDEC ...)
    bool pclmulqdq;  ///< carry-less multiply (GHASH / GCM)
    bool sha;        ///< SHA-NI (SHA1/SHA256 round instructions)
    bool vaes;       ///< vectorized AES (VEX/EVEX-encoded AES on 256/512-bit)
    bool vpclmulqdq; ///< vectorized carry-less multiply
    /* --- x86: bit-manipulation / misc --- */
    bool popcnt;  ///< POPCNT instruction
    bool bmi1;    ///< BMI1 (ANDN, BLSR, TZCNT ...)
    bool bmi2;    ///< BMI2 (BZHI, PDEP, PEXT, MULX ...)
    bool lzcnt;   ///< LZCNT (leading-zero count; ABM)
    bool movbe;   ///< MOVBE (load/store with byte swap)
    bool f16c;    ///< F16C (half-precision float <-> single convert)
    bool adx;     ///< ADX (ADCX/ADOX multiprecision add)
    bool rdrand;  ///< RDRAND (on-chip RNG)
    bool rdseed;  ///< RDSEED (seed-grade on-chip RNG)
    /* --- aarch64 (all false on non-aarch64 targets) --- */
    bool neon;     ///< AdvSIMD/NEON (always present on ARMv8-A baseline)
    bool fp16;     ///< half-precision floating point (FEAT_FP16)
    bool atomics;  ///< Large System Extensions (FEAT_LSE atomic instructions)
    bool crc32;    ///< CRC32 instructions (FEAT_CRC32)
    bool aes_a64;  ///< AES instructions (FEAT_AES)
    bool pmull;    ///< polynomial multiply long (FEAT_PMULL — GHASH)
    bool sha1;     ///< SHA1 instructions (FEAT_SHA1)
    bool sha2;     ///< SHA-256 instructions (FEAT_SHA256)
    bool sha512;   ///< SHA-512 instructions (FEAT_SHA512)
    bool sha3;     ///< SHA3 instructions (FEAT_SHA3)
    bool dotprod;  ///< dot-product instructions (FEAT_DotProd)
    bool sve;      ///< Scalable Vector Extension (FEAT_SVE)
} AxlCpuFeatures;

/**
 * @brief SIMD dispatch tier — a single ordered value naming the best
 *     usable kernel.
 *
 * Monotonic: a higher value is a strict superset of the work a lower
 * one can do, so a dispatcher picks the highest tier for which it has
 * a kernel. AVX2 is only reported once `axl_cpu_enable_avx` has
 * succeeded — querying the tier never changes CPU state on its own.
 */
typedef enum {
    AXL_SIMD_SCALAR   = 0,  ///< no SIMD (not expected on our targets)
    AXL_SIMD_BASELINE = 1,  ///< 128-bit: SSE2 (x86) or NEON (aarch64); always available
    AXL_SIMD_SSE41    = 2,  ///< x86 SSE4.1 — 128-bit, richer pixel ops; detection only
    AXL_SIMD_AVX2     = 3,  ///< x86 AVX2 — 256-bit; requires axl_cpu_enable_avx()
} AxlSimdTier;

/**
 * @brief Query detected CPU features (cached after first call).
 *
 * Pure detection — never changes CPU state. The returned pointer is
 * to SDK-owned static storage valid for the program's lifetime;
 * never NULL.
 *
 * @return pointer to the cached feature set.
 */
const AxlCpuFeatures *
axl_cpu_features(void);

/**
 * @brief Enable AVX (YMM) register state so AVX/AVX2 instructions run
 *     without a \#UD fault.
 *
 * UEFI firmware enables SSE state (the calling convention needs XMM)
 * but does **not** enable AVX state, so AVX instructions trap until a
 * CPL0 caller sets `CR4.OSXSAVE` and the AVX bits in `XCR0`. A UEFI
 * application runs at CPL0, so it may do this itself; this routine
 * performs the sequence (CPUID-gated) once and is idempotent.
 *
 * **Per-logical-processor.** `CR4`/`XCR0` are per-CPU; code that runs
 * AVX kernels on application processors (via MP services) must call
 * this on each AP as well as the BSP. The capability gate is a live
 * `CPUID` on the calling core rather than a read of the cached
 * `axl_cpu_features` vector, which describes whichever core populated
 * it first — on a hybrid part those disagree, and trusting the cache
 * would mean writing `XCR0.YMM` on a core with no AVX, which \#GPs.
 *
 * No-op returning `false` when this core lacks AVX (or on non-x86),
 * leaving `axl_cpu_simd_tier` at `AXL_SIMD_SSE41`/`BASELINE`.
 *
 * @return `true` if AVX is usable after the call (already-enabled
 *     counts), `false` if the CPU has no AVX to enable.
 */
bool
axl_cpu_enable_avx(void);

/**
 * @brief Enable AVX-512 (opmask + ZMM) register state.
 *
 * The AVX-512 counterpart to `axl_cpu_enable_avx`: sets `CR4.OSXSAVE`
 * and the `XCR0` bits for x87 + SSE + AVX **and** the AVX-512 state
 * components (opmask, ZMM_Hi256, Hi16_ZMM), so EVEX-encoded AVX-512
 * instructions run without a \#UD. Implies AVX enable. Gated on the CPU
 * advertising AVX-512F and the XSAVE state components being supported;
 * idempotent; per-logical-processor (same caveat as
 * `axl_cpu_enable_avx`).
 *
 * Note: `axl_cpu_simd_tier` tops out at `AXL_SIMD_AVX2` — that is the
 * widest tier AXL's own kernels use. AVX-512 is exposed for consumers
 * who write their own AVX-512 code: call this and branch on the result.
 * Branch on the return value, not on `avx512f` in `axl_cpu_features` —
 * the feature vector describes the machine, this answers for the core
 * you are on, and only the latter decides whether the instruction runs.
 *
 * @return `true` if AVX-512 is usable after the call (already-enabled
 *     counts), `false` if the CPU has no AVX-512 to enable (or non-x86).
 */
bool
axl_cpu_enable_avx512(void);

/**
 * @brief The best SIMD tier usable *right now* for kernel dispatch.
 *
 * x86: `AXL_SIMD_AVX2` if AVX2 is present **and** enabled (call
 * `axl_cpu_enable_avx` first), else `AXL_SIMD_SSE41` if SSE4.1 is
 * present, else `AXL_SIMD_BASELINE` (SSE2, always). aarch64:
 * `AXL_SIMD_BASELINE` (NEON). Does not change CPU state.
 *
 * **Resolve this once per operation — never per row, pixel or element.**
 * The answer is deliberately *not* cached, because it must describe the
 * core that is asking rather than the machine (that is the whole reason
 * to prefer it over `axl_cpu_features`). Live means `CPUID`: on x86 one
 * call here executes several leaves, and paired with
 * `axl_cpu_enable_avx` a dispatch check costs 4 `CPUID` plus 2 `XGETBV`.
 *
 * Under virtualisation `CPUID` is unconditionally intercepted, so each
 * one is a VM exit — roughly a microsecond under KVM, against a few
 * cycles for the vector work it is guarding. AXL shipped exactly this
 * mistake: a per-scanline blend kernel called it per row, and a
 * 1280x800 alpha blend paid ~800 x 6 VM exits. It reached a consumer as
 * a compositor whose post-key repaint went from ~0 to 1.75 s, and cost a
 * bisect across three repositories to find.
 *
 * Hoist it to the enclosing blit / fill / decode entry point and pass
 * the chosen kernel down. In UEFI boot services there is no preemption
 * and no scheduler, so the BSP cannot migrate mid-call and the answer
 * cannot change underneath a single operation. An AP worker is the one
 * place a kernel genuinely runs off-BSP — there, resolve it once per
 * task, on the task's own core.
 *
 * @return the highest currently-usable `AxlSimdTier`.
 */
AxlSimdTier
axl_cpu_simd_tier(void);

/**
 * @brief Retire the SIMD-tier memo — call before running code on another
 *     processor by any route AXL does not own.
 *
 * `axl_cpu_simd_tier` memoises its answer while the BSP is provably the
 * only core executing AXL code: UEFI boot services has no scheduler and
 * no preemption, so with no AP ever dispatched the BSP cannot migrate
 * and one cached answer *is* this core's answer. That makes the query
 * free in a hot path instead of several `CPUID` leaves — which, under
 * virtualisation where every `CPUID` is a VM exit, is the difference
 * between a usable dispatch check and a ruinous one.
 *
 * AXL's own AP dispatch retires the memo automatically. Call this
 * yourself **before** the first time you run code on another processor
 * through `EFI_MP_SERVICES_PROTOCOL` directly, or through anything else
 * that steps outside AXL. Retirement is permanent and cheap: every
 * later call simply does the live query again, which is what the
 * function did unconditionally before.
 *
 * Skipping it on a hybrid part is a \#UD, not a mis-report — an
 * efficiency core would be handed the performance core's AVX2 verdict
 * and execute a 256-bit kernel it does not implement.
 */
void
axl_cpu_simd_memo_invalidate(void);

// ---------------------------------------------------------------------------
// Processor topology (MP-services inventory)
// ---------------------------------------------------------------------------

/**
 * @brief One logical processor's location and status.
 *
 * `package` / `core` / `thread` are the firmware-reported physical
 * coordinates (`EFI_CPU_PHYSICAL_LOCATION`); together they identify
 * where the logical processor sits in the package/core/SMT topology.
 * The three booleans decode the MP-services status flags:
 *  - `bsp` — this is the bootstrap processor (exactly one entry has
 *    `bsp == true`).
 *  - `enabled` — the processor is enabled and usable. Disabled
 *    processors are still reported (so the count of entries matches
 *    `total`), with `enabled == false`.
 *  - `healthy` — firmware's built-in self test passed for this
 *    processor. A `false` here on an `enabled` processor flags a
 *    BIST failure worth surfacing.
 */
typedef struct {
    uint32_t  package;   ///< physical package (socket) index
    uint32_t  core;      ///< core index within the package
    uint32_t  thread;    ///< hardware thread (SMT) index within the core
    bool      bsp;       ///< bootstrap processor (exactly one is true)
    bool      enabled;   ///< processor is enabled / usable
    bool      healthy;   ///< firmware self-test passed
} AxlCpuProcessor;

/**
 * @brief Enumerate the machine's logical processors and their status.
 *
 * Reads `EFI_MP_SERVICES_PROTOCOL` (the same data the task pool uses
 * for AP fan-out) and reports, for every logical processor the
 * firmware knows about, its physical location and status flags. This
 * is the headless mechanism behind a consumer "CPU inventory" view;
 * formatting and policy belong to the caller.
 *
 * **Counts vs. fill, decoupled.** `*total` and `*enabled` are always
 * set to the true machine-wide counts regardless of @p out_cap, so a
 * caller can query sizes first (pass `out == NULL`) and then size an
 * array, or pass a generous buffer and detect truncation. `*out_n` is
 * the number of entries written to @p out — always `min(*total,
 * out_cap)`. When `*out_n < *total` the buffer was too small and the
 * tail processors were not written; the counts are still accurate.
 *
 * **Dense, index-keyed.** `out[i]` describes firmware processor
 * number `i` — the array index *is* the processor's identity, so a
 * caller needs no separate id field. The bootstrap processor is
 * whichever entry has `bsp == true` (index 0 on typical firmware, but
 * not guaranteed). To keep the index aligned with the processor
 * number the fill is never sparse: if the firmware fails to return
 * info for a processor it claimed exists, that slot is written as a
 * zeroed entry (`enabled == healthy == false`, location `{0,0,0}`)
 * rather than skipped, and still counts toward `*out_n`.
 *
 * **Uniprocessor firmware.** `EFI_MP_SERVICES_PROTOCOL` is optional
 * and commonly absent on single-processor platforms. When it is not
 * published this reports the honest floor — `*total == *enabled == 1`
 * with `*out_n == 0` (no per-processor entry written) — rather than
 * failing or fabricating status bits, since the caller is by
 * definition executing on at least the BSP but no enumeration source
 * is available to characterize it. A caller seeing `*out_n == 0`
 * alongside `*total >= 1` knows per-processor detail was unavailable.
 *
 * All out-parameters are optional (pass `NULL` to skip any). Passing
 * `out == NULL` (or `out_cap == 0`) queries the counts without
 * filling; `*out_n` is then 0.
 *
 * @param total    [out] total logical processors reported (optional).
 * @param enabled  [out] count with the enabled flag set (optional).
 * @param out      [out] caller array filled with up to @p out_cap
 *                   entries, indexed by processor number; `NULL` to
 *                   query counts only.
 * @param out_cap  [in]  capacity of @p out, in entries.
 * @param out_n    [out] entries written to @p out (optional).
 *
 * @return AXL_OK on success (including the uniprocessor-floor path);
 *     AXL_ERR only if the MP-services protocol is published but its
 *     machine-wide processor count cannot be read.
 */
int
axl_cpu_topology(
    size_t           *total,
    size_t           *enabled,
    AxlCpuProcessor  *out,
    size_t            out_cap,
    size_t           *out_n
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CPU_H */
