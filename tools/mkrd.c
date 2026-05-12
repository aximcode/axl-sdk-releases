/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file MkRd.c
    Create/list/destroy FAT16/FAT32 RAM disks.

    Build with axl-cc:
      axl-cc MkRd.c -o MkRd.efi

    Usage:
      MkRd.efi <label> [-s size]    Create RAM disk (default 4 MB)
      MkRd.efi -l                   List existing RAM disks
      MkRd.efi -d <label>           Destroy RAM disk by label
      MkRd.efi --driver <name>      Override embedded RamDiskDxe with
                                    a copy named <name> on disk
      MkRd.efi -h                   Help

    Driver discovery: EFI_RAM_DISK_PROTOCOL is optional in UEFI 2.6+
    and absent on some firmware. When the protocol isn't already
    registered, MkRd auto-loads RamDiskDxe via the in-SDK helper:
        1. LocateProtocol short-circuit (most OEM firmware ships it),
        2. disk search for RamDiskDxe.efi (or the --driver override),
        3. an embedded copy baked into MkRd at build time
           (third_party/edk2/RamDiskDxe-<arch>.efi).
    The embedded fallback makes MkRd a self-contained binary that
    works on minimal firmware without the user staging any extra
    files. See `axl_driver_ensure_with_embedded` in axl-driver.h.
**/

#include <axl.h>
#include <uefi/axl-uefi.h>

/* Embedded RamDiskDxe.efi blob — emitted into .rodata by
 * tools/mkrd-blob.S via the GNU assembler's `.incbin` directive
 * (the per-arch path is supplied by the Makefile). The length is
 * the linker-resolved pointer difference between the two markers,
 * not a separate `_len` constant. */
AXL_EMBED_DECLARE(ramdiskdxe);

// ---------------------------------------------------------------------------
// Device path constants (UEFI spec Table 10-46, 10-62)
// ---------------------------------------------------------------------------

#define MEDIA_DEVICE_PATH       0x04
#define MEDIA_RAM_DISK_DP       0x09

#pragma pack(1)
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL    Header;
    uint32_t                    StartingAddr[2];
    uint32_t                    EndingAddr[2];
    EFI_GUID                    TypeGuid;
    uint16_t                    Instance;
} MEDIA_RAM_DISK_DEVICE_PATH;
#pragma pack()

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define MKRD_DEFAULT_SIZE_MB  4
#define MKRD_MIN_SIZE_MB      1
#define MKRD_MAX_SIZE_MB      32768

// ---------------------------------------------------------------------------
// FAT16/FAT32 on-disk layout — reused by the FAT-formatting helpers
// further down. Pragma-packed so field offsets match the spec.
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    uint8_t   jmp_boot[3];
    char      oem_name[8];
    uint16_t  bytes_per_sec;
    uint8_t   sec_per_clus;
    uint16_t  rsvd_sec_cnt;
    uint8_t   num_fats;
    uint16_t  root_ent_cnt;
    uint16_t  tot_sec16;
    uint8_t   media;
    uint16_t  fat_sz16;
    uint16_t  sec_per_trk;
    uint16_t  num_heads;
    uint32_t  hidd_sec;
    uint32_t  tot_sec32;
    /* FAT32 extension */
    uint32_t  fat_sz32;
    uint16_t  ext_flags;
    uint16_t  fs_ver;
    uint32_t  root_clus;
    uint16_t  fs_info;
    uint16_t  bk_boot_sec;
    uint8_t   reserved[12];
    uint8_t   drv_num;
    uint8_t   reserved1;
    uint8_t   boot_sig;
    uint32_t  vol_id;
    char      vol_lab[11];
    char      fil_sys_type[8];
    uint8_t   boot_code[420];
    uint16_t  signature;
} fat_boot_sector;

typedef struct {
    uint32_t  lead_sig;
    uint8_t   reserved1[480];
    uint32_t  struc_sig;
    uint32_t  free_count;
    uint32_t  nxt_free;
    uint8_t   reserved2[12];
    uint32_t  trail_sig;
} fat32_fsinfo;

typedef struct {
    char      name[11];
    uint8_t   attr;
    uint8_t   reserved[20];
} fat_dir_entry;
#pragma pack()

static bool verbose = false;

/* argv stash — used by `gpt` and `wrap` after run_mkrd's leaf
   handler has consumed and forwarded the original AxlArgs slot,
   so the main RAM-disk install code can still read positional
   args (image path, opt flags) by index. Set once at the top of
   run_mkrd. */
static int    g_argc;
static char **g_argv;

static const AxlArgDesc flags[] = {
    { .name = "size",    .short_name = 's', .type = AXL_ARG_U32, .base = 0,
      .min = MKRD_MIN_SIZE_MB, .max = MKRD_MAX_SIZE_MB,
      .help = "Size in MB (default 4)" },
    { .name = "destroy", .short_name = 'd', .type = AXL_ARG_STRING,
      .help = "Destroy RAM disk by volume label" },
    { .name = "list",    .short_name = 'l', .type = AXL_ARG_BOOL,
      .help = "List existing RAM disks" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output" },
    { .name = "driver",  .type = AXL_ARG_STRING,
      .help = "Driver name to load instead of embedded RamDiskDxe" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "label", .type = AXL_ARG_STRING,
      .help = "Volume label for the new RAM disk (create mode)" },
    {0}
};

// ---------------------------------------------------------------------------
// Device path helpers
// ---------------------------------------------------------------------------

static bool
is_ramdisk_dp(
    EFI_DEVICE_PATH_PROTOCOL *dp,
    uint64_t                 *start_out,
    uint64_t                 *size_out
    )
{
    const MEDIA_RAM_DISK_DEVICE_PATH *rd = axl_device_path_find(
        dp, MEDIA_DEVICE_PATH, MEDIA_RAM_DISK_DP);
    if (rd == NULL) {
        return false;
    }
    uint64_t start = (uint64_t)rd->StartingAddr[0] |
                     ((uint64_t)rd->StartingAddr[1] << 32);
    uint64_t end   = (uint64_t)rd->EndingAddr[0] |
                     ((uint64_t)rd->EndingAddr[1] << 32);
    if (start_out != NULL) *start_out = start;
    if (size_out  != NULL) *size_out  = end - start + 1;
    return true;
}

// ---------------------------------------------------------------------------
// FAT volume label helper (uppercased, space-padded to 11 chars)
// ---------------------------------------------------------------------------

static void
pad_label(
    const char *label,
    char        padded[11]
    )
{
    axl_memset(padded, ' ', 11);
    for (int i = 0; i < 11 && label[i] != '\0'; i++) {
        padded[i] = (char)axl_toupper((unsigned char)label[i]);
    }
}

// ---------------------------------------------------------------------------
// Case-insensitive label comparison (strips trailing spaces)
// ---------------------------------------------------------------------------

static bool
labels_match(
    const char *a,
    const char *b
    )
{
    char ua[64], ub[64];

    /* Copy + uppercase a */
    size_t i;
    for (i = 0; i < 63 && a[i] != '\0'; i++) {
        ua[i] = (a[i] >= 'a' && a[i] <= 'z') ? a[i] - 32 : a[i];
    }
    ua[i] = '\0';

    /* Copy + uppercase b, strip trailing spaces */
    size_t j;
    for (j = 0; j < 63 && b[j] != '\0'; j++) {
        ub[j] = (b[j] >= 'a' && b[j] <= 'z') ? b[j] - 32 : b[j];
    }
    while (j > 0 && ub[j - 1] == ' ') {
        j--;
    }
    ub[j] = '\0';

    return axl_strcmp(ua, ub) == 0;
}

// ===========================================================================
// FAT Formatting (FAT16 for <=512MB, FAT32 for >512MB)
// ===========================================================================

static uint8_t
fat16_sec_per_clus(
    uint32_t total_sectors
    )
{
    static const struct { uint32_t max; uint8_t spc; } table[] = {
        { 0x00000800, 1 },  { 0x00001000, 1 },
        { 0x00001800, 1 },  { 0x00007FA8, 2 },
        { 0x00040000, 4 },  { 0x00080000, 8 },
        { 0x00100000, 16 }, { 0xFFFFFFFF, 0 }
    };
    for (size_t i = 0; table[i].max != 0xFFFFFFFF; i++) {
        if (total_sectors <= table[i].max) {
            return table[i].spc;
        }
    }
    return 0;
}

static void
format_fat16(
    void       *disk,
    size_t      disk_bytes,
    const char *label
    )
{
    uint32_t total_sec   = (uint32_t)(disk_bytes / 512);
    uint8_t  spc         = fat16_sec_per_clus(total_sec);
    uint16_t rsvd        = 1;
    uint8_t  num_fats    = 2;
    uint16_t root_ent    = 512;
    uint32_t root_sec    = (root_ent * 32 + 511) / 512;
    uint32_t tmp1        = total_sec - (rsvd + root_sec);
    uint32_t tmp2        = 256 * spc + num_fats;
    uint16_t fat_sz      = (uint16_t)((tmp1 + tmp2 - 1) / tmp2);

    fat_boot_sector *bpb = (fat_boot_sector *)disk;
    bpb->jmp_boot[0]  = 0xEB; bpb->jmp_boot[1] = 0x00; bpb->jmp_boot[2] = 0x90;
    axl_memcpy(bpb->oem_name, "MKRD    ", 8);
    bpb->bytes_per_sec = 512;
    bpb->sec_per_clus  = spc;
    bpb->rsvd_sec_cnt  = rsvd;
    bpb->num_fats      = num_fats;
    bpb->root_ent_cnt  = root_ent;
    bpb->media         = 0xF8;
    bpb->fat_sz16      = fat_sz;

    if (total_sec <= 0xFFFF) {
        bpb->tot_sec16 = (uint16_t)total_sec;
    } else {
        bpb->tot_sec32 = total_sec;
    }

    /* FAT16 BPB tail at fixed offsets */
    uint8_t *raw = (uint8_t *)disk;
    raw[36] = 0x80; raw[37] = 0; raw[38] = 0x29;
    raw[39] = 0x4D; raw[40] = 0x4B; raw[41] = 0x52; raw[42] = 0x44;
    axl_memcpy(&raw[43], label, 11);
    axl_memcpy(&raw[54], "FAT16   ", 8);
    raw[510] = 0x55; raw[511] = 0xAA;

    /* FAT tables */
    uint8_t *fat1 = (uint8_t *)disk + rsvd * 512;
    uint8_t *fat2 = (uint8_t *)disk + (rsvd + fat_sz) * 512;
    fat1[0] = 0xF8; fat1[1] = 0xFF; fat1[2] = 0xFF; fat1[3] = 0xFF;
    fat2[0] = 0xF8; fat2[1] = 0xFF; fat2[2] = 0xFF; fat2[3] = 0xFF;

    /* Root directory volume label */
    uint32_t root_off = (rsvd + num_fats * fat_sz) * 512;
    fat_dir_entry *root = (fat_dir_entry *)((uint8_t *)disk + root_off);
    axl_memcpy(root->name, label, 11);
    root->attr = 0x08;
}

static void
format_fat32(
    void       *disk,
    size_t      disk_bytes,
    const char *label
    )
{
    uint32_t total_sec = (uint32_t)(disk_bytes / 512);
    uint64_t disk_mb   = (uint64_t)total_sec * 512 / (1024 * 1024);
    uint8_t  spc       = (disk_mb <= 260) ? 1 : (disk_mb <= 8192) ? 8 : 16;
    uint16_t rsvd      = 32;
    uint8_t  num_fats  = 2;

    uint32_t tmp1    = total_sec - rsvd;
    uint32_t tmp2    = (256 * spc + num_fats) / 2;
    uint32_t fat_sz  = (tmp1 + tmp2 - 1) / tmp2;
    uint32_t data_st = rsvd + num_fats * fat_sz;

    fat_boot_sector *bpb = (fat_boot_sector *)disk;
    bpb->jmp_boot[0]  = 0xEB; bpb->jmp_boot[1] = 0x58; bpb->jmp_boot[2] = 0x90;
    axl_memcpy(bpb->oem_name, "MKRD    ", 8);
    bpb->bytes_per_sec = 512;
    bpb->sec_per_clus  = spc;
    bpb->rsvd_sec_cnt  = rsvd;
    bpb->num_fats      = num_fats;
    bpb->media         = 0xF8;
    bpb->sec_per_trk   = 63;
    bpb->num_heads     = 255;
    bpb->tot_sec32     = total_sec;
    bpb->fat_sz32      = fat_sz;
    bpb->root_clus     = 2;
    bpb->fs_info       = 1;
    bpb->bk_boot_sec   = 6;
    bpb->drv_num       = 0x80;
    bpb->boot_sig      = 0x29;
    bpb->vol_id        = 0x4D4B5244;
    axl_memcpy(bpb->vol_lab, label, 11);
    axl_memcpy(bpb->fil_sys_type, "FAT32   ", 8);
    bpb->signature     = 0xAA55;

    /* Backup boot sector */
    axl_memcpy((uint8_t *)disk + 6 * 512, bpb, 512);

    /* FSInfo */
    uint32_t total_clus = (total_sec - data_st) / spc;
    fat32_fsinfo *fsi = (fat32_fsinfo *)((uint8_t *)disk + 512);
    fsi->lead_sig   = 0x41615252;
    fsi->struc_sig  = 0x61417272;
    fsi->free_count = total_clus - 1;
    fsi->nxt_free   = 3;
    fsi->trail_sig  = 0xAA550000;
    axl_memcpy((uint8_t *)disk + 7 * 512, fsi, 512);

    /* FAT tables */
    for (uint8_t f = 0; f < num_fats; f++) {
        uint32_t *fat = (uint32_t *)((uint8_t *)disk +
                                     (rsvd + f * fat_sz) * 512);
        fat[0] = 0x0FFFFFF8;
        fat[1] = 0x0FFFFFFF;
        fat[2] = 0x0FFFFFFF;
    }

    /* Root directory volume label */
    fat_dir_entry *root = (fat_dir_entry *)((uint8_t *)disk + data_st * 512);
    axl_memcpy(root->name, label, 11);
    root->attr = 0x08;
}

static const char *
format_disk(
    void       *disk,
    size_t      disk_bytes,
    const char *label
    )
{
    if (disk_bytes / (1024 * 1024) <= 512) {
        format_fat16(disk, disk_bytes, label);
        return "FAT16";
    } else {
        format_fat32(disk, disk_bytes, label);
        return "FAT32";
    }
}

// ===========================================================================
// Create
// ===========================================================================

static int
do_create(
    const char *label,
    size_t      size_mb
    )
{
    /* Get RAM disk protocol */
    EFI_RAM_DISK_PROTOCOL *rd_proto = NULL;
    if (axl_protocol_find("ram-disk", (void **)&rd_proto) != AXL_OK ||
        rd_proto == NULL) {
        axl_printf("MkRd: EFI_RAM_DISK_PROTOCOL not available.\n");
        axl_printf("Load RamDiskDxe.efi first.\n");
        return 1;
    }

    /* Check for duplicate label */
    AxlVolume vols[16];
    size_t    nvols = 0;
    if (axl_volume_enumerate(vols, sizeof(vols)/sizeof(vols[0]), &nvols) == AXL_OK) {
        for (size_t i = 0; i < nvols; i++) {
            if (vols[i].device_path == NULL
                || !is_ramdisk_dp(vols[i].device_path, NULL, NULL)) {
                continue;
            }
            char *vol = axl_volume_get_label_by_handle(vols[i].handle);
            if (vol != NULL) {
                bool match = labels_match(label, vol);
                axl_free(vol);
                if (match) {
                    axl_printf("RAM disk \"%s\" already exists.\n", label);
                    return 0;
                }
            }
        }
    }

    /* Allocate pages */
    uint64_t disk_bytes = (uint64_t)size_mb * 1024 * 1024;
    size_t   pages = (size_t)((disk_bytes + 4095) / 4096);
    uint64_t phys_addr = 0;

    if (axl_alloc_pages(pages, &phys_addr) != AXL_OK) {
        axl_printf("MkRd: failed to allocate %zu MB\n", size_mb);
        return 1;
    }

    /* Format */
    void *buf = (void *)(uintptr_t)phys_addr;
    axl_memset(buf, 0, (size_t)disk_bytes);
    char padded[11];
    pad_label(label, padded);
    const char *fs_type = format_disk(buf, (size_t)disk_bytes, padded);

    /* Register */
    EFI_DEVICE_PATH_PROTOCOL *dev_path = NULL;
    EFI_STATUS status = rd_proto->Register(
        phys_addr, disk_bytes,
        (EFI_GUID *)&EFI_VIRTUAL_DISK_GUID,
        NULL, &dev_path);
    if (EFI_ERROR(status)) {
        axl_printf("MkRd: Register failed: 0x%llx\n",
                   (unsigned long long)status);
        axl_free_pages(phys_addr, pages);
        return 1;
    }

    /* Let firmware bind the FAT driver to the new disk */
    axl_msleep(500);
    axl_map_refresh();

    axl_printf("RAM disk \"%s\" created: %zu MB %s\n",
               label, size_mb, fs_type);
    axl_printf("Run 'map -r' to refresh drive mappings.\n");
    return 0;
}

// ===========================================================================
// List
// ===========================================================================

static int
do_list(void)
{
    AxlVolume vols[16];
    size_t    nvols = 0;

    if (axl_volume_enumerate(vols, sizeof(vols)/sizeof(vols[0]), &nvols) != AXL_OK
        || nvols == 0) {
        axl_printf("No filesystems found.\n");
        return 1;
    }

    axl_printf("LABEL        SIZE     \n");
    axl_printf("-----------  -------\n");

    size_t found = 0;
    for (size_t i = 0; i < nvols; i++) {
        if (vols[i].device_path == NULL) continue;

        uint64_t start = 0, size = 0;
        if (!is_ramdisk_dp(vols[i].device_path, &start, &size)) {
            continue;
        }

        char *vol = axl_volume_get_label_by_handle(vols[i].handle);
        const char *display_label = (vol != NULL && vol[0] != '\0')
                                    ? vol : "(unlabeled)";

        axl_printf("%-11s  %4llu MB\n",
                   display_label,
                   (unsigned long long)(size / (1024 * 1024)));
        found++;
        axl_free(vol);
    }

    if (found == 0) {
        axl_printf("No RAM disks found.\n");
    }

    return 0;
}

// ===========================================================================
// Destroy
// ===========================================================================

static int
do_destroy(
    const char *label
    )
{
    EFI_RAM_DISK_PROTOCOL *rd_proto = NULL;
    if (axl_protocol_find("ram-disk", (void **)&rd_proto) != AXL_OK ||
        rd_proto == NULL) {
        axl_printf("MkRd: EFI_RAM_DISK_PROTOCOL not available.\n");
        return 1;
    }

    AxlVolume vols[16];
    size_t    nvols = 0;
    if (axl_volume_enumerate(vols, sizeof(vols)/sizeof(vols[0]), &nvols) != AXL_OK
        || nvols == 0) {
        axl_printf("MkRd: no filesystems found.\n");
        return 1;
    }

    for (size_t i = 0; i < nvols; i++) {
        if (vols[i].device_path == NULL) continue;

        uint64_t start = 0, size = 0;
        if (!is_ramdisk_dp(vols[i].device_path, &start, &size)) {
            continue;
        }

        char *vol = axl_volume_get_label_by_handle(vols[i].handle);
        if (vol == NULL || !labels_match(label, vol)) {
            axl_free(vol);
            continue;
        }
        axl_free(vol);

        /* Found — unregister */
        EFI_STATUS status = rd_proto->Unregister(vols[i].device_path);
        if (EFI_ERROR(status)) {
            axl_printf("MkRd: Unregister failed: 0x%llx\n",
                       (unsigned long long)status);
            return 1;
        }

        /* Free backing memory */
        if (start != 0 && size > 0) {
            axl_free_pages(start, (size_t)((size + 4095) / 4096));
        }

        axl_printf("RAM disk \"%s\" destroyed (%llu MB freed).\n",
                   label, (unsigned long long)(size / (1024 * 1024)));
        return 0;
    }

    axl_printf("MkRd: no RAM disk with label \"%s\" found.\n", label);
    return 1;
}

// ===========================================================================
// Entry point
// ===========================================================================

static int
run_mkrd(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");

    /* AXL_DIAG-gated cross-tool startup dump (no-op when env unset). */
    axl_diag_startup(g_argc, g_argv);

    if (verbose) {
        /* mkrd-specific verbose: the EFI_RAM_DISK_PROTOCOL state at
         * entry tells you whether ensure-with-embedded had any work
         * to do or the firmware already published the driver. For
         * library-level debug output (axl_driver_ensure's traces,
         * SMBus / driver-load detail), set AXL_LOG_LEVEL=debug. */
        axl_diag_probe_protocol(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "EFI_RAM_DISK_PROTOCOL");
    }

    /* All non-help modes need EFI_RAM_DISK_PROTOCOL. Resolution
     * order (see axl_driver_ensure_with_embedded):
     *   1. firmware already published the protocol (most modern OEM firmware,
     *      Supermicro boxes hit this path),
     *   2. --driver <name> override: search disk only, no fallback,
     *   3. canonical search for RamDiskDxe.efi on disk,
     *   4. embedded RamDiskDxe.efi blob baked in at build time. */
    const char *driver_override = axl_args_get_string(a, "driver");
    if (axl_driver_ensure_with_embedded(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "RamDiskDxe.efi",
            AXL_EMBED_DATA(ramdiskdxe), AXL_EMBED_SIZE(ramdiskdxe),
            driver_override,
            NULL, 0) != 0) {  /* RamDiskDxe doesn't read LoadOptions */
        if (driver_override != NULL) {
            axl_printf("MkRd: '%s' not found on any mounted volume "
                       "(--driver overrides embedded fallback).\n",
                       driver_override);
        } else {
            axl_printf("MkRd: failed to register EFI_RAM_DISK_PROTOCOL "
                       "(disk search and embedded fallback both failed).\n");
        }
        return 1;
    }

    if (verbose) {
        axl_diag_probe_protocol(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "EFI_RAM_DISK_PROTOCOL (post-ensure)");
    }

    if (axl_args_get_bool(a, "list")) {
        return do_list();
    }
    const char *destroy_label = axl_args_get_string(a, "destroy");
    if (destroy_label != NULL) {
        return do_destroy(destroy_label);
    }

    const char *label = axl_args_get_string(a, "label");
    if (label == NULL) {
        axl_printf("MkRd: label required (or use --list / --destroy)\n");
        return 1;
    }
    uint32_t size_mb = (uint32_t)axl_args_get_uint(a, "size");
    if (size_mb == 0) {
        size_mb = MKRD_DEFAULT_SIZE_MB;
    }
    return do_create(label, size_mb);
}

AXL_TOOL_MAIN(mkrd)
{
    g_argc = argc;
    g_argv = argv;
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "MkRd",
        .help         = "Create / list / destroy RAM disks via EFI_RAM_DISK_PROTOCOL",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_mkrd,
    });
}
