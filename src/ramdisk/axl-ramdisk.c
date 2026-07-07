/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ramdisk.c
    Create / list / destroy FAT RAM disks over EFI_RAM_DISK_PROTOCOL.

    The orchestration here was lifted from the `mkrd` tool so any
    consumer reuses it instead of copying the FAT formatters and the
    driver-ensure flow. The tool is now a thin CLI over this module.

    The embedded RamDiskDxe fallback is reached via
    `axl_ramdisk_ensure_driver`, which wraps
    `axl_driver_ensure_with_embedded` for the RAM-disk GUID. The blob is
    a caller-supplied parameter (not baked into libaxl.a) so only
    binaries that want the fallback carry it.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>
#include <axl/axl-ramdisk.h>
#include <axl/axl-driver.h>
#include <axl/axl-fs.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>
#include <axl/axl-wait.h>   /* axl_msleep */

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

#define RAMDISK_MIN_SIZE_MB  1u
#define RAMDISK_MAX_SIZE_MB  32768u
#define RAMDISK_MAX_VOLUMES  16

// ---------------------------------------------------------------------------
// FAT16/FAT32 on-disk layout — pragma-packed so field offsets match the spec.
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

// ---------------------------------------------------------------------------
// RAM-disk device-path detection + FAT label helpers
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
    if (start_out != NULL) {
        *start_out = start;
    }
    if (size_out != NULL) {
        *size_out = end - start + 1;
    }
    return true;
}

/* Uppercase + space-pad a label to the 11-char FAT field. */
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

/* Case-insensitive label compare. The query @p a is truncated to the
   11-char stored FAT label before comparing (so a >11-char label given
   to create matches in destroy / the dup-check), and trailing padding
   on the stored label @p b is stripped. */
static bool
labels_match(
    const char *a,
    const char *b
    )
{
    char ua[12];
    char ub[12];

    size_t i;
    for (i = 0; i < 11 && a[i] != '\0'; i++) {
        ua[i] = (a[i] >= 'a' && a[i] <= 'z') ? (char)(a[i] - 32) : a[i];
    }
    ua[i] = '\0';

    size_t j;
    for (j = 0; j < 11 && b[j] != '\0'; j++) {
        ub[j] = (b[j] >= 'a' && b[j] <= 'z') ? (char)(b[j] - 32) : b[j];
    }
    while (j > 0 && ub[j - 1] == ' ') {
        j--;
    }
    ub[j] = '\0';

    return axl_strcmp(ua, ub) == 0;
}

/* Find the firmware device path of a registered RAM disk by FAT @p label.
   Returns AXL_OK and sets *dev_path_out (firmware-owned; do not free) on a
   match, AXL_ERR otherwise. Shared by axl_ramdisk_create's idempotent lookup,
   axl_ramdisk_find, and (via the latter) destroy-time alias cleanup. */
static int
find_ramdisk_dp(
    const char *label,
    void      **dev_path_out
    )
{
    AxlVolume vols[RAMDISK_MAX_VOLUMES];
    size_t    nvols = 0;
    if (axl_volume_enumerate(vols, RAMDISK_MAX_VOLUMES, &nvols) != AXL_OK) {
        return AXL_ERR;
    }
    for (size_t i = 0; i < nvols; i++) {
        if (vols[i].device_path == NULL
            || !is_ramdisk_dp(vols[i].device_path, NULL, NULL)) {
            continue;
        }
        char *vol = axl_volume_get_label_by_handle(vols[i].handle);
        if (vol == NULL) {
            continue;
        }
        bool match = labels_match(label, vol);
        axl_free(vol);
        if (match) {
            *dev_path_out = vols[i].device_path;
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

int
axl_ramdisk_find(
    const char *label,
    void      **dev_path_out
    )
{
    if (label == NULL || dev_path_out == NULL) {
        return AXL_ERR;
    }
    *dev_path_out = NULL;
    return find_ramdisk_dp(label, dev_path_out);
}

// ---------------------------------------------------------------------------
// FAT formatting (FAT16 for <=512 MB, FAT32 above)
// ---------------------------------------------------------------------------

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

static void
format_disk(
    void       *disk,
    size_t      disk_bytes,
    const char *label
    )
{
    if (disk_bytes / (1024 * 1024) <= 512) {
        format_fat16(disk, disk_bytes, label);
    } else {
        format_fat32(disk, disk_bytes, label);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_ramdisk_ensure_driver(
    const unsigned char *dxe_image,
    size_t               dxe_image_size,
    const char          *override_name
    )
{
    return axl_driver_ensure_with_embedded(
        (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
        "RamDiskDxe.efi",
        dxe_image, dxe_image_size,
        override_name,
        NULL, 0) == 0 ? AXL_OK : AXL_ERR;
}

int
axl_ramdisk_create(
    const char *label,
    size_t      size_mb,
    void      **dev_path_out
    )
{
    if (dev_path_out != NULL) {
        *dev_path_out = NULL;
    }
    if (label == NULL
        || size_mb < RAMDISK_MIN_SIZE_MB
        || size_mb > RAMDISK_MAX_SIZE_MB) {
        return AXL_ERR;
    }

    EFI_RAM_DISK_PROTOCOL *rd_proto = NULL;
    if (axl_protocol_find("ram-disk", (void **)&rd_proto) != AXL_OK
        || rd_proto == NULL) {
        return AXL_ERR;
    }

    /* Idempotent on the label: an existing RAM disk with this label
       wins, reporting its device path instead of allocating again. */
    void *existing = NULL;
    if (find_ramdisk_dp(label, &existing) == AXL_OK) {
        if (dev_path_out != NULL) {
            *dev_path_out = existing;
        }
        return AXL_OK;
    }

    uint64_t disk_bytes = (uint64_t)size_mb * 1024 * 1024;
    size_t   pages      = (size_t)((disk_bytes + 4095) / 4096);
    uint64_t phys_addr  = 0;
    if (axl_alloc_pages(pages, &phys_addr) != AXL_OK) {
        return AXL_ERR;
    }

    void *buf = (void *)(uintptr_t)phys_addr;
    axl_memset(buf, 0, (size_t)disk_bytes);
    char padded[11];
    pad_label(label, padded);
    format_disk(buf, (size_t)disk_bytes, padded);

    EFI_DEVICE_PATH_PROTOCOL *dev_path = NULL;
    EFI_STATUS status = rd_proto->Register(
        phys_addr, disk_bytes,
        (EFI_GUID *)&EFI_VIRTUAL_DISK_GUID,
        NULL, &dev_path);
    if (EFI_ERROR(status)) {
        axl_free_pages(phys_addr, pages);
        return AXL_ERR;
    }

    /* Let firmware bind the FAT driver, then refresh the volume map so
       the disk is immediately enumerable / reachable as fsN:. */
    axl_msleep(500);
    axl_map_refresh();

    if (dev_path_out != NULL) {
        *dev_path_out = dev_path;
    }
    return AXL_OK;
}

int
axl_ramdisk_register_image(
    void          *image,
    uint64_t       size_bytes,
    AxlRamDiskKind kind,
    void         **dev_path_out
    )
{
    if (dev_path_out != NULL) {
        *dev_path_out = NULL;
    }
    if (image == NULL || size_bytes == 0
        || (kind != AXL_RAMDISK_DISK && kind != AXL_RAMDISK_CDROM)) {
        return AXL_ERR;
    }

    EFI_RAM_DISK_PROTOCOL *rd_proto = NULL;
    if (axl_protocol_find("ram-disk", (void **)&rd_proto) != AXL_OK
        || rd_proto == NULL) {
        return AXL_ERR;
    }

    /* The type GUID the firmware sees: an El Torito CD-ROM or a raw disk.
       The image is registered verbatim — no FAT formatting — so its own
       on-image filesystem (ISO9660 / partition image) is what enumerates. */
    const EFI_GUID *type_guid = (kind == AXL_RAMDISK_CDROM)
        ? (const EFI_GUID *)&EFI_VIRTUAL_CD_GUID
        : (const EFI_GUID *)&EFI_VIRTUAL_DISK_GUID;

    EFI_DEVICE_PATH_PROTOCOL *dev_path = NULL;
    EFI_STATUS status = rd_proto->Register(
        (uint64_t)(uintptr_t)image, size_bytes,
        (EFI_GUID *)type_guid, NULL, &dev_path);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    /* Bind the firmware's block / FAT / ISO9660 drivers to the new device
       (the EDK2 ConnectController(...,TRUE) step) so it enumerates and a
       bootable image becomes a boot option, then refresh the volume map.
       Unlike axl_ramdisk_create — which FAT-formats and relies on the
       RamDiskDxe auto-connect of the FAT driver (a stall + map refresh) —
       an arbitrary image's drivers (an El Torito ISO9660 binding, a
       partitioned disk) need an explicit connect, so this does a global
       connect (axl_driver_connect(NULL) == `connect -r`). It only binds,
       never unbinds, so controllers already in use are undisturbed. */
    axl_driver_connect(NULL);
    axl_map_refresh();

    if (dev_path_out != NULL) {
        *dev_path_out = dev_path;
    }
    return AXL_OK;
}

int
axl_ramdisk_unregister(
    void *dev_path
    )
{
    if (dev_path == NULL) {
        return AXL_ERR;
    }

    EFI_RAM_DISK_PROTOCOL *rd_proto = NULL;
    if (axl_protocol_find("ram-disk", (void **)&rd_proto) != AXL_OK
        || rd_proto == NULL) {
        return AXL_ERR;
    }

    /* Unregister only — the backing memory is the caller's (it allocated
       the page-aligned image and frees it after this returns). */
    if (EFI_ERROR(rd_proto->Unregister((EFI_DEVICE_PATH_PROTOCOL *)dev_path))) {
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_ramdisk_destroy(
    const char *label
    )
{
    if (label == NULL) {
        return AXL_ERR;
    }
    EFI_RAM_DISK_PROTOCOL *rd_proto = NULL;
    if (axl_protocol_find("ram-disk", (void **)&rd_proto) != AXL_OK
        || rd_proto == NULL) {
        return AXL_ERR;
    }

    AxlVolume vols[RAMDISK_MAX_VOLUMES];
    size_t    nvols = 0;
    if (axl_volume_enumerate(vols, RAMDISK_MAX_VOLUMES, &nvols) != AXL_OK) {
        return AXL_ERR;
    }

    for (size_t i = 0; i < nvols; i++) {
        if (vols[i].device_path == NULL) {
            continue;
        }
        uint64_t start = 0;
        uint64_t size  = 0;
        if (!is_ramdisk_dp(vols[i].device_path, &start, &size)) {
            continue;
        }
        char *vol = axl_volume_get_label_by_handle(vols[i].handle);
        if (vol == NULL || !labels_match(label, vol)) {
            axl_free(vol);
            continue;
        }
        axl_free(vol);

        if (EFI_ERROR(rd_proto->Unregister(vols[i].device_path))) {
            return AXL_ERR;
        }
        if (start != 0 && size > 0) {
            axl_free_pages(start, (size_t)((size + 4095) / 4096));
        }
        return AXL_OK;
    }
    return AXL_ERR;   /* no RAM disk with that label */
}

int
axl_ramdisk_list(
    AxlRamDisk *out,
    size_t      cap,
    size_t     *count
    )
{
    if (count == NULL) {
        return AXL_ERR;
    }
    *count = 0;

    AxlVolume vols[RAMDISK_MAX_VOLUMES];
    size_t    nvols = 0;
    if (axl_volume_enumerate(vols, RAMDISK_MAX_VOLUMES, &nvols) != AXL_OK) {
        return AXL_ERR;
    }

    size_t n = 0;
    for (size_t i = 0; i < nvols; i++) {
        if (vols[i].device_path == NULL) {
            continue;
        }
        uint64_t start = 0;
        uint64_t size  = 0;
        if (!is_ramdisk_dp(vols[i].device_path, &start, &size)) {
            continue;
        }
        if (out != NULL && n < cap) {
            axl_memset(&out[n], 0, sizeof(out[n]));
            char *vol = axl_volume_get_label_by_handle(vols[i].handle);
            if (vol != NULL) {
                size_t k;
                for (k = 0; k + 1 < sizeof(out[n].label) && vol[k] != '\0'; k++) {
                    out[n].label[k] = vol[k];
                }
                out[n].label[k] = '\0';
                axl_free(vol);
            }
            out[n].start_addr  = start;
            out[n].size_bytes  = size;
            out[n].device_path = vols[i].device_path;
        }
        n++;
    }

    *count = n;
    return AXL_OK;
}
