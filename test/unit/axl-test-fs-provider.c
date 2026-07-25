/** @file axl-test-fs-provider.c
    Test application for AxlFsProvider — backend-neutral filesystem
    publisher, EFI_FILE_PROTOCOL + EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    thunks, AxlDevicePath vendor-path constructor.

    Strategy:
    - Mock provider with a small in-memory tree (read-only and
      read-write variants).
    - Publish via axl_fs_provider_publish, then exercise the
      synthesized EFI_FILE_PROTOCOL through the real EFI handle
      (open / read / dir-iter / get-info / write / set-info / delete /
      unpublish).
    - Pin the UCS-2 / UTF-8 boundary with a non-ASCII filename
      ("résumé.txt", "日本語.bin") round-trip through GetInfo.
    - Pin force-close-on-unpublish: open files survive the publish
      handle going away (next call returns AXL_FS_ERR_IO).
**/

#include "axl-test.h"
#include <axl/axl-device-path.h>
#include <axl/axl-fs-provider.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>
#include <uefi/axl-uefi.h>

static inline int
test_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Mock provider — tiny in-memory tree
// ---------------------------------------------------------------------------

typedef struct MockEntry {
    const char *name;             /* basename (UTF-8) */
    bool        is_dir;
    const char *content;          /* NUL-terminated; size = strlen */
} MockEntry;

/* Five-file flat root: regular ASCII files plus the two non-ASCII
   stress cases that pin the UTF-8 / UCS-2 boundary. The "subdir"
   entry tests the directory branch of read_dir + open dispatch. */
static const MockEntry g_root_entries[] = {
    { "alpha.txt",    false, "alpha contents\n" },
    { "beta.bin",     false, "BBBB" },
    { "résumé.txt",   false, "non-ascii body\n" },
    { "日本語.bin",   false, "JP" },
    { "subdir",       true,  NULL },
    { NULL, false, NULL }
};

struct AxlFsProviderFile {
    /* Provider-owned copy of the path. The thunk passes a buffer
       valid only for the duration of the `open` call (documented in
       <axl/axl-fs-provider.h>); providers that need to retain the
       path copy here. */
    char             path[256];
    bool             is_root;
    bool             is_dir;
    /* For files: cursor into content. */
    const MockEntry *entry;        /* NULL for root */
    size_t           cursor;
    /* For dirs: iteration position into g_root_entries. */
    size_t           dir_index;
    /* Marker so unpublish-force-close test can detect close was called. */
    bool             closed;
};

static int g_close_count;

/// Find an entry by absolute UTF-8 path in the flat root; NULL if
/// missing. Dogfoods axl_path_get_basename instead of rolling a
/// per-test loop.
static const MockEntry *
mock_find_by_path(const char *path)
{
    char *base = axl_path_get_basename(path);
    if (base == NULL) return NULL;
    const MockEntry *match = NULL;
    for (const MockEntry *e = g_root_entries; e->name != NULL; e++) {
        if (axl_strcmp(e->name, base) == 0) { match = e; break; }
    }
    axl_free(base);
    return match;
}

/// Borrowed pointer to the basename portion of `path`. Used by
/// get_info to populate the name field; not retained beyond the
/// call. Dogfoods axl_path_get_basename via a tiny per-call buffer.
static void
mock_copy_basename(const char *path, char *out, size_t out_size)
{
    char *b = axl_path_get_basename(path);
    axl_strlcpy(out, b != NULL ? b : "", out_size);
    axl_free(b);
}

static AxlFsStatus
mock_open(
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

    /* Root opens. */
    bool root = (path[0] == '\0' ||
                 (path[0] == '/' && path[1] == '\0'));
    const MockEntry *entry = NULL;
    bool is_dir;

    if (root) {
        is_dir = true;
    } else {
        entry = mock_find_by_path(path);
        if (entry == NULL) {
            if (!(mode & AXL_FS_OPEN_CREATE)) {
                return AXL_FS_ERR_NOT_FOUND;
            }
            /* Create-on-open is not supported in this read-mostly
               mock; the rw test uses a fixed slot below. */
            return AXL_FS_ERR_WRITE_PROTECTED;
        }
        is_dir = entry->is_dir;
    }

    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) return AXL_FS_ERR_NO_MEMORY;
    axl_strlcpy(f->path, path, sizeof(f->path));
    f->is_root = root;
    f->entry = entry;
    f->is_dir = is_dir;
    *out = f;
    *out_is_dir = is_dir;
    return AXL_FS_OK;
}

static AxlFsStatus
mock_close(AxlFsProviderFile *file)
{
    if (file == NULL) return AXL_FS_ERR_INVALID;
    file->closed = true;
    g_close_count++;
    axl_free(file);
    return AXL_FS_OK;
}

static AxlFsStatus
mock_read(AxlFsProviderFile *file, void *buf, size_t *inout_size)
{
    if (file == NULL || buf == NULL || inout_size == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->is_dir || file->entry == NULL) {
        return AXL_FS_ERR_IS_DIR;
    }
    size_t total = axl_strlen(file->entry->content);
    if (file->cursor >= total) {
        *inout_size = 0;
        return AXL_FS_OK;
    }
    size_t avail = total - file->cursor;
    size_t want  = *inout_size;
    size_t n     = (want < avail) ? want : avail;
    axl_memcpy(buf, file->entry->content + file->cursor, n);
    file->cursor += n;
    *inout_size = n;
    return AXL_FS_OK;
}

static AxlFsStatus
mock_read_dir(
    AxlFsProviderFile *file,
    AxlFsEntry        *out,
    bool              *out_end
    )
{
    if (file == NULL || out == NULL || out_end == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (!file->is_dir) {
        return AXL_FS_ERR_NOT_DIR;
    }
    if (g_root_entries[file->dir_index].name == NULL) {
        *out_end = true;
        return AXL_FS_OK;
    }
    const MockEntry *e = &g_root_entries[file->dir_index];
    file->dir_index++;
    *out_end = false;

    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_PROVIDER_VERSION;
    axl_strlcpy(out->name, e->name, sizeof(out->name));
    out->size        = e->is_dir ? 0u : axl_strlen(e->content);
    out->mtime_unix  = 0;
    out->attributes  = e->is_dir ? AXL_FS_ATTR_DIRECTORY : 0u;
    return AXL_FS_OK;
}

static AxlFsStatus
mock_seek(AxlFsProviderFile *file, uint64_t position)
{
    if (file == NULL) return AXL_FS_ERR_INVALID;
    if (file->is_dir) {
        if (position == 0) {
            file->dir_index = 0;
            return AXL_FS_OK;
        }
        return AXL_FS_ERR_UNSUPPORTED;
    }
    size_t total = (file->entry != NULL) ? axl_strlen(file->entry->content) : 0u;
    file->cursor = (position == (uint64_t)-1) ? total : (size_t)position;
    return AXL_FS_OK;
}

static AxlFsStatus
mock_get_info(AxlFsProviderFile *file, AxlFsEntry *out)
{
    if (file == NULL || out == NULL) return AXL_FS_ERR_INVALID;
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;

    if (file->is_root) {
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
    } else {
        mock_copy_basename(file->path, out->name, sizeof(out->name));
        out->size = (file->entry != NULL && !file->entry->is_dir)
                    ? axl_strlen(file->entry->content) : 0u;
        out->attributes = file->is_dir ? AXL_FS_ATTR_DIRECTORY : 0u;
    }
    out->mtime_unix = 0;
    return AXL_FS_OK;
}

static const AxlFsProvider g_mock_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = mock_open,
    .close         = mock_close,
    .read          = mock_read,
    .read_dir      = mock_read_dir,
    .seek          = mock_seek,
    .get_info      = mock_get_info,
    .default_label = "MockFs",
};

/* Vendor GUID for the mock filesystem. Random — only matters that
   it's unique to this test binary. */
static const AxlGuid g_mock_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4001,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd);

/* A second publication that DOES answer volume_info, so the volume
   space query has a deterministic backing store to read. The tests
   drive g_mock_space between calls to cover known / unknown figures;
   g_mock_provider (no volume_info callback) covers the thunk's
   synthesized (uint64_t)-1 "unknown" default. */
static uint64_t    g_mock_space_total  = 0;
static uint64_t    g_mock_space_free   = 0;
static AxlFsStatus g_mock_space_status = AXL_FS_OK;

static AxlFsStatus
mock_volume_info(
    void                    *backend_ctx,
    AxlFsProviderVolumeInfo *out
    )
{
    (void)backend_ctx;
    if (g_mock_space_status != AXL_FS_OK) {
        return g_mock_space_status;
    }
    out->read_only   = false;
    out->volume_size = g_mock_space_total;
    out->free_space  = g_mock_space_free;
    out->block_size  = 512;
    axl_strlcpy(out->label, "MockFs", sizeof(out->label));
    return AXL_FS_OK;
}

static const AxlFsProvider g_mock_sized_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = mock_open,
    .close         = mock_close,
    .read          = mock_read,
    .read_dir      = mock_read_dir,
    .seek          = mock_seek,
    .get_info      = mock_get_info,
    .volume_info   = mock_volume_info,
    .default_label = "MockFs",
};

static const AxlGuid g_mock_sized_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4002,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xce);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* axl_fs_provider_publish returns the underlying EFI_HANDLE as its
   opaque `void *out_handle` (documented in the header). Tests reach
   the synthesized SimpleFs interface via gBS->HandleProtocol — fine
   here because tests are allowed to speak EFI; consumers aren't. */
static EFI_FILE_PROTOCOL *
open_root_via_efi(void *publish_handle)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_STATUS s = gBS->HandleProtocol(
        (EFI_HANDLE)publish_handle,
        &gEfiSimpleFileSystemProtocolGuid,
        (void **)&sfs);
    if (s != EFI_SUCCESS || sfs == NULL) return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS) return NULL;
    return root;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void
test_publish_unpublish(void)
{
    void *handle = NULL;
    int   rc;

    rc = axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &handle);
    test_check(rc == AXL_OK && handle != NULL,
               "fs-provider: publish succeeds");

    rc = axl_fs_provider_unpublish(handle);
    test_check(rc == AXL_OK,
               "fs-provider: unpublish succeeds");

    /* Double unpublish is rejected. */
    test_check(axl_fs_provider_unpublish(handle) != AXL_OK,
               "fs-provider: double unpublish rejected");

    /* NULL is a no-op (returns OK or ERR; pinned as OK per docstring). */
    test_check(axl_fs_provider_unpublish(NULL) == AXL_OK,
               "fs-provider: unpublish(NULL) is no-op");
}

static void
test_publish_validates_vtable(void)
{
    AxlFsProvider bad = g_mock_provider;
    void *handle = NULL;

    /* Wrong struct_size (forward-compat probe). */
    bad.struct_size = 4;
    test_check(axl_fs_provider_publish(&bad, &g_mock_guid, &handle) != AXL_OK,
               "fs-provider: publish rejects bad struct_size");
    bad.struct_size = sizeof(AxlFsProvider);

    /* Required callback missing. */
    bad.open = NULL;
    test_check(axl_fs_provider_publish(&bad, &g_mock_guid, &handle) != AXL_OK,
               "fs-provider: publish rejects missing open");
    bad.open = g_mock_provider.open;

    /* NULL guid. */
    test_check(axl_fs_provider_publish(&g_mock_provider, NULL, &handle) != AXL_OK,
               "fs-provider: publish rejects NULL guid");

    /* NULL out_handle. */
    test_check(axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, NULL) != AXL_OK,
               "fs-provider: publish rejects NULL out_handle");
}

static void
test_open_root_and_dir_iter(void)
{
    void *handle = NULL;
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &handle)
            != AXL_OK) {
        test_fail("fs-provider: dir-iter setup: publish failed");
        return;
    }

    EFI_FILE_PROTOCOL *root = open_root_via_efi(handle);
    test_check(root != NULL, "fs-provider: OpenVolume returns root handle");
    if (root == NULL) {
        axl_fs_provider_unpublish(handle);
        return;
    }

    /* Read the directory; expect 5 entries (alpha.txt, beta.bin,
       résumé.txt, 日本語.bin, subdir). */
    int seen_alpha = 0, seen_resume = 0, seen_jp = 0, seen_subdir = 0;
    int total_entries = 0;
    for (;;) {
        uint8_t buf[512];
        UINTN   sz = sizeof(buf);
        EFI_STATUS s = root->Read(root, &sz, buf);
        if (s != EFI_SUCCESS || sz == 0) break;
        total_entries++;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        char utf8[256];
        axl_ucs2_to_utf8_buf((const unsigned short *)info->FileName,
                             utf8, sizeof(utf8));
        if (axl_strcmp(utf8, "alpha.txt") == 0)  seen_alpha = 1;
        if (axl_strcmp(utf8, "résumé.txt") == 0) seen_resume = 1;
        if (axl_strcmp(utf8, "日本語.bin") == 0) seen_jp = 1;
        if (axl_strcmp(utf8, "subdir") == 0 &&
            (info->Attribute & EFI_FILE_DIRECTORY)) seen_subdir = 1;
    }
    test_check(total_entries == 5,
               "fs-provider: dir-iter: 5 entries");
    test_check(seen_alpha,   "fs-provider: dir-iter: ASCII name");
    test_check(seen_resume,  "fs-provider: dir-iter: résumé.txt round-trip");
    test_check(seen_jp,      "fs-provider: dir-iter: 日本語.bin round-trip");
    test_check(seen_subdir,  "fs-provider: dir-iter: subdir flagged DIRECTORY");

    root->Close(root);
    axl_fs_provider_unpublish(handle);
}

static void
test_open_file_and_read(void)
{
    void *handle = NULL;
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &handle)
            != AXL_OK) {
        test_fail("fs-provider: open-read setup: publish failed");
        return;
    }
    EFI_FILE_PROTOCOL *root = open_root_via_efi(handle);
    if (root == NULL) {
        axl_fs_provider_unpublish(handle);
        test_fail("fs-provider: open-read: root NULL");
        return;
    }

    EFI_FILE_PROTOCOL *fh = NULL;
    /* CHAR16 literal "alpha.txt" — 9 chars + NUL. */
    CHAR16 name[] = { 'a','l','p','h','a','.','t','x','t', 0 };
    EFI_STATUS s = root->Open(root, &fh, name, EFI_FILE_MODE_READ, 0);
    test_check(s == EFI_SUCCESS && fh != NULL,
               "fs-provider: Open alpha.txt succeeds");
    if (fh == NULL) {
        root->Close(root);
        axl_fs_provider_unpublish(handle);
        return;
    }

    char rd[64];
    UINTN rd_sz = sizeof(rd);
    s = fh->Read(fh, &rd_sz, rd);
    test_check(s == EFI_SUCCESS && rd_sz == axl_strlen("alpha contents\n"),
               "fs-provider: Read returns full content size");
    rd[rd_sz] = '\0';
    test_check(axl_strcmp(rd, "alpha contents\n") == 0,
               "fs-provider: Read content matches");

    fh->Close(fh);
    root->Close(root);
    axl_fs_provider_unpublish(handle);
}

static void
test_get_info_non_ascii(void)
{
    void *handle = NULL;
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &handle)
            != AXL_OK) {
        test_fail("fs-provider: getinfo-non-ascii setup: publish failed");
        return;
    }
    EFI_FILE_PROTOCOL *root = open_root_via_efi(handle);
    if (root == NULL) {
        axl_fs_provider_unpublish(handle);
        test_fail("fs-provider: getinfo-non-ascii: root NULL");
        return;
    }

    /* Open "résumé.txt" via CHAR16 (UCS-2 BMP). */
    CHAR16 name[] = { 'r', 0x00E9, 's', 'u', 'm', 0x00E9, '.',
                      't', 'x', 't', 0 };
    EFI_FILE_PROTOCOL *fh = NULL;
    EFI_STATUS s = root->Open(root, &fh, name, EFI_FILE_MODE_READ, 0);
    test_check(s == EFI_SUCCESS && fh != NULL,
               "fs-provider: Open résumé.txt via UCS-2");
    if (fh == NULL) goto out;

    /* GetInfo probe-then-resize. */
    UINTN need = 0;
    s = fh->GetInfo(fh, &gEfiFileInfoGuid, &need, NULL);
    test_check(s == EFI_BUFFER_TOO_SMALL && need > 0,
               "fs-provider: GetInfo probe returns BUFFER_TOO_SMALL");

    uint8_t *buf = axl_malloc(need);
    if (buf == NULL) {
        test_fail("fs-provider: GetInfo non-ascii: malloc");
        fh->Close(fh);
        goto out;
    }
    s = fh->GetInfo(fh, &gEfiFileInfoGuid, &need, buf);
    test_check(s == EFI_SUCCESS, "fs-provider: GetInfo second call OK");

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
    /* Trailing UCS-2 name: r é s u m é . t x t \0 = 11 cells. */
    CHAR16 expect[] = { 'r', 0x00E9, 's', 'u', 'm', 0x00E9, '.',
                        't', 'x', 't', 0 };
    int eq = 1;
    for (int i = 0; i < 11; i++) {
        if (info->FileName[i] != expect[i]) { eq = 0; break; }
    }
    test_check(eq,
               "fs-provider: GetInfo trailer carries UCS-2 résumé.txt");
    test_check(info->FileSize == axl_strlen("non-ascii body\n"),
               "fs-provider: GetInfo FileSize matches mock");

    axl_free(buf);
    fh->Close(fh);
out:
    root->Close(root);
    axl_fs_provider_unpublish(handle);
}

static void
test_force_close_on_unpublish(void)
{
    void *handle = NULL;
    g_close_count = 0;
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &handle)
            != AXL_OK) {
        test_fail("fs-provider: force-close setup: publish failed");
        return;
    }
    EFI_FILE_PROTOCOL *root = open_root_via_efi(handle);
    if (root == NULL) {
        axl_fs_provider_unpublish(handle);
        test_fail("fs-provider: force-close: root NULL");
        return;
    }

    EFI_FILE_PROTOCOL *fh = NULL;
    CHAR16 name[] = { 'b','e','t','a','.','b','i','n', 0 };
    if (root->Open(root, &fh, name, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        test_fail("fs-provider: force-close: open beta.bin");
        root->Close(root);
        axl_fs_provider_unpublish(handle);
        return;
    }

    int before = g_close_count;
    /* Unpublish while two handles (root + beta.bin) are open. */
    axl_fs_provider_unpublish(handle);
    test_check(g_close_count >= before + 2,
               "fs-provider: unpublish force-closes outstanding handles");

    /* Subsequent calls on stale EFI_FILE_PROTOCOL pointer must
       return DEVICE_ERROR rather than fault. */
    UINTN sz = 16; uint8_t b[16];
    EFI_STATUS s = fh->Read(fh, &sz, b);
    test_check(s == EFI_DEVICE_ERROR,
               "fs-provider: stale handle Read returns DEVICE_ERROR");
}

static void
test_device_path_make_vendor(void)
{
    AxlGuid g = AXL_GUID(0xdeadbeef, 0xcafe, 0x4002,
                         0x80, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    AxlDevicePath *dp = NULL;
    int rc = axl_device_path_new_vendor(&g, &dp);
    test_check(rc == AXL_OK && dp != NULL,
               "device-path: make_vendor allocates");

    /* Layout: VENDOR_DEVICE_PATH header (Type=0x01, SubType=0x04,
       Length=20) followed by an END node (Type=0x7F, SubType=0xFF,
       Length=4). */
    const uint8_t *raw = (const uint8_t *)dp;
    test_check(raw[0] == 0x01 && raw[1] == 0x04,
               "device-path: vendor header type/subtype");
    uint16_t len = (uint16_t)(raw[2] | (raw[3] << 8));
    test_check(len == 20, "device-path: vendor node length=20");

    /* GUID lives at offset 4 (after the 4-byte header). */
    test_check(test_memcmp(raw + 4, &g, sizeof(g)) == 0,
               "device-path: GUID copied verbatim");

    const uint8_t *end = raw + len;
    test_check(end[0] == 0x7F && end[1] == 0xFF,
               "device-path: END node type/subtype");
    uint16_t end_len = (uint16_t)(end[2] | (end[3] << 8));
    test_check(end_len == 4, "device-path: END node length=4");

    test_check(axl_device_path_new_vendor(NULL, &dp) != AXL_OK,
               "device-path: rejects NULL guid");
    test_check(axl_device_path_new_vendor(&g, NULL) != AXL_OK,
               "device-path: rejects NULL out");
    axl_free(dp);
}

// ---------------------------------------------------------------------------
// Round-trip dogfooding: prove the consumer side of <axl/axl-fs.h> sees
// fs-provider publications transparently.
//
// 1. Publish the mock; walk axl_volume_enumerate; expect to find a
//    handle whose label is "MockFs". (Proves AxlVolume / volume-info
//    integration.)
// 2. Use axl_file_get_contents against the mock's "alpha.txt" via a
//    SimpleFs handle alias the SDK assigns. (Proves the consumer's
//    path-stat / read pipeline reaches our thunk correctly.)
// ---------------------------------------------------------------------------

static void
test_volume_enumerate_round_trip(void)
{
    void *handle = NULL;
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &handle)
            != AXL_OK) {
        test_fail("fs-provider: round-trip setup: publish failed");
        return;
    }

    AxlVolume volumes[16];
    size_t    n = 0;
    int rc = axl_volume_enumerate(volumes, 16, &n);
    test_check(rc == AXL_OK, "round-trip: axl_volume_enumerate returns OK");
    test_check(n > 0, "round-trip: at least one volume present");

    /* Locate ours by handle. The publish-handle IS the EFI handle, so
       AxlVolume.handle should match for one of the entries. */
    bool found = false;
    char *label = NULL;
    for (size_t i = 0; i < n; i++) {
        if (volumes[i].handle == handle) {
            found = true;
            label = axl_volume_get_label_by_handle(volumes[i].handle);
            break;
        }
    }
    test_check(found,
               "round-trip: published volume found in axl_volume_enumerate");
    test_check(label != NULL && axl_strcmp(label, "MockFs") == 0,
               "round-trip: axl_volume_get_label_by_handle returns 'MockFs'");
    axl_free(label);

    axl_fs_provider_unpublish(handle);
}

// ---------------------------------------------------------------------------
// axl_volume_get_space_by_handle — exact figures, and the distinction
// between "reported" and "cannot report".
//
// The mock provider is the only volume in the suite whose free space we
// control, so it is what pins the numbers. UINT64_MAX is the provider
// layer's documented "unknown" marker (AxlFsProviderVolumeInfo) and must
// surface as AXL_UNSUPPORTED, never as a plausible figure -- a caller
// sizing an upload has to tell "no room" from "cannot say".
// ---------------------------------------------------------------------------

#define SPACE_UNTOUCHED  0xD15EA5EDD15EA5EDull

static void
test_volume_get_space(void)
{
    void *handle = NULL;
    if (axl_fs_provider_publish(&g_mock_sized_provider, &g_mock_sized_guid,
                                &handle) != AXL_OK) {
        test_fail("fs-provider: get_space setup: publish failed");
        return;
    }

    uint64_t total = SPACE_UNTOUCHED;
    uint64_t avail = SPACE_UNTOUCHED;

    /* Both figures known: exact pass-through, no rounding, no scaling. */
    g_mock_space_total = 64ull * 1024 * 1024;
    g_mock_space_free  = 12ull * 1024 * 1024 + 3;
    test_check(axl_volume_get_space_by_handle(handle, &total, &avail) == AXL_OK,
               "get_space: known figures return AXL_OK");
    test_check(total == 64ull * 1024 * 1024,
               "get_space: total is the volume's exact VolumeSize");
    test_check(avail == 12ull * 1024 * 1024 + 3,
               "get_space: free is the volume's exact FreeSpace");

    /* Free unknown: a free-space query must fail, and must NOT write a
       value -- the whole point is that the caller cannot mistake it for
       a real figure. */
    g_mock_space_free = (uint64_t)-1;
    total = SPACE_UNTOUCHED;
    avail = SPACE_UNTOUCHED;
    test_check(axl_volume_get_space_by_handle(handle, &total, &avail)
                   == AXL_UNSUPPORTED,
               "get_space: unknown free space returns AXL_UNSUPPORTED");
    test_check(avail == SPACE_UNTOUCHED && total == SPACE_UNTOUCHED,
               "get_space: out params untouched on AXL_UNSUPPORTED");

    /* ...but a caller that only asked for the total still gets it: only
       the figures actually requested gate the result. */
    total = SPACE_UNTOUCHED;
    test_check(axl_volume_get_space_by_handle(handle, &total, NULL) == AXL_OK,
               "get_space: total-only query unaffected by unknown free");
    test_check(total == 64ull * 1024 * 1024,
               "get_space: total-only query writes the total");

    /* Symmetric case: total unknown, free known, free-only query. */
    g_mock_space_total = (uint64_t)-1;
    g_mock_space_free  = 4096;
    avail = SPACE_UNTOUCHED;
    test_check(axl_volume_get_space_by_handle(handle, NULL, &avail) == AXL_OK,
               "get_space: free-only query unaffected by unknown total");
    test_check(avail == 4096,
               "get_space: free-only query writes the free space");
    total = SPACE_UNTOUCHED;
    test_check(axl_volume_get_space_by_handle(handle, &total, &avail)
                   == AXL_UNSUPPORTED,
               "get_space: unknown total fails a total+free query");

    /* A volume that FAILS is not a volume that cannot say. A device
       error must not be laundered into AXL_UNSUPPORTED -- the caller
       distinguishes "decide some other way" from "this volume is
       sick", and only one of those is worth retrying. */
    g_mock_space_status = AXL_FS_ERR_IO;
    total = SPACE_UNTOUCHED;
    avail = SPACE_UNTOUCHED;
    test_check(axl_volume_get_space_by_handle(handle, &total, &avail) == AXL_ERR,
               "get_space: a device error is AXL_ERR, not AXL_UNSUPPORTED");
    test_check(total == SPACE_UNTOUCHED && avail == SPACE_UNTOUCHED,
               "get_space: out params untouched on a device error");

    /* A volume that genuinely has no filesystem information is the
       AXL_UNSUPPORTED case, and stays distinct from the above. */
    g_mock_space_status = AXL_FS_ERR_UNSUPPORTED;
    test_check(axl_volume_get_space_by_handle(handle, &total, &avail)
                   == AXL_UNSUPPORTED,
               "get_space: no filesystem information is AXL_UNSUPPORTED");
    g_mock_space_status = AXL_FS_OK;

    /* Argument validation. */
    test_check(axl_volume_get_space_by_handle(NULL, &total, &avail) == AXL_ERR,
               "get_space: NULL handle is AXL_ERR");
    test_check(axl_volume_get_space_by_handle(handle, NULL, NULL) == AXL_ERR,
               "get_space: asking for neither figure is AXL_ERR");

    axl_fs_provider_unpublish(handle);

    /* A provider with no volume_info callback at all: the thunk
       synthesizes (uint64_t)-1 for both, which must read as
       "cannot report", not as a full or empty volume. */
    void *plain = NULL;
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &plain)
            != AXL_OK) {
        test_fail("fs-provider: get_space setup: plain publish failed");
        return;
    }
    total = SPACE_UNTOUCHED;
    avail = SPACE_UNTOUCHED;
    test_check(axl_volume_get_space_by_handle(plain, &total, &avail)
                   == AXL_UNSUPPORTED,
               "get_space: provider without volume_info is AXL_UNSUPPORTED");
    test_check(total == SPACE_UNTOUCHED && avail == SPACE_UNTOUCHED,
               "get_space: no-volume_info leaves out params untouched");
    axl_fs_provider_unpublish(plain);

    /* Don't leave the mock's volume figures parked on the "unknown"
       marker for whatever test is appended after this one. */
    g_mock_space_total  = 0;
    g_mock_space_free   = 0;
    g_mock_space_status = AXL_FS_OK;
}

// A provider that ACCEPTS a size change and ignores it — the fixture for
// axl_file_truncate's post-resize re-read.
//
// <axl/axl-fs-provider.h> tells authors SetInfo exists for renames and
// attribute changes; a provider written to that contract reads `in->name`
// and `in->attributes`, never `in->size`, and returns AXL_FS_OK. Without
// the re-read, axl_file_truncate would report AXL_OK for a resize that
// never happened — on this whole class of volumes. The single assertion
// below is what fails if the re-read in axl_file_truncate is removed;
// the FAT-backed tests in AxlTestIO cannot catch that, because there the
// size really does change.
// ---------------------------------------------------------------------------

#define LIAR_FILE_NAME  "f"
#define LIAR_FILE_BODY  "abc"
#define LIAR_FILE_SIZE  3u

static bool
liar_is_our_file(const char *path)
{
    char *base = axl_path_get_basename(path);
    bool  ours = (base != NULL && axl_strcmp(base, LIAR_FILE_NAME) == 0);
    axl_free(base);
    return ours;
}

static AxlFsStatus
liar_open(
    void               *backend_ctx,
    const char         *path,
    unsigned            mode,
    unsigned            attributes,
    AxlFsProviderFile **out,
    bool               *out_is_dir
    )
{
    (void)backend_ctx;
    (void)mode;
    (void)attributes;
    if (path == NULL || out == NULL || out_is_dir == NULL) {
        return AXL_FS_ERR_INVALID;
    }

    bool root = (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'));
    if (!root && !liar_is_our_file(path)) {
        return AXL_FS_ERR_NOT_FOUND;
    }

    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) return AXL_FS_ERR_NO_MEMORY;
    axl_strlcpy(f->path, path, sizeof(f->path));
    f->is_root  = root;
    f->is_dir   = root;
    *out        = f;
    *out_is_dir = root;
    return AXL_FS_OK;
}

static AxlFsStatus
liar_close(AxlFsProviderFile *file)
{
    if (file == NULL) return AXL_FS_ERR_INVALID;
    axl_free(file);
    return AXL_FS_OK;
}

static AxlFsStatus
liar_read(AxlFsProviderFile *file, void *buf, size_t *inout_size)
{
    if (file == NULL || buf == NULL || inout_size == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->is_dir) return AXL_FS_ERR_IS_DIR;
    size_t avail = (file->cursor < LIAR_FILE_SIZE)
                 ? (LIAR_FILE_SIZE - file->cursor) : 0u;
    size_t n = (*inout_size < avail) ? *inout_size : avail;
    axl_memcpy(buf, LIAR_FILE_BODY + file->cursor, n);
    file->cursor += n;
    *inout_size = n;
    return AXL_FS_OK;
}

static AxlFsStatus
liar_read_dir(AxlFsProviderFile *file, AxlFsEntry *out, bool *out_end)
{
    if (file == NULL || out == NULL || out_end == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (!file->is_dir) return AXL_FS_ERR_NOT_DIR;
    if (file->dir_index > 0) {
        *out_end = true;
        return AXL_FS_OK;
    }
    file->dir_index++;
    *out_end = false;
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    axl_strlcpy(out->name, LIAR_FILE_NAME, sizeof(out->name));
    out->size        = LIAR_FILE_SIZE;
    out->mtime_unix  = 0;
    out->attributes  = 0;
    return AXL_FS_OK;
}

static AxlFsStatus
liar_seek(AxlFsProviderFile *file, uint64_t position)
{
    if (file == NULL) return AXL_FS_ERR_INVALID;
    if (file->is_dir) {
        if (position != 0) return AXL_FS_ERR_UNSUPPORTED;
        file->dir_index = 0;
        return AXL_FS_OK;
    }
    file->cursor = (position == (uint64_t)-1)
                 ? LIAR_FILE_SIZE : (size_t)position;
    return AXL_FS_OK;
}

/* Always reports the file's ORIGINAL length — set_info never changed it. */
static AxlFsStatus
liar_get_info(AxlFsProviderFile *file, AxlFsEntry *out)
{
    if (file == NULL || out == NULL) return AXL_FS_ERR_INVALID;
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->mtime_unix  = 0;
    if (file->is_root) {
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
    } else {
        axl_strlcpy(out->name, LIAR_FILE_NAME, sizeof(out->name));
        out->size       = LIAR_FILE_SIZE;
        out->attributes = 0;
    }
    return AXL_FS_OK;
}

/* Success, but the size in @p in is dropped on the floor — the exact
   rename-and-attributes-only implementation the header describes. */
static AxlFsStatus
liar_set_info(AxlFsProviderFile *file, const AxlFsEntry *in)
{
    if (file == NULL || in == NULL) return AXL_FS_ERR_INVALID;
    return AXL_FS_OK;
}

static const AxlFsProvider g_liar_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = liar_open,
    .close         = liar_close,
    .read          = liar_read,
    .read_dir      = liar_read_dir,
    .seek          = liar_seek,
    .get_info      = liar_get_info,
    .set_info      = liar_set_info,
    .default_label = "LiarFs",
};

static const AxlGuid g_liar_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4002,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xce);

static void
test_truncate_over_size_ignoring_provider(void)
{
    void *handle = NULL;
    if (axl_fs_provider_publish(&g_liar_provider, &g_liar_guid, &handle)
            != AXL_OK) {
        test_fail("liar-fs: publish failed");
        return;
    }

    /* Reach the publication by path the way any consumer would: find its
       device path via axl_volume_enumerate, then give it a shell map name.
       SetMap needs a shell, so the no-shell case is a legitimate SKIP. */
    AxlVolume   vols[16];
    size_t      n  = 0;
    const void *dp = NULL;
    if (axl_volume_enumerate(vols, 16, &n) == AXL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (vols[i].handle == handle) {
                dp = vols[i].device_path;
                break;
            }
        }
    }

    const char *map  = "AXLLIAR";
    const char *path = "AXLLIAR:\\" LIAR_FILE_NAME;
    if (dp != NULL && axl_volume_set_map(dp, map) == AXL_OK) {
        AxlFsEntry e;
        test_check(axl_file_info(path, &e) == AXL_OK
                   && e.size == LIAR_FILE_SIZE,
                   "liar-fs: published file reachable by path, 3 bytes");
        test_check(axl_file_truncate(path, 5) == AXL_ERR,
                   "liar-fs: truncate over a size-ignoring set_info -> AXL_ERR");
        test_check(axl_file_info(path, &e) == AXL_OK
                   && e.size == LIAR_FILE_SIZE,
                   "liar-fs: refused truncate left the length at 3");
        test_check(axl_volume_unmap(map) == AXL_OK,
                   "liar-fs: test mapping removed");
    } else {
        axl_printf("SKIP: liar-fs truncate (no shell map for the "
                   "published volume)\n");
        test_check(true, "liar-fs: truncate SKIP balance");
        test_check(true, "liar-fs: truncate SKIP balance");
        test_check(true, "liar-fs: truncate SKIP balance");
        test_check(true, "liar-fs: truncate SKIP balance");
    }

    axl_fs_provider_unpublish(handle);
}

// ---------------------------------------------------------------------------
// Status-code numbering convention
// ---------------------------------------------------------------------------

/* AxlFsStatus follows the Axl<Module>Status convention: _OK == 0, every
   error member strictly negative (matching AxlStatus). Pin the exact
   values so a stray positive member is caught at test time. */
static void
test_status_codes_negative_convention(void)
{
    test_check(AXL_FS_OK == 0,
               "status: AXL_FS_OK is 0");
    test_check(AXL_FS_ERR_NOT_FOUND        == -1,  "status: NOT_FOUND == -1");
    test_check(AXL_FS_ERR_ACCESS_DENIED    == -2,  "status: ACCESS_DENIED == -2");
    test_check(AXL_FS_ERR_WRITE_PROTECTED  == -3,  "status: WRITE_PROTECTED == -3");
    test_check(AXL_FS_ERR_NO_SPACE         == -4,  "status: NO_SPACE == -4");
    test_check(AXL_FS_ERR_NOT_DIR          == -5,  "status: NOT_DIR == -5");
    test_check(AXL_FS_ERR_IS_DIR           == -6,  "status: IS_DIR == -6");
    test_check(AXL_FS_ERR_INVALID          == -7,  "status: INVALID == -7");
    test_check(AXL_FS_ERR_NO_MEMORY        == -8,  "status: NO_MEMORY == -8");
    test_check(AXL_FS_ERR_IO               == -9,  "status: IO == -9");
    test_check(AXL_FS_ERR_UNSUPPORTED      == -10, "status: UNSUPPORTED == -10");
    test_check(AXL_FS_ERR_END_OF_FILE      == -11, "status: END_OF_FILE == -11");
    test_check(AXL_FS_ERR_VOLUME_CORRUPTED == -12, "status: VOLUME_CORRUPTED == -12");
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

int
test_fs_provider_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlFsProvider");

    test_device_path_make_vendor();
    test_publish_unpublish();
    test_publish_validates_vtable();
    test_open_root_and_dir_iter();
    test_open_file_and_read();
    test_get_info_non_ascii();
    test_volume_enumerate_round_trip();
    test_volume_get_space();
    test_truncate_over_size_ignoring_provider();
    test_force_close_on_unpublish();
    test_status_codes_negative_convention();

    return test_print_results();
}

AXL_APP(test_fs_provider_main)
