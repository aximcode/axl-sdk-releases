/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cpu.c
    CPU exception handling — backend-neutral wrap of
    EFI_CPU_ARCH_PROTOCOL.RegisterInterruptHandler. Translates
    EFI_SYSTEM_CONTEXT into a typed AxlCpuException so consumer
    callbacks never spell EFI_*.

    Layout:
      - Per-kind slot table holds the consumer callback + user
        pointer. One slot per AxlCpuExceptionKind (max 16).
      - One per-arch translation table maps AxlCpuExceptionKind ↔
        EFI_EXCEPTION_TYPE. The forward direction picks the right
        EFI_EXCEPTION_TYPE at registration; the reverse direction
        picks the right AxlCpuExceptionKind inside the thunk so we
        can find the slot.
      - One generic thunk that the firmware calls. It receives the
        EFI_EXCEPTION_TYPE the firmware decided was firing, walks
        the table to recover the AxlCpuExceptionKind we registered
        as, looks up that slot's callback, builds the
        AxlCpuException from EFI_SYSTEM_CONTEXT, and dispatches.

    Concurrency: register / unregister are BSP-only safe-context
    operations. The thunk runs in exception context and only READS
    the slot it was dispatched into. On UEFI Boot Services
    (single-threaded except for MP-service AP fan-out), this means
    slot mutation races with thunk reads only if an AP raises an
    exception during a BSP register call — and since slot writes
    are pointer-sized and word-aligned, a torn read is impossible
    on x86_64 / aarch64. No locking needed under these conditions;
    a multi-core re-entrant register implementation would need a
    lock and is out of scope.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-cpu.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>  /* axl_memset */

AXL_LOG_DOMAIN("cpu");

// ---------------------------------------------------------------------------
// Per-kind slot table
// ---------------------------------------------------------------------------

typedef struct {
    AxlCpuExceptionFn cb;
    void             *user;
} CpuSlot;

static CpuSlot g_slots[AXL_CPU_EXCEPTION_KIND_MAX];   /* zero-init */

// ---------------------------------------------------------------------------
// EFI_CPU_ARCH_PROTOCOL access (lazy locate + cache)
// ---------------------------------------------------------------------------

static EFI_CPU_ARCH_PROTOCOL *g_cpu_arch = NULL;

/* Lazy-cache `EFI_CPU_ARCH_PROTOCOL` on first use. The cache lives
   for the lifetime of Boot Services; UEFI doesn't unpublish
   tier-1 architectural protocols between BS init and
   ExitBootServices in practice. Not refreshed across
   ExitBootServices — runtime-services consumers don't have an
   EFI_CPU_ARCH_PROTOCOL to bind to anyway. */
static EFI_CPU_ARCH_PROTOCOL *
cpu_arch(void)
{
    if (g_cpu_arch != NULL) {
        return g_cpu_arch;
    }
    EFI_GUID guid = gEfiCpuArchProtocolGuid;
    EFI_CPU_ARCH_PROTOCOL *p = NULL;
    EFI_STATUS status = axl_bs()->LocateProtocol(&guid, NULL, (void **)&p);
    if (EFI_ERROR(status) || p == NULL) {
        return NULL;
    }
    g_cpu_arch = p;
    return g_cpu_arch;
}

// ---------------------------------------------------------------------------
// AxlCpuExceptionKind ↔ EFI_EXCEPTION_TYPE mapping (per arch)
// ---------------------------------------------------------------------------

/* Returns the EFI_EXCEPTION_TYPE @p kind maps to on the current
   arch, or -1 if @p kind isn't available on this arch. Encodes
   the per-arch availability matrix documented in <axl/axl-cpu.h>. */
static EFI_EXCEPTION_TYPE
kind_to_efi_type(AxlCpuExceptionKind kind)
{
#if defined(__x86_64__)
    switch (kind) {
    case AXL_CPU_EXCEPTION_DIVIDE_ERROR:    return EXCEPT_X64_DIVIDE_ERROR;
    case AXL_CPU_EXCEPTION_DEBUG:           return EXCEPT_X64_DEBUG;
    case AXL_CPU_EXCEPTION_OVERFLOW:        return EXCEPT_X64_OVERFLOW;
    case AXL_CPU_EXCEPTION_BOUND:           return EXCEPT_X64_BOUND;
    case AXL_CPU_EXCEPTION_INVALID_OPCODE:  return EXCEPT_X64_INVALID_OPCODE;
    case AXL_CPU_EXCEPTION_DEVICE_NA:       return 7; /* #NM — no spec constant */
    case AXL_CPU_EXCEPTION_DOUBLE_FAULT:    return EXCEPT_X64_DOUBLE_FAULT;
    case AXL_CPU_EXCEPTION_SEGMENT_NP:      return EXCEPT_X64_SEG_NOT_PRESENT;
    case AXL_CPU_EXCEPTION_STACK_FAULT:     return EXCEPT_X64_STACK_FAULT;
    case AXL_CPU_EXCEPTION_GP_FAULT:        return EXCEPT_X64_GP_FAULT;
    case AXL_CPU_EXCEPTION_PAGE_FAULT:      return EXCEPT_X64_PAGE_FAULT;
    case AXL_CPU_EXCEPTION_FP_ERROR:        return EXCEPT_X64_FP_ERROR;
    case AXL_CPU_EXCEPTION_ALIGNMENT_CHECK: return EXCEPT_X64_ALIGNMENT_CHECK;
    case AXL_CPU_EXCEPTION_SIMD:            return EXCEPT_X64_SIMD;
    case AXL_CPU_EXCEPTION_SYNCHRONOUS:
    case AXL_CPU_EXCEPTION_SERROR:
    case AXL_CPU_EXCEPTION_KIND_MAX:        return -1;  /* aa64-only */
    }
    return -1;
#elif defined(__aarch64__)
    switch (kind) {
    case AXL_CPU_EXCEPTION_SYNCHRONOUS:     return EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS;
    case AXL_CPU_EXCEPTION_SERROR:          return EXCEPT_AARCH64_SERROR;
    default:                                return -1;  /* x64-only */
    }
#else
    (void)kind;
    return -1;
#endif
}

/* Reverse map — given the EFI_EXCEPTION_TYPE the firmware delivered,
   what AxlCpuExceptionKind did the consumer register? Returns
   AXL_CPU_EXCEPTION_KIND_MAX on miss. The thunk uses this to find
   the right slot to dispatch into.

   Linear scan over the 16-entry kind table — intentional, since a
   static reverse table would duplicate the per-arch #ifdef block
   in kind_to_efi_type() and the iteration is N≤16 in the
   exception path (cold). */
static AxlCpuExceptionKind
efi_type_to_kind(EFI_EXCEPTION_TYPE t)
{
    for (AxlCpuExceptionKind k = 1; k < AXL_CPU_EXCEPTION_KIND_MAX; k++) {
        if (kind_to_efi_type(k) == t) {
            return k;
        }
    }
    return AXL_CPU_EXCEPTION_KIND_MAX;
}

// ---------------------------------------------------------------------------
// EFI_SYSTEM_CONTEXT → AxlCpuException translation
// ---------------------------------------------------------------------------

static void
fill_exc_from_context(
    AxlCpuException      *out,
    AxlCpuExceptionKind   kind,
    EFI_EXCEPTION_TYPE    efi_type,
    EFI_SYSTEM_CONTEXT    sc)
{
    /* Zero base + union so an arch-tagged consumer that reads
       past its arm sees defined bytes rather than stack
       garbage. */
    axl_memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->version     = AXL_CPU_EXCEPTION_VERSION;
    out->kind        = kind;

#if defined(__x86_64__)
    EFI_SYSTEM_CONTEXT_X64 *ctx = sc.SystemContextX64;
    out->arch            = AXL_CPU_ARCH_X64;
    /* CR2 is only meaningful for #PF — for any other kind it
       carries whatever CR2 had at fault time (stale from the
       last page fault on this CPU, or zero if none). Gate it so
       the public-API field semantics match the header doc. */
    out->fault_address   = (kind == AXL_CPU_EXCEPTION_PAGE_FAULT) ? ctx->Cr2 : 0;
    out->instruction_ptr = ctx->Rip;
    out->stack_ptr       = ctx->Rsp;
    out->frame_ptr       = ctx->Rbp;
    out->error_code      = ctx->ExceptionData;
    (void)efi_type;

    out->regs.x64.rax    = ctx->Rax;
    out->regs.x64.rbx    = ctx->Rbx;
    out->regs.x64.rcx    = ctx->Rcx;
    out->regs.x64.rdx    = ctx->Rdx;
    out->regs.x64.rsi    = ctx->Rsi;
    out->regs.x64.rdi    = ctx->Rdi;
    out->regs.x64.rbp    = ctx->Rbp;
    out->regs.x64.rsp    = ctx->Rsp;
    out->regs.x64.r8     = ctx->R8;
    out->regs.x64.r9     = ctx->R9;
    out->regs.x64.r10    = ctx->R10;
    out->regs.x64.r11    = ctx->R11;
    out->regs.x64.r12    = ctx->R12;
    out->regs.x64.r13    = ctx->R13;
    out->regs.x64.r14    = ctx->R14;
    out->regs.x64.r15    = ctx->R15;
    out->regs.x64.rip    = ctx->Rip;
    out->regs.x64.rflags = ctx->Rflags;
    out->regs.x64.cr2    = (kind == AXL_CPU_EXCEPTION_PAGE_FAULT) ? ctx->Cr2 : 0;
#elif defined(__aarch64__)
    EFI_SYSTEM_CONTEXT_AARCH64 *ctx = sc.SystemContextAArch64;
    out->arch            = AXL_CPU_ARCH_AA64;
    out->fault_address   = ctx->FAR;
    out->instruction_ptr = ctx->ELR;
    out->stack_ptr       = ctx->SP;
    out->frame_ptr       = ctx->FP;
    out->error_code      = ctx->ESR;
    (void)efi_type;

    /* X0..X28 are flat fields in EFI_SYSTEM_CONTEXT_AARCH64;
       copy with explicit register names so a future reorganization
       of the EFI struct can be caught at compile time. */
    out->regs.aa64.x[0]  = ctx->X0;
    out->regs.aa64.x[1]  = ctx->X1;
    out->regs.aa64.x[2]  = ctx->X2;
    out->regs.aa64.x[3]  = ctx->X3;
    out->regs.aa64.x[4]  = ctx->X4;
    out->regs.aa64.x[5]  = ctx->X5;
    out->regs.aa64.x[6]  = ctx->X6;
    out->regs.aa64.x[7]  = ctx->X7;
    out->regs.aa64.x[8]  = ctx->X8;
    out->regs.aa64.x[9]  = ctx->X9;
    out->regs.aa64.x[10] = ctx->X10;
    out->regs.aa64.x[11] = ctx->X11;
    out->regs.aa64.x[12] = ctx->X12;
    out->regs.aa64.x[13] = ctx->X13;
    out->regs.aa64.x[14] = ctx->X14;
    out->regs.aa64.x[15] = ctx->X15;
    out->regs.aa64.x[16] = ctx->X16;
    out->regs.aa64.x[17] = ctx->X17;
    out->regs.aa64.x[18] = ctx->X18;
    out->regs.aa64.x[19] = ctx->X19;
    out->regs.aa64.x[20] = ctx->X20;
    out->regs.aa64.x[21] = ctx->X21;
    out->regs.aa64.x[22] = ctx->X22;
    out->regs.aa64.x[23] = ctx->X23;
    out->regs.aa64.x[24] = ctx->X24;
    out->regs.aa64.x[25] = ctx->X25;
    out->regs.aa64.x[26] = ctx->X26;
    out->regs.aa64.x[27] = ctx->X27;
    out->regs.aa64.x[28] = ctx->X28;
    out->regs.aa64.x[29] = ctx->FP;   /* X29 */
    out->regs.aa64.x[30] = ctx->LR;   /* X30 */
    out->regs.aa64.sp    = ctx->SP;
    out->regs.aa64.elr   = ctx->ELR;
    out->regs.aa64.spsr  = ctx->SPSR;
    out->regs.aa64.esr   = ctx->ESR;
    out->regs.aa64.far   = ctx->FAR;
#else
    (void)sc;
    (void)efi_type;
    out->arch = AXL_CPU_ARCH_UNKNOWN;
#endif
}

// ---------------------------------------------------------------------------
// Generic thunk — firmware-facing entry point
// ---------------------------------------------------------------------------

/* Must carry EFIAPI: the real EDK2 EFI_CPU_INTERRUPT_HANDLER typedef
   (MdePkg/Include/Protocol/Cpu.h) is EFIAPI, and CpuDxe invokes the
   registered handler with that convention. On x86_64 EFIAPI is ms_abi
   (args in RCX/RDX) vs the System V default (RDI/RSI); without EFIAPI
   the thunk reads InterruptType/SystemContext from the wrong registers,
   mis-decodes the kind, returns early, and CommonExceptionHandler then
   IRETs to the faulting instruction — re-firing the exception forever.
   (AXL's generated cpu-arch.h drops the EFIAPI from the typedef; this
   attribute restores the correct ABI at the one call site that matters.) */
static void EFIAPI
cpu_exception_thunk(
    EFI_EXCEPTION_TYPE  efi_type,
    EFI_SYSTEM_CONTEXT  sc)
{
    AxlCpuExceptionKind kind = efi_type_to_kind(efi_type);
    if (kind >= AXL_CPU_EXCEPTION_KIND_MAX) {
        /* Firmware dispatched something we never registered for —
           defensive guard; should be unreachable. */
        return;
    }
    CpuSlot *slot = &g_slots[kind];
    if (slot->cb == NULL) {
        return;
    }
    AxlCpuException exc;
    fill_exc_from_context(&exc, kind, efi_type, sc);
    slot->cb(&exc, slot->user);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_cpu_register_exception(
    AxlCpuExceptionKind kind,
    AxlCpuExceptionFn   cb,
    void               *user)
{
    if (cb == NULL || kind < 1 || kind >= AXL_CPU_EXCEPTION_KIND_MAX) {
        return AXL_ERR;
    }
    EFI_EXCEPTION_TYPE efi_type = kind_to_efi_type(kind);
    if (efi_type < 0) {
        return AXL_ERR;  /* kind not available on this arch */
    }

    EFI_CPU_ARCH_PROTOCOL *p = cpu_arch();
    if (p == NULL) {
        axl_warning("EFI_CPU_ARCH_PROTOCOL not published - "
                    "exception monitoring unavailable");
        return AXL_ERR;
    }

    /* Fill the slot BEFORE telling firmware about us — once
       RegisterInterruptHandler succeeds, the thunk can be called
       at any time, so the slot must already be valid. */
    g_slots[kind].cb   = cb;
    g_slots[kind].user = user;

    /* UEFI spec: RegisterInterruptHandler with NULL handler unhooks
       a previously-registered handler. We need that first when
       replacing, since registering a second handler over a live one
       fails with EFI_ALREADY_STARTED on some firmwares. */
    p->RegisterInterruptHandler(p, efi_type, NULL);
    EFI_STATUS status = p->RegisterInterruptHandler(p, efi_type,
                                                   cpu_exception_thunk);
    if (EFI_ERROR(status)) {
        /* Tell the firmware to release whatever it might have
           partially installed before we clear the slot, so a
           late-arriving exception can't dispatch into the thunk
           and find an empty slot (which would silently swallow
           the trap). Only after firmware-side teardown is
           guaranteed do we clear the slot. */
        p->RegisterInterruptHandler(p, efi_type, NULL);
        g_slots[kind].cb   = NULL;
        g_slots[kind].user = NULL;
        axl_warning("RegisterInterruptHandler(kind=%d) failed: 0x%lx",
                    (int)kind, (unsigned long)status);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_cpu_unregister_exception(AxlCpuExceptionKind kind)
{
    if (kind < 1 || kind >= AXL_CPU_EXCEPTION_KIND_MAX) {
        return AXL_ERR;
    }
    EFI_EXCEPTION_TYPE efi_type = kind_to_efi_type(kind);
    if (efi_type < 0) {
        return AXL_ERR;
    }

    /* Short-circuit if nothing is currently registered for this
       kind. Some firmwares return EFI_INVALID_PARAMETER when
       RegisterInterruptHandler is asked to unhook a handler that
       was never installed; doing the firmware call only when we
       know we previously installed something keeps the contract
       "unregister-an-unregistered-kind is a no-op AXL_OK" clean. */
    if (g_slots[kind].cb == NULL) {
        return AXL_OK;
    }

    EFI_CPU_ARCH_PROTOCOL *p = cpu_arch();
    if (p == NULL) {
        return AXL_ERR;
    }

    /* Tell the firmware first so the thunk can't fire after we
       clear the slot. */
    p->RegisterInterruptHandler(p, efi_type, NULL);
    g_slots[kind].cb   = NULL;
    g_slots[kind].user = NULL;
    return AXL_OK;
}

// ===================================================================
// Instruction-set feature detection + SIMD dispatch
// ===================================================================
//
// Detection runs once (CPUID on x86) and caches into a file-static
// AxlCpuFeatures.  Pure-detection paths never change CPU state; only
// axl_cpu_enable_avx() writes CR4/XCR0 — and it runs at CPL0, which a
// UEFI app always has.  SSE/SSE4 need no enabling (firmware already
// turned on XMM state for its own calling convention); AVX adds YMM
// state UEFI does not enable, hence the explicit opt-in.

static AxlCpuFeatures g_features;

/* One-shot detection state. A plain bool is not enough: axl_cpu_features()
   is reachable from an AP (an AxlTaskProc asking what it may execute), so
   publication has to be a single ordered transition rather than a
   read-modify-write any core can lose. */
enum {
    DETECT_NONE    = 0,
    DETECT_RUNNING = 1,
    DETECT_DONE    = 2
};
static volatile uint32_t g_features_state = DETECT_NONE;

/* Backstop on waiting for another core's detect() to publish. detect()
   is a handful of CPUID instructions, so any real wait is microseconds;
   this only bounds the case where the detecting core never returns. */
#define DETECT_WAIT_SPINS  1000000u

/* BSP-only memo for the per-core dispatch queries -- see the long note on
   axl_cpu_simd_tier below. Declared here because axl_cpu_enable_avx, further
   down, is the other half of the same dispatch check and shares the guard. */
static bool          g_simd_memo_valid   = false;
static bool          g_avx_memo_valid    = false;
static bool          g_avx_memo_enabled  = false;
static volatile bool g_simd_memo_retired = false;

static inline void
cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

#if defined(__x86_64__)

/* CPUID leaf/subleaf — rbx is a normal callee-saved register under the
 * x86-64 ABI (the i386-PIC ebx restriction does not apply), so the
 * "=b" constraint handles its save/restore. */
static inline void
cpuid_count(
    uint32_t  leaf,
    uint32_t  subleaf,
    uint32_t *a,
    uint32_t *b,
    uint32_t *c,
    uint32_t *d
    )
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

static inline uint64_t
read_xcr0(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static inline void
write_xcr0(
    uint64_t  val
    )
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("xsetbv" : : "a"(lo), "d"(hi), "c"(0));
}

static inline uint64_t
read_cr4(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void
write_cr4(
    uint64_t  v
    )
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v));
}

static void
detect(void)
{
    uint32_t a, b, c, d;

    /* Leaf 1 ECX/EDX: SSE family, FMA, crypto, XSAVE, AVX, RNG. */
    cpuid_count(1, 0, &a, &b, &c, &d);
    g_features.sse2      = (d & (1u << 26)) != 0;
    g_features.sse3      = (c & (1u <<  0)) != 0;
    g_features.pclmulqdq = (c & (1u <<  1)) != 0;
    g_features.ssse3     = (c & (1u <<  9)) != 0;
    g_features.fma       = (c & (1u << 12)) != 0;
    g_features.sse41     = (c & (1u << 19)) != 0;
    g_features.sse42     = (c & (1u << 20)) != 0;
    g_features.movbe     = (c & (1u << 22)) != 0;
    g_features.popcnt    = (c & (1u << 23)) != 0;
    g_features.aes       = (c & (1u << 25)) != 0;
    g_features.xsave     = (c & (1u << 26)) != 0;
    g_features.avx       = (c & (1u << 28)) != 0;
    g_features.f16c      = (c & (1u << 29)) != 0;
    g_features.rdrand    = (c & (1u << 30)) != 0;

    /* Leaf 7, subleaf 0: AVX2/AVX-512, BMI, SHA-NI, more crypto.
       Guard on the max basic leaf first. */
    uint32_t max_leaf;
    cpuid_count(0, 0, &max_leaf, &b, &c, &d);
    if (max_leaf >= 7) {
        cpuid_count(7, 0, &a, &b, &c, &d);
        g_features.bmi1       = (b & (1u <<  3)) != 0;
        g_features.avx2       = (b & (1u <<  5)) != 0;
        g_features.bmi2       = (b & (1u <<  8)) != 0;
        g_features.avx512f    = (b & (1u << 16)) != 0;
        g_features.avx512dq   = (b & (1u << 17)) != 0;
        g_features.rdseed     = (b & (1u << 18)) != 0;
        g_features.adx        = (b & (1u << 19)) != 0;
        g_features.avx512cd   = (b & (1u << 28)) != 0;
        g_features.sha        = (b & (1u << 29)) != 0;
        g_features.avx512bw   = (b & (1u << 30)) != 0;
        g_features.avx512vl   = (b & (1u << 31)) != 0;
        g_features.vaes       = (c & (1u <<  9)) != 0;
        g_features.vpclmulqdq = (c & (1u << 10)) != 0;
        g_features.avx512vnni = (c & (1u << 11)) != 0;
    }

    /* Extended leaf 0x80000001 ECX: LZCNT (ABM). */
    cpuid_count(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000001u) {
        cpuid_count(0x80000001u, 0, &a, &b, &c, &d);
        g_features.lzcnt = (c & (1u << 5)) != 0;
    }

    /* Defensive invariants: the 256/512-bit ISAs require AVX. */
    if (g_features.avx2 && !g_features.avx) {
        g_features.avx2 = false;
    }
}

/* Is AVX (YMM) state actually enabled on THIS logical processor right
   now?  CR4/XCR0 are per-CPU, so we read the live hardware state rather
   than a global flag — an AP that hasn't run the enable sequence itself
   correctly reports "not enabled" even after the BSP enabled AVX. */
static bool
avx_active_here(void)
{
    /* XGETBV #UDs unless CR4.OSXSAVE is set, so gate on it first. */
    if ((read_cr4() & (1ull << 18)) == 0) {
        return false;
    }
    return (read_xcr0() & 0x4ull) != 0;   /* XCR0 bit 2 = AVX/YMM state */
}

/* Does THIS logical processor support AVX + XSAVE, right now?

   A live CPUID rather than a read of the cached feature vector. The
   cache holds whatever core ran detect() first; on a hybrid part the
   calling core need not have the same ISA. Gating the XCR0 write on the
   cache is a fault, not a mis-report: setting XCR0.YMM (bit 2) on a core
   without AVX #GPs. One CPUID inside a function that is already writing
   control registers is not a cost worth optimising away. */
static bool
avx_supported_here(void)
{
    uint32_t a, b, c, d;

    cpuid_count(1, 0, &a, &b, &c, &d);
    return (c & (1u << 28)) != 0     /* AVX   */
        && (c & (1u << 26)) != 0;    /* XSAVE */
}

/* AVX2 on THIS logical processor.

   axl_cpu_simd_tier gates kernel dispatch on this rather than on the
   cached vector's avx2 bit. Enabling AVX state and having AVX2 are
   different questions: a core can pass avx_supported_here and still
   lack AVX2, and dispatching a 256-bit integer kernel there is a #UD. */
static bool
avx2_supported_here(void)
{
    uint32_t a, b, c, d;
    uint32_t max_leaf;

    if (!avx_supported_here()) {
        return false;
    }
    cpuid_count(0, 0, &max_leaf, &b, &c, &d);
    if (max_leaf < 7) {
        return false;
    }
    cpuid_count(7, 0, &a, &b, &c, &d);
    return (b & (1u << 5)) != 0;     /* AVX2 */
}

/* AVX-512 Foundation on THIS logical processor. Same reasoning as
   avx_supported_here; the CPUID.0xD state-component check in
   axl_cpu_enable_avx512 is complementary, not a substitute — it
   answers "can XSETBV manage this state", not "does this core have
   the ISA". */
static bool
avx512_supported_here(void)
{
    uint32_t a, b, c, d;
    uint32_t max_leaf;

    cpuid_count(0, 0, &max_leaf, &b, &c, &d);
    if (max_leaf < 7) {
        return false;
    }
    cpuid_count(7, 0, &a, &b, &c, &d);
    return (b & (1u << 16)) != 0;    /* AVX-512F */
}

bool
axl_cpu_enable_avx(void)
{
    /* Same BSP-only memo as axl_cpu_simd_tier, and for the same reason: the
       two are called together as one dispatch check, so memoising only the
       tier leaves this half paying CPUID + XGETBV per operation -- measured
       as the ENTIRE remaining cost once the tier was cached. Enabling is
       idempotent and the state is per-core and sticky, so on a core already
       enabled the answer holds until an AP dispatch retires the memo. */
    if (g_avx_memo_valid && !g_simd_memo_retired) {
        return g_avx_memo_enabled;
    }

    bool enabled;
    if (!avx_supported_here()) {
        enabled = false;
    } else if (avx_active_here()) {
        enabled = true;          /* idempotent — already enabled on this CPU */
    } else {
        /* CR4.OSXSAVE (bit 18) lets XGETBV/XSETBV run and signals OS
           support for extended state. */
        write_cr4(read_cr4() | (1ull << 18));
        /* XCR0 bits 0 (x87), 1 (SSE), 2 (AVX/YMM) — SSE must stay set
           alongside AVX or XSETBV #GPs. */
        write_xcr0(read_xcr0() | 0x7ull);
        enabled = true;
        /* The tier just CHANGED on this core: axl_cpu_simd_tier gates AVX2 on
           avx_active_here(), which was false a moment ago and is true now. A
           memo taken before this point says SSE41 and would survive the very
           transition that invalidates it -- caught by the pre-existing
           enable_avx test, which queries the tier both sides of the enable.
           The core did not change, so the AP guard does not cover this; the
           STATE changed, and only this function can change it. */
        g_simd_memo_valid = false;
    }

    if (!g_simd_memo_retired) {
        g_avx_memo_enabled = enabled;
        g_avx_memo_valid   = true;
    }
    return enabled;
}

/* Are the AVX-512 state components (opmask + ZMM_Hi256 + Hi16_ZMM)
   live on THIS logical processor?  XCR0 bits 5,6,7. */
static bool
avx512_active_here(void)
{
    if ((read_cr4() & (1ull << 18)) == 0) {
        return false;
    }
    return (read_xcr0() & 0xE0ull) == 0xE0ull;   /* bits 5,6,7 all set */
}

bool
axl_cpu_enable_avx512(void)
{
    if (!avx512_supported_here() || !avx_supported_here()) {
        return false;
    }
    /* CPUID.0xD.0:EAX advertises which XSAVE state components the
       processor can manage — require SSE(1), AVX(2), opmask(5),
       ZMM_Hi256(6), Hi16_ZMM(7) before we ask XSETBV to enable them. */
    uint32_t a, b, c, d;
    cpuid_count(0x0Du, 0, &a, &b, &c, &d);
    const uint32_t need = (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 7);
    if ((a & need) != need) {
        return false;
    }
    if (avx512_active_here()) {
        return true;   /* idempotent */
    }
    write_cr4(read_cr4() | (1ull << 18));
    /* A single XCR0 write sets x87|SSE|AVX|opmask|ZMM_Hi256|Hi16_ZMM
       (bits 0,1,2,5,6,7) atomically — XSETBV requires the AVX-512 state
       bits to be accompanied by AVX(2)+SSE(1) in the same write. */
    write_xcr0(read_xcr0() | 0xE7ull);
    return true;
}

#elif defined(__aarch64__)

static void
detect(void)
{
    /* AdvSIMD/NEON is mandatory on the ARMv8-A baseline this SDK
       targets (and the compiler already emits it). */
    g_features.neon = true;

    /* Optional ISA extensions live in the EL1-readable feature ID
       registers.  Each field is 4 bits; "absent" is 0 for the ISAR0
       crypto/CRC/atomic/dotprod fields. */
    uint64_t isar0, pfr0;
    __asm__ volatile ("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    __asm__ volatile ("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));

    uint32_t aes_f  = (uint32_t)((isar0 >>  4) & 0xF);  /* 1=AES, 2=AES+PMULL */
    uint32_t sha1_f = (uint32_t)((isar0 >>  8) & 0xF);
    uint32_t sha2_f = (uint32_t)((isar0 >> 12) & 0xF);  /* 1=SHA256, 2=+SHA512 */
    uint32_t crc_f  = (uint32_t)((isar0 >> 16) & 0xF);
    uint32_t atom_f = (uint32_t)((isar0 >> 20) & 0xF);  /* 2=LSE */
    uint32_t sha3_f = (uint32_t)((isar0 >> 32) & 0xF);
    uint32_t dp_f   = (uint32_t)((isar0 >> 44) & 0xF);

    g_features.aes_a64 = aes_f  >= 1;
    g_features.pmull   = aes_f  >= 2;
    g_features.sha1    = sha1_f >= 1;
    g_features.sha2    = sha2_f >= 1;
    g_features.sha512  = sha2_f >= 2;
    g_features.sha3    = sha3_f >= 1;
    g_features.crc32   = crc_f  >= 1;
    g_features.atomics = atom_f >= 2;
    g_features.dotprod = dp_f   >= 1;

    /* PFR0: FP[19:16] and AdvSIMD[23:20] use 0=present, 1=present+FP16,
       0xF=absent; SVE[35:32] is 1 when present. */
    uint32_t fp_f  = (uint32_t)((pfr0 >> 16) & 0xF);
    uint32_t sve_f = (uint32_t)((pfr0 >> 32) & 0xF);
    g_features.fp16 = (fp_f == 1);
    g_features.sve  = (sve_f >= 1);
}

bool
axl_cpu_enable_avx(void)
{
    return false;   /* no AVX on AArch64 */
}

bool
axl_cpu_enable_avx512(void)
{
    return false;   /* no AVX-512 on AArch64 */
}

static bool
avx_active_here(void)
{
    return false;   /* no AVX on AArch64 */
}

static bool
avx2_supported_here(void)
{
    return false;   /* no AVX2 outside x86 */
}

#else

static void
detect(void)
{
}

bool
axl_cpu_enable_avx(void)
{
    return false;
}

bool
axl_cpu_enable_avx512(void)
{
    return false;
}

static bool
avx_active_here(void)
{
    return false;
}

static bool
avx2_supported_here(void)
{
    return false;   /* no AVX2 outside x86 */
}

#endif

const AxlCpuFeatures *
axl_cpu_features(void)
{
    /* Fast path still needs the acquire. `&g_features` is a fixed static
       address, so there is no address dependency carrying the ordering:
       nothing stops the compiler hoisting a plain read of g_features.*
       above this volatile test (volatile orders volatile accesses against
       each other, not against ordinary ones), and nothing stops aarch64
       reordering the two loads in hardware. */
    if (__atomic_load_n(&g_features_state, __ATOMIC_ACQUIRE) == DETECT_DONE) {
        return &g_features;
    }

    /* Exactly one core runs detect(). The plain `if (!done) { detect();
       done = true; }` this replaces was not safe to call from an AP: two
       cores could run detect() concurrently, and a third could observe
       done == true before the field writes landed. Worse on a hybrid
       part, where two cores racing do not even compute the same answer,
       so an interleaving could publish a vector no single core has. */
    if (__sync_bool_compare_and_swap(&g_features_state,
                                     DETECT_NONE, DETECT_RUNNING)) {
        detect();
        __sync_synchronize();      /* release — fields visible before the flag */
        g_features_state = DETECT_DONE;
    } else {
        /* Bounded. Every other cross-CPU wait in the SDK has a ceiling,
           and this one can wedge the BSP with no diagnostic: if the core
           that won the CAS is terminated inside detect() -- which is
           exactly what firmware does to a straggling AP -- the state is
           stuck at DETECT_RUNNING forever and nothing ever resets it.
           On expiry, detect locally: the result is idempotent and the
           caller only ever needed an answer, not the shared cache. */
        size_t spins;

        for (spins = 0; spins < DETECT_WAIT_SPINS; spins++) {
            if (__atomic_load_n(&g_features_state,
                                __ATOMIC_ACQUIRE) == DETECT_DONE) {
                return &g_features;
            }
            cpu_relax();
        }

        axl_warning("CPU feature detection never published; detecting "
                    "locally");
        detect();
        __sync_synchronize();
    }

    return &g_features;
}

/* Per-call CPUID is the honest answer and, in a hot path, a ruinous one.
 *
 * axl_cpu_simd_tier() must describe the CALLING core -- that is the whole
 * reason it exists rather than reading the cached feature vector -- so it
 * runs live CPUID. Under virtualisation every CPUID is a VM exit, and a
 * consumer measured ~1.5s per keystroke from it even after the dispatch was
 * hoisted out of the row loop to once per operation: their compositor issues
 * on the order of 10^5 small blends per repaint, so "once per operation" is
 * nowhere near once per screen.
 *
 * The memo below is exact rather than approximate, and rests on a property
 * UEFI actually guarantees: with no AP ever dispatched, the BSP is the only
 * core executing AXL code, and the BSP cannot migrate -- there is no
 * scheduler and no preemption in boot services. So until something starts an
 * AP, one cached answer IS this core's answer, and serving it costs no CPUID
 * at all.
 *
 * The moment an AP is dispatched that stops holding: two cores with possibly
 * different ISAs are live, and a shared answer could hand an E-core the
 * P-core's AVX2 verdict -- the #UD that 7e9b5b97 fixed and that a consumer
 * has since reproduced empirically. From that point the memo is retired
 * permanently and every caller pays the live query again. Permanently,
 * because AXL's AP workers are long-lived: there is no point at which they
 * are known to be finished, so re-enabling could only ever be a guess.
 */
static AxlSimdTier  g_simd_memo_tier    = AXL_SIMD_SCALAR;

void
axl_cpu_simd_memo_invalidate(void)
{
    g_simd_memo_retired = true;
    g_simd_memo_valid   = false;
    g_avx_memo_valid    = false;
}

AxlSimdTier
axl_cpu_simd_tier(void)
{
    if (g_simd_memo_valid && !g_simd_memo_retired) {
        return g_simd_memo_tier;
    }

    const AxlCpuFeatures *f = axl_cpu_features();
    /* avx2_supported_here, not f->avx2: the cached vector describes the
       core that ran detection first. This function decides which
       instructions a caller will execute, so it has to answer for the
       core asking -- on a hybrid part the two disagree and the cached
       answer dispatches a 256-bit kernel into a #UD. */
    AxlSimdTier tier;
    if (avx2_supported_here() && avx_active_here()) {
        tier = AXL_SIMD_AVX2;
    } else if (f->sse41) {
        tier = AXL_SIMD_SSE41;
    } else if (f->sse2 || f->neon) {
        tier = AXL_SIMD_BASELINE;
    } else {
        tier = AXL_SIMD_SCALAR;
    }

    /* Only memoise while the BSP is provably the only core running AXL
       code. After an AP dispatch this stays retired. */
    if (!g_simd_memo_retired) {
        g_simd_memo_tier  = tier;
        g_simd_memo_valid = true;
    }
    return tier;
}

// ---------------------------------------------------------------------------
// EFI_MP_SERVICES_PROTOCOL access (lazy locate + cache)
// ---------------------------------------------------------------------------

static EFI_MP_SERVICES_PROTOCOL *g_mp_services = NULL;

/* Lazy-cache `EFI_MP_SERVICES_PROTOCOL`. Optional and commonly absent
   on single-processor platforms; a NULL return is the documented
   "no enumeration source" case, not an error. */
static EFI_MP_SERVICES_PROTOCOL *
mp_services(void)
{
    if (g_mp_services != NULL) {
        return g_mp_services;
    }
    EFI_GUID guid = gEfiMpServicesProtocolGuid;
    EFI_MP_SERVICES_PROTOCOL *p = NULL;
    EFI_STATUS status = axl_bs()->LocateProtocol(&guid, NULL, (void **)&p);
    if (EFI_ERROR(status) || p == NULL) {
        return NULL;
    }
    g_mp_services = p;
    return g_mp_services;
}

int
axl_cpu_topology(
    size_t           *total,
    size_t           *enabled,
    AxlCpuProcessor  *out,
    size_t            out_cap,
    size_t           *out_n
    )
{
    if (out_n != NULL) {
        *out_n = 0;
    }

    EFI_MP_SERVICES_PROTOCOL *mp = mp_services();
    if (mp == NULL) {
        /* Uniprocessor floor: no enumeration source, but the caller is
           by definition running on at least the BSP. Report 1/1 and
           write no per-processor entry (no status to characterize). */
        if (total != NULL) {
            *total = 1;
        }
        if (enabled != NULL) {
            *enabled = 1;
        }
        return AXL_OK;
    }

    UINTN num_proc = 0;
    UINTN num_enabled = 0;
    EFI_STATUS status = mp->GetNumberOfProcessors(mp, &num_proc, &num_enabled);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    if (total != NULL) {
        *total = (size_t)num_proc;
    }
    if (enabled != NULL) {
        *enabled = (size_t)num_enabled;
    }

    if (out == NULL || out_cap == 0) {
        return AXL_OK;
    }

    size_t write = ((size_t)num_proc < out_cap) ? (size_t)num_proc : out_cap;
    for (size_t i = 0; i < write; i++) {
        EFI_PROCESSOR_INFORMATION info;
        AxlCpuProcessor *p = &out[i];

        status = mp->GetProcessorInfo(mp, (UINTN)i, &info);
        if (EFI_ERROR(status)) {
            /* Keep the index aligned with the processor number: a slot
               we could not read is written zeroed (not enabled, not
               healthy) rather than skipped. */
            p->package = 0;
            p->core = 0;
            p->thread = 0;
            p->bsp = false;
            p->enabled = false;
            p->healthy = false;
            continue;
        }

        p->package = info.Location.Package;
        p->core = info.Location.Core;
        p->thread = info.Location.Thread;
        p->bsp = (info.StatusFlag & PROCESSOR_AS_BSP_BIT) != 0;
        p->enabled = (info.StatusFlag & PROCESSOR_ENABLED_BIT) != 0;
        p->healthy = (info.StatusFlag & PROCESSOR_HEALTH_STATUS_BIT) != 0;
    }

    if (out_n != NULL) {
        *out_n = write;
    }
    return AXL_OK;
}
