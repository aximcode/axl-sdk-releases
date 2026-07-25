/** @file axl-crashrecord.h
    UEFI Crash Record -- binary format stored in NVRAM.
    The contract shared by the CrashHandler driver (writer), the
    host-side rsod-decode reader, and any consumer of the CrashDump<N>
    NVRAM variables. Standard C types only; safe to include from
    on-device and host code alike.
**/

#ifndef AXL_CRASHRECORD_H
#define AXL_CRASHRECORD_H

#include <stdint.h>

/* Vendor GUID for all CrashHandler NVRAM variables */
#define AXL_CRASH_HANDLER_VARIABLE_GUID \
    { 0x7c4e5d8a, 0x3f2b, 0x4a1e, { 0x9d, 0x6c, 0x8b, 0x5a, 0x0e, 0x3f, 0x7c, 0x12 } }

/* Sentinel protocol GUID (installed to detect re-load) and the
   axl-sdk name binding used at register/find call sites. */
#define AXL_CRASH_HANDLER_PROTOCOL_GUID \
    { 0xa1b2c3d4, 0xe5f6, 0x4718, { 0x89, 0x0a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56 } }
#define AXL_CRASH_HANDLER_PROTOCOL_NAME "crash-handler-sentinel"

/* NVRAM key names. Stored in axl-sdk's "crashdump" namespace,
   addressed by UTF-8 keys (axl_nvstore_set/get convert to UCS-2
   internally for the firmware-side wire format). */
#define AXL_CRASH_DUMP_VAR_PREFIX    "CrashDump"   /* CrashDump0, CrashDump1, CrashDump2 */
#define AXL_CRASH_DUMP_IDX_VAR       "CrashDumpIdx"
#define AXL_CRASH_DUMP_SLOTS         3

/* Record constants */
#define AXL_CRASH_RECORD_MAGIC       0x48535243  /* "CRSH" */
#define AXL_CRASH_RECORD_VERSION     1

#define AXL_CRASH_ARCH_X64           0
#define AXL_CRASH_ARCH_AARCH64       1

#define AXL_CRASH_MAX_IMAGES         32
#define AXL_CRASH_MAX_FRAMES         16
#define AXL_CRASH_IMAGE_NAME_LEN     48

/* ------------------------------------------------------------------------ */
/* Binary record layout                                                     */
/* ------------------------------------------------------------------------ */

#pragma pack(1)

typedef struct {
    uint32_t magic;            /* AXL_CRASH_RECORD_MAGIC */
    uint16_t version;          /* AXL_CRASH_RECORD_VERSION */
    uint8_t  arch;             /* AXL_CRASH_ARCH_X64 or AXL_CRASH_ARCH_AARCH64 */
    uint8_t  reserved;
    uint32_t exception_type;
    uint64_t timestamp;        /* EFI_TIME packed as minutes since 2000, or 0 */
    uint32_t image_count;      /* Number of AxlCrashImageEntry following registers */
    uint32_t frame_count;      /* Number of uint64_t return addresses following images */
} AxlCrashRecordHeader;           /* 32 bytes */

typedef struct {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rflags;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t cr2;              /* Page fault linear address */
    uint64_t exception_data;   /* Error code pushed by CPU */
} AxlCrashRegsX64;                /* 160 bytes */

typedef struct {
    uint64_t elr;              /* Exception Link Register (faulting address) */
    uint64_t sp;
    uint64_t fp;               /* X29 */
    uint64_t lr;               /* X30 */
    uint64_t x0;
    uint64_t x1;
    uint64_t x2;
    uint64_t x3;
    uint64_t x4;
    uint64_t x5;
    uint64_t x6;
    uint64_t x7;
    uint64_t x8;
    uint64_t x9;
    uint64_t x10;
    uint64_t x11;
    uint64_t x12;
    uint64_t x13;
    uint64_t x14;
    uint64_t x15;
    uint64_t x16;
    uint64_t x17;
    uint64_t x18;
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t esr;              /* Exception Syndrome Register */
    uint64_t far_el;           /* Fault Address Register */
    uint64_t spsr;             /* Saved Processor Status Register */
} AxlCrashRegsAarch64;            /* 288 bytes */

typedef struct {
    uint64_t base;
    uint64_t size;
    char     name[AXL_CRASH_IMAGE_NAME_LEN];
} AxlCrashImageEntry;             /* 64 bytes */

/* .sym file format for optional function name resolution */
#define AXL_SYM_MAGIC  0x4D595300  /* "SYM\0" */

typedef struct {
    uint32_t magic;
    uint32_t entry_count;
} AxlSymFileHeader;

typedef struct {
    uint32_t offset;           /* Relative to image base */
    char     name[60];         /* Null-terminated function name */
} AxlSymEntry;                    /* 64 bytes */

#pragma pack()

/* ------------------------------------------------------------------------ */
/* Record layout in NVRAM:                                                  */
/*   AxlCrashRecordHeader                                                      */
/*   AxlCrashRegsX64 or AxlCrashRegsAarch64                                       */
/*   AxlCrashImageEntry  [0]           (faulting image, or zeroed if unknown)  */
/*   AxlCrashImageEntry  [1..image_count-1]  (loaded image table)             */
/*   uint64_t         [0..frame_count-1]  (stack frame return addresses)   */
/* ------------------------------------------------------------------------ */

/* Maximum record size:
   header(32) + regs(288) + faulting_image(64) + table(32*64) + frames(16*8)
   = 32 + 288 + 64 + 2048 + 128 = 2560 */
#define AXL_CRASH_RECORD_MAX_SIZE  2560

#endif /* AXL_CRASHRECORD_H */
