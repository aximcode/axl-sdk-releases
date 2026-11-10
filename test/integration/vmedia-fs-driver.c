/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* vmedia-fs-driver.c -- publish a read-only volume that behaves like BMC
 * virtual media: any single Read that asks for MORE than 64 KB comes back
 * with one byte flipped, while reads of 64 KB or less are always exact.
 *
 * Measured on real hardware booted through a BMC's browser virtual media:
 * a packed tool loaded by the firmware's LoadImage (one large read of the
 * whole file) failed its CRC 7 times in 10, while sha256sum of the same file
 * (64 KB reads) was correct 5 times in 5. This driver reproduces that shape
 * deterministically, so a test can tell "read in bounded chunks" from "read
 * in one large call" by the bytes that come back.
 *
 * The corruption is placed, not random: the byte at the MIDDLE of the file
 * (offset size / 2) is inverted whenever a single Read of more than
 * VM_RELIABLE_READ bytes covers it. The test checks on the host that the
 * middle of each file it stages lands where a flip is observable -- inside a
 * packed image's payload, inside the padded driver's verified table -- so a
 * flip can never fall somewhere nothing checks.
 *
 * Every flip prints one line, `VMFS: corrupted a <n>-byte read of <name>`, so
 * the test can see that the large read it expects really happened (and that
 * the fixture can fire at all), and that a fixed path made none.
 *
 * A file whose name starts with `ioerr-` also fails every read that does not
 * start at offset 0 (EFI_DEVICE_ERROR), printing `VMFS: failed a read of
 * <name> at <offset>`: a chunked reader gets its first chunk and then an
 * error, the shape of media that goes bad part-way through a file. A single
 * whole-file read from offset 0 still succeeds (with its middle byte
 * flipped, as for every file), so falling back to one large read "works".
 * The failure PERSISTS (every such read fails, not just the first): the
 * backend does not latch an error, so a fail-once fixture would let the
 * retry succeed and silently weaken the error=1 / refuse assertions.
 *
 * At start the driver copies every file in fs0:\vm\ into memory and
 * publishes them, flat, as `AXLVM:`. `load`ed from the shell it stays
 * resident, so the volume is live for every later command in that shell.
 *
 * Driven by test/integration/test-vmedia-reads-qemu.sh.
 *
 * Output contract (exact lines the harness greps for):
 *   VMFS: READY volume=AXLVM files=<n>
 *   VMFS: corrupted a <n>-byte read of <name>
 *   VMFS: failed a read of <name> at <offset>
 *   VMFS: SKIP no-shell-map
 *   VMFS: FAIL <stage>
 */
#include <axl.h>
#include <axl/axl-fs-provider.h>

#define VM_MAP            "AXLVM"
#define VM_SRC_DIR        "fs0:\\vm"
#define VM_MAX_FILES      8u
/* The largest read the media returns intact. Deliberately the fixture's own
   constant, not the library's: the fixture models the media, and must not
   move if the library's chunk size does. */
#define VM_RELIABLE_READ  (64u * 1024u)
/* Files named with this prefix fail every read past offset 0. */
#define VM_IOERR_PREFIX   "ioerr-"

typedef struct {
    char     name[64];
    uint8_t *data;
    size_t   len;
} VmFile;

struct AxlFsProviderFile {
    VmFile *file;          /* NULL for the root directory */
    size_t  cursor;        /* file byte cursor */
    size_t  dir_index;     /* root iteration cursor */
};

static VmFile vm_files[VM_MAX_FILES];
static size_t vm_count;
static void  *vm_handle;
static bool   vm_mapped;

static const char *
vm_basename(
    const char *path
    )
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static AxlFsStatus
vm_open(
    void               *backend_ctx,
    const char         *path,
    unsigned            mode,
    unsigned            attributes,
    AxlFsProviderFile **out,
    bool               *out_is_dir
    )
{
    (void)backend_ctx;
    (void)attributes;
    if (path == NULL || out == NULL || out_is_dir == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if ((mode & (AXL_FS_OPEN_CREATE | AXL_FS_OPEN_WRITE)) != 0) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }
    const char *base = vm_basename(path);
    VmFile     *file = NULL;
    if (*base != '\0') {
        for (size_t i = 0; i < vm_count; i++) {
            if (axl_strcasecmp(vm_files[i].name, base) == 0) {
                file = &vm_files[i];
                break;
            }
        }
        if (file == NULL) {
            return AXL_FS_ERR_NOT_FOUND;
        }
    }
    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->file     = file;
    *out        = f;
    *out_is_dir = (file == NULL);
    return AXL_FS_OK;
}

static AxlFsStatus
vm_close(
    AxlFsProviderFile *f
    )
{
    if (f == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    axl_free(f);
    return AXL_FS_OK;
}

/* The media's fault: a single read of more than VM_RELIABLE_READ bytes that
   covers the file's middle byte returns it inverted. */
static AxlFsStatus
vm_read(
    AxlFsProviderFile *f,
    void              *buf,
    size_t            *inout_size
    )
{
    if (f == NULL || buf == NULL || inout_size == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (f->file == NULL) {
        return AXL_FS_ERR_IS_DIR;
    }
    if (f->cursor > 0 && axl_str_has_prefix(f->file->name, VM_IOERR_PREFIX)) {
        axl_printf("VMFS: failed a read of %s at %zu\n", f->file->name,
                   f->cursor);
        return AXL_FS_ERR_IO;
    }
    size_t asked = *inout_size;
    size_t avail = (f->cursor < f->file->len) ? f->file->len - f->cursor : 0u;
    size_t n     = (asked < avail) ? asked : avail;
    size_t flip  = f->file->len / 2;
    axl_memcpy(buf, f->file->data + f->cursor, n);
    if (asked > VM_RELIABLE_READ && flip >= f->cursor && flip < f->cursor + n) {
        ((uint8_t *)buf)[flip - f->cursor] ^= 0xFFu;
        axl_printf("VMFS: corrupted a %zu-byte read of %s\n", asked,
                   f->file->name);
    }
    f->cursor  += n;
    *inout_size = n;
    return AXL_FS_OK;
}

static AxlFsStatus
vm_read_dir(
    AxlFsProviderFile *f,
    AxlFsEntry        *out,
    bool              *out_end
    )
{
    if (f == NULL || out == NULL || out_end == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (f->file != NULL) {
        return AXL_FS_ERR_NOT_DIR;
    }
    if (f->dir_index >= vm_count) {
        *out_end = true;
        return AXL_FS_OK;
    }
    const VmFile *v = &vm_files[f->dir_index++];
    *out_end         = false;
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    axl_strlcpy(out->name, v->name, sizeof(out->name));
    out->size        = v->len;
    out->mtime_unix  = 0;
    out->attributes  = AXL_FS_ATTR_READ_ONLY;
    return AXL_FS_OK;
}

static AxlFsStatus
vm_seek(
    AxlFsProviderFile *f,
    uint64_t           position
    )
{
    if (f == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (f->file == NULL) {
        if (position != 0) {
            return AXL_FS_ERR_UNSUPPORTED;
        }
        f->dir_index = 0;
        return AXL_FS_OK;
    }
    f->cursor = (position == UINT64_MAX) ? f->file->len : (size_t)position;
    return AXL_FS_OK;
}

static AxlFsStatus
vm_get_info(
    AxlFsProviderFile *f,
    AxlFsEntry        *out
    )
{
    if (f == NULL || out == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->mtime_unix  = 0;
    if (f->file == NULL) {
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
    } else {
        axl_strlcpy(out->name, f->file->name, sizeof(out->name));
        out->size       = f->file->len;
        out->attributes = AXL_FS_ATTR_READ_ONLY;
    }
    return AXL_FS_OK;
}

static const AxlFsProvider vm_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = vm_open,
    .close         = vm_close,
    .read          = vm_read,
    .read_dir      = vm_read_dir,
    .seek          = vm_seek,
    .get_info      = vm_get_info,
    .default_label = "VirtualMedia",
};

static const AxlGuid vm_guid = AXL_GUID(
    0x6d1f0a52, 0x3c47, 0x4b9e,
    0x8a, 0x21, 0x5e, 0x90, 0x7c, 0x33, 0xd4, 0x18);

/* Copy every regular file in VM_SRC_DIR into vm_files. */
static bool
vm_load_sources(void)
{
    AxlDir *dir = axl_dir_open(VM_SRC_DIR);
    if (dir == NULL) {
        return false;
    }
    AxlFsEntry e;
    bool       ok = true;
    while (ok && axl_dir_read(dir, &e)) {
        if (axl_fs_entry_is_dir(&e)) {
            continue;
        }
        if (vm_count >= VM_MAX_FILES
            || axl_strlen(e.name) >= sizeof(vm_files[0].name)) {
            ok = false;
            break;
        }
        AXL_AUTO_FREE char *src = axl_asprintf("%s\\%s", VM_SRC_DIR, e.name);
        VmFile             *v   = &vm_files[vm_count];
        void               *buf = NULL;
        if (src == NULL
            || axl_file_get_contents(src, &buf, &v->len) != AXL_OK) {
            ok = false;
            break;
        }
        v->data = buf;
        axl_strlcpy(v->name, e.name, sizeof(v->name));
        vm_count++;
    }
    axl_dir_close(dir);
    return ok && vm_count > 0;
}

static void
vm_free_sources(void)
{
    for (size_t i = 0; i < vm_count; i++) {
        axl_free(vm_files[i].data);
        vm_files[i].data = NULL;
    }
    vm_count = 0;
}

/* Publish and map as VM_MAP. False when there is no shell to map through. */
static bool
vm_fs_up(void)
{
    if (axl_fs_provider_publish(&vm_provider, &vm_guid, &vm_handle) != AXL_OK) {
        return false;
    }
    AxlVolume   vols[16];
    size_t      n  = 0;
    const void *dp = NULL;
    if (axl_volume_enumerate(vols, 16, &n) == AXL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (vols[i].handle == vm_handle) {
                dp = vols[i].device_path;
                break;
            }
        }
    }
    if (dp == NULL || axl_volume_set_map(dp, VM_MAP) != AXL_OK) {
        if (axl_fs_provider_unpublish(vm_handle) != AXL_OK) {
            axl_printf("VMFS: NOTE unpublish failed\n");
        }
        vm_handle = NULL;
        return false;
    }
    vm_mapped = true;
    return true;
}

static int
vmedia_fs_main(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    (void)image;
    (void)st;

    if (!vm_load_sources()) {
        axl_printf("VMFS: FAIL sources\n");
        vm_free_sources();
        return AXL_ERR;
    }
    if (!vm_fs_up()) {
        axl_printf("VMFS: SKIP no-shell-map\n");
        vm_free_sources();
        return AXL_ERR;
    }
    axl_printf("VMFS: READY volume=%s files=%zu\n", VM_MAP, vm_count);
    return AXL_OK;
}

static int
vmedia_fs_unload(
    AxlHandle image
    )
{
    (void)image;

    if (vm_mapped) {
        if (axl_volume_unmap(VM_MAP) != AXL_OK) {
            axl_printf("VMFS: NOTE map not removed\n");
        }
        vm_mapped = false;
    }
    if (vm_handle != NULL) {
        if (axl_fs_provider_unpublish(vm_handle) != AXL_OK) {
            axl_printf("VMFS: NOTE unpublish failed\n");
        }
        vm_handle = NULL;
    }
    vm_free_sources();
    return AXL_OK;
}

AXL_DRIVER(vmedia_fs_main, vmedia_fs_unload)
