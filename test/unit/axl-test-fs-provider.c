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
#include "../../src/fs/axl-fs-internal.h"
#include <axl/axl-device-path.h>
#include <axl/axl-file-view.h>
#include <axl/axl-fs-provider.h>
#include <axl/axl-http-server.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-shell.h>
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
    /* The tree provider (tp_*): which node this handle is. */
    int              tp_node;
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

    /* Closing an ORPHANED handle still reclaims its thunk. Unpublish
       deliberately leaves the thunk allocated — a UEFI consumer may hold the
       EFI_FILE_PROTOCOL pointer indefinitely and the spec does not bound
       that — so Close is the only reclaim point, and a test that walks away
       without calling it leaks (which is exactly what the teardown leak gate
       caught here). Close after the orphan step touches no Publication and
       makes no provider call; per UEFI 2.11 13.5.4 it must still say
       SUCCESS. Nothing may dereference fh or root after this point. */
    test_check(fh->Close(fh) == EFI_SUCCESS,
               "fs-provider: Close on an orphaned handle returns SUCCESS");
    test_check(root->Close(root) == EFI_SUCCESS,
               "fs-provider: Close reclaims the orphaned root thunk");
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
#define LIAR_MAP        "AXLLIAR"
#define LIAR_PATH       LIAR_MAP ":\\" LIAR_FILE_NAME

/* Report the file as carrying the read-only attribute -- which this
   provider still accepts every set_info for, as the FAT driver does. */
static bool g_liar_read_only;

/* "Ghost" mode: set_info DOES rename -- the file opens under the new name
   too -- while get_info keeps reporting the old one. The rename lands and
   its verification fails, which is the one way a move-aside can leave the
   source somewhere the caller was never told about. */
static bool g_liar_ghost;
static char g_liar_alias[64];

static bool
liar_is_our_file(const char *path)
{
    char *base = axl_path_get_basename(path);
    bool  ours = (base != NULL
                  && (axl_strcmp(base, LIAR_FILE_NAME) == 0
                      || (g_liar_alias[0] != '\0'
                          && axl_strcmp(base, g_liar_alias) == 0)));
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
    axl_memcpy(buf, &LIAR_FILE_BODY[file->cursor], n);
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
        out->attributes = g_liar_read_only ? AXL_FS_ATTR_READ_ONLY : 0u;
    }
    return AXL_FS_OK;
}

/* Success, but the size in @p in is dropped on the floor — the exact
   rename-and-attributes-only implementation the header describes. */
static AxlFsStatus
liar_set_info(AxlFsProviderFile *file, const AxlFsEntry *in)
{
    if (file == NULL || in == NULL) return AXL_FS_ERR_INVALID;
    if (g_liar_ghost && in->name[0] != '\0') {
        axl_strlcpy(g_liar_alias, in->name, sizeof(g_liar_alias));
    }
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

/* Publish the liar volume and reach it by path the way any consumer would:
   find its device path via axl_volume_enumerate, then give it a shell map
   name. SetMap needs a shell, so false (published or not; @p handle says)
   is a legitimate SKIP for the caller. */
static bool
liar_up(void **handle)
{
    *handle = NULL;
    if (axl_fs_provider_publish(&g_liar_provider, &g_liar_guid, handle)
            != AXL_OK) {
        test_fail("liar-fs: publish failed");
        *handle = NULL;
        return false;
    }

    AxlVolume   vols[16];
    size_t      n  = 0;
    const void *dp = NULL;
    if (axl_volume_enumerate(vols, 16, &n) == AXL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (vols[i].handle == *handle) {
                dp = vols[i].device_path;
                break;
            }
        }
    }
    return dp != NULL && axl_volume_set_map(dp, LIAR_MAP) == AXL_OK;
}

static void
test_truncate_over_size_ignoring_provider(void)
{
    void       *handle = NULL;
    const char *map    = LIAR_MAP;
    const char *path   = LIAR_PATH;

    if (liar_up(&handle)) {
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
        test_skip_n(4, "liar-fs truncate (no shell map for the published volume)");
    }

    if (handle != NULL) {
        axl_fs_provider_unpublish(handle);
    }
}

/* A filesystem that answers AXL_FS_OK to every set_info and changes
   nothing: every metadata call that promises VERIFIED must say AXL_ERR,
   and one that is told the entry is read-only must say AXL_DENIED. */
static void
test_metadata_calls_over_a_lying_provider(void)
{
    void      *handle = NULL;
    AxlStream *s;

    if (!liar_up(&handle)) {
        test_skip_n(13, "liar-fs metadata (no shell map for the published volume)");
        if (handle != NULL) {
            axl_fs_provider_unpublish(handle);
        }
        return;
    }

    test_check(axl_file_rename(LIAR_PATH, "g") == AXL_ERR,
               "liar-fs: rename the provider never performs -> AXL_ERR");
    /* "f." strips to "f", the name the file already had: still no rename. */
    test_check(axl_file_rename(LIAR_PATH, "f.") == AXL_ERR,
               "liar-fs: a name that strips to the old one is not a rename");
    test_check(axl_file_set_mtime(LIAR_PATH, 1000000000) == AXL_ERR,
               "liar-fs: set_mtime the provider never keeps -> AXL_ERR");

    s = axl_fopen(LIAR_PATH, "w");
    test_check(s != NULL && axl_ftruncate(s, 5) == AXL_ERR,
               "liar-fs: ftruncate the provider ignores -> AXL_ERR");
    test_check(s != NULL && axl_fset_mtime(s, 1000000000) == AXL_ERR,
               "liar-fs: fset_mtime the provider never keeps -> AXL_ERR");
    AxlFsEntry e;
    test_check(s != NULL && axl_fstat(s, &e) == AXL_OK
               && axl_strcmp(e.name, LIAR_FILE_NAME) == 0
               && e.size == LIAR_FILE_SIZE,
               "liar-fs: fstat still reads the file: f, 3 bytes");
    axl_fclose(s);

    g_liar_read_only = true;
    test_check(axl_file_set_mtime(LIAR_PATH, 1000000000) == AXL_DENIED,
               "liar-fs: set_mtime on a read-only entry -> AXL_DENIED");
    s = axl_fopen(LIAR_PATH, "w");
    test_check(s != NULL && axl_fset_mtime(s, 1000000000) == AXL_DENIED,
               "liar-fs: fset_mtime on a read-only entry -> AXL_DENIED");
    axl_fclose(s);
    test_check(axl_file_replace(LIAR_PATH, "g", 0, NULL) == AXL_DENIED,
               "liar-fs: replace of a read-only entry -> AXL_DENIED");
    g_liar_read_only = false;

    /* A rename the provider answers "done" to and never performs: the new
       name even resolves (the ghost), but so does the old one, so the move
       did not happen. AXL_ERR, nothing removed, nothing to report. */
    g_liar_ghost = true;
    test_log_quiet_begin("fs");
    int  mrc   = axl_file_move(LIAR_PATH, LIAR_MAP ":\\moved");
    bool quiet = test_log_quiet_armed && test_log_quiet_hits == 0;
    test_log_quiet_abort();
    test_check(mrc == AXL_ERR && quiet && axl_file_info(LIAR_PATH, &e) == AXL_OK,
               "liar-fs: a move the provider never performs -> AXL_ERR, the file kept");
    g_liar_ghost    = false;
    g_liar_alias[0] = '\0';

    /* No `del`: the thunk answers EFI_WARN_DELETE_FAILURE -- a WARNING,
       which EFI_ERROR() reads as success. The delete must report it as
       the failure it is. */
    test_check(axl_file_delete(LIAR_PATH) == AXL_ERR,
               "liar-fs: delete on a provider without del -> AXL_ERR");
    test_check(axl_file_info(LIAR_PATH, &e) == AXL_OK
               && e.size == LIAR_FILE_SIZE,
               "liar-fs: the file is still reachable and 3 bytes");
    test_check(axl_volume_unmap(LIAR_MAP) == AXL_OK,
               "liar-fs: test mapping removed");
    axl_fs_provider_unpublish(handle);
}

// ---------------------------------------------------------------------------
// A provider with one renamable directory holding one file -- the fixture
// for "a directory renamed in place changes every path below it". FAT
// cannot pin that: a view holds its file open, and the FAT driver refuses
// to rename a directory with an open file below it. This provider does not.
// ---------------------------------------------------------------------------

#define MVD_MAP   "AXLMVD"
#define MVD_FILE  "x"
#define MVD_BODY  "X"

static char g_mvd_dir[32] = "d";
/* A second, EMPTY directory at the root, and whether each directory still
   exists. Deleting "d" is RECURSIVE here -- the file below it goes too --
   which is what makes a directory moved onto a non-empty one observable:
   FAT refuses to delete a non-empty directory and so hides the defect. */
static char g_mvd_src[32] = "s";
static bool g_mvd_dir_alive;
static bool g_mvd_src_alive;
/* The file below "d" answers a delete with success and stays: a provider
   that lies about delete, which a recursive remove must not loop on. */
static bool g_mvd_lying_del;

/* Node a path names: 0 root, 1 the directory, 2 the file below it, 3 the
   empty directory, -1 nothing. */
static int
mvd_node(const char *path)
{
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return 0;
    }
    const char *p   = (path[0] == '/') ? path + 1 : path;
    if (g_mvd_src_alive && axl_strcmp(p, g_mvd_src) == 0) {
        return 3;
    }
    size_t      dln = axl_strlen(g_mvd_dir);
    if (!g_mvd_dir_alive || axl_strncmp(p, g_mvd_dir, dln) != 0) {
        return -1;
    }
    if (p[dln] == '\0') {
        return 1;
    }
    if (p[dln] == '/' && axl_strcmp(p + dln + 1, MVD_FILE) == 0) {
        return 2;
    }
    return -1;
}

static AxlFsStatus
mvd_open(
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
    int node = mvd_node(path);
    if (node < 0) {
        return AXL_FS_ERR_NOT_FOUND;
    }
    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->is_root    = (node == 0);
    f->is_dir     = (node != 2);
    f->cursor     = 0;
    f->dir_index  = (size_t)node;   /* which node this handle is */
    *out          = f;
    *out_is_dir   = f->is_dir;
    return AXL_FS_OK;
}

static AxlFsStatus
mvd_close(AxlFsProviderFile *file)
{
    axl_free(file);
    return AXL_FS_OK;
}

static AxlFsStatus
mvd_read(AxlFsProviderFile *file, void *buf, size_t *inout_size)
{
    if (file == NULL || buf == NULL || inout_size == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->is_dir) {
        return AXL_FS_ERR_IS_DIR;
    }
    size_t len   = sizeof(MVD_BODY) - 1;
    size_t avail = (file->cursor < len) ? len - file->cursor : 0;
    size_t n     = (*inout_size < avail) ? *inout_size : avail;
    axl_memcpy(buf, &MVD_BODY[file->cursor], n);
    file->cursor += n;
    *inout_size   = n;
    return AXL_FS_OK;
}

static AxlFsStatus
mvd_read_dir(AxlFsProviderFile *file, AxlFsEntry *out, bool *out_end)
{
    if (file == NULL || out == NULL || out_end == NULL || !file->is_dir) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->cursor > 0 || file->dir_index == 3) {
        *out_end = true;   /* the empty directory lists nothing */
        return AXL_FS_OK;
    }
    file->cursor = 1;
    *out_end = false;
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    bool root = file->is_root;
    axl_strlcpy(out->name, root ? g_mvd_dir : MVD_FILE, sizeof(out->name));
    out->size        = root ? 0 : sizeof(MVD_BODY) - 1;
    out->mtime_unix  = 0;
    out->attributes  = root ? AXL_FS_ATTR_DIRECTORY : 0u;
    return AXL_FS_OK;
}

static AxlFsStatus
mvd_seek(AxlFsProviderFile *file, uint64_t position)
{
    if (file == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    file->cursor = (position == (uint64_t)-1) ? sizeof(MVD_BODY) - 1
                                              : (size_t)position;
    return AXL_FS_OK;
}

static AxlFsStatus
mvd_get_info(AxlFsProviderFile *file, AxlFsEntry *out)
{
    if (file == NULL || out == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->mtime_unix  = 0;
    switch (file->dir_index) {
    case 0:
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
        break;
    case 1:
        axl_strlcpy(out->name, g_mvd_dir, sizeof(out->name));
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
        break;
    case 3:
        axl_strlcpy(out->name, g_mvd_src, sizeof(out->name));
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
        break;
    default:
        axl_strlcpy(out->name, MVD_FILE, sizeof(out->name));
        out->size       = sizeof(MVD_BODY) - 1;
        out->attributes = 0;
        break;
    }
    return AXL_FS_OK;
}

/* Renames either directory; anything else is accepted and ignored. */
static AxlFsStatus
mvd_set_info(AxlFsProviderFile *file, const AxlFsEntry *in)
{
    if (file == NULL || in == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->dir_index == 1 && in->name[0] != '\0') {
        axl_strlcpy(g_mvd_dir, in->name, sizeof(g_mvd_dir));
    }
    if (file->dir_index == 3 && in->name[0] != '\0') {
        axl_strlcpy(g_mvd_src, in->name, sizeof(g_mvd_src));
    }
    return AXL_FS_OK;
}

/* Deletes a directory WITH what is below it. */
static AxlFsStatus
mvd_del(AxlFsProviderFile *file)
{
    if (file == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->dir_index == 1) {
        g_mvd_dir_alive = false;
    } else if (file->dir_index == 2 && g_mvd_lying_del) {
        return AXL_FS_OK;   /* "deleted", and still there */
    } else if (file->dir_index == 3) {
        g_mvd_src_alive = false;
    } else {
        return AXL_FS_ERR_ACCESS_DENIED;
    }
    return AXL_FS_OK;
}

static const AxlFsProvider g_mvd_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = mvd_open,
    .close         = mvd_close,
    .read          = mvd_read,
    .read_dir      = mvd_read_dir,
    .seek          = mvd_seek,
    .get_info      = mvd_get_info,
    .set_info      = mvd_set_info,
    .del           = mvd_del,
    .default_label = "MvDir",
};

static const AxlGuid g_mvd_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4003,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcf);

/* Regression: renaming a directory IN PLACE changes the path of everything
   below it just as a move does, so an AxlFileView open below it must be
   told. Only a move to another directory invalidated every view. */
static void
test_rename_dir_in_place_reaches_a_view_below_it(void)
{
    void *handle = NULL;
    axl_strlcpy(g_mvd_dir, "d", sizeof(g_mvd_dir));
    axl_strlcpy(g_mvd_src, "s", sizeof(g_mvd_src));
    g_mvd_dir_alive = true;
    g_mvd_src_alive = true;
    if (axl_fs_provider_publish(&g_mvd_provider, &g_mvd_guid, &handle)
            != AXL_OK) {
        test_fail("mvdir: publish failed");
        return;
    }
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
    if (dp == NULL || axl_volume_set_map(dp, MVD_MAP) != AXL_OK) {
        test_skip_n(6, "mvdir (no shell map for the published volume)");
        axl_fs_provider_unpublish(handle);
        return;
    }

    /* A view holds its file open, so the directory above it cannot be
       renamed -- refused as the FAT driver refuses it, by every provider. */
    AxlFileView *v = axl_file_view_open(MVD_MAP ":\\d\\" MVD_FILE, 4096, 2);
    char         b[4] = { 0 };
    test_check(v != NULL && axl_file_view_read(v, 0, b, 1) == 1 && b[0] == 'X',
               "mvdir: a view open on d\\x reads X");
    test_check(axl_file_rename(MVD_MAP ":\\d", "e") == AXL_ERR
               && axl_strcmp(g_mvd_dir, "d") == 0,
               "mvdir: a directory with a view open below it is not renamed");
    axl_file_view_close(v);
    /* With nothing open below it the rename in place goes through, and a
       name below it no longer resolves. */
    test_check(axl_file_rename(MVD_MAP ":\\d", "e") == AXL_OK
               && axl_strcmp(g_mvd_dir, "e") == 0,
               "mvdir: the directory renamed in place, d -> e");
    AxlFsEntry gone;
    test_check(axl_file_info(MVD_MAP ":\\d\\" MVD_FILE, &gone) != AXL_OK
               && axl_file_info(MVD_MAP ":\\e\\" MVD_FILE, &gone) == AXL_OK,
               "mvdir: d\\x is gone, e\\x is there");

    /* POSIX ENOTEMPTY: a directory onto a non-empty one is refused before
       anything is touched -- this provider deletes recursively, so a
       move that removed the destination would take the file below it. */
    char x[4] = { 0 };
    test_check(axl_file_move(MVD_MAP ":\\s", MVD_MAP ":\\e") == AXL_NOT_EMPTY,
               "mvdir: an empty directory onto a non-empty one -> AXL_NOT_EMPTY");
    AxlStream *xs = axl_fopen(MVD_MAP ":\\e\\" MVD_FILE, "r");
    test_check(xs != NULL && axl_read(xs, x, 1) == 1 && x[0] == 'X'
               && g_mvd_src_alive && axl_strcmp(g_mvd_src, "s") == 0,
               "mvdir: the non-empty directory and the source are intact");
    axl_fclose(xs);

    if (axl_volume_unmap(MVD_MAP) != AXL_OK) {
        axl_printf("NOTE: mvdir map not removed\n");
    }
    axl_fs_provider_unpublish(handle);
}

/* Publish the mvd volume afresh -- "d" holding "x", and the empty "s" --
   and map it. false (with nothing left published) is a legitimate SKIP. */
static bool
mvd_up(void **handle)
{
    axl_strlcpy(g_mvd_dir, "d", sizeof(g_mvd_dir));
    axl_strlcpy(g_mvd_src, "s", sizeof(g_mvd_src));
    g_mvd_dir_alive = true;
    g_mvd_src_alive = true;
    *handle = NULL;
    if (axl_fs_provider_publish(&g_mvd_provider, &g_mvd_guid, handle)
            != AXL_OK) {
        test_fail("mvdir: publish failed");
        *handle = NULL;
        return false;
    }
    AxlVolume   vols[16];
    size_t      n  = 0;
    const void *dp = NULL;
    if (axl_volume_enumerate(vols, 16, &n) == AXL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (vols[i].handle == *handle) {
                dp = vols[i].device_path;
                break;
            }
        }
    }
    if (dp == NULL || axl_volume_set_map(dp, MVD_MAP) != AXL_OK) {
        axl_fs_provider_unpublish(*handle);
        *handle = NULL;
        return false;
    }
    return true;
}

/* A WebDAV MOVE with Overwrite: T onto an existing collection sets the
   destination aside, moves, and only then removes the aside -- through
   axl_file_replace. Here that removal meets a provider whose delete of the
   file inside reports success and keeps it: the remove must not loop on it
   (it once looped forever). The provider deletes a directory with what is
   below it, so the aside goes whole, and nothing is logged. */
static void
test_serve_fs_overwrite_over_a_lying_delete(void)
{
    void *handle = NULL;

    if (!mvd_up(&handle)) {
        test_skip_n(3, "serve-fs lying delete (no shell map for the volume)");
        return;
    }
    AxlFsRoot *root = axl_fs_root_new(MVD_MAP ":\\", 0);
    if (root == NULL) {
        test_fail("serve-fs lying delete: axl_fs_root_new");
        if (axl_volume_unmap(MVD_MAP) != AXL_OK) {
            axl_printf("NOTE: mvdir map not removed\n");
        }
        axl_fs_provider_unpublish(handle);
        return;
    }

    g_mvd_lying_del = true;
    test_log_quiet_begin("fs");
    int    rc    = axl_fs_webdav_ops()->move(root, "/s", "/d", true);
    size_t hits  = test_log_quiet_armed ? (size_t)test_log_quiet_hits : 1u;
    test_log_quiet_abort();
    g_mvd_lying_del = false;

    test_check(rc == AXL_OK && g_mvd_src_alive
               && axl_strcmp(g_mvd_src, "d") == 0,
               "serve-fs lying delete: the MOVE happened -- the source is d");
    test_check(!g_mvd_dir_alive && axl_str_has_prefix(g_mvd_dir, "~AXL"),
               "serve-fs lying delete: the old d, set aside, is gone whole -- not looped on");
    test_check(hits == 0, "serve-fs lying delete: nothing to log");

    axl_fs_root_free(root);
    if (axl_volume_unmap(MVD_MAP) != AXL_OK) {
        axl_printf("NOTE: mvdir map not removed\n");
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
// Shell names: FS20 and up, removed by device path
// ---------------------------------------------------------------------------

/* A second provider kind, so the two volumes below have different device
   paths: the shell matches names to devices by device path, and cannot tell
   two publications with the same vendor GUID apart. */
static const AxlGuid g_mock_names_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4003,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcf);

/* Assertions test_shell_names makes when the modern shell is present. */
#define SHELL_NAMES_CHECKS  10

static void *
published_device_path(void *handle)
{
    void *dp = NULL;
    if (gBS->HandleProtocol((EFI_HANDLE)handle, &gEfiDevicePathProtocolGuid,
                            &dp) != EFI_SUCCESS) {
        return NULL;
    }
    return dp;
}

/* Publish names each volume FS20 or above, and unpublish removes the
   volume's names by device path. The name publish assigned can move to
   another volume -- a user's `map`, or the shell's own `map -u`, which
   hands out numbers from a counter that never checks the map -- and
   removing it by name would take it from that volume. */
static void
test_shell_names(void)
{
    if (axl_shell_kind() != AXL_SHELL_KIND_UEFI) {
        test_skip_n(SHELL_NAMES_CHECKS,
                    "fs-provider: shell names need EFI_SHELL_PROTOCOL");
        return;
    }

    void *a = NULL;
    void *b = NULL;
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_guid, &a) != AXL_OK) {
        test_fail("fs-provider: shell names setup: publish A failed");
        return;
    }
    if (axl_fs_provider_publish(&g_mock_provider, &g_mock_names_guid, &b)
            != AXL_OK) {
        test_fail("fs-provider: shell names setup: publish B failed");
        axl_fs_provider_unpublish(a);
        return;
    }
    void *dpa = published_device_path(a);
    void *dpb = published_device_path(b);

    char     name_a[16] = "";
    char     name_b[16] = "";
    uint32_t num_a      = 0;
    bool named = dpa != NULL && dpb != NULL
        && axl_volume_map_name(dpa, name_a, sizeof(name_a)) == AXL_OK
        && axl_volume_map_name(dpb, name_b, sizeof(name_b)) == AXL_OK;
    test_check(named, "fs-provider: publish gives each volume an fsN name");
    test_check(named
               && axl_str_to_u32(name_a + 2, 10, &num_a, NULL) == AXL_OK
               && num_a >= AXL_VOLUME_FSN_FLOOR,
               "fs-provider: the name is FS20 or above, clear of map -u's numbers");
    test_check(named && axl_volume_map_is(name_a, dpa)
               && !axl_volume_map_is(name_a, dpb),
               "fs-provider: map_is tells which volume a name belongs to");
    /* A buffer too small for the name is an error, never a shortened name
       ("fs2" for "fs20") that would resolve to another volume. */
    char small[4];
    test_check(named && axl_volume_map_name(dpa, small, sizeof(small)) != AXL_OK,
               "fs-provider: map_name refuses a buffer too small for the name");

    /* Give A's name to B, as `map <name> <B>` or `map -u` would, and give A
       names of its own that publish did not set -- one not ASCII, one
       longer than any fixed buffer -- which unpublish must remove whole.
       The shell lists a device's names in the order they were set, so the
       non-ASCII one comes first: a removal that cannot spell it stops
       there, before the long one. */
    char long_name[160];
    size_t li = 0;
    while (li + 1 < 151) {
        long_name[li] = (char)('A' + (int)(li % 26));
        li++;
    }
    long_name[li] = '\0';
    bool moved = named
        && axl_volume_set_map(dpb, name_a) == AXL_OK
        && axl_volume_set_map(dpa, "FSPNAMES") == AXL_OK
        && axl_volume_set_map(dpa, "FSPNÄMES") == AXL_OK
        && axl_volume_set_map(dpa, long_name) == AXL_OK;
    test_check(moved && axl_volume_map_is(name_a, dpb),
               "fs-provider: setup: A's name now belongs to B");
    /* A now has no fsN of its own, only custom names. One that merely
       starts like an fsN is not one: taking it for the volume's fsN would
       stop mkrd from giving a disk labelled that way its FS20 back. */
    char probe[16];
    test_check(moved && axl_volume_set_map(dpa, "fs1a") == AXL_OK
               && axl_volume_map_name(dpa, probe, sizeof(probe)) != AXL_OK,
               "fs-provider: map_name does not take \"fs1a\" for an fsN");

    int rc = axl_fs_provider_unpublish(a);
    test_check(rc == AXL_OK && axl_volume_map_is(name_a, dpb),
               "fs-provider: unpublish leaves a name that moved to another volume");
    test_check(moved && !axl_volume_map_taken("FSPNAMES")
               && !axl_volume_map_taken("fs1a")
               && !axl_volume_map_taken(long_name)
               && !axl_volume_map_taken("FSPNÄMES"),
               "fs-provider: unpublish removes names publish did not set");

    rc = axl_fs_provider_unpublish(b);
    test_check(named && rc == AXL_OK && !axl_volume_map_taken(name_a)
               && !axl_volume_map_taken(name_b),
               "fs-provider: unpublish removes every name the volume had");

    /* Nothing is remembered between picks. A was published first, so it got
       the lowest free name; with both volumes gone that name is handed out
       again. */
    char next[16] = "";
    test_check(named
               && axl_volume_pick_free_fsn(next, sizeof(next)) == AXL_OK
               && axl_strcasecmp(next, name_a) == 0,
               "fs-provider: pick_free_fsn hands out a freed name again");
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// A provider that lists names that are not one entry
// ---------------------------------------------------------------------------

/* What a 9P server hands up for Linux names: `..\x`, `a/b`, `c:d`, an empty
   name, and "." / ".." -- none of them ONE entry once a walker joins it onto
   a path. One ordinary entry between them. */
static const char *const g_bad_names[] = {
    "..\\x", "a/b", "ok.txt", "c:d", "", ".", "..",
};
#define BAD_MAP  "AXLBAD"

static AxlFsStatus
bad_open(void *ctx, const char *path, unsigned mode, unsigned attributes,
         AxlFsProviderFile **out, bool *out_is_dir)
{
    (void)ctx; (void)mode; (void)attributes;
    if (path == NULL || out == NULL || out_is_dir == NULL) return AXL_FS_ERR_INVALID;
    bool root = (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'));
    if (!root) return AXL_FS_ERR_NOT_FOUND;
    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) return AXL_FS_ERR_NO_MEMORY;
    f->is_root  = true;
    f->is_dir   = true;
    *out        = f;
    *out_is_dir = true;
    return AXL_FS_OK;
}

static AxlFsStatus
bad_close(AxlFsProviderFile *file)
{
    axl_free(file);
    return AXL_FS_OK;
}

static AxlFsStatus
bad_read(AxlFsProviderFile *file, void *buf, size_t *inout_size)
{
    (void)file; (void)buf; (void)inout_size;
    return AXL_FS_ERR_IS_DIR;
}

static AxlFsStatus
bad_read_dir(AxlFsProviderFile *file, AxlFsEntry *out, bool *out_end)
{
    if (file->dir_index >= sizeof(g_bad_names) / sizeof(g_bad_names[0])) {
        *out_end = true;
        return AXL_FS_OK;
    }
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    axl_strlcpy(out->name, g_bad_names[file->dir_index++], sizeof(out->name));
    out->size        = 0;
    out->mtime_unix  = 0;
    out->attributes  = 0;
    *out_end = false;
    return AXL_FS_OK;
}

static AxlFsStatus
bad_seek(AxlFsProviderFile *file, uint64_t position)
{
    if (position != 0) return AXL_FS_ERR_UNSUPPORTED;
    file->dir_index = 0;
    return AXL_FS_OK;
}

static AxlFsStatus
bad_get_info(AxlFsProviderFile *file, AxlFsEntry *out)
{
    (void)file;
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->name[0]     = '\0';
    out->size        = 0;
    out->mtime_unix  = 0;
    out->attributes  = AXL_FS_ATTR_DIRECTORY;
    return AXL_FS_OK;
}

static const AxlFsProvider g_bad_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = bad_open,
    .close         = bad_close,
    .read          = bad_read,
    .read_dir      = bad_read_dir,
    .seek          = bad_seek,
    .get_info      = bad_get_info,
    .default_label = "BadNames",
};

static const AxlGuid g_bad_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4005,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xd1);

/* Walk BAD_MAP's root once: how many entries other than "." and "..", and
   whether they are exactly ok.txt and c:d. */
static size_t
bad_walk(bool *exact)
{
    AxlDir    *d = axl_dir_open(BAD_MAP ":\\");
    AxlFsEntry e;
    size_t     seen = 0;
    bool       ok   = d != NULL;
    while (d != NULL && axl_dir_read(d, &e)) {
        if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0) {
            continue;
        }
        seen++;
        ok = ok && (axl_strcmp(e.name, "ok.txt") == 0 || axl_strcmp(e.name, "c:d") == 0);
    }
    axl_dir_close(d);
    *exact = ok;
    return seen;
}

/* The provider seam hands up no name that cannot be ONE path component:
   empty, "." / "..", or holding '\' or '/'. A ':' is no separator inside a
   joined path (only a path's FIRST colon names a volume) and is an
   ordinary Linux name character, so "c:d" is listed. The first such name
   in a directory is a warning naming the provider and the escaped name;
   the rest, and later listings of the same directory, are debug.
   axl_dir_read applies the same check for every driver: with the seam
   bypassed, a walk still sees only ok.txt and c:d. */
static void
test_seam_drops_names_that_are_not_one_entry(void)
{
    void *handle = NULL;
    if (axl_fs_provider_publish(&g_bad_provider, &g_bad_guid, &handle) != AXL_OK) {
        test_fail("badnames: publish failed");
        return;
    }
    AxlVolume   vols[16];
    size_t      n  = 0;
    const void *dp = NULL;
    if (axl_volume_enumerate(vols, 16, &n) == AXL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (vols[i].handle == handle) {
                dp = vols[i].device_path;
            }
        }
    }
    if (dp == NULL || axl_volume_set_map(dp, BAD_MAP) != AXL_OK) {
        test_skip_n(6, "badnames (no shell map for the published volume)");
        axl_fs_provider_unpublish(handle);
        return;
    }

    bool exact = false;
    test_log_quiet_begin("fs-provider");
    size_t   seen  = bad_walk(&exact);
    unsigned hits  = test_log_quiet_hits;
    bool     armed = test_log_quiet_armed;
    bool     first = test_log_quiet_first_level == AXL_LOG_WARNING
                     && axl_strcmp(test_log_quiet_first_msg,
                                   "provider 'BadNames' listed \"..\\\\x\", which is "
                                   "not one entry name, or is over 255 UTF-16 units; "
                                   "skipped (more in this directory are logged at "
                                   "debug)") == 0;
    test_log_quiet_abort();
    test_check(exact && seen == 2, "badnames: a walk sees exactly ok.txt and c:d");
    test_check(armed && hits == 1 && first,
               "badnames: one warning for the directory, naming the provider, escaped");

    test_log_quiet_begin("fs-provider");
    size_t again = bad_walk(&exact);
    bool   quiet = test_log_quiet_armed && test_log_quiet_hits == 0;
    test_log_quiet_abort();
    test_check(again == 2 && exact && quiet,
               "badnames: listing the same directory again warns no more");

    axl_fs_provider_test_unfiltered = true;
    size_t raw = bad_walk(&exact);
    axl_fs_provider_test_unfiltered = false;
    test_check(raw == 2 && exact,
               "badnames: with the seam bypassed, axl_dir_read still drops them");
    /* A driver that hands a name up without its NUL: the name ends where
       the info does, not wherever a stale byte happens to be zero. */
    axl_fs_provider_test_unterminated = true;
    size_t bare = bad_walk(&exact);
    axl_fs_provider_test_unterminated = false;
    test_check(bare == 2 && exact,
               "badnames: names handed up without a NUL read back exactly");
    test_check(axl_fs_name_is_one_component("c:d") && !axl_fs_name_is_one_component("a/b")
               && !axl_fs_name_is_one_component("..\\x") && !axl_fs_name_is_one_component("")
               && !axl_fs_name_is_one_component("..") && !axl_fs_name_is_one_component("."),
               "badnames: the shared check -- ':' yes; '/', '\\', empty, . and .. no");

    if (axl_volume_unmap(BAD_MAP) != AXL_OK) {
        axl_printf("NOTE: badnames map not removed\n");
    }
    axl_fs_provider_unpublish(handle);
}

// ---------------------------------------------------------------------------
// The tree provider: a small mutable in-memory tree with times, used to pin
// provider version 2 -- the `rename` callback, times through get_info and
// set_info, the open mode, and the path limits. Two vtables: one with
// `rename`, one without.
// ---------------------------------------------------------------------------

#define TP_NODES   16
#define TP_NAME    1024
#define TP_MAP     "AXLTP"
#define TP_T0      1767225600ULL   /* 2026-01-01 00:00:00 UTC */

typedef struct {
    bool     used;
    int      parent;
    bool     dir;
    char     name[TP_NAME];
    uint64_t size;
    uint64_t mtime;
    uint32_t attr;
} TpNode;

static TpNode     g_tp[TP_NODES];
static bool       g_tp_keep_time;
static int        g_tp_open_calls;
static size_t     g_tp_open_len;
static int        g_tp_setinfo_calls;
static AxlFsEntry g_tp_setinfo_last;
static int        g_tp_rename_calls;
static char       g_tp_rename_last[AXL_FS_PROVIDER_PATH_MAX];

static int
tp_add(int parent, const char *name, bool dir, uint64_t size)
{
    for (int i = 1; i < TP_NODES; i++) {
        if (!g_tp[i].used) {
            g_tp[i].used   = true;
            g_tp[i].parent = parent;
            g_tp[i].dir    = dir;
            axl_strlcpy(g_tp[i].name, name, sizeof(g_tp[i].name));
            g_tp[i].size   = size;
            g_tp[i].mtime  = TP_T0;
            g_tp[i].attr   = dir ? AXL_FS_ATTR_DIRECTORY : 0u;
            return i;
        }
    }
    return -1;
}

/* Root; /a (dir) holding f.txt (3 bytes); /b (dir); /c.txt (4 bytes). */
static void
tp_reset(void)
{
    axl_memset(g_tp, 0, sizeof(g_tp));
    g_tp[0].used  = true;
    g_tp[0].dir   = true;
    g_tp[0].attr  = AXL_FS_ATTR_DIRECTORY;
    int a = tp_add(0, "a", true, 0);
    tp_add(a, "f.txt", false, 3);
    tp_add(0, "b", true, 0);
    tp_add(0, "c.txt", false, 4);
    g_tp_keep_time     = true;
    g_tp_open_calls    = 0;
    g_tp_open_len      = 0;
    g_tp_setinfo_calls = 0;
    g_tp_rename_calls  = 0;
    g_tp_rename_last[0] = '\0';
    axl_memset(&g_tp_setinfo_last, 0, sizeof(g_tp_setinfo_last));
}

static int
tp_child(int parent, const char *name, size_t len)
{
    for (int i = 1; i < TP_NODES; i++) {
        if (g_tp[i].used && g_tp[i].parent == parent
            && axl_strlen(g_tp[i].name) == len
            && axl_strncmp(g_tp[i].name, name, len) == 0) {
            return i;
        }
    }
    return -1;
}

/* Node @p path names, or -1; *@p parent gets the node of its directory
   (-1 when that does not exist either), *@p last its final component. */
static int
tp_lookup(const char *path, int *parent, const char **last)
{
    int         node = 0;
    const char *p    = path;

    *parent = -1;
    *last   = "";
    while (*p == '/') {
        p++;
    }
    while (*p != '\0') {
        const char *end = p;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        *parent = node;
        *last   = p;
        node    = (node >= 0 && g_tp[node].dir) ? tp_child(node, p, (size_t)(end - p))
                                                : -2;
        if (node == -2) {
            *parent = -1;
            return -1;
        }
        p = end;
        while (*p == '/') {
            p++;
        }
        if (*p != '\0' && node < 0) {
            *parent = -1;
            return -1;
        }
    }
    return node;
}

static AxlFsStatus
tp_open(void *ctx, const char *path, unsigned mode, unsigned attributes,
        AxlFsProviderFile **out, bool *out_is_dir)
{
    (void)ctx;
    g_tp_open_calls++;
    g_tp_open_len = axl_strlen(path);
    int         parent = -1;
    const char *last   = NULL;
    int         node   = tp_lookup(path, &parent, &last);
    if (node < 0) {
        if ((mode & AXL_FS_OPEN_CREATE) == 0u || parent < 0) {
            return AXL_FS_ERR_NOT_FOUND;
        }
        node = tp_add(parent, last, (attributes & AXL_FS_ATTR_DIRECTORY) != 0u, 0);
        if (node < 0) {
            return AXL_FS_ERR_NO_SPACE;
        }
    }
    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->tp_node  = node;
    f->is_dir   = g_tp[node].dir;
    *out        = f;
    *out_is_dir = f->is_dir;
    return AXL_FS_OK;
}

static AxlFsStatus
tp_close(AxlFsProviderFile *file)
{
    axl_free(file);
    return AXL_FS_OK;
}

static AxlFsStatus
tp_read(AxlFsProviderFile *file, void *buf, size_t *inout_size)
{
    uint64_t size  = g_tp[file->tp_node].size;
    size_t   avail = (file->cursor < size) ? (size_t)(size - file->cursor) : 0;
    size_t   n     = (*inout_size < avail) ? *inout_size : avail;
    axl_memset(buf, 'x', n);
    file->cursor += n;
    *inout_size   = n;
    return AXL_FS_OK;
}

static AxlFsStatus
tp_write(AxlFsProviderFile *file, const void *buf, size_t *inout_size)
{
    (void)buf;
    file->cursor += *inout_size;
    if (file->cursor > g_tp[file->tp_node].size) {
        g_tp[file->tp_node].size = file->cursor;
    }
    return AXL_FS_OK;
}

static void
tp_fill(int node, AxlFsEntry *out)
{
    axl_memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    axl_strlcpy(out->name, g_tp[node].name, sizeof(out->name));
    out->size        = g_tp[node].dir ? 0 : g_tp[node].size;
    out->mtime_unix  = g_tp[node].mtime;
    out->attributes  = g_tp[node].attr;
}

static AxlFsStatus
tp_read_dir(AxlFsProviderFile *file, AxlFsEntry *out, bool *out_end)
{
    for (size_t i = file->dir_index; i < TP_NODES; i++) {
        if (i > 0 && g_tp[i].used && g_tp[i].parent == file->tp_node) {
            file->dir_index = i + 1;
            tp_fill((int)i, out);
            *out_end = false;
            return AXL_FS_OK;
        }
    }
    file->dir_index = TP_NODES;
    *out_end = true;
    return AXL_FS_OK;
}

static AxlFsStatus
tp_seek(AxlFsProviderFile *file, uint64_t position)
{
    if (file->is_dir) {
        file->dir_index = 0;
        return AXL_FS_OK;
    }
    file->cursor = (position == (uint64_t)-1) ? (size_t)g_tp[file->tp_node].size
                                              : (size_t)position;
    return AXL_FS_OK;
}

static AxlFsStatus
tp_get_info(AxlFsProviderFile *file, AxlFsEntry *out)
{
    tp_fill(file->tp_node, out);
    return AXL_FS_OK;
}

/* Applies size, attributes and (when g_tp_keep_time) the time; renames in
   place when the vtable has no `rename`. */
static AxlFsStatus
tp_set_info(AxlFsProviderFile *file, const AxlFsEntry *in)
{
    TpNode *n = &g_tp[file->tp_node];
    g_tp_setinfo_calls++;
    g_tp_setinfo_last = *in;
    if (axl_strcmp(in->name, n->name) != 0) {
        if (tp_child(n->parent, in->name, axl_strlen(in->name)) >= 0) {
            return AXL_FS_ERR_ACCESS_DENIED;
        }
        axl_strlcpy(n->name, in->name, sizeof(n->name));
    }
    if (!n->dir) {
        n->size = in->size;
    }
    n->attr = in->attributes;
    if (in->mtime_unix != 0 && g_tp_keep_time) {
        n->mtime = in->mtime_unix;
    }
    return AXL_FS_OK;
}

static AxlFsStatus
tp_rename(AxlFsProviderFile *file, const char *new_path)
{
    g_tp_rename_calls++;
    axl_strlcpy(g_tp_rename_last, new_path, sizeof(g_tp_rename_last));
    int         parent = -1;
    const char *last   = NULL;
    int         node   = tp_lookup(new_path, &parent, &last);
    if (node >= 0 && node != file->tp_node) {
        return AXL_FS_ERR_ACCESS_DENIED;
    }
    if (parent < 0) {
        return AXL_FS_ERR_NOT_FOUND;
    }
    if (!g_tp[parent].dir) {
        return AXL_FS_ERR_NOT_DIR;
    }
    g_tp[file->tp_node].parent = parent;
    axl_strlcpy(g_tp[file->tp_node].name, last, sizeof(g_tp[file->tp_node].name));
    return AXL_FS_OK;
}

static AxlFsStatus
tp_del(AxlFsProviderFile *file)
{
    g_tp[file->tp_node].used = false;
    return AXL_FS_OK;
}

static const AxlFsProvider g_tp_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = tp_open,
    .close         = tp_close,
    .read          = tp_read,
    .read_dir      = tp_read_dir,
    .write         = tp_write,
    .seek          = tp_seek,
    .del           = tp_del,
    .get_info      = tp_get_info,
    .set_info      = tp_set_info,
    .default_label = "Tree",
    .rename        = tp_rename,
};

static const AxlGuid g_tp_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4007,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xd1);

/* Publish @p p afresh over a reset tree and map it as TP_MAP. false (with
   nothing left published) when the volume cannot be mapped. */
static bool
tp_up(const AxlFsProvider *p, void **handle)
{
    tp_reset();
    *handle = NULL;
    if (axl_fs_provider_publish(p, &g_tp_guid, handle) != AXL_OK) {
        test_fail("tree: publish failed");
        *handle = NULL;
        return false;
    }
    void *dp = published_device_path(*handle);
    if (dp == NULL || axl_volume_set_map(dp, TP_MAP) != AXL_OK) {
        axl_fs_provider_unpublish(*handle);
        *handle = NULL;
        return false;
    }
    return true;
}

static void
tp_down(void *handle)
{
    if (axl_volume_unmap(TP_MAP) != AXL_OK) {
        axl_printf("NOTE: tree map not removed\n");
    }
    axl_fs_provider_unpublish(handle);
}

/* Open @p name below @p root through EFI. */
static EFI_FILE_PROTOCOL *
tp_efi_open(EFI_FILE_PROTOCOL *root, const CHAR16 *name, UINT64 mode)
{
    EFI_FILE_PROTOCOL *f = NULL;
    if (root == NULL
        || root->Open(root, &f, (CHAR16 *)name, mode, 0) != EFI_SUCCESS) {
        return NULL;
    }
    return f;
}

/* Scratch EFI_FILE_INFO for SetInfo calls: GetInfo of @p f, FileName
   replaced by @p name (NULL keeps it). Returns its size, 0 on failure. */
static UINT8 g_tp_info[4096];

static UINTN
tp_info_for(EFI_FILE_PROTOCOL *f, const CHAR16 *name)
{
    UINTN n = sizeof(g_tp_info);
    if (f == NULL || f->GetInfo(f, &gEfiFileInfoGuid, &n, g_tp_info) != EFI_SUCCESS) {
        return 0;
    }
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)g_tp_info;
    if (name != NULL) {
        size_t len = 0;
        while (name[len] != 0) {
            len++;
        }
        if (SIZE_OF_EFI_FILE_INFO + (len + 1) * sizeof(CHAR16) > sizeof(g_tp_info)) {
            return 0;
        }
        for (size_t i = 0; i <= len; i++) {
            fi->FileName[i] = name[i];
        }
        fi->Size = SIZE_OF_EFI_FILE_INFO + (len + 1) * sizeof(CHAR16);
    }
    return (UINTN)fi->Size;
}

static EFI_STATUS
tp_set(EFI_FILE_PROTOCOL *f, UINTN size)
{
    return f->SetInfo(f, &gEfiFileInfoGuid, size, g_tp_info);
}

/* Moves between directories go to `rename`, and so does every other name
   change; set_info is not called for a rename that changes nothing else. */
static void
test_tree_rename_callback(void)
{
    void *h = NULL;
    if (!tp_up(&g_tp_provider, &h)) {
        test_skip_n(9, "tree rename (no shell map for the published volume)");
        return;
    }

    test_check(axl_file_rename(TP_MAP ":\\c.txt", TP_MAP ":\\a\\c2.txt") == AXL_OK
               && g_tp[4].parent == 1 && axl_strcmp(g_tp[4].name, "c2.txt") == 0,
               "tree: a move to another directory -> AXL_OK, the node moved");
    test_check(g_tp_rename_calls == 1 && axl_strcmp(g_tp_rename_last, "/a/c2.txt") == 0,
               "tree: rename was handed /a/c2.txt");
    test_check(g_tp_setinfo_calls == 0,
               "tree: set_info is not called when only the name changes");

    test_check(axl_file_rename(TP_MAP ":\\a\\c2.txt", "c3.txt") == AXL_OK
               && g_tp_rename_calls == 2
               && axl_strcmp(g_tp_rename_last, "/a/c3.txt") == 0
               && g_tp_setinfo_calls == 0,
               "tree: a rename in place also goes to rename, as /a/c3.txt");

    test_check(axl_file_move(TP_MAP ":\\a\\c3.txt", TP_MAP ":\\b\\c3.txt") == AXL_OK
               && g_tp_rename_calls == 3 && g_tp[4].used && g_tp[4].parent == 3,
               "tree: axl_file_move moves the same node, not a copy");

    /* A directory with a handle open below it is refused, as FAT does. */
    AxlStream *s = axl_fopen(TP_MAP ":\\a\\f.txt", "r");
    int        rc = axl_file_rename(TP_MAP ":\\a", TP_MAP ":\\b\\a");
    test_check(s != NULL && rc == AXL_ERR && g_tp_rename_calls == 3 && g_tp[1].parent == 0,
               "tree: a directory with a file open below it -> AXL_ERR, rename not called");
    axl_fclose(s);
    test_check(axl_file_rename(TP_MAP ":\\a", TP_MAP ":\\b\\a") == AXL_OK
               && g_tp[1].parent == 3 && g_tp_rename_calls == 4,
               "tree: the same move once the file is closed -> AXL_OK");

    /* An existing destination is the provider's refusal -- axl_file_rename
       looks first and answers AXL_EXISTS without calling it. */
    test_check(axl_file_rename(TP_MAP ":\\b\\c3.txt", TP_MAP ":\\b\\a") == AXL_EXISTS
               && g_tp_rename_calls == 4,
               "tree: an existing destination -> AXL_EXISTS, rename not called");
    test_check(axl_file_rename(TP_MAP ":\\b\\c3.txt", TP_MAP ":\\nodir\\x") == AXL_ERR,
               "tree: a missing destination directory -> AXL_ERR");
    tp_down(h);
}

/* SetInfo straight through EFI: the name resolution FAT uses, and every
   refusal the thunk makes before calling the provider. */
static void
test_tree_setinfo_rules(void)
{
    void *h = NULL;
    if (!tp_up(&g_tp_provider, &h)) {
        test_skip_n(11, "tree setinfo (no shell map for the published volume)");
        return;
    }
    EFI_FILE_PROTOCOL *root = open_root_via_efi(h);
    EFI_FILE_PROTOCOL *f    = tp_efi_open(root, L"\\a\\f.txt",
                                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE);
    UINTN              n;

    n = tp_info_for(f, L"..\\b\\g.txt");
    test_check(n != 0 && tp_set(f, n) == EFI_SUCCESS
               && axl_strcmp(g_tp_rename_last, "/b/g.txt") == 0
               && g_tp[2].parent == 3,
               "tree: FileName ..\\b\\g.txt is taken from the file's parent -> /b/g.txt");
    n = tp_info_for(f, L"\\a\\f.txt");
    test_check(n != 0 && tp_set(f, n) == EFI_SUCCESS && g_tp[2].parent == 1
               && axl_strcmp(g_tp_rename_last, "/a/f.txt") == 0,
               "tree: a leading \\ is the volume root -> /a/f.txt");
    int calls = g_tp_rename_calls;
    n = tp_info_for(f, L"..\\..\\x");
    test_check(n != 0 && tp_set(f, n) == EFI_INVALID_PARAMETER
               && g_tp_rename_calls == calls,
               "tree: a FileName climbing above the root -> EFI_INVALID_PARAMETER");
    n = tp_info_for(f, L"f.txt");
    test_check(n != 0 && tp_set(f, n) == EFI_SUCCESS && g_tp_rename_calls == calls
               && g_tp_setinfo_calls == 0,
               "tree: the file's own name -> success, nothing called");

    EFI_FILE_PROTOCOL *d = tp_efi_open(root, L"\\b",
                                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE);
    n = tp_info_for(d, L"\\b\\inner");
    test_check(n != 0 && tp_set(d, n) == EFI_ACCESS_DENIED && g_tp_rename_calls == calls,
               "tree: a directory into itself -> EFI_ACCESS_DENIED");
    n = tp_info_for(d, NULL);
    ((EFI_FILE_INFO *)g_tp_info)->Attribute &= ~(UINT64)EFI_FILE_DIRECTORY;
    test_check(n != 0 && tp_set(d, n) == EFI_ACCESS_DENIED && g_tp_setinfo_calls == 0,
               "tree: clearing the DIRECTORY bit -> EFI_ACCESS_DENIED");
    n = tp_info_for(root, L"x");
    test_check(n != 0 && tp_set(root, n) == EFI_ACCESS_DENIED,
               "tree: renaming the root -> EFI_ACCESS_DENIED");

    /* A name too long for AxlFsEntry.name is refused, never cut. */
    static CHAR16 longname[801];
    for (size_t i = 0; i < 800; i++) {
        longname[i] = (CHAR16)'q';
    }
    longname[800] = 0;
    n = tp_info_for(f, longname);
    test_check(n != 0 && tp_set(f, n) == EFI_INVALID_PARAMETER
               && g_tp_rename_calls == calls && axl_strcmp(g_tp[2].name, "f.txt") == 0,
               "tree: an 800-unit FileName -> EFI_INVALID_PARAMETER, nothing renamed");

    /* A handle opened read-only may change only the attributes. */
    EFI_FILE_PROTOCOL *ro = tp_efi_open(root, L"\\c.txt", EFI_FILE_MODE_READ);
    n = tp_info_for(ro, NULL);
    ((EFI_FILE_INFO *)g_tp_info)->FileSize = 9;
    test_check(n != 0 && tp_set(ro, n) == EFI_ACCESS_DENIED && g_tp[4].size == 4,
               "tree: a read-only handle changing the size -> EFI_ACCESS_DENIED");
    n = tp_info_for(ro, L"c9.txt");
    test_check(n != 0 && tp_set(ro, n) == EFI_ACCESS_DENIED
               && axl_strcmp(g_tp[4].name, "c.txt") == 0,
               "tree: a read-only handle renaming -> EFI_ACCESS_DENIED");
    n = tp_info_for(ro, NULL);
    ((EFI_FILE_INFO *)g_tp_info)->Attribute |= EFI_FILE_HIDDEN;
    test_check(n != 0 && tp_set(ro, n) == EFI_SUCCESS
               && (g_tp[4].attr & AXL_FS_ATTR_HIDDEN) != 0u,
               "tree: a read-only handle changing an attribute -> applied");

    if (ro != NULL) ro->Close(ro);
    if (d != NULL) d->Close(d);
    if (f != NULL) f->Close(f);
    if (root != NULL) root->Close(root);
    tp_down(h);
}

/* Times: get_info's mtime reaches EFI, set_info gets a time only when one
   is asked for, and an invalid time is refused before the provider. */
static void
test_tree_times(void)
{
    void *h = NULL;
    if (!tp_up(&g_tp_provider, &h)) {
        test_skip_n(11, "tree times (no shell map for the published volume)");
        return;
    }
    AxlFsEntry e;
    test_check(axl_file_info(TP_MAP ":\\a\\f.txt", &e) == AXL_OK && e.mtime_unix == TP_T0,
               "tree: get_info's mtime reaches axl_file_info");
    test_check(axl_file_set_mtime(TP_MAP ":\\a\\f.txt", TP_T0 + 3601u) == AXL_OK
               && g_tp[2].mtime == TP_T0 + 3601u,
               "tree: axl_file_set_mtime reaches set_info, to the second");
    test_check(g_tp_setinfo_last.mtime_unix == TP_T0 + 3601u
               && g_tp_setinfo_last.size == 3
               && axl_strcmp(g_tp_setinfo_last.name, "f.txt") == 0,
               "tree: set_info got the time, the size and the current name");

    g_tp_keep_time = false;
    test_check(axl_file_set_mtime(TP_MAP ":\\a\\f.txt", TP_T0 + 7200u) == AXL_ERR
               && g_tp[2].mtime == TP_T0 + 3601u,
               "tree: a provider that keeps its own time -> AXL_ERR");
    g_tp_keep_time = true;

    EFI_FILE_PROTOCOL *root = open_root_via_efi(h);
    EFI_FILE_PROTOCOL *f    = tp_efi_open(root, L"\\c.txt",
                                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE);
    EFI_FILE_INFO     *fi   = (EFI_FILE_INFO *)g_tp_info;
    UINTN              n;

    /* The current time echoed back with a new size: the time is passed. */
    int calls = g_tp_setinfo_calls;
    n = tp_info_for(f, NULL);
    fi->FileSize = 10;
    test_check(n != 0 && tp_set(f, n) == EFI_SUCCESS && g_tp_setinfo_calls == calls + 1
               && g_tp_setinfo_last.size == 10 && g_tp_setinfo_last.mtime_unix == TP_T0,
               "tree: a resize echoing the old time passes the time with it");
    /* The current time echoed back with nothing else: no call at all. */
    n = tp_info_for(f, NULL);
    test_check(n != 0 && tp_set(f, n) == EFI_SUCCESS && g_tp_setinfo_calls == calls + 1,
               "tree: an unchanged SetInfo -> success, set_info not called");
    /* An all-zero time with an attribute change: mtime_unix 0. */
    n = tp_info_for(f, NULL);
    axl_memset(&fi->ModificationTime, 0, sizeof(fi->ModificationTime));
    fi->Attribute |= EFI_FILE_ARCHIVE;
    test_check(n != 0 && tp_set(f, n) == EFI_SUCCESS && g_tp_setinfo_calls == calls + 2
               && g_tp_setinfo_last.mtime_unix == 0 && g_tp[4].mtime == TP_T0,
               "tree: an all-zero ModificationTime is 'leave it'");

    n = tp_info_for(f, NULL);
    fi->ModificationTime.Month = 13;
    test_check(n != 0 && tp_set(f, n) == EFI_INVALID_PARAMETER
               && g_tp_setinfo_calls == calls + 2,
               "tree: month 13 -> EFI_INVALID_PARAMETER, set_info not called");
    n = tp_info_for(f, NULL);
    fi->ModificationTime.Year = 1969;
    test_check(n != 0 && tp_set(f, n) == EFI_INVALID_PARAMETER,
               "tree: a 1969 time -> EFI_INVALID_PARAMETER");
    n = tp_info_for(f, NULL);
    fi->ModificationTime = (EFI_TIME){ .Year = 1970, .Month = 1, .Day = 1,
                                       .TimeZone = 0x07FF };
    test_check(n != 0 && tp_set(f, n) == EFI_INVALID_PARAMETER,
               "tree: exactly 1970-01-01 00:00:00 -> EFI_INVALID_PARAMETER");
    n = tp_info_for(f, NULL);
    fi->ModificationTime.Hour = 1;
    fi->ModificationTime.TimeZone = 60;   /* local = UTC - 1h */
    test_check(n != 0 && tp_set(f, n) == EFI_SUCCESS
               && g_tp_setinfo_last.mtime_unix == TP_T0 + 7200u,
               "tree: a time with a zone is passed as UTC (01:00 at +60 -> 02:00)");

    if (f != NULL) f->Close(f);
    if (root != NULL) root->Close(root);
    tp_down(h);
}

/* Without `rename`: a move to another directory is EFI_UNSUPPORTED (copy
   instead) and a rename in place still reaches set_info. Published with a
   version-1-sized vtable whose `rename` bytes hold a poison pointer: the
   thunk must neither read nor call it. */
static void
test_tree_without_rename(void)
{
    static uint8_t buf[sizeof(AxlFsProvider) + 16];
    AxlFsProvider  p = g_tp_provider;
    p.version     = 1;
    p.struct_size = (uint32_t)offsetof(AxlFsProvider, rename);
    p.rename      = (AxlFsProviderRename)(uintptr_t)0x1;
    axl_memcpy(buf, &p, sizeof(p));

    void *h = NULL;
    tp_reset();
    int rc = axl_fs_provider_publish((const AxlFsProvider *)buf, &g_tp_guid, &h);
    test_check(rc == AXL_OK, "tree v1: a vtable sized to version 1 publishes");
    void *dp = (rc == AXL_OK) ? published_device_path(h) : NULL;
    if (dp == NULL || axl_volume_set_map(dp, TP_MAP) != AXL_OK) {
        test_skip_n(5, "tree v1 (no shell map for the published volume)");
        if (rc == AXL_OK) {
            axl_fs_provider_unpublish(h);
        }
        return;
    }
    test_check(axl_file_rename(TP_MAP ":\\c.txt", TP_MAP ":\\a\\c.txt") == AXL_UNSUPPORTED
               && g_tp[4].parent == 0,
               "tree v1: a move to another directory -> AXL_UNSUPPORTED, unchanged");
    test_check(axl_file_rename(TP_MAP ":\\c.txt", "d.txt") == AXL_OK
               && axl_strcmp(g_tp[4].name, "d.txt") == 0
               && axl_strcmp(g_tp_setinfo_last.name, "d.txt") == 0,
               "tree v1: a rename in place reaches set_info as the new basename");
    test_check(axl_file_move(TP_MAP ":\\d.txt", TP_MAP ":\\b\\d.txt") == AXL_OK
               && g_tp[4].used == false,
               "tree v1: axl_file_move copies instead (the source is gone)");

    /* A directory with a file open below it is not renamed in place
       either: the handle's path would keep the old name. */
    AxlStream *s = axl_fopen(TP_MAP ":\\a\\f.txt", "r");
    test_check(s != NULL && axl_file_rename(TP_MAP ":\\a", "z") == AXL_ERR
               && axl_strcmp(g_tp[1].name, "a") == 0,
               "tree v1: a directory with a file open below it -> AXL_ERR, unchanged");
    axl_fclose(s);

    /* A FileName of "\" names the root: refused before set_info, which
       would otherwise be handed an empty name. */
    EFI_FILE_PROTOCOL *root  = open_root_via_efi(h);
    EFI_FILE_PROTOCOL *f     = tp_efi_open(root, L"\\a\\f.txt",
                                           EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE);
    int                calls = g_tp_setinfo_calls;
    UINTN              n     = tp_info_for(f, L"\\");
    test_check(n != 0 && tp_set(f, n) == EFI_ACCESS_DENIED
               && g_tp_setinfo_calls == calls && axl_strcmp(g_tp[2].name, "f.txt") == 0,
               "tree v1: a FileName of \\ -> EFI_ACCESS_DENIED, set_info not called");
    if (f != NULL) f->Close(f);
    if (root != NULL) root->Close(root);
    tp_down(h);
}

/* A vtable whose struct_size ends INSIDE `rename` would hand the thunk part
   of a function pointer: refused. Only the version-1 size (ending at
   backend_ctx) or one covering all of `rename` publishes. */
static void
test_tree_partial_rename_size(void)
{
    static uint8_t buf[sizeof(AxlFsProvider) + 16];
    AxlFsProvider  p = g_tp_provider;
    p.struct_size = (uint32_t)(offsetof(AxlFsProvider, rename) + 4);
    p.rename      = (AxlFsProviderRename)(uintptr_t)0x1;
    axl_memcpy(buf, &p, sizeof(p));
    void *h = NULL;
    test_check(axl_fs_provider_publish((const AxlFsProvider *)buf, &g_tp_guid, &h)
                   == AXL_ERR,
               "tree: a struct_size ending inside rename -> publish refused");
    if (h != NULL) {
        axl_fs_provider_unpublish(h);
    }
}

/* A directory Read into a buffer too small for the next entry answers
   EFI_BUFFER_TOO_SMALL with the size it needs and KEEPS the entry, as the
   FAT driver does: the retry with a bigger buffer returns the same one. */
static void
test_tree_read_dir_too_small_keeps_the_entry(void)
{
    void *h = NULL;
    if (!tp_up(&g_tp_provider, &h)) {
        test_skip_n(2, "tree too-small read (no shell map for the published volume)");
        return;
    }
    EFI_FILE_PROTOCOL *root = open_root_via_efi(h);
    static UINT8       big[4096];
    UINTN              n     = sizeof(big);
    char               first[64] = "";
    EFI_FILE_INFO     *fi    = (EFI_FILE_INFO *)big;

    /* The first entry, read whole, then the directory rewound. */
    if (root != NULL && root->Read(root, &n, big) == EFI_SUCCESS && n > 0) {
        axl_ucs2_to_utf8_buf((const unsigned short *)fi->FileName, first, sizeof(first));
    }
    if (root != NULL) {
        root->SetPosition(root, 0);
    }

    UINTN      small = 8;
    EFI_STATUS s     = (root != NULL) ? root->Read(root, &small, big) : EFI_DEVICE_ERROR;
    test_check(first[0] != '\0' && s == EFI_BUFFER_TOO_SMALL && small > 8,
               "tree: a too-small directory Read -> EFI_BUFFER_TOO_SMALL with the size");
    char again[64] = "";
    n = sizeof(big);
    if (root != NULL && root->Read(root, &n, big) == EFI_SUCCESS && n > 0) {
        axl_ucs2_to_utf8_buf((const unsigned short *)fi->FileName, again, sizeof(again));
    }
    test_check(again[0] != '\0' && axl_strcmp(again, first) == 0,
               "tree: the retry returns the SAME entry, not the next one");
    if (root != NULL) root->Close(root);
    tp_down(h);
}

/* A provider name of 765 bytes -- 255 x U+20AC, the longest FAT creates and
   past the 256 bytes AxlFsEntry.name once held -- lists, opens and renames
   whole through the thunk. */
static void
test_tree_long_name(void)
{
    void *h = NULL;
    if (!tp_up(&g_tp_provider, &h)) {
        test_skip_n(5, "tree long name (no shell map for the published volume)");
        return;
    }
    static char name[800];
    static char path[900];
    size_t      k = 0;
    for (size_t i = 0; i < 255; i++) {
        name[k++] = (char)0xE2;
        name[k++] = (char)0x82;
        name[k++] = (char)0xAC;
    }
    name[k] = '\0';
    int node = tp_add(0, name, false, 2);
    axl_snprintf(path, sizeof(path), "%s:\\%s", TP_MAP, name);

    AxlDir    *d     = axl_dir_open(TP_MAP ":\\");
    AxlFsEntry e;
    bool       found = false;
    while (d != NULL && axl_dir_read(d, &e)) {
        found = found || axl_strcmp(e.name, name) == 0;
    }
    axl_dir_close(d);
    test_check(node > 0 && found, "tree: a 765-byte provider name is listed whole");
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 2,
               "tree: the 765-byte name opens");
    test_check(axl_file_rename(path, "short.txt") == AXL_OK
               && axl_file_rename(TP_MAP ":\\short.txt", name) == AXL_OK
               && node > 0 && axl_strcmp(g_tp[node].name, name) == 0,
               "tree: renamed away from and back to the 765-byte name");

    /* 300 ASCII characters: 300 bytes, which AxlFsEntry.name holds, but
       300 UTF-16 units, which no FAT volume does -- skipped in a listing
       (warning once) and refused in a path, never cut. */
    static char ascii[301];
    for (size_t i = 0; i < 300; i++) {
        ascii[i] = 'A';
    }
    ascii[300] = '\0';
    int anode = tp_add(0, ascii, false, 1);
    found = false;
    d = axl_dir_open(TP_MAP ":\\");
    test_log_quiet_begin("fs-provider");
    while (d != NULL && axl_dir_read(d, &e)) {
        found = found || axl_str_has_prefix(e.name, "AAAA");
    }
    unsigned hits = test_log_quiet_hits;
    test_log_quiet_abort();
    axl_dir_close(d);
    test_check(anode > 0 && !found && hits == 1,
               "tree: a 300-unit name is skipped, with one warning");
    axl_snprintf(path, sizeof(path), "%s:\\%s", TP_MAP, ascii);
    test_check(axl_file_info(path, &e) != AXL_OK,
               "tree: a 300-unit name in a path is refused");
    tp_down(h);
}

/* Paths the thunk hands a provider are whole, or refused -- never cut. */
static void
test_tree_path_limits(void)
{
    void *h = NULL;
    if (!tp_up(&g_tp_provider, &h)) {
        test_skip_n(3, "tree paths (no shell map for the published volume)");
        return;
    }
    EFI_FILE_PROTOCOL *root = open_root_via_efi(h);
    static CHAR16      path[700];
    EFI_FILE_PROTOCOL *f = NULL;

    /* Four 120-unit components of U+00E9: 964 bytes of UTF-8, 484 units --
       over the 512 bytes the thunk once cut every path to. */
    size_t k = 0;
    path[k++] = (CHAR16)'\\';
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < 120; i++) {
            path[k++] = (CHAR16)0x00E9;
        }
        path[k++] = (CHAR16)'\\';
    }
    path[--k] = 0;
    g_tp_open_calls = 0;
    EFI_STATUS s = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    test_check(s == EFI_NOT_FOUND && g_tp_open_calls == 1 && g_tp_open_len == 964u,
               "tree: a 964-byte path reaches the provider whole");

    /* One 300-unit component of U+20AC: 900 bytes, over 765. */
    k = 0;
    path[k++] = (CHAR16)'\\';
    for (int i = 0; i < 300; i++) {
        path[k++] = (CHAR16)0x20AC;
    }
    path[k] = 0;
    g_tp_open_calls = 0;
    s = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    test_check(s == EFI_INVALID_PARAMETER && g_tp_open_calls == 0,
               "tree: a 900-byte component -> EFI_INVALID_PARAMETER, open not called");

    /* 600 units: over 511 -- refused, not cut. */
    for (k = 0; k < 600; k++) {
        path[k] = (k % 50 == 0) ? (CHAR16)'\\' : (CHAR16)'p';
    }
    path[600] = 0;
    g_tp_open_calls = 0;
    s = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    test_check(s == EFI_INVALID_PARAMETER && g_tp_open_calls == 0,
               "tree: a 600-unit path -> EFI_INVALID_PARAMETER, open not called");

    if (root != NULL) root->Close(root);
    tp_down(h);
}

/* Out of memory anywhere in a thunk Open is EFI_OUT_OF_RESOURCES -- never
   EFI_INVALID_PARAMETER, which says the NAME is bad -- and leaves nothing
   behind: a clean Open of the same name works after each failure. The
   sweep runs until an Open succeeds, so it covers every allocation. */
static void
test_tree_open_oom_sweep(void)
{
    void *h = NULL;
    if (!tp_up(&g_tp_provider, &h)) {
        test_skip_n(1, "tree open OOM sweep (no shell map for the published volume)");
        return;
    }
    EFI_FILE_PROTOCOL *root = open_root_via_efi(h);
    bool   ok     = root != NULL;
    bool   opened = false;
    size_t oom    = 0;
    for (size_t n = 1; ok && !opened && n <= 256; n++) {
        EFI_FILE_PROTOCOL *f = NULL;
        axl_mem_fail_next_alloc(n);
        EFI_STATUS s = root->Open(root, &f, (CHAR16 *)L"\\a\\f.txt", EFI_FILE_MODE_READ, 0);
        axl_mem_fail_next_alloc(0);
        if (s == EFI_SUCCESS) {
            opened = true;
            f->Close(f);
            break;
        }
        oom += (s == EFI_OUT_OF_RESOURCES) ? 1u : 0u;
        EFI_FILE_PROTOCOL *g = NULL;
        EFI_STATUS s2 = root->Open(root, &g, (CHAR16 *)L"\\a\\f.txt", EFI_FILE_MODE_READ, 0);
        if (s2 == EFI_SUCCESS) {
            g->Close(g);
        }
        if (s != EFI_OUT_OF_RESOURCES || s2 != EFI_SUCCESS) {
            axl_printf("      allocation %u: open 0x%llx, next open 0x%llx\n", (unsigned)n,
                       (unsigned long long)s, (unsigned long long)s2);
            ok = false;
        }
    }
    test_check(ok && opened && oom > 0,
               "tree: OOM at any allocation of an Open -> EFI_OUT_OF_RESOURCES, the next Open works");
    if (root != NULL) root->Close(root);
    tp_down(h);
}

// ---------------------------------------------------------------------------
// A raw EFI_FILE_PROTOCOL driver: names no AxlFsProvider can hand up and FAT
// cannot create -- the only way to drive axl_dir_read's and axl_fstat's
// handling of a name too long for AxlFsEntry.name. A read-only root holding
// three files, in this order:
//   [0] 300 x U+20AC  -- 300 units, 900 bytes of UTF-8: over any capacity
//   [1] 900 x 'w'     -- 900 units: more than AxlDir's buffer holds, so the
//                        firmware Read answers EFI_BUFFER_TOO_SMALL
//   [2] "ok.txt"
// ---------------------------------------------------------------------------

#define RAW_MAP      "AXLRAW"
#define RAW_ENTRIES  3u

static CHAR16 g_raw_names[RAW_ENTRIES][901];

typedef struct {
    EFI_FILE_PROTOCOL proto;
    int               node;   /* -1 = the root */
    size_t            pos;    /* the root's next entry */
} RawFile;

static EFI_FILE_PROTOCOL *raw_new(int node);

static size_t
raw_units(const CHAR16 *s)
{
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

/* EFI_FILE_INFO for @p node into @p buf, with the probe-then-resize
   EFI_BUFFER_TOO_SMALL contract. */
static EFI_STATUS
raw_info(int node, UINTN *size, VOID *buf)
{
    const CHAR16 *name  = (node < 0) ? L"" : g_raw_names[node];
    size_t        units = raw_units(name);
    UINTN         need  = SIZE_OF_EFI_FILE_INFO + (units + 1) * sizeof(CHAR16);
    if (buf == NULL || *size < need) {
        *size = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    EFI_FILE_INFO *fi = buf;
    axl_memset(fi, 0, need);
    fi->Size      = need;
    fi->Attribute = EFI_FILE_READ_ONLY | ((node < 0) ? EFI_FILE_DIRECTORY : 0);
    for (size_t i = 0; i <= units; i++) {
        fi->FileName[i] = name[i];
    }
    *size = need;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
raw_open(EFI_FILE_PROTOCOL *this, EFI_FILE_PROTOCOL **out, CHAR16 *name,
         UINT64 mode, UINT64 attributes)
{
    (void)this;
    (void)attributes;
    if ((mode & EFI_FILE_MODE_WRITE) != 0u) {
        return EFI_WRITE_PROTECTED;
    }
    while (*name == L'\\') {
        name++;
    }
    int node = -2;
    if (*name == 0 || (name[0] == L'.' && name[1] == 0)) {
        node = -1;
    }
    for (int i = 0; i < (int)RAW_ENTRIES && node == -2; i++) {
        size_t n = raw_units(name);
        if (n == raw_units(g_raw_names[i])
            && axl_memcmp(name, g_raw_names[i], n * sizeof(CHAR16)) == 0) {
            node = i;
        }
    }
    if (node == -2) {
        return EFI_NOT_FOUND;
    }
    *out = raw_new(node);
    return (*out != NULL) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI
raw_close(EFI_FILE_PROTOCOL *this)
{
    axl_free(this);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
raw_delete(EFI_FILE_PROTOCOL *this)
{
    axl_free(this);
    return EFI_WARN_DELETE_FAILURE;
}

static EFI_STATUS EFIAPI
raw_read(EFI_FILE_PROTOCOL *this, UINTN *size, VOID *buf)
{
    RawFile *f = (RawFile *)this;
    if (f->node >= 0) {
        *size = 0;   /* every file is empty */
        return EFI_SUCCESS;
    }
    if (f->pos >= RAW_ENTRIES) {
        *size = 0;
        return EFI_SUCCESS;
    }
    EFI_STATUS s = raw_info((int)f->pos, size, buf);
    if (s == EFI_SUCCESS) {
        f->pos++;
    }
    return s;
}

static EFI_STATUS EFIAPI
raw_write(EFI_FILE_PROTOCOL *this, UINTN *size, VOID *buf)
{
    (void)this;
    (void)size;
    (void)buf;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
raw_get_pos(EFI_FILE_PROTOCOL *this, UINT64 *pos)
{
    (void)this;
    *pos = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
raw_set_pos(EFI_FILE_PROTOCOL *this, UINT64 pos)
{
    RawFile *f = (RawFile *)this;
    if (f->node < 0 && pos == 0) {
        f->pos = 0;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
raw_get_info(EFI_FILE_PROTOCOL *this, EFI_GUID *type, UINTN *size, VOID *buf)
{
    RawFile *f = (RawFile *)this;
    if (!axl_guid_equal((const AxlGuid *)type, (const AxlGuid *)&gEfiFileInfoGuid)) {
        return EFI_UNSUPPORTED;
    }
    return raw_info(f->node, size, buf);
}

static EFI_STATUS EFIAPI
raw_set_info(EFI_FILE_PROTOCOL *this, EFI_GUID *type, UINTN size, VOID *buf)
{
    (void)this;
    (void)type;
    (void)size;
    (void)buf;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
raw_flush(EFI_FILE_PROTOCOL *this)
{
    (void)this;
    return EFI_SUCCESS;
}

static EFI_FILE_PROTOCOL *
raw_new(int node)
{
    RawFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->proto.Revision    = EFI_FILE_PROTOCOL_REVISION;
    f->proto.Open        = raw_open;
    f->proto.Close       = raw_close;
    f->proto.Delete      = raw_delete;
    f->proto.Read        = raw_read;
    f->proto.Write       = raw_write;
    f->proto.GetPosition = raw_get_pos;
    f->proto.SetPosition = raw_set_pos;
    f->proto.GetInfo     = raw_get_info;
    f->proto.SetInfo     = raw_set_info;
    f->proto.Flush       = raw_flush;
    f->node              = node;
    return &f->proto;
}

static EFI_STATUS EFIAPI
raw_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *this, EFI_FILE_PROTOCOL **root)
{
    (void)this;
    *root = raw_new(-1);
    return (*root != NULL) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL g_raw_sfs = {
    .Revision   = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION,
    .OpenVolume = raw_open_volume,
};

static const AxlGuid g_raw_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4009,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xd3);

/* axl_dir_read skips a name AxlFsEntry cannot hold -- one past its buffer
   included, which the firmware answers EFI_BUFFER_TOO_SMALL -- and lists
   on past it; axl_fstat leaves such a name EMPTY, never cut. */
static void
test_raw_driver_long_names(void)
{
    for (size_t i = 0; i < 300; i++) {
        g_raw_names[0][i] = 0x20AC;
    }
    g_raw_names[0][300] = 0;
    for (size_t i = 0; i < 900; i++) {
        g_raw_names[1][i] = L'w';
    }
    g_raw_names[1][900] = 0;
    const CHAR16 ok[] = L"ok.txt";
    axl_memcpy(g_raw_names[2], ok, sizeof(ok));

    AxlDevicePath *dp = NULL;
    EFI_HANDLE     h  = NULL;
    if (axl_device_path_new_vendor(&g_raw_guid, &dp) != AXL_OK
        || gBS->InstallMultipleProtocolInterfaces(
               &h, &gEfiSimpleFileSystemProtocolGuid, &g_raw_sfs,
               &gEfiDevicePathProtocolGuid, dp, NULL) != EFI_SUCCESS) {
        test_fail("raw: install the driver");
        axl_free(dp);
        return;
    }
    if (axl_volume_set_map(dp, RAW_MAP) != AXL_OK) {
        test_skip_n(6, "raw long names (no shell map for the volume)");
    } else {
        AxlDir    *d     = axl_dir_open(RAW_MAP ":\\");
        AxlFsEntry e;
        size_t     n     = 0;
        bool       exact = true;
        test_log_quiet_begin("fs");
        while (d != NULL && axl_dir_read(d, &e)) {
            n++;
            exact = exact && axl_strcmp(e.name, "ok.txt") == 0;
        }
        unsigned hits   = test_log_quiet_hits;
        bool     warned = test_log_quiet_armed
                          && test_log_quiet_first_level == AXL_LOG_WARNING
                          && axl_strcmp(test_log_quiet_first_msg,
                                        "dir_read: skipped \"\\xE2\\x82\\xAC\\xE2\\x82\\xAC"
                                        "\\xE2\\x82\\xAC\\xE2\\x82\\xAC...\": not one entry "
                                        "name, or longer than 765 bytes") == 0;
        test_log_quiet_abort();
        axl_dir_close(d);
        test_check(d != NULL && n == 1 && exact,
                   "raw: axl_dir_read skips both long names and lists ok.txt after them");
        test_check(hits == 1 && warned,
                   "raw: one warning for the directory, the name escaped (the second at debug)");
        d = axl_dir_open(RAW_MAP ":\\");
        test_log_quiet_begin("fs");
        n = 0;
        while (d != NULL && axl_dir_read(d, &e)) {
            n++;
        }
        bool quiet = test_log_quiet_armed && test_log_quiet_hits == 0;
        test_log_quiet_abort();
        axl_dir_close(d);
        test_check(n == 1 && quiet, "raw: listing the same directory again warns no more");

        char path[1024];
        int  k = axl_snprintf(path, sizeof(path), "%s:\\", RAW_MAP);
        for (size_t i = 0; i < 300 && k > 0 && (size_t)k + 4 < sizeof(path); i++) {
            path[k++] = (char)0xE2;
            path[k++] = (char)0x82;
            path[k++] = (char)0xAC;
        }
        path[k] = '\0';
        AxlStream *s  = axl_fopen(path, "r");
        int        rc = axl_fstat(s, &e);
        test_check(s != NULL && rc == AXL_OK && e.name[0] == '\0'
                   && axl_fs_entry_is_read_only(&e),
                   "raw: axl_fstat of a 900-byte name -> AXL_OK, name EMPTY, never cut");
        axl_fclose(s);

        s  = axl_fopen(RAW_MAP ":\\ok.txt", "r");
        rc = axl_fstat(s, &e);
        test_check(s != NULL && rc == AXL_OK && axl_strcmp(e.name, "ok.txt") == 0,
                   "raw: axl_fstat of ok.txt names it");
        axl_fclose(s);
        test_check(axl_file_info(path, &e) == AXL_OK && e.name[0] == '\0',
                   "raw: axl_file_info of the long name -> AXL_OK, name empty");

        if (axl_volume_unmap(RAW_MAP) != AXL_OK) {
            axl_printf("NOTE: raw map not removed\n");
        }
    }
    gBS->UninstallMultipleProtocolInterfaces(
        h, &gEfiSimpleFileSystemProtocolGuid, &g_raw_sfs,
        &gEfiDevicePathProtocolGuid, dp, NULL);
    axl_free(dp);
}

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
    test_metadata_calls_over_a_lying_provider();
    test_rename_dir_in_place_reaches_a_view_below_it();
    test_serve_fs_overwrite_over_a_lying_delete();
    test_force_close_on_unpublish();
    test_status_codes_negative_convention();
    test_shell_names();
    test_seam_drops_names_that_are_not_one_entry();
    test_tree_rename_callback();
    test_tree_setinfo_rules();
    test_tree_times();
    test_tree_without_rename();
    test_tree_partial_rename_size();
    test_tree_read_dir_too_small_keeps_the_entry();
    test_tree_path_limits();
    test_tree_open_oom_sweep();
    test_tree_long_name();
    test_raw_driver_long_names();

    return test_print_results();
}

AXL_APP(test_fs_provider_main)
