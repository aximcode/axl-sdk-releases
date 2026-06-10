/** @file exception.c
    CPU exception handler -- captures crash data to NVRAM.

    Runs in exception context. Only allocation-free SDK calls
    are used: axl_nvstore_{get,set} (stack-buffer UCS-2
    conversion), axl_time_realtime (wraps gRT->GetTime),
    axl_printf (best effort). No heap allocation; no firmware-
    type names appear in consumer code.

    The handler signature is AxlCpuExceptionFn — axl-sdk has
    already translated EFI_SYSTEM_CONTEXT into AxlCpuException
    before we see it.
**/

#include "crashhandler.h"

static const char *
get_exception_name(AxlCpuExceptionKind kind)
{
    switch (kind) {
    case AXL_CPU_EXCEPTION_DIVIDE_ERROR:    return "#DE (Divide Error)";
    case AXL_CPU_EXCEPTION_DEBUG:           return "#DB (Debug)";
    case AXL_CPU_EXCEPTION_OVERFLOW:        return "#OF (Overflow)";
    case AXL_CPU_EXCEPTION_BOUND:           return "#BR (Bound Range)";
    case AXL_CPU_EXCEPTION_INVALID_OPCODE:  return "#UD (Invalid Opcode)";
    case AXL_CPU_EXCEPTION_DEVICE_NA:       return "#NM (Device Not Available)";
    case AXL_CPU_EXCEPTION_DOUBLE_FAULT:    return "#DF (Double Fault)";
    case AXL_CPU_EXCEPTION_SEGMENT_NP:      return "#NP (Segment Not Present)";
    case AXL_CPU_EXCEPTION_STACK_FAULT:     return "#SS (Stack Fault)";
    case AXL_CPU_EXCEPTION_GP_FAULT:        return "#GP (General Protection)";
    case AXL_CPU_EXCEPTION_PAGE_FAULT:      return "#PF (Page Fault)";
    case AXL_CPU_EXCEPTION_FP_ERROR:        return "#MF (FPU Error)";
    case AXL_CPU_EXCEPTION_ALIGNMENT_CHECK: return "#AC (Alignment Check)";
    case AXL_CPU_EXCEPTION_SIMD:            return "#XM (SIMD Exception)";
    case AXL_CPU_EXCEPTION_SYNCHRONOUS:     return "Synchronous";
    case AXL_CPU_EXCEPTION_SERROR:          return "SError";
    case AXL_CPU_EXCEPTION_KIND_MAX:        return "Unknown";
    }
    return "Unknown";
}

/**
 * Extract the basename from a UTF-8 path: "fs0:\\drivers\\foo.efi" → "foo.efi"
 */
static void
copy_basename(const char *utf8_path, char *out, size_t out_size)
{
    if (utf8_path == NULL || out_size == 0) {
        if (out_size > 0) { out[0] = '\0'; }
        return;
    }
    const char *base = utf8_path;
    for (const char *p = utf8_path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    axl_strlcpy(out, *base != '\0' ? base : "Unknown", out_size);
}

static int
collect_loaded_image(const AxlImageInfo *info, void *ctx)
{
    (void)ctx;
    if (g_image_count >= CRASH_MAX_IMAGES) {
        return 1;  /* stop early — table full */
    }
    g_image_table[g_image_count].base = (uint64_t)info->base;
    g_image_table[g_image_count].size = info->size;
    copy_basename(info->path,
                  g_image_table[g_image_count].name,
                  CRASH_IMAGE_NAME_LEN);
    g_image_count++;
    return 0;
}

/**
 * Snapshot all loaded images into the pre-allocated g_image_table.
 * Called during driver init (safe heap context).
 */
void
snapshot_loaded_images(void)
{
    g_image_count = 0;
    (void)axl_image_enumerate(collect_loaded_image, NULL);
}

/**
 * Find which loaded image contains the given address.
 * Returns the image entry index, or -1 if not found.
 */
static int
find_image_by_address(uint64_t address)
{
    for (uint32_t i = 0; i < g_image_count; i++) {
        if (address >= g_image_table[i].base &&
            address < g_image_table[i].base + g_image_table[i].size) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Walk the frame pointer chain for a stack trace.
 * Returns the number of frames captured.
 */
static uint32_t
walk_stack_frames(
    uint64_t  frame_pointer,
    uint64_t  stack_pointer,
    uint64_t *frames,
    uint32_t  max_frames)
{
    uint32_t  count;
    uint64_t *fp_ptr;
    uint64_t  ret_addr;
    uint64_t  next_fp;

    count = 0;

    /* Validate initial frame pointer */
    if (frame_pointer == 0 || frame_pointer < stack_pointer) {
        return 0;
    }

    fp_ptr = (uint64_t *)(UINTN)frame_pointer;

    while (count < max_frames) {
        /* Read return address and next frame pointer
           X64:     [RBP+0] = saved RBP, [RBP+8] = return address
           AARCH64: [FP+0]  = saved FP,  [FP+8]  = saved LR */
        next_fp  = fp_ptr[0];
        ret_addr = fp_ptr[1];

        /* Validate return address (must be in a reasonable range) */
        if (ret_addr == 0 || ret_addr > 0x0000FFFFFFFFFFFFULL) {
            break;
        }

        frames[count++] = ret_addr;

        /* Validate next frame pointer (must increase for stack growth down) */
        if (next_fp == 0 || next_fp <= (uint64_t)(UINTN)fp_ptr) {
            break;
        }

        fp_ptr = (uint64_t *)(UINTN)next_fp;
    }

    return count;
}

/**
 * CPU exception handler -- captures crash state to NVRAM.
 *
 * Signature is `AxlCpuExceptionFn` — axl-sdk has already
 * translated EFI_SYSTEM_CONTEXT into the typed AxlCpuException
 * before we see it. No EFI types referenced.
 */
void
crash_exception_handler(
    const AxlCpuException *exc,
    void                  *user)
{
    (void)user;

    /* Static buffer -- no heap allocation in crash context */
    static uint8_t   record_buf[CRASH_RECORD_MAX_SIZE];
    static uint64_t  stack_frames[CRASH_MAX_FRAMES];

    CrashRecordHeader *hdr;
    uint8_t           *ptr;
    uint64_t           fault_addr;
    int                fault_image_idx;
    uint32_t           frame_count;
    size_t             record_size;
    uint8_t            slot_idx;
    size_t             slot_size;
    char               var_key[] = "CrashDump0";
    AxlRealtime        now;

    axl_memset(record_buf, 0, sizeof(record_buf));

    /* --- Build header ---
       exception_type stores the AxlCpuExceptionKind for v1 records.
       Host-side rsod-decode.py reads this field; the stored values
       are stable (the enum's numeric values are part of the SDK
       ABI). */
    hdr = (CrashRecordHeader *)record_buf;
    hdr->magic          = CRASH_RECORD_MAGIC;
    hdr->version        = CRASH_RECORD_VERSION;
    hdr->exception_type = (uint32_t)exc->kind;

    if (exc->arch == AXL_CPU_ARCH_X64) {
        hdr->arch = CRASH_ARCH_X64;
    } else {
        hdr->arch = CRASH_ARCH_AARCH64;
    }

    /* Timestamp (best effort). axl_time_realtime is allocation-free —
       safe in crash context. */
    if (axl_time_realtime(&now) == AXL_OK) {
        hdr->timestamp = (uint64_t)now.year * 525960 +
                         (uint64_t)now.month * 43830 +
                         (uint64_t)now.day * 1440 +
                         (uint64_t)now.hour * 60 +
                         (uint64_t)now.minute;
    }

    ptr = record_buf + sizeof(CrashRecordHeader);

    /* --- Copy registers from the typed AxlCpuException --- */
    if (exc->arch == AXL_CPU_ARCH_X64) {
        CrashRegsX64 *regs = (CrashRegsX64 *)ptr;

        regs->rip            = exc->regs.x64.rip;
        regs->rsp            = exc->regs.x64.rsp;
        regs->rbp            = exc->regs.x64.rbp;
        regs->rflags         = exc->regs.x64.rflags;
        regs->rax            = exc->regs.x64.rax;
        regs->rbx            = exc->regs.x64.rbx;
        regs->rcx            = exc->regs.x64.rcx;
        regs->rdx            = exc->regs.x64.rdx;
        regs->rsi            = exc->regs.x64.rsi;
        regs->rdi            = exc->regs.x64.rdi;
        regs->r8             = exc->regs.x64.r8;
        regs->r9             = exc->regs.x64.r9;
        regs->r10            = exc->regs.x64.r10;
        regs->r11            = exc->regs.x64.r11;
        regs->r12            = exc->regs.x64.r12;
        regs->r13            = exc->regs.x64.r13;
        regs->r14            = exc->regs.x64.r14;
        regs->r15            = exc->regs.x64.r15;
        regs->cr2            = exc->regs.x64.cr2;
        regs->exception_data = exc->error_code;

        fault_addr  = exc->instruction_ptr;
        ptr        += sizeof(CrashRegsX64);

        frame_count = walk_stack_frames(exc->regs.x64.rbp,
                                        exc->regs.x64.rsp,
                                        stack_frames, CRASH_MAX_FRAMES);
    } else {
        CrashRegsAarch64 *regs = (CrashRegsAarch64 *)ptr;

        regs->elr  = exc->regs.aa64.elr;
        regs->sp   = exc->regs.aa64.sp;
        regs->fp   = exc->regs.aa64.x[29];
        regs->lr   = exc->regs.aa64.x[30];
        regs->x0   = exc->regs.aa64.x[0];   regs->x1  = exc->regs.aa64.x[1];
        regs->x2   = exc->regs.aa64.x[2];   regs->x3  = exc->regs.aa64.x[3];
        regs->x4   = exc->regs.aa64.x[4];   regs->x5  = exc->regs.aa64.x[5];
        regs->x6   = exc->regs.aa64.x[6];   regs->x7  = exc->regs.aa64.x[7];
        regs->x8   = exc->regs.aa64.x[8];   regs->x9  = exc->regs.aa64.x[9];
        regs->x10  = exc->regs.aa64.x[10];  regs->x11 = exc->regs.aa64.x[11];
        regs->x12  = exc->regs.aa64.x[12];  regs->x13 = exc->regs.aa64.x[13];
        regs->x14  = exc->regs.aa64.x[14];  regs->x15 = exc->regs.aa64.x[15];
        regs->x16  = exc->regs.aa64.x[16];  regs->x17 = exc->regs.aa64.x[17];
        regs->x18  = exc->regs.aa64.x[18];  regs->x19 = exc->regs.aa64.x[19];
        regs->x20  = exc->regs.aa64.x[20];  regs->x21 = exc->regs.aa64.x[21];
        regs->x22  = exc->regs.aa64.x[22];  regs->x23 = exc->regs.aa64.x[23];
        regs->x24  = exc->regs.aa64.x[24];  regs->x25 = exc->regs.aa64.x[25];
        regs->x26  = exc->regs.aa64.x[26];  regs->x27 = exc->regs.aa64.x[27];
        regs->x28  = exc->regs.aa64.x[28];
        regs->esr    = exc->regs.aa64.esr;
        regs->far_el = exc->regs.aa64.far;
        regs->spsr   = exc->regs.aa64.spsr;

        fault_addr  = exc->instruction_ptr;
        ptr        += sizeof(CrashRegsAarch64);

        frame_count = walk_stack_frames(exc->frame_ptr, exc->stack_ptr,
                                        stack_frames, CRASH_MAX_FRAMES);
    }

    /* --- Faulting image (slot 0) + loaded image table --- */
    fault_image_idx = find_image_by_address(fault_addr);

    {
        CrashImageEntry *img_entry;
        uint32_t         idx;

        /* Slot 0: faulting image (or zeroed if unknown) */
        img_entry = (CrashImageEntry *)ptr;
        if (fault_image_idx >= 0) {
            axl_memcpy(img_entry, &g_image_table[fault_image_idx], sizeof(CrashImageEntry));
        }
        ptr += sizeof(CrashImageEntry);

        /* Remaining slots: full image table */
        for (idx = 0; idx < g_image_count; idx++) {
            img_entry = (CrashImageEntry *)ptr;
            axl_memcpy(img_entry, &g_image_table[idx], sizeof(CrashImageEntry));
            ptr += sizeof(CrashImageEntry);
        }

        hdr->image_count = g_image_count + 1;  /* faulting image + table */
    }

    /* --- Stack frames --- */
    {
        uint64_t *frame_ptr;
        uint32_t  idx;

        frame_ptr = (uint64_t *)ptr;
        for (idx = 0; idx < frame_count; idx++) {
            frame_ptr[idx] = stack_frames[idx];
        }
        ptr += frame_count * sizeof(uint64_t);

        hdr->frame_count = frame_count;
    }

    record_size = (size_t)(ptr - record_buf);

    /* --- Write to NVRAM ---
       axl_nvstore_{get,set} are allocation-free at this surface
       (stack-allocated UCS-2 conversion buffer; no heap), so they
       are safe in crash context. Wire-format is identical to what
       process_crash_records on the safe-context side reads. */
    slot_idx  = 0;
    slot_size = sizeof(slot_idx);
    axl_nvstore_get("crashdump", "CrashDumpIdx",
                    &slot_idx, &slot_size);
    slot_idx = slot_idx % CRASH_DUMP_SLOTS;

    /* Build variable key: "CrashDump" + digit */
    var_key[9] = '0' + (char)slot_idx;

    axl_nvstore_set("crashdump", var_key,
                    record_buf, record_size,
                    AXL_NV_PERSISTENT | AXL_NV_BOOT);

    slot_idx++;
    axl_nvstore_set("crashdump", "CrashDumpIdx",
                    &slot_idx, sizeof(slot_idx),
                    AXL_NV_PERSISTENT | AXL_NV_BOOT);

    /* --- Print crash summary (best effort) --- */
    if (fault_image_idx >= 0) {
        axl_printf("\n\n!!! CRASH: %s at 0x%lX (%s+0x%lX) !!!\n",
                   get_exception_name(exc->kind),
                   (unsigned long)fault_addr,
                   g_image_table[fault_image_idx].name,
                   (unsigned long)(fault_addr - g_image_table[fault_image_idx].base));
    } else {
        axl_printf("\n\n!!! CRASH: %s at 0x%lX !!!\n",
                   get_exception_name(exc->kind),
                   (unsigned long)fault_addr);
    }
    axl_printf("Crash record saved to NVRAM (slot %d). Reboot to see full report.\n\n",
               slot_idx - 1);

    for (;;) {}
}
