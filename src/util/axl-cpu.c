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
    out->arch = 0;
#endif
}

// ---------------------------------------------------------------------------
// Generic thunk — firmware-facing entry point
// ---------------------------------------------------------------------------

/* Signature matches EFI_CPU_INTERRUPT_HANDLER — note no EFIAPI:
   the spec's typedef declares this callback without it (unlike
   the EFI_CPU_ARCH_PROTOCOL's vtable methods, which do carry
   EFIAPI). */
static void
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
        axl_warning("EFI_CPU_ARCH_PROTOCOL not published — "
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
    (void)p->RegisterInterruptHandler(p, efi_type, NULL);
    EFI_STATUS status = p->RegisterInterruptHandler(p, efi_type,
                                                   cpu_exception_thunk);
    if (EFI_ERROR(status)) {
        /* Tell the firmware to release whatever it might have
           partially installed before we clear the slot, so a
           late-arriving exception can't dispatch into the thunk
           and find an empty slot (which would silently swallow
           the trap). Only after firmware-side teardown is
           guaranteed do we clear the slot. */
        (void)p->RegisterInterruptHandler(p, efi_type, NULL);
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
    (void)p->RegisterInterruptHandler(p, efi_type, NULL);
    g_slots[kind].cb   = NULL;
    g_slots[kind].user = NULL;
    return AXL_OK;
}
