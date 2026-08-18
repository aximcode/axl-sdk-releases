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

const char *
crash_exception_name(AxlCpuExceptionKind kind)
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
    if (out_size == 0) {
        return;
    }
    if (utf8_path == NULL) {
        /* "Unknown", matching the empty-basename case below rather than
           writing an empty string. axl_image_enumerate documents path as
           NULL for images whose firmware FilePath cannot be decoded, which
           is MOST of them at DXE time -- so the empty-string branch left
           every Name column in the report blank. */
        axl_strlcpy(out, "Unknown", out_size);
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
    uint32_t slot;
    bool     appending = (g_image_count < AXL_CRASH_MAX_IMAGES);

    if (appending) {
        /* Note the count is NOT bumped here. It is published below, after
           the row is filled, so a reader can never see a count that
           includes a half-written entry. */
        slot = g_image_count;
    } else {
        /* Table full: DROP THE OLDEST and keep walking. Stopping here is
           what made the report unusable. Measured on OVMF: the walk filled
           all 32 slots and NONE of them contained the faulting RIP, so the
           report listed 32 images, omitted the one that crashed, left the
           "Image:" line off entirely, and gave rsod-decode no base to
           rebase against. The application is enumerated after the
           firmware's own images, so the entries worth keeping are the LAST
           ones, not the first. Shifting 31 entries is a TPL_CALLBACK cost
           paid on image load, never in exception context. */
        for (uint32_t i = 1; i < AXL_CRASH_MAX_IMAGES; i++) {
            g_image_table[i - 1] = g_image_table[i];
        }
        slot = AXL_CRASH_MAX_IMAGES - 1;
    }
    g_image_table[slot].base = (uint64_t)info->base;
    g_image_table[slot].size = info->size;
    copy_basename(info->path,
                  g_image_table[slot].name,
                  AXL_CRASH_IMAGE_NAME_LEN);
    if (appending) {
        g_image_count++;   /* publish the row only now that it is complete */
    }
    return 0;
}

/**
 * Snapshot all loaded images into the pre-allocated g_image_table.
 * Called during driver init (safe heap context).
 */
void
snapshot_loaded_images(void)
{
    /* Keep the OLD table if the walk fails. This runs on every image load
       now, not once at init, so a single failed enumeration would otherwise
       leave the table EMPTY -- strictly worse than the stale table this
       refresh replaced, because a crash would then be attributed to nothing
       at all rather than to the wrong thing.
       Restoring the count is exact rather than approximate:
       axl_image_enumerate returns AXL_ERR only when LocateHandleBuffer
       fails, which is BEFORE collect_loaded_image runs even once, so on
       that path no row has been touched. */
    uint32_t previous = g_image_count;

    g_image_count = 0;
    if (axl_image_enumerate(collect_loaded_image, NULL) != AXL_OK) {
        g_image_count = previous;
    }
}

void
refresh_loaded_images(void *ctx)
{
    (void)ctx;
    /* Runs at TPL_CALLBACK from the firmware's loaded-image notify, which
       is why it may re-enumerate at all: axl_image_enumerate allocates.
       It rebuilds the table IN PLACE, and does not double-buffer, so there
       is a window in which g_image_count has been reset to 0 and the rows
       are being refilled. That window is not closed, it is argued away:
       UEFI Boot Services are single-threaded on the BSP, so a BSP fault
       during this function would be a fault inside this function, and an
       AP fault mid-refresh is out of scope for a handler that captures BSP
       state. If AP capture is ever added, this needs a shadow table --
       g_image_count going to zero first is the thing that would bite.
       Within the window the count never OUTRUNS the data: collect_loaded_
       image publishes g_image_count only after its row is fully written. */
    snapshot_loaded_images();
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
    static uint8_t   record_buf[AXL_CRASH_RECORD_MAX_SIZE];
    static uint64_t  stack_frames[AXL_CRASH_MAX_FRAMES];

    AxlCrashRecordHeader *hdr;
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
    hdr = (AxlCrashRecordHeader *)record_buf;
    hdr->magic          = AXL_CRASH_RECORD_MAGIC;
    hdr->version        = AXL_CRASH_RECORD_VERSION;
    hdr->exception_type = (uint32_t)exc->kind;

    if (exc->arch == AXL_CPU_ARCH_X64) {
        hdr->arch = AXL_CRASH_ARCH_X64;
    } else {
        hdr->arch = AXL_CRASH_ARCH_AARCH64;
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

    ptr = record_buf + sizeof(AxlCrashRecordHeader);

    /* --- Copy registers from the typed AxlCpuException --- */
    if (exc->arch == AXL_CPU_ARCH_X64) {
        AxlCrashRegsX64 *regs = (AxlCrashRegsX64 *)ptr;

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
        ptr        += sizeof(AxlCrashRegsX64);

        frame_count = walk_stack_frames(exc->regs.x64.rbp,
                                        exc->regs.x64.rsp,
                                        stack_frames, AXL_CRASH_MAX_FRAMES);
    } else {
        AxlCrashRegsAarch64 *regs = (AxlCrashRegsAarch64 *)ptr;

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
        ptr        += sizeof(AxlCrashRegsAarch64);

        frame_count = walk_stack_frames(exc->frame_ptr, exc->stack_ptr,
                                        stack_frames, AXL_CRASH_MAX_FRAMES);
    }

    /* --- Faulting image (slot 0) + loaded image table --- */
    fault_image_idx = find_image_by_address(fault_addr);

    {
        AxlCrashImageEntry *img_entry;
        uint32_t         idx;

        /* Slot 0: faulting image (or zeroed if unknown) */
        img_entry = (AxlCrashImageEntry *)ptr;
        if (fault_image_idx >= 0) {
            axl_memcpy(img_entry, &g_image_table[fault_image_idx], sizeof(AxlCrashImageEntry));
        }
        ptr += sizeof(AxlCrashImageEntry);

        /* Remaining slots: full image table */
        for (idx = 0; idx < g_image_count; idx++) {
            img_entry = (AxlCrashImageEntry *)ptr;
            axl_memcpy(img_entry, &g_image_table[idx], sizeof(AxlCrashImageEntry));
            ptr += sizeof(AxlCrashImageEntry);
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
    slot_idx = slot_idx % AXL_CRASH_DUMP_SLOTS;

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
                   crash_exception_name(exc->kind),
                   (unsigned long)fault_addr,
                   g_image_table[fault_image_idx].name,
                   (unsigned long)(fault_addr - g_image_table[fault_image_idx].base));
    } else {
        axl_printf("\n\n!!! CRASH: %s at 0x%lX !!!\n",
                   crash_exception_name(exc->kind),
                   (unsigned long)fault_addr);
    }
    axl_printf("Crash record saved to NVRAM (slot %d). Reboot to see full report.\n\n",
               slot_idx - 1);

    for (;;) {}
}
