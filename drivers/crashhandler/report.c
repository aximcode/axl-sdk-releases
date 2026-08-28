/** @file report.c
    Reads crash records from NVRAM and writes crash-report.txt on the
    boot volume (with fallback to any writable volume). Runs during
    driver init (safe context, full Boot Services available).

    Migrated to axl-sdk's NVRAM + filesystem primitives so this
    translation unit no longer references EFI_* / gRT / gBS:
      - axl_nvstore_* replaces gRT->{Get,Set}Variable
      - axl_app_boot_path + axl_file_set_contents replaces the
        EFI_LOADED_IMAGE_PROTOCOL / EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
        / EFI_FILE_PROTOCOL walk
      - axl_volume_enumerate handles the alternate-volume fallback
**/

#include "crashhandler.h"

/* Formatting buffer (file accumulator + scratch). The crash report
   is small (under 16 KiB even with the worst-case image table +
   stack frames), so we buffer it whole and flush once via
   axl_file_set_contents — that's the right shape for the new SDK
   surface, which is path-then-buffer rather than open-write-close. */
#define REPORT_BUF_SIZE  (16 * 1024)
static char report_buf[REPORT_BUF_SIZE];
static size_t report_len;
static char   fmt_buf[256];

static void
report_append(const char *str)
{
    size_t n = axl_strlen(str);
    if (report_len + n >= sizeof(report_buf)) {
        n = sizeof(report_buf) - report_len - 1;
    }
    axl_memcpy(report_buf + report_len, str, n);
    report_len += n;
    report_buf[report_len] = '\0';
}

#define REPORT_PRINT(...) \
    do { axl_snprintf(fmt_buf, sizeof(fmt_buf), __VA_ARGS__); report_append(fmt_buf); } while (0)

/**
 * Append one crash record's worth of text into report_buf.
 * @return AXL_OK if the record was valid, AXL_ERR if it was malformed.
 */
static int
append_single_crash_report(uint8_t *record, size_t record_size)
{
    AxlCrashRecordHeader *hdr;
    uint8_t           *ptr;
    AxlCrashImageEntry   *fault_image;
    AxlCrashImageEntry   *images;
    uint64_t          *frames;
    uint64_t           fault_addr;
    uint64_t           fault_offset;
    uint32_t           idx;

    if (record_size < sizeof(AxlCrashRecordHeader)) {
        return AXL_ERR;
    }

    hdr = (AxlCrashRecordHeader *)record;
    if (hdr->magic != AXL_CRASH_RECORD_MAGIC || hdr->version != AXL_CRASH_RECORD_VERSION) {
        return AXL_ERR;
    }

    /* EVERY LENGTH BELOW COMES FROM NVRAM, so bound them against the record
       we were actually handed before using any of them to walk. magic and
       version say the record is OURS; they say nothing about a truncated
       write, a flash bit-flip, or a slot half-overwritten by a crash during
       the crash save -- and image_count/frame_count are used directly as
       loop bounds over a 2560-byte stack buffer. Read hdr-> exactly once
       here; everything below uses the CLAMPED locals. */
    size_t regs_size = (hdr->arch == AXL_CRASH_ARCH_X64)
                       ? sizeof(AxlCrashRegsX64)
                       : sizeof(AxlCrashRegsAarch64);
    size_t fixed = sizeof(AxlCrashRecordHeader) + regs_size;

    /* At least one image entry: slot 0 is the faulting image and is read
       unconditionally below, whatever image_count claims. */
    if (record_size < fixed + sizeof(AxlCrashImageEntry)) {
        return AXL_ERR;
    }

    uint32_t image_count = hdr->image_count;
    uint32_t frame_count = hdr->frame_count;
    if (image_count > AXL_CRASH_MAX_IMAGES) {
        image_count = AXL_CRASH_MAX_IMAGES;
    }
    if (frame_count > AXL_CRASH_MAX_FRAMES) {
        frame_count = AXL_CRASH_MAX_FRAMES;
    }
    if (image_count == 0) {
        image_count = 1;   /* slot 0 always exists; see above */
    }
    if (fixed + (size_t)image_count * sizeof(AxlCrashImageEntry)
              + (size_t)frame_count * sizeof(uint64_t) > record_size) {
        return AXL_ERR;
    }

    ptr = record + sizeof(AxlCrashRecordHeader);

    /* Get fault address from registers */
    if (hdr->arch == AXL_CRASH_ARCH_X64) {
        AxlCrashRegsX64 *regs = (AxlCrashRegsX64 *)ptr;
        fault_addr = regs->rip;
        ptr += sizeof(AxlCrashRegsX64);
    } else {
        AxlCrashRegsAarch64 *regs = (AxlCrashRegsAarch64 *)ptr;
        fault_addr = regs->elr;
        ptr += sizeof(AxlCrashRegsAarch64);
    }

    /* Image table starts after registers */
    fault_image = (AxlCrashImageEntry *)ptr;
    fault_offset = (fault_image->base != 0) ? fault_addr - fault_image->base : 0;
    images = fault_image;  /* slot 0 = faulting image, rest = full table */
    ptr += (size_t)image_count * sizeof(AxlCrashImageEntry);

    /* Stack frames */
    frames = (uint64_t *)ptr;

    /* --- Write report --- */
    report_append("UEFI Crash Report\r\n");
    report_append("==================\r\n");

    /* Range-check BEFORE the cast. magic + version above say the record is
       ours, not that every field is in range, and converting an arbitrary
       uint32_t to an enum is undefined -- a compiler may emit a jump table
       with no default for a dense one. crash_exception_name happens to be
       total; the caller should not depend on that. */
    const char *exc_name = "Unknown";
    if (hdr->exception_type > 0
        && hdr->exception_type < (uint32_t)AXL_CPU_EXCEPTION_KIND_MAX) {
        exc_name = crash_exception_name(
            (AxlCpuExceptionKind)hdr->exception_type);
    }
    REPORT_PRINT("Exception:    %s at 0x%016lX\r\n",
        exc_name, (unsigned long)fault_addr);

    if (fault_image->base != 0) {
        REPORT_PRINT("Image:        %s (base 0x%lX, size 0x%lX)\r\n",
            fault_image->name, (unsigned long)fault_image->base,
            (unsigned long)fault_image->size);
        REPORT_PRINT("Offset:       0x%lX\r\n", (unsigned long)fault_offset);
    }

    REPORT_PRINT("Architecture: %s\r\n",
        (hdr->arch == AXL_CRASH_ARCH_X64) ? "X64" : "AARCH64");
    report_append("\r\n");

    /* --- Registers --- */
    report_append("Registers:\r\n");

    if (hdr->arch == AXL_CRASH_ARCH_X64) {
        AxlCrashRegsX64 *r = (AxlCrashRegsX64 *)(record + sizeof(AxlCrashRecordHeader));

        REPORT_PRINT("  RAX=%016lX  RBX=%016lX\r\n", (unsigned long)r->rax, (unsigned long)r->rbx);
        REPORT_PRINT("  RCX=%016lX  RDX=%016lX\r\n", (unsigned long)r->rcx, (unsigned long)r->rdx);
        REPORT_PRINT("  RSI=%016lX  RDI=%016lX\r\n", (unsigned long)r->rsi, (unsigned long)r->rdi);
        REPORT_PRINT("  RBP=%016lX  RSP=%016lX\r\n", (unsigned long)r->rbp, (unsigned long)r->rsp);
        /* `R8=` not `R8 =`: a space before the `=` is not how any register
           dump spells an assignment, and every consumer that scans for
           NAME=VALUE -- rsod-decode.py included -- silently dropped these two.
           The column stays square because the name is padded on the RIGHT of
           the value instead. */
        REPORT_PRINT("  R8=%016lX   R9=%016lX\r\n", (unsigned long)r->r8, (unsigned long)r->r9);
        REPORT_PRINT("  R10=%016lX  R11=%016lX\r\n", (unsigned long)r->r10, (unsigned long)r->r11);
        REPORT_PRINT("  R12=%016lX  R13=%016lX\r\n", (unsigned long)r->r12, (unsigned long)r->r13);
        REPORT_PRINT("  R14=%016lX  R15=%016lX\r\n", (unsigned long)r->r14, (unsigned long)r->r15);
        REPORT_PRINT("  RIP=%016lX  RFLAGS=%016lX\r\n", (unsigned long)r->rip, (unsigned long)r->rflags);
        REPORT_PRINT("  CR2=%016lX  ErrCode=%016lX\r\n", (unsigned long)r->cr2, (unsigned long)r->exception_data);
    } else {
        AxlCrashRegsAarch64 *r = (AxlCrashRegsAarch64 *)(record + sizeof(AxlCrashRecordHeader));

        REPORT_PRINT("  ELR=%016lX   SP=%016lX\r\n", (unsigned long)r->elr, (unsigned long)r->sp);
        REPORT_PRINT("  FP=%016lX   LR=%016lX\r\n", (unsigned long)r->fp, (unsigned long)r->lr);
        for (idx = 0; idx < 29; idx += 2) {
            uint64_t *xn = &r->x0;
            /* See the x64 note above: `X0 =` cost ten of these registers plus
               FP on every decoded aa64 report. */
            REPORT_PRINT("  X%u=%016lX  X%u=%016lX\r\n",
                idx, (unsigned long)xn[idx],
                idx + 1, (unsigned long)((idx + 1 < 29) ? xn[idx + 1] : 0));
        }
        REPORT_PRINT("  ESR=%016lX  FAR=%016lX  SPSR=%016lX\r\n",
            (unsigned long)r->esr, (unsigned long)r->far_el, (unsigned long)r->spsr);
    }

    report_append("\r\n");

    /* --- Stack trace --- */
    if (frame_count > 0) {
        report_append("Stack Trace:\r\n");
        for (idx = 0; idx < frame_count; idx++) {
            uint64_t addr = frames[idx];
            uint32_t img_idx;
            bool found = false;

            for (img_idx = 0; img_idx < image_count; img_idx++) {
                if (addr >= images[img_idx].base &&
                    addr < images[img_idx].base + images[img_idx].size) {
                    REPORT_PRINT("  0x%016lX  %s+0x%lX\r\n",
                        (unsigned long)addr, images[img_idx].name,
                        (unsigned long)(addr - images[img_idx].base));
                    found = true;
                    break;
                }
            }
            if (!found) {
                REPORT_PRINT("  0x%016lX  ???\r\n", (unsigned long)addr);
            }
        }
        report_append("\r\n");
    }

    /* --- Loaded image table --- */
    report_append("Loaded Images:\r\n");
    REPORT_PRINT("  %-18s %-10s %s\r\n", "Base", "Size", "Name");
    for (idx = 0; idx < image_count; idx++) {
        if (images[idx].base == 0) {
            continue;
        }
        REPORT_PRINT("  0x%016lX 0x%06lX  %s\r\n",
            (unsigned long)images[idx].base,
            (unsigned long)images[idx].size,
            images[idx].name);
    }

    report_append("\r\nDecode with debug symbols:\r\n");
    if (fault_image->base != 0) {
        /* No :BASE. This report states the load base on the Image: line above
           and again in the loaded-image table, and rsod-decode reads both --
           so requiring the reader to copy it back in is make-work. The
           unattributed branch below still asks for one, because there it is
           the one thing we genuinely could not determine.

           `.so`, not `.efi`: the ELF beside the image is where the DWARF is.
           The .efi is stripped. The name here is the LOADED name, which
           carries its extension -- appending blindly printed
           `CrashTest.efi.so`, a path that exists nowhere. */
        {
            char   stem[sizeof(fault_image->name)];
            size_t n;

            axl_strlcpy(stem, fault_image->name, sizeof(stem));
            n = axl_strlen(stem);
            if (n >= 4 && axl_strncasecmp(stem + n - 4, ".efi", 4) == 0) {
                stem[n - 4] = '\0';
            }
            REPORT_PRINT("  rsod-decode.py --image <build>/%s.so "
                         "--rsod crash-report.txt\r\n", stem);
        }
    } else {
        report_append("  rsod-decode.py --image <build>/<image>.so:<base> "
                      "--rsod crash-report.txt\r\n");
    }
    report_append("\r\n");

    return AXL_OK;
}

/* Try writing report_buf to every mounted volume in turn until one
   accepts the write. Used as a fallback when the boot volume is
   read-only (iDRAC virtual ISO, etc.). Returns the index of the
   volume that accepted the write, or -1 if all refused. */
#define CRASH_REPORT_MAX_VOLUMES  32

static int
try_write_to_any_volume(char *out_label, size_t out_label_size)
{
    AxlVolume volumes[CRASH_REPORT_MAX_VOLUMES];
    size_t    count = 0;

    if (axl_volume_enumerate(volumes, CRASH_REPORT_MAX_VOLUMES, &count)
        != AXL_OK)
    {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        char path[64];
        if (axl_snprintf(path, sizeof(path),
                         "%s:\\crash-report.txt", volumes[i].name) <= 0) {
            continue;
        }
        if (axl_file_set_contents(path, report_buf, report_len) == AXL_OK) {
            axl_strlcpy(out_label, volumes[i].name, out_label_size);
            return (int)i;
        }
    }
    return -1;
}

/**
 * Process all crash records in NVRAM. Append all valid records into
 * report_buf, write to boot volume (or fallback), delete processed
 * NVRAM variables.
 *
 * @return Number of crash records processed.
 */
size_t
process_crash_records(void)
{
    uint8_t      slot_idx;
    size_t       slot_size;
    uint8_t      record_buf[AXL_CRASH_RECORD_MAX_SIZE];
    size_t       record_size;
    char         var_key[] = "CrashDump0";
    size_t       count;
    uint32_t     slot;
    char         path_buf[128];

    /* Reset accumulator. */
    report_len    = 0;
    report_buf[0] = '\0';

    /* Read current slot index from the crashdump namespace. */
    slot_idx  = 0;
    slot_size = sizeof(slot_idx);
    if (axl_nvstore_get("crashdump", "CrashDumpIdx",
                        &slot_idx, &slot_size) != AXL_OK
        || slot_idx == 0)
    {
        return 0;  /* No crashes recorded */
    }

    /* Append each non-empty slot into the accumulator. */
    count = 0;
    for (slot = 0; slot < AXL_CRASH_DUMP_SLOTS; slot++) {
        var_key[9] = '0' + (char)slot;

        record_size = sizeof(record_buf);
        if (axl_nvstore_get("crashdump", var_key,
                            record_buf, &record_size) != AXL_OK)
        {
            continue;
        }

        if (append_single_crash_report(record_buf, record_size) == AXL_OK) {
            count++;
        }

        /* Delete the processed variable. */
        axl_nvstore_delete("crashdump", var_key);
    }

    if (count == 0) {
        return 0;
    }

    /* Reset slot index — all known slots have been processed. */
    slot_idx = 0;
    axl_nvstore_set("crashdump", "CrashDumpIdx",
                    &slot_idx, sizeof(slot_idx),
                    AXL_NV_PERSISTENT | AXL_NV_BOOT);

    /* Write the accumulated report. First try the boot volume; on
       a read-only boot volume (iDRAC virtual ISO etc.) fall back to
       any other mounted volume that accepts the write. */
    bool wrote_ok = false;
    if (axl_app_boot_path("crash-report.txt", path_buf, sizeof(path_buf)) == AXL_OK
        && axl_file_set_contents(path_buf, report_buf, report_len) == AXL_OK)
    {
        wrote_ok = true;
    } else {
        char chosen_label[32] = {0};
        if (try_write_to_any_volume(chosen_label,
                                    sizeof(chosen_label)) >= 0) {
            axl_printf("CrashHandler: writing crash report to %s\n",
                       chosen_label);
            wrote_ok = true;
        }
    }

    if (!wrote_ok) {
        axl_printf("CrashHandler: no writable volume for crash-report.txt\n");
        return 0;
    }

    return count;
}
