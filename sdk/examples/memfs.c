/**
 * memfs.c — publish a UEFI-visible filesystem from in-memory data,
 *           using only `<axl/axl-fs-provider.h>`. No EFI_* identifier
 *           appears in this file.
 *
 * Build with: axl-cc memfs.c -o memfs.efi
 *
 * Usage in UEFI Shell:
 *   memfs.efi
 *   map -r          # Shell discovers the new fsN: mapping
 *   dir fsN:        # see the three demo files
 *   type fsN:\\hello.txt
 *
 * The example publishes a 3-file read-only root, prints the GUID it
 * registered against, and exits without unpublishing — leaving the
 * filesystem mounted for the rest of the Shell session. Run a second
 * time to publish a second instance.
 *
 * The interesting reading order is:
 *   1. The static MockEntry table — what the FS contains.
 *   2. The seven callback bodies (open / close / read / read_dir /
 *      seek / get_info; the read-only side has no write / del /
 *      set_info / flush).
 *   3. main() — five lines of plumbing: provider struct, GUID,
 *      publish, print, return.
 */

#include <axl.h>

// ---------------------------------------------------------------------------
// 1. The data
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;       /* basename, UTF-8 */
    const char *content;    /* NUL-terminated; size = strlen */
} MemFile;

static const MemFile g_files[] = {
    { "hello.txt",  "Hello from memfs.efi!\n" },
    { "readme.md",  "# memfs\n\nA demo of <axl/axl-fs-provider.h>.\n" },
    { "résumé.txt", "Non-ASCII filename test (UCS-2/UTF-8 boundary).\n" }, // ascii-allow: UCS-2/UTF-8 filename fixture
    { NULL, NULL }
};

// ---------------------------------------------------------------------------
// 2. Provider state — one per open file handle
// ---------------------------------------------------------------------------

struct AxlFsProviderFile {
    bool            is_root;       /* true for OpenVolume's returned root */
    bool            is_dir;
    char            path[256];     /* SDK passes path with stack lifetime; copy! */
    const MemFile  *entry;         /* NULL for root */
    size_t          cursor;        /* file: byte position; dir: entry index */
};

static const MemFile *
mem_find_by_basename(const char *path)
{
    /* axl_path_get_basename is the SDK-canonical "last segment of a
       '/'-separated path" helper; reach for it instead of rolling a
       per-provider basename loop. Returns an axl_malloc'd copy. */
    char *base = axl_path_get_basename(path);
    if (base == NULL) return NULL;
    const MemFile *match = NULL;
    for (const MemFile *e = g_files; e->name != NULL; e++) {
        if (axl_strcmp(e->name, base) == 0) { match = e; break; }
    }
    axl_free(base);
    return match;
}

// ---------------------------------------------------------------------------
// 3. The vtable callbacks
// ---------------------------------------------------------------------------

static AxlFsStatus
mem_open(
    void               *ctx,
    const char         *path,
    unsigned            mode,
    unsigned            attributes,
    AxlFsProviderFile **out,
    bool               *out_is_dir
    )
{
    (void)ctx;
    (void)attributes;
    /* Read-only filesystem — no CREATE / WRITE allowed. The thunk
       maps WRITE_PROTECTED to EFI_WRITE_PROTECTED for us. */
    if (mode & (AXL_FS_OPEN_WRITE | AXL_FS_OPEN_CREATE)) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }

    bool root = (path[0] == '/' && path[1] == '\0');
    const MemFile *entry = NULL;
    if (!root) {
        entry = mem_find_by_basename(path);
        if (entry == NULL) return AXL_FS_ERR_NOT_FOUND;
    }

    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) return AXL_FS_ERR_NO_MEMORY;
    f->is_root = root;
    f->is_dir  = root;
    f->entry   = entry;
    axl_strlcpy(f->path, path, sizeof(f->path));

    *out = f;
    *out_is_dir = f->is_dir;
    return AXL_FS_OK;
}

static AxlFsStatus
mem_close(AxlFsProviderFile *f)
{
    axl_free(f);
    return AXL_FS_OK;
}

static AxlFsStatus
mem_read(AxlFsProviderFile *f, void *buf, size_t *inout_size)
{
    size_t total = axl_strlen(f->entry->content);
    if (f->cursor >= total) {
        *inout_size = 0;
        return AXL_FS_OK;
    }
    size_t avail = total - f->cursor;
    size_t n = (*inout_size < avail) ? *inout_size : avail;
    axl_memcpy(buf, f->entry->content + f->cursor, n);
    f->cursor += n;
    *inout_size = n;
    return AXL_FS_OK;
}

static AxlFsStatus
mem_read_dir(AxlFsProviderFile *f, AxlFsEntry *out, bool *out_end)
{
    if (g_files[f->cursor].name == NULL) {
        *out_end = true;
        return AXL_FS_OK;
    }
    const MemFile *e = &g_files[f->cursor];
    f->cursor++;

    /* Zero first: AxlFsEntry is versioned and grows, so a provider that fills
       only the fields it knows about leaves the rest defined rather than
       whatever was on the caller's stack. */
    axl_memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    axl_strlcpy(out->name, e->name, sizeof(out->name));
    out->size       = axl_strlen(e->content);
    out->mtime_unix = 0;
    out->attributes = 0;
    *out_end = false;
    return AXL_FS_OK;
}

static AxlFsStatus
mem_seek(AxlFsProviderFile *f, uint64_t pos)
{
    if (f->is_dir) {
        if (pos != 0) return AXL_FS_ERR_UNSUPPORTED;
        f->cursor = 0;
        return AXL_FS_OK;
    }
    size_t total = axl_strlen(f->entry->content);
    f->cursor = (pos == (uint64_t)-1) ? total : (size_t)pos;
    return AXL_FS_OK;
}

static AxlFsStatus
mem_get_info(AxlFsProviderFile *f, AxlFsEntry *out)
{
    axl_memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->mtime_unix  = 0;
    if (f->is_root) {
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
    } else {
        axl_strlcpy(out->name, f->entry->name, sizeof(out->name));
        out->size       = axl_strlen(f->entry->content);
        out->attributes = AXL_FS_ATTR_READ_ONLY;
    }
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// 4. The vtable + GUID + main
// ---------------------------------------------------------------------------

static const AxlFsProvider g_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = mem_open,
    .close         = mem_close,
    .read          = mem_read,
    .read_dir      = mem_read_dir,
    .seek          = mem_seek,
    .get_info      = mem_get_info,
    .default_label = "MemFs",
    /* write/del/set_info/flush left NULL — read-only volume. */
};

/* Random vendor GUID for the example. Use your own when adapting. */
static const AxlGuid g_memfs_guid = AXL_GUID(
    0x57bf63a0, 0x91a2, 0x4d3e,
    0xb1, 0x2c, 0x0e, 0x47, 0x69, 0x82, 0xfb, 0x10);

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    void *handle = NULL;
    if (axl_fs_provider_publish(&g_provider, &g_memfs_guid, &handle)
            != AXL_OK) {
        axl_printf("memfs: publish failed\n");
        return 1;
    }

    axl_printf("memfs: published. Run `dir fsN:` "
               "(N depends on what's already mounted) -- publish assigns "
               "the fsN: mapping automatically, no `map -r` needed.\n");
    return 0;
}
