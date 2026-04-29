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

/* Embedded RamDiskDxe.efi blob — definitions are force-included via
 * the Makefile's `-include $(BUILDDIR)/mkrd-ramdisk-blob.h` flag,
 * which is regenerated from third_party/edk2/RamDiskDxe-$(ARCH).efi
 * by `xxd -i`. The forward declarations here let editors and static
 * analyzers (clangd, ccls) parse mkrd.c standalone without seeing
 * the -include flag. The actual storage lives in .rodata in mkrd.o. */
extern const unsigned char axl_embedded_ramdiskdxe[];
extern const unsigned int  axl_embedded_ramdiskdxe_len;

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

static bool verbose = false;

static const AxlConfigDesc descs[] = {
    { "size",    AXL_CFG_STRING, NULL,    's', "Size in MB (default 4)",                          0, 0 },
    { "destroy", AXL_CFG_STRING, NULL,    'd', "Destroy RAM disk by volume label",                0, 0 },
    { "list",    AXL_CFG_BOOL,   "false", 'l', "List existing RAM disks",                         0, 0 },
    { "verbose", AXL_CFG_BOOL,   "false", 'v', "Verbose output",                                  0, 0 },
    { "driver",  AXL_CFG_STRING, NULL,    0,   "Driver name to load instead of embedded RamDiskDxe", 0, 0 },
    { "help",    AXL_CFG_BOOL,   "false", 'h', "Show this help",                                  0, 0 },
    { 0 }
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
    for (EFI_DEVICE_PATH_PROTOCOL *node = dp;
         !EFI_DP_IS_END(node);
         node = EFI_DP_NEXT(node)) {
        if (EFI_DP_TYPE(node) == MEDIA_DEVICE_PATH &&
            node->SubType == MEDIA_RAM_DISK_DP) {
            MEDIA_RAM_DISK_DEVICE_PATH *rd =
                (MEDIA_RAM_DISK_DEVICE_PATH *)node;
            uint64_t start = (uint64_t)rd->StartingAddr[0] |
                             ((uint64_t)rd->StartingAddr[1] << 32);
            uint64_t end   = (uint64_t)rd->EndingAddr[0] |
                             ((uint64_t)rd->EndingAddr[1] << 32);
            if (start_out != NULL) {
                *start_out = start;
            }
            if (size_out != NULL) {
                *size_out = end - start + 1;
            }
            return true;
        }
    }
    return false;
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
        char ch = label[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = ch - 'a' + 'A';
        }
        padded[i] = ch;
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
    if (axl_service_find("ram-disk", (void **)&rd_proto) != 0 ||
        rd_proto == NULL) {
        axl_printf("MkRd: EFI_RAM_DISK_PROTOCOL not available.\n");
        axl_printf("Load RamDiskDxe.efi first.\n");
        return 1;
    }

    /* Check for duplicate label */
    void   **handles = NULL;
    size_t   handle_count = 0;
    if (axl_service_enumerate("simple-fs", &handles, &handle_count) == 0) {
        for (size_t i = 0; i < handle_count; i++) {
            /* Get device path for this handle */
            EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
            axl_handle_get_service(handles[i], "device-path", (void **)&dp);
            if (dp == NULL || !is_ramdisk_dp(dp, NULL, NULL)) {
                continue;
            }
            /* Check volume label */
            char *vol = axl_volume_get_label_by_handle(handles[i]);
            if (vol != NULL) {
                bool match = labels_match(label, vol);
                axl_free(vol);
                if (match) {
                    axl_printf("RAM disk \"%s\" already exists.\n", label);
                    axl_free(handles);
                    return 0;
                }
            }
        }
        axl_free(handles);
    }

    /* Allocate pages */
    uint64_t disk_bytes = (uint64_t)size_mb * 1024 * 1024;
    size_t   pages = (size_t)((disk_bytes + 4095) / 4096);
    uint64_t phys_addr = 0;

    if (axl_alloc_pages(pages, &phys_addr) != 0) {
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
    void   **handles = NULL;
    size_t   count = 0;

    if (axl_service_enumerate("simple-fs", &handles, &count) != 0 ||
        count == 0) {
        axl_printf("No filesystems found.\n");
        return 1;
    }

    axl_printf("LABEL        SIZE     \n");
    axl_printf("-----------  -------\n");

    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        axl_handle_get_service(handles[i], "device-path", (void **)&dp);
        if (dp == NULL) {
            continue;
        }

        uint64_t start = 0, size = 0;
        if (!is_ramdisk_dp(dp, &start, &size)) {
            continue;
        }

        char *vol = axl_volume_get_label_by_handle(handles[i]);
        const char *display_label = (vol != NULL && vol[0] != '\0')
                                    ? vol : "(unlabeled)";

        axl_printf("%-11s  %4llu MB\n",
                   display_label,
                   (unsigned long long)(size / (1024 * 1024)));
        found++;
        axl_free(vol);
    }

    axl_free(handles);

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
    if (axl_service_find("ram-disk", (void **)&rd_proto) != 0 ||
        rd_proto == NULL) {
        axl_printf("MkRd: EFI_RAM_DISK_PROTOCOL not available.\n");
        return 1;
    }

    void   **handles = NULL;
    size_t   count = 0;
    if (axl_service_enumerate("simple-fs", &handles, &count) != 0 ||
        count == 0) {
        axl_printf("MkRd: no filesystems found.\n");
        return 1;
    }

    for (size_t i = 0; i < count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        axl_handle_get_service(handles[i], "device-path", (void **)&dp);
        if (dp == NULL) {
            continue;
        }

        uint64_t start = 0, size = 0;
        if (!is_ramdisk_dp(dp, &start, &size)) {
            continue;
        }

        char *vol = axl_volume_get_label_by_handle(handles[i]);
        if (vol == NULL) {
            continue;
        }

        if (!labels_match(label, vol)) {
            axl_free(vol);
            continue;
        }
        axl_free(vol);

        /* Found — unregister */
        EFI_STATUS status = rd_proto->Unregister(dp);
        if (EFI_ERROR(status)) {
            axl_printf("MkRd: Unregister failed: 0x%llx\n",
                       (unsigned long long)status);
            axl_free(handles);
            return 1;
        }

        /* Free backing memory */
        if (start != 0 && size > 0) {
            axl_free_pages(start, (size_t)((size + 4095) / 4096));
        }

        axl_printf("RAM disk \"%s\" destroyed (%llu MB freed).\n",
                   label, (unsigned long long)(size / (1024 * 1024)));
        axl_free(handles);
        return 0;
    }

    axl_free(handles);
    axl_printf("MkRd: no RAM disk with label \"%s\" found.\n", label);
    return 1;
}

// ===========================================================================
// Entry point
// ===========================================================================

int
main(
    int    argc,
    char **argv
    )
{
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    if (cfg == NULL || axl_config_parse_args(cfg, argc, argv) != 0) {
        axl_printf("MkRd: invalid option\n");
        axl_config_usage(cfg, "MkRd",
                         "<label> [-s size] | -l | -d <label>");
        return 1;
    }

    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "MkRd",
                         "<label> [-s size] | -l | -d <label>");
        return 0;
    }

    verbose = axl_config_get_bool(cfg, "verbose");

    if (verbose) {
        /* Raise log level so axl_driver_ensure's debug trace surfaces. */
        axl_log_set_level(AXL_LOG_DEBUG);
        axl_diag_startup(argc, argv);
        axl_diag_probe_protocol(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "EFI_RAM_DISK_PROTOCOL");
    }

    /* All non-help modes need EFI_RAM_DISK_PROTOCOL. Resolution
     * order (see axl_driver_ensure_with_embedded):
     *   1. firmware already published the protocol (most Dell/HP/
     *      Supermicro boxes hit this path),
     *   2. --driver <name> override: search disk only, no fallback,
     *   3. canonical search for RamDiskDxe.efi on disk,
     *   4. embedded RamDiskDxe.efi blob baked in at build time.
     * The embedded blob makes MkRd self-contained on minimal
     * firmware (e.g. Dell client BIOS without UEFI 2.6+ optional
     * modules). axl_embedded_ramdiskdxe{,_len} are forced-included
     * by the Makefile rule that consumes
     * third_party/edk2/RamDiskDxe-$(ARCH).efi via xxd -i. */
    const char *driver_override = axl_config_get(cfg, "driver");
    if (axl_driver_ensure_with_embedded(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "RamDiskDxe.efi",
            axl_embedded_ramdiskdxe, axl_embedded_ramdiskdxe_len,
            driver_override) != 0) {
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
        /* Re-probe — proves whether axl_driver_ensure short-circuited
         * (status matches the pre-ensure probe) or actually loaded a
         * driver (status flips from NOT registered to REGISTERED). */
        axl_diag_probe_protocol(
            (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
            "EFI_RAM_DISK_PROTOCOL (post-ensure)");
    }

    /* List mode */
    if (axl_config_get_bool(cfg, "list")) {
        return do_list();
    }

    /* Destroy mode */
    const char *destroy_label = axl_config_get(cfg, "destroy");
    if (destroy_label != NULL) {
        return do_destroy(destroy_label);
    }

    /* Create mode */
    const char *label = axl_config_pos(cfg, 0);
    if (label == NULL) {
        axl_printf("MkRd: label required\n");
        axl_config_usage(cfg, "MkRd",
                         "<label> [-s size] | -l | -d <label>");
        return 1;
    }

    size_t size_mb = MKRD_DEFAULT_SIZE_MB;
    const char *size_str = axl_config_get(cfg, "size");
    if (size_str != NULL) {
        size_mb = (size_t)axl_strtou64(size_str);
        if (size_mb < MKRD_MIN_SIZE_MB || size_mb > MKRD_MAX_SIZE_MB) {
            axl_printf("MkRd: size must be %d-%d MB\n",
                       MKRD_MIN_SIZE_MB, MKRD_MAX_SIZE_MB);
            return 1;
        }
    }

    return do_create(label, size_mb);
}
