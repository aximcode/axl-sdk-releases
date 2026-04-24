/** @file generated/cpu-arch.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_CPU_ARCH_H
#define AXL_UEFI_GEN_CPU_ARCH_H

#include "types.h"
#include "status.h"

typedef INTN EFI_EXCEPTION_TYPE;

#define EXCEPT_X64_DIVIDE_ERROR          0

#define EXCEPT_X64_DEBUG                 1

#define EXCEPT_X64_NMI                   2

#define EXCEPT_X64_BREAKPOINT            3

#define EXCEPT_X64_OVERFLOW              4

#define EXCEPT_X64_BOUND                 5

#define EXCEPT_X64_INVALID_OPCODE        6

#define EXCEPT_X64_DOUBLE_FAULT          8

#define EXCEPT_X64_INVALID_TSS           10

#define EXCEPT_X64_SEG_NOT_PRESENT       11

#define EXCEPT_X64_STACK_FAULT           12

#define EXCEPT_X64_GP_FAULT              13

#define EXCEPT_X64_PAGE_FAULT            14

#define EXCEPT_X64_FP_ERROR              16

#define EXCEPT_X64_ALIGNMENT_CHECK       17

#define EXCEPT_X64_MACHINE_CHECK         18

#define EXCEPT_X64_SIMD                  19

#define EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS 0

#define EXCEPT_AARCH64_IRQ 1

#define EXCEPT_AARCH64_FIQ 2

#define EXCEPT_AARCH64_SERROR 3

#define MAX_AARCH64_EXCEPTION EXCEPT_AARCH64_SERROR

typedef struct _EFI_SYSTEM_CONTEXT_X64 {
   UINT64 ExceptionData;   // ExceptionData is
                           // additional data pushed
                           // on the stack by some
                           // types of x64 64-bit
                           // mode exceptions
   void  *FxSaveState;
   UINT64                  Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
   UINT64                  Cr0, Cr1 /* Reserved */, Cr2, Cr3, Cr4, Cr8;
   UINT64                  Rflags;
   UINT64                  Ldtr, Tr;
   UINT64                  Gdtr[2], Idtr[2];
   UINT64                  Rip;
   UINT64                  Gs, Fs, Es, Ds, Cs, Ss;
   UINT64                  Rdi, Rsi, Rbp, Rsp, Rbx, Rdx, Rcx, Rax;
   UINT64                  R8, R9, R10, R11, R12, R13, R14, R15;
} EFI_SYSTEM_CONTEXT_X64;

typedef struct _EFI_SYSTEM_CONTEXT_AARCH64 {
// General Purpose Registers
   UINT64 X0;
   UINT64 X1;
   UINT64 X2;
   UINT64 X3;
   UINT64 X4;
   UINT64 X5;
   UINT64 X6;
   UINT64 X7;
   UINT64 X8;
   UINT64 X9;
   UINT64 X10;
   UINT64 X11;
   UINT64 X12;
   UINT64 X13;
   UINT64 X14;
   UINT64 X15;
   UINT64 X16;
   UINT64 X17;
   UINT64 X18;
   UINT64 X19;
   UINT64 X20;
   UINT64 X21;
   UINT64 X22;
   UINT64 X23;
   UINT64 X24;
   UINT64 X25;
   UINT64 X26;
   UINT64 X27;
   UINT64 X28;
   UINT64 FP; // x29 - Frame Pointer
   UINT64 LR; // x30 - Link Register
   UINT64 SP; // x31 - Stack Pointer
              // FP/SIMD Registers
   UINT64 V0[2];
   UINT64 V1[2];
   UINT64 V2[2];
   UINT64 V3[2];
   UINT64 V4[2];
   UINT64 V5[2];
   UINT64 V6[2];
   UINT64 V7[2];
   UINT64 V8[2];
   UINT64 V9[2];
   UINT64 V10[2];
   UINT64 V11[2];
   UINT64 V12[2];
   UINT64 V13[2];
   UINT64 V14[2];
   UINT64 V15[2];
   UINT64 V16[2];
   UINT64 V17[2];
   UINT64 V18[2];
   UINT64 V19[2];
   UINT64 V20[2];
   UINT64 V21[2];
   UINT64 V22[2];
   UINT64 V23[2];
   UINT64 V24[2];
   UINT64 V25[2];
   UINT64 V26[2];
   UINT64 V27[2];
   UINT64 V28[2];
   UINT64 V29[2];
   UINT64 V30[2];
   UINT64 V31[2];
   UINT64 ELR;       // Exception Link Register
   UINT64 SPSR;      // Saved Processor Status Register
   UINT64 FPSR;      // Floating Point Status Register
   UINT64 ESR;       // Exception syndrome register
   UINT64 FAR;       // Fault Address Register

} EFI_SYSTEM_CONTEXT_AARCH64;

typedef union _EFI_SYSTEM_CONTEXT {
    void  *SystemContextEbc;
    void  *SystemContextIa32;
    EFI_SYSTEM_CONTEXT_X64         *SystemContextX64;
    void  *SystemContextIpf;
    void  *SystemContextArm;
    EFI_SYSTEM_CONTEXT_AARCH64     *SystemContextAArch64;
    void  *SystemContextRiscV32;
    void  *SystemContextRiscV64;
    void  *SystemContextRiscv128;
    void  *SystemContextLongArch64;
 } EFI_SYSTEM_CONTEXT;

typedef enum {
  EfiCpuInit,
  EfiCpuMaxInitType
} EFI_CPU_INIT_TYPE;

typedef
VOID
(*EFI_CPU_INTERRUPT_HANDLER) (
  IN EFI_EXCEPTION_TYPE  InterruptType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_FLUSH_DATA_CACHE) (
  IN CONST EFI_CPU_ARCH_PROTOCOL   *This,
  IN EFI_PHYSICAL_ADDRESS          Start,
  IN UINT64                        Length,
  IN VOID *FlushType
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_ENABLE_INTERRUPT) (
  IN CONST EFI_CPU_ARCH_PROTOCOL  *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_DISABLE_INTERRUPT) (
  IN CONST EFI_CPU_ARCH_PROTOCOL   *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_GET_INTERRUPT_STATE) (
  IN CONST EFI_CPU_ARCH_PROTOCOL   *This,
  OUT BOOLEAN                      *State
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_INIT) (
  IN CONST EFI_CPU_ARCH_PROTOCOL   *This,
  IN EFI_CPU_INIT_TYPE             InitType
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_REGISTER_INTERRUPT_HANDLER) (
  IN CONST EFI_CPU_ARCH_PROTOCOL       *This,
  IN EFI_EXCEPTION_TYPE                InterruptType,
  IN EFI_CPU_INTERRUPT_HANDLER         InterruptHandler
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_GET_TIMER_VALUE) (
  IN CONST EFI_CPU_ARCH_PROTOCOL     *This,
  IN UINT32                          TimerIndex,
  OUT UINT64                         *TimerValue,
  OUT UINT64                         *TimerPeriod OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_SET_MEMORY_ATTRIBUTES) (
  IN CONST EFI_CPU_ARCH_PROTOCOL   *This,
  IN EFI_PHYSICAL_ADDRESS          BaseAddress,
  IN UINT64                        Length,
  IN UINT64                        Attributes
  );

typedef struct _EFI_CPU_ARCH_PROTOCOL {
  EFI_CPU_FLUSH_DATA_CACHE            FlushDataCache;
  EFI_CPU_ENABLE_INTERRUPT            EnableInterrupt;
  EFI_CPU_DISABLE_INTERRUPT           DisableInterrupt;
  EFI_CPU_GET_INTERRUPT_STATE         GetInterruptState;
  EFI_CPU_INIT                        Init;
  EFI_CPU_REGISTER_INTERRUPT_HANDLER  RegisterInterruptHandler;
  EFI_CPU_GET_TIMER_VALUE             GetTimerValue;
  void  *SetMemoryAttributes;
  UINT32                              NumberOfTimers;
  UINT32                              DmaBufferAlignment;
} EFI_CPU_ARCH_PROTOCOL;


#endif /* AXL_UEFI_GEN_CPU_ARCH_H */
