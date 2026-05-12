/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-cpu.h:
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
    AXL_CPU_EXCEPTION_DIVIDE_ERROR    = 1,   ///< x64 #DE
    AXL_CPU_EXCEPTION_DEBUG           = 2,   ///< x64 #DB
    AXL_CPU_EXCEPTION_OVERFLOW        = 3,   ///< x64 #OF
    AXL_CPU_EXCEPTION_BOUND           = 4,   ///< x64 #BR
    AXL_CPU_EXCEPTION_INVALID_OPCODE  = 5,   ///< x64 #UD
    AXL_CPU_EXCEPTION_DEVICE_NA       = 6,   ///< x64 #NM
    AXL_CPU_EXCEPTION_DOUBLE_FAULT    = 7,   ///< x64 #DF
    AXL_CPU_EXCEPTION_SEGMENT_NP      = 8,   ///< x64 #NP
    AXL_CPU_EXCEPTION_STACK_FAULT     = 9,   ///< x64 #SS
    AXL_CPU_EXCEPTION_GP_FAULT        = 10,  ///< x64 #GP
    AXL_CPU_EXCEPTION_PAGE_FAULT      = 11,  ///< x64 #PF
    AXL_CPU_EXCEPTION_FP_ERROR        = 12,  ///< x64 #MF
    AXL_CPU_EXCEPTION_ALIGNMENT_CHECK = 13,  ///< x64 #AC
    AXL_CPU_EXCEPTION_SIMD            = 14,  ///< x64 #XM

    /* AArch64 exception kinds — register on aa64 only.
       aa64 collapses x64-style traps into broad classes; the
       SYNCHRONOUS umbrella covers what x64 splits across #UD /
       #GP / #PF / #AC. Consumers that want finer detail on aa64
       inspect `ESR_EL1.EC` inside the callback. */
    AXL_CPU_EXCEPTION_SYNCHRONOUS     = 15,  ///< aa64 synchronous-exception umbrella
    AXL_CPU_EXCEPTION_SERROR          = 16,  ///< aa64 SError

    AXL_CPU_EXCEPTION_KIND_MAX        = 17,  ///< exclusive upper bound
} AxlCpuExceptionKind;

// ---------------------------------------------------------------------------
// Architecture tag (for the register-snapshot union below)
// ---------------------------------------------------------------------------

#define AXL_CPU_ARCH_X64    1
#define AXL_CPU_ARCH_AA64   2

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
 *    (x64 #PF = CR2; aa64 sync = FAR_EL1). Zero for kinds where
 *    no fault address applies (#DE, #GP without memory, #UD, ...).
 *  - `error_code` — exception-specific:
 *    x64 #PF / #GP / #DF / #NP / #SS / #AC carry an error code
 *    pushed by the CPU; for other kinds, 0.
 *    aa64 carries `ESR_EL1` here so consumers can recover
 *    finer-grained classification on synchronous exceptions
 *    (EC field) without a separate accessor.
 */
typedef struct {
    uint32_t            struct_size;      ///< sizeof(AxlCpuException) as written by the SDK
    uint32_t            version;          ///< AXL_CPU_EXCEPTION_VERSION at emit time
    AxlCpuExceptionKind kind;
    int                 arch;             ///< AXL_CPU_ARCH_X64 / _AA64
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
);

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

#ifdef __cplusplus
}
#endif

#endif /* AXL_CPU_H */
