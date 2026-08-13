/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fs-provider.c
    EFI_FILE_PROTOCOL + EFI_SIMPLE_FILE_SYSTEM_PROTOCOL thunk
    generator. Lets a consumer publish a UEFI-visible filesystem
    without writing any EFI_* identifier — see
    <axl/axl-fs-provider.h> for the public surface and
    docs/AXL-EFI-Encapsulation-Plan.md (Phase C) for the design.
**/

#include <axl/axl-device-path.h>
#include <axl/axl-driver.h>
#include <axl/axl-fs-provider.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-path.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>
#include <uefi/axl-uefi.h>

AXL_LOG_DOMAIN("fs-provider");

#define MAX_PATH_BYTES  512u

/* Ceiling for the free-"fsN"-slot scan in publish_shell_map — mirrors
   mkrd.c's MKRD_MAX_FS_SCAN (same shell-mapping problem, same bound). */
#define FS_PROVIDER_MAX_FS_SCAN  256u

// ===================================================================
// Internal types
// ===================================================================

typedef struct FileThunk    FileThunk;
typedef struct Publication  Publication;

/**
 * @brief Live publication record.
 *
 * One per axl_fs_provider_publish call. Owns the synthesized
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL vtable (embedded), the EFI handle
 * the protocols are installed on, the vendor device-path, and a
 * doubly-linked list of every still-open FileThunk so unpublish can
 * force-close them.
 */
struct Publication {
    /* Embedded vtable so SFS_FROM_PUB works via container-of. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  sfs;
    AxlGuid                          vendor_guid;

    /* Internal copy of the consumer's vtable — taken at publish time
       so the consumer can pass a stack-local AxlFsProvider whose
       `backend_ctx` they just filled, without committing to keep the
       struct alive across the publication's lifetime. */
    AxlFsProvider                    provider;

    /* EFI handle protocols are installed on (also returned to caller
       as the opaque void *). */
    EFI_HANDLE                       handle;

    /* Device-path allocated by axl_device_path_new_vendor; freed on
       unpublish. */
    AxlDevicePath                   *device_path;

    /* "fsN" shell mapping publish_shell_map assigned to device_path, or ""
       if none was (no shell / no free slot). Unpublish must remove it
       BEFORE freeing device_path below -- otherwise the shell's map table
       is left holding a dangling pointer to freed memory, and a later
       `dir`/`type fsN:` (or another axl_volume_enumerate walking every
       mapped entry's device path) dereferences it. */
    char                             shell_name[16];

    /* Open-file list (singly-anchored doubly-linked). Walked on
       unpublish to force-close + mark dead. Single-threaded
       firmware so no locking. */
    FileThunk                       *open_first;
    FileThunk                       *open_last;

    /* True after axl_fs_provider_unpublish has run. New calls into
       any FileThunk that's still floating around return DEVICE_ERROR. */
    bool                             dead;
};

/**
 * @brief One per Open call surfaced to UEFI.
 *
 * Embedded EFI_FILE_PROTOCOL vtable points back into this struct via
 * container-of. The thunk owns:
 *   - the resolved absolute UTF-8 path (so child Opens with relative
 *     names can be combined here without the provider seeing the
 *     mess)
 *   - the AxlFsProviderFile * the provider gave us at open time
 *   - the is_dir flag from the same call
 *   - a "dead" flag set when force-closed during unpublish.
 */
struct FileThunk {
    EFI_FILE_PROTOCOL    file;
    Publication         *pub;
    AxlFsProviderFile   *backing;     ///< NULL once dead/closed
    char                 path[MAX_PATH_BYTES];   ///< absolute UTF-8, '/'
    bool                 is_dir;
    bool                 dead;        ///< set on unpublish or force-close

    /* Linked-list pointers in pub->open_files. */
    FileThunk           *prev;
    FileThunk           *next;
};

// ===================================================================
// Forward decls (attr converters used by thunk_open before defn)
// ===================================================================

static uint32_t attr_efi_to_axl(uint64_t efi_attr);

// ===================================================================
// AxlFsStatus → EFI_STATUS mapping
// ===================================================================

static EFI_STATUS
status_to_efi(AxlFsStatus s)
{
    switch (s) {
        case AXL_FS_OK:                   return EFI_SUCCESS;
        case AXL_FS_ERR_NOT_FOUND:        return EFI_NOT_FOUND;
        case AXL_FS_ERR_ACCESS_DENIED:    return EFI_ACCESS_DENIED;
        case AXL_FS_ERR_WRITE_PROTECTED:  return EFI_WRITE_PROTECTED;
        case AXL_FS_ERR_NO_SPACE:         return EFI_VOLUME_FULL;
        /* Three "wrong-kind / bad-args" cases all map to
           EFI_INVALID_PARAMETER per UEFI 2.11 §13.5 — the spec has
           no separate "kind mismatch" code. The AXL enum still
           distinguishes them so the provider can express intent
           and so a future spec extension can split the mapping. */
        case AXL_FS_ERR_NOT_DIR:          /* fallthrough */
        case AXL_FS_ERR_IS_DIR:           /* fallthrough */
        case AXL_FS_ERR_INVALID:          return EFI_INVALID_PARAMETER;
        case AXL_FS_ERR_NO_MEMORY:        return EFI_OUT_OF_RESOURCES;
        case AXL_FS_ERR_IO:               return EFI_DEVICE_ERROR;
        case AXL_FS_ERR_UNSUPPORTED:      return EFI_UNSUPPORTED;
        case AXL_FS_ERR_END_OF_FILE:      return EFI_END_OF_FILE;
        case AXL_FS_ERR_VOLUME_CORRUPTED: return EFI_VOLUME_CORRUPTED;
    }
    return EFI_DEVICE_ERROR;
}

// ===================================================================
// Container-of helpers
// ===================================================================

/* Macro hygiene: parameter names must NOT shadow the field names
   used in the second argument of AXL_CONTAINER_OF, otherwise the
   preprocessor substitutes the parameter where we wanted the field.
   Hence the leading-underscore param names. */
#define FILE_FROM_EFI(_efi_file) \
    AXL_CONTAINER_OF((_efi_file), FileThunk, file)

#define PUB_FROM_SFS(_sfs_ptr) \
    AXL_CONTAINER_OF((_sfs_ptr), Publication, sfs)

// ===================================================================
// Open-file list management
// ===================================================================

static void
pub_list_init(Publication *p)
{
    p->open_first = NULL;
    p->open_last  = NULL;
}

static void
pub_list_add(Publication *p, FileThunk *f)
{
    f->prev = NULL;
    f->next = p->open_first;
    if (p->open_first != NULL) p->open_first->prev = f;
    p->open_first = f;
    if (p->open_last == NULL)  p->open_last = f;
}

static void
pub_list_remove(FileThunk *f)
{
    Publication *p = f->pub;
    /* `f->pub` is non-NULL throughout the live → close-via-thunk
       lifetime; the only path that nulls it is the orphan step in
       axl_fs_provider_unpublish, which happens AFTER the in-loop
       pub_list_remove call. clang-tidy can't prove that across
       function boundaries — guard explicitly so the null-deref
       analyzer pass clears. */
    if (p == NULL) {
        f->prev = NULL;
        f->next = NULL;
        return;
    }
    if (f->prev != NULL) f->prev->next = f->next;
    else                 p->open_first  = f->next;
    if (f->next != NULL) f->next->prev = f->prev;
    else                 p->open_last   = f->prev;
    f->prev = NULL;
    f->next = NULL;
}

// ===================================================================
// Path normalization (CHAR16 + relative → absolute UTF-8)
// ===================================================================

/**
 * @brief Convert a UCS-2 EFI filename and resolve it relative to
 *     @p base into a UTF-8 absolute path with '/' separators.
 *
 * Handles:
 *   - "" / "." → return base verbatim
 *   - leading '/' or '\\' → absolute (base ignored)
 *   - ".." segments collapse against base
 *   - mixed '\\' / '/' input (Shell uses '\\') → all become '/'
 */
static int
resolve_efi_path(
    const char    *base,           ///< absolute UTF-8 path of caller
    const CHAR16  *efi_name,
    char          *out,
    size_t         out_size
    )
{
    if (efi_name == NULL || out == NULL || out_size == 0) return AXL_ERR;

    /* Convert to UTF-8 on the stack. EFI filename limit is 1023
       CHAR16 (per spec); cap at MAX_PATH_BYTES which is plenty for
       any realistic path. */
    char name[MAX_PATH_BYTES];
    axl_ucs2_to_utf8_buf((const unsigned short *)efi_name, name, sizeof(name));

    /* Normalize backslashes to forward slashes. */
    for (char *p = name; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    /* "" / "." → caller's path verbatim. */
    if (name[0] == '\0' || (name[0] == '.' && name[1] == '\0')) {
        if (axl_strlen(base) >= out_size) return AXL_ERR;
        axl_strlcpy(out, base, out_size);
        return AXL_OK;
    }

    return axl_path_resolve(base, name, out, out_size);
}

// ===================================================================
// EFI_FILE_INFO marshalling (header + UCS-2 trailer)
// ===================================================================

/**
 * @brief Compute byte size of EFI_FILE_INFO + UCS-2 name trailer.
 *
 * UCS-2 expansion of a UTF-8 BMP-only name is 1 cell per code-point;
 * compute the cell count via axl_utf8_to_ucs2_buf with a NULL probe.
 * Multi-byte UTF-8 names produce fewer cells than bytes (one cell
 * per code-point), so we can never exceed the source byte count.
 *
 * Hard-caps the source at sizeof(AxlFsEntry.name) — 256 — so
 * a provider that hands back a non-NUL-terminated buffer can't make
 * us walk off the end. axl_utf8_to_ucs2_buf is itself NUL-driven
 * but we use a stack-local NUL-terminated copy to enforce the cap.
 */
static size_t
efi_file_info_size(const char *utf8_name)
{
    char bounded[256];
    axl_strlcpy(bounded, utf8_name, sizeof(bounded));
    unsigned short scratch[256];
    size_t cells = axl_utf8_to_ucs2_buf(bounded, scratch,
                                        sizeof(scratch) / sizeof(scratch[0]));
    return SIZE_OF_EFI_FILE_INFO + (cells + 1) * sizeof(CHAR16);
}

/* AXL_FS_ATTR_* values are intentionally bit-identical to
   EFI_FILE_* (READ_ONLY=0x01, HIDDEN=0x02, SYSTEM=0x04,
   DIRECTORY=0x10, ARCHIVE=0x20 — EFI_FILE_VALID_ATTR = 0x37 is the
   union). Translation is therefore a mask copy in both directions —
   matches the same idiom in axl-fs.c's axl_dir_read. Static_assert
   the invariant so a future divergence trips a compile error rather
   than silently shipping wrong attribute bits. */
_Static_assert(AXL_FS_ATTR_READ_ONLY == EFI_FILE_READ_ONLY,
               "AXL_FS_ATTR_READ_ONLY must equal EFI_FILE_READ_ONLY");
_Static_assert(AXL_FS_ATTR_HIDDEN    == EFI_FILE_HIDDEN,
               "AXL_FS_ATTR_HIDDEN must equal EFI_FILE_HIDDEN");
_Static_assert(AXL_FS_ATTR_SYSTEM    == EFI_FILE_SYSTEM,
               "AXL_FS_ATTR_SYSTEM must equal EFI_FILE_SYSTEM");
_Static_assert(AXL_FS_ATTR_DIRECTORY == EFI_FILE_DIRECTORY,
               "AXL_FS_ATTR_DIRECTORY must equal EFI_FILE_DIRECTORY");
_Static_assert(AXL_FS_ATTR_ARCHIVE   == EFI_FILE_ARCHIVE,
               "AXL_FS_ATTR_ARCHIVE must equal EFI_FILE_ARCHIVE");

#define AXL_EFI_ATTR_MASK  (EFI_FILE_VALID_ATTR)  /* 0x37u */

static uint64_t
attr_axl_to_efi(uint32_t axl_attr)
{
    return (uint64_t)(axl_attr & AXL_EFI_ATTR_MASK);
}

static uint32_t
attr_efi_to_axl(uint64_t efi_attr)
{
    return (uint32_t)(efi_attr & AXL_EFI_ATTR_MASK);
}

/**
 * @brief Lay out an EFI_FILE_INFO from an AxlFsEntry into
 *     @p buf. @p inout_size is the caller-provided buffer cap on
 *     entry; the actually-written size on exit. EFI_BUFFER_TOO_SMALL
 *     is returned when probe semantics apply.
 */
static EFI_STATUS
write_efi_file_info(
    const AxlFsEntry *info,
    UINTN                   *inout_size,
    void                    *buf
    )
{
    size_t needed = efi_file_info_size(info->name);
    if (buf == NULL || *inout_size < needed) {
        *inout_size = needed;
        return EFI_BUFFER_TOO_SMALL;
    }

    EFI_FILE_INFO *fi = buf;
    axl_memset(fi, 0, needed);
    fi->Size         = needed;
    fi->FileSize     = info->size;
    /* Spec PhysicalSize is "the amount of physical space allocated on
       the volume for the file." Providers that distinguish logical
       from physical (sparse, compressed, dedup'd) fill alloc_size;
       providers that don't, leave it zero and we fall back to
       FileSize so consumers always see a sane non-zero value. */
    fi->PhysicalSize = (info->alloc_size != 0) ? info->alloc_size : info->size;
    fi->Attribute    = attr_axl_to_efi(info->attributes);
    /* CreateTime / LastAccessTime / ModificationTime intentionally
       left zeroed; AxlFsEntry only carries a single mtime
       and we don't lossily duplicate it across all three. Future:
       add per-time accessors to the provider vtable per
       AXL_FS_PROVIDER_VERSION bump. */

    /* Trailer: UCS-2 name + NUL into FileName[]. */
    size_t cells_cap = (needed - SIZE_OF_EFI_FILE_INFO) / sizeof(CHAR16);
    axl_utf8_to_ucs2_buf(info->name,
                         (unsigned short *)fi->FileName,
                         cells_cap);

    *inout_size = needed;
    return EFI_SUCCESS;
}

// ===================================================================
// EFI_FILE_PROTOCOL thunk entries
// ===================================================================

static EFI_STATUS EFIAPI thunk_open(
    EFI_FILE_PROTOCOL  *this,
    EFI_FILE_PROTOCOL **new_handle,
    CHAR16             *file_name,
    UINT64              open_mode,
    UINT64              attributes);
static EFI_STATUS EFIAPI thunk_close(EFI_FILE_PROTOCOL *this);
static EFI_STATUS EFIAPI thunk_delete(EFI_FILE_PROTOCOL *this);
static EFI_STATUS EFIAPI thunk_read(
    EFI_FILE_PROTOCOL *this, UINTN *buffer_size, VOID *buffer);
static EFI_STATUS EFIAPI thunk_write(
    EFI_FILE_PROTOCOL *this, UINTN *buffer_size, VOID *buffer);
static EFI_STATUS EFIAPI thunk_get_position(
    EFI_FILE_PROTOCOL *this, UINT64 *position);
static EFI_STATUS EFIAPI thunk_set_position(
    EFI_FILE_PROTOCOL *this, UINT64 position);
static EFI_STATUS EFIAPI thunk_get_info(
    EFI_FILE_PROTOCOL *this, EFI_GUID *info_type,
    UINTN *buffer_size, VOID *buffer);
static EFI_STATUS EFIAPI thunk_set_info(
    EFI_FILE_PROTOCOL *this, EFI_GUID *info_type,
    UINTN buffer_size, VOID *buffer);
static EFI_STATUS EFIAPI thunk_flush(EFI_FILE_PROTOCOL *this);

static void
file_thunk_init_vtable(EFI_FILE_PROTOCOL *f)
{
    f->Revision    = EFI_FILE_PROTOCOL_REVISION;
    f->Open        = thunk_open;
    f->Close       = thunk_close;
    f->Delete      = thunk_delete;
    f->Read        = thunk_read;
    f->Write       = thunk_write;
    f->GetPosition = thunk_get_position;
    f->SetPosition = thunk_set_position;
    f->GetInfo     = thunk_get_info;
    f->SetInfo     = thunk_set_info;
    f->Flush       = thunk_flush;
    /* Revision 2 _EX entries left NULL — the spec says callers must
       check Revision before invoking them. */
    f->OpenEx  = NULL;
    f->ReadEx  = NULL;
    f->WriteEx = NULL;
    f->FlushEx = NULL;
}

/* Allocate and link a new FileThunk, pre-wired but with backing /
   path / is_dir filled by the caller. Returns NULL on OOM. */
static FileThunk *
file_thunk_new(Publication *pub)
{
    FileThunk *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) return NULL;
    file_thunk_init_vtable(&f->file);
    f->pub = pub;
    pub_list_add(pub, f);
    return f;
}

/* Free a thunk's resources after provider->close has been called.
   Safe regardless of dead state. */
static void
file_thunk_free(FileThunk *f)
{
    pub_list_remove(f);
    axl_free(f);
}

// -------------------------------------------------------------------
// Open
// -------------------------------------------------------------------

static EFI_STATUS EFIAPI
thunk_open(
    EFI_FILE_PROTOCOL  *this,
    EFI_FILE_PROTOCOL **new_handle,
    CHAR16             *file_name,
    UINT64              open_mode,
    UINT64              attributes
    )
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (new_handle == NULL || file_name == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    Publication *pub = self->pub;
    char resolved[MAX_PATH_BYTES];
    if (resolve_efi_path(self->path, file_name, resolved, sizeof(resolved))
            != AXL_OK) {
        return EFI_INVALID_PARAMETER;
    }

    /* Translate EFI_FILE_MODE_* → AXL_FS_OPEN_* and EFI_FILE_*
       attributes → AXL_FS_ATTR_*. attr_efi_to_axl is defined a few
       sections below; we rely on the forward declaration at the top
       of this file. */
    unsigned mode = 0;
    if (open_mode & EFI_FILE_MODE_READ)   mode |= AXL_FS_OPEN_READ;
    if (open_mode & EFI_FILE_MODE_WRITE)  mode |= AXL_FS_OPEN_WRITE;
    if (open_mode & EFI_FILE_MODE_CREATE) mode |= AXL_FS_OPEN_CREATE;
    unsigned attr = attr_efi_to_axl(attributes);

    AxlFsProviderFile *backing = NULL;
    bool is_dir = false;
    AxlFsStatus s = pub->provider.open(pub->provider.backend_ctx,
                                        resolved, mode, attr,
                                        &backing, &is_dir);
    if (s != AXL_FS_OK) return status_to_efi(s);

    FileThunk *child = file_thunk_new(pub);
    if (child == NULL) {
        pub->provider.close(backing);
        return EFI_OUT_OF_RESOURCES;
    }
    child->backing = backing;
    child->is_dir  = is_dir;
    axl_strlcpy(child->path, resolved, sizeof(child->path));
    *new_handle = &child->file;
    return EFI_SUCCESS;
}

// -------------------------------------------------------------------
// Close
// -------------------------------------------------------------------

static EFI_STATUS EFIAPI
thunk_close(EFI_FILE_PROTOCOL *this)
{
    FileThunk *self = FILE_FROM_EFI(this);
    /* Per UEFI 2.11 §13.5.4, Close always returns EFI_SUCCESS.
       The pub-NULL guard mirrors the orphan-on-unpublish contract:
       once unpublish has run, self->dead is true AND self->pub is
       NULL; either flag short-circuits the provider call. */
    if (!self->dead && self->pub != NULL && self->backing != NULL) {
        self->pub->provider.close(self->backing);
    }
    self->backing = NULL;
    file_thunk_free(self);
    return EFI_SUCCESS;
}

// -------------------------------------------------------------------
// Delete
// -------------------------------------------------------------------

static EFI_STATUS EFIAPI
thunk_delete(EFI_FILE_PROTOCOL *this)
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) {
        thunk_close(this);
        return EFI_WARN_DELETE_FAILURE;
    }

    /* Per UEFI 2.11 §13.5.5: Delete closes the file regardless of
       outcome. The thunk owns the close call after delete; provider
       just deletes the backing object. */
    EFI_STATUS rc;
    if (self->pub->provider.del == NULL) {
        rc = EFI_WARN_DELETE_FAILURE;
    } else {
        AxlFsStatus s = self->pub->provider.del(self->backing);
        rc = (s == AXL_FS_OK) ? EFI_SUCCESS : EFI_WARN_DELETE_FAILURE;
    }

    self->pub->provider.close(self->backing);
    self->backing = NULL;
    file_thunk_free(self);
    return rc;
}

// -------------------------------------------------------------------
// Read / ReadDir
// -------------------------------------------------------------------

static EFI_STATUS EFIAPI
thunk_read(EFI_FILE_PROTOCOL *this, UINTN *buffer_size, VOID *buffer)
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (buffer_size == NULL) return EFI_INVALID_PARAMETER;

    if (self->is_dir) {
        AxlFsEntry info = {
            .struct_size = sizeof(info),
            .version     = AXL_FS_ENTRY_VERSION,
        };
        bool end = false;
        AxlFsStatus s = self->pub->provider.read_dir(self->backing,
                                                      &info, &end);
        if (s != AXL_FS_OK) return status_to_efi(s);
        if (end) {
            /* End-of-directory: spec says return success with
               *BufferSize = 0. */
            *buffer_size = 0;
            return EFI_SUCCESS;
        }
        return write_efi_file_info(&info, buffer_size, buffer);
    }

    /* Regular file. */
    if (buffer == NULL && *buffer_size > 0) return EFI_INVALID_PARAMETER;
    size_t want = *buffer_size;
    AxlFsStatus s = self->pub->provider.read(self->backing, buffer, &want);
    if (s != AXL_FS_OK) {
        /* UEFI 2.11 §13.5.6: regular-file Read signals EOF as
           EFI_SUCCESS with BufferSize=0. Naive providers may
           return AXL_FS_ERR_END_OF_FILE instead — translate so
           Shell `type` and LoadImage don't mishandle. */
        if (s == AXL_FS_ERR_END_OF_FILE) {
            *buffer_size = 0;
            return EFI_SUCCESS;
        }
        *buffer_size = 0;
        return status_to_efi(s);
    }
    *buffer_size = want;
    return EFI_SUCCESS;
}

// -------------------------------------------------------------------
// Write
// -------------------------------------------------------------------

static EFI_STATUS EFIAPI
thunk_write(EFI_FILE_PROTOCOL *this, UINTN *buffer_size, VOID *buffer)
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (buffer_size == NULL || buffer == NULL) return EFI_INVALID_PARAMETER;
    if (self->is_dir) return EFI_UNSUPPORTED;
    if (self->pub->provider.write == NULL) return EFI_WRITE_PROTECTED;

    size_t want = *buffer_size;
    AxlFsStatus s = self->pub->provider.write(self->backing, buffer, &want);
    if (s != AXL_FS_OK) {
        *buffer_size = 0;
        return status_to_efi(s);
    }
    *buffer_size = want;
    return EFI_SUCCESS;
}

// -------------------------------------------------------------------
// GetPosition / SetPosition
// -------------------------------------------------------------------

/* GetPosition isn't in the AxlFsProvider vtable — UEFI's spec says
   directory handles return EFI_UNSUPPORTED, and for files we'd need
   either a dedicated callback or a side-cursor inside the thunk.
   Side-cursor is the wrong call: the provider is the source of truth
   for "where am I in the byte stream" (it might do compression /
   on-the-fly transcode). For v1 we expose the position via GetInfo's
   FileSize hint and otherwise return EFI_UNSUPPORTED on
   GetPosition. Most consumers never call it. */
static EFI_STATUS EFIAPI
thunk_get_position(EFI_FILE_PROTOCOL *this, UINT64 *position)
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (position == NULL) return EFI_INVALID_PARAMETER;
    if (self->is_dir) return EFI_UNSUPPORTED;
    /* Future: add an optional get_position callback. v1 callers that
       need this can track it themselves; almost none do. */
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
thunk_set_position(EFI_FILE_PROTOCOL *this, UINT64 position)
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (self->is_dir) {
        /* Only Position == 0 is meaningful per UEFI 2.11 §13.5.10. */
        if (position != 0) return EFI_UNSUPPORTED;
    }
    AxlFsStatus s = self->pub->provider.seek(self->backing, position);
    return status_to_efi(s);
}

// -------------------------------------------------------------------
// GetInfo / SetInfo
// -------------------------------------------------------------------

static EFI_STATUS
get_volume_info(
    Publication *pub,
    UINTN       *buffer_size,
    void        *buffer
    )
{
    AxlFsProviderVolumeInfo vi = {
        .struct_size = sizeof(vi),
        .version     = AXL_FS_PROVIDER_VERSION,
        .read_only   = false,
        .volume_size = (uint64_t)-1,
        .free_space  = (uint64_t)-1,
        .block_size  = 512,
    };
    if (pub->provider.volume_info != NULL) {
        AxlFsStatus s = pub->provider.volume_info(pub->provider.backend_ctx,
                                                   &vi);
        if (s != AXL_FS_OK) return status_to_efi(s);
    } else {
        const char *lbl = pub->provider.default_label;
        if (lbl == NULL) lbl = "";
        axl_strlcpy(vi.label, lbl, sizeof(vi.label));
    }

    /* Layout EFI_FILE_SYSTEM_INFO + UCS-2 label trailer. */
    unsigned short label_ucs2[64];
    size_t cells = axl_utf8_to_ucs2_buf(vi.label, label_ucs2,
                                        sizeof(label_ucs2) / sizeof(label_ucs2[0]));
    size_t needed = SIZE_OF_EFI_FILE_SYSTEM_INFO + (cells + 1) * sizeof(CHAR16);
    if (buffer == NULL || *buffer_size < needed) {
        *buffer_size = needed;
        return EFI_BUFFER_TOO_SMALL;
    }

    EFI_FILE_SYSTEM_INFO *fsi = buffer;
    axl_memset(fsi, 0, needed);
    fsi->Size       = needed;
    fsi->ReadOnly   = vi.read_only;
    fsi->VolumeSize = vi.volume_size;
    fsi->FreeSpace  = vi.free_space;
    fsi->BlockSize  = vi.block_size;
    for (size_t i = 0; i < cells; i++) {
        fsi->VolumeLabel[i] = label_ucs2[i];
    }
    fsi->VolumeLabel[cells] = 0;
    *buffer_size = needed;
    return EFI_SUCCESS;
}

static EFI_STATUS
get_volume_label(
    Publication *pub,
    UINTN       *buffer_size,
    void        *buffer
    )
{
    AxlFsProviderVolumeInfo vi = {
        .struct_size = sizeof(vi),
        .version     = AXL_FS_PROVIDER_VERSION,
    };
    if (pub->provider.volume_info != NULL) {
        AxlFsStatus s = pub->provider.volume_info(pub->provider.backend_ctx,
                                                   &vi);
        if (s != AXL_FS_OK) return status_to_efi(s);
    } else {
        const char *lbl = pub->provider.default_label;
        if (lbl == NULL) lbl = "";
        axl_strlcpy(vi.label, lbl, sizeof(vi.label));
    }

    unsigned short label_ucs2[64];
    size_t cells = axl_utf8_to_ucs2_buf(vi.label, label_ucs2,
                                        sizeof(label_ucs2) / sizeof(label_ucs2[0]));
    size_t needed = sizeof(EFI_FILE_SYSTEM_VOLUME_LABEL)
                    + cells * sizeof(CHAR16);
    if (buffer == NULL || *buffer_size < needed) {
        *buffer_size = needed;
        return EFI_BUFFER_TOO_SMALL;
    }

    EFI_FILE_SYSTEM_VOLUME_LABEL *lbl = buffer;
    for (size_t i = 0; i < cells; i++) lbl->VolumeLabel[i] = label_ucs2[i];
    lbl->VolumeLabel[cells] = 0;
    *buffer_size = needed;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
thunk_get_info(
    EFI_FILE_PROTOCOL *this,
    EFI_GUID          *info_type,
    UINTN             *buffer_size,
    VOID              *buffer
    )
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (info_type == NULL || buffer_size == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (axl_guid_equal((const AxlGuid *)info_type,
                     (const AxlGuid *)&gEfiFileInfoGuid)) {
        AxlFsEntry info = {
            .struct_size = sizeof(info),
            .version     = AXL_FS_ENTRY_VERSION,
        };
        AxlFsStatus s = self->pub->provider.get_info(self->backing, &info);
        if (s != AXL_FS_OK) return status_to_efi(s);
        return write_efi_file_info(&info, buffer_size, buffer);
    }
    if (axl_guid_equal((const AxlGuid *)info_type,
                     (const AxlGuid *)&gEfiFileSystemInfoGuid)) {
        return get_volume_info(self->pub, buffer_size, buffer);
    }
    if (axl_guid_equal((const AxlGuid *)info_type,
                     (const AxlGuid *)&gEfiFileSystemVolumeLabelInfoIdGuid)) {
        return get_volume_label(self->pub, buffer_size, buffer);
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
thunk_set_info(
    EFI_FILE_PROTOCOL *this,
    EFI_GUID          *info_type,
    UINTN              buffer_size,
    VOID              *buffer
    )
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (info_type == NULL || buffer == NULL) return EFI_INVALID_PARAMETER;
    if (!axl_guid_equal((const AxlGuid *)info_type,
                      (const AxlGuid *)&gEfiFileInfoGuid)) {
        return EFI_UNSUPPORTED;
    }
    if (self->pub->provider.set_info == NULL) return EFI_WRITE_PROTECTED;
    if (buffer_size < SIZE_OF_EFI_FILE_INFO) return EFI_INVALID_PARAMETER;

    EFI_FILE_INFO *fi = buffer;
    AxlFsEntry info = {
        .struct_size = sizeof(info),
        .version     = AXL_FS_ENTRY_VERSION,
        .size        = fi->FileSize,
        .mtime_unix  = 0,
        .attributes  = attr_efi_to_axl(fi->Attribute),
    };
    /* Trailing UCS-2 name → UTF-8 in info.name. */
    axl_ucs2_to_utf8_buf((const unsigned short *)fi->FileName,
                         info.name, sizeof(info.name));

    AxlFsStatus s = self->pub->provider.set_info(self->backing, &info);
    return status_to_efi(s);
}

// -------------------------------------------------------------------
// Flush
// -------------------------------------------------------------------

static EFI_STATUS EFIAPI
thunk_flush(EFI_FILE_PROTOCOL *this)
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (self->pub->provider.flush == NULL) return EFI_SUCCESS;
    AxlFsStatus s = self->pub->provider.flush(self->backing);
    return status_to_efi(s);
}

// ===================================================================
// EFI_SIMPLE_FILE_SYSTEM_PROTOCOL: OpenVolume
// ===================================================================

static EFI_STATUS EFIAPI
thunk_open_volume(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *this,
    EFI_FILE_PROTOCOL               **root
    )
{
    Publication *pub = PUB_FROM_SFS(this);
    if (pub->dead) return EFI_DEVICE_ERROR;
    if (root == NULL) return EFI_INVALID_PARAMETER;

    AxlFsProviderFile *backing = NULL;
    bool is_dir = false;
    AxlFsStatus s = pub->provider.open(pub->provider.backend_ctx,
                                        "/", AXL_FS_OPEN_READ, 0u,
                                        &backing, &is_dir);
    if (s != AXL_FS_OK) return status_to_efi(s);

    FileThunk *f = file_thunk_new(pub);
    if (f == NULL) {
        pub->provider.close(backing);
        return EFI_OUT_OF_RESOURCES;
    }
    f->backing = backing;
    f->is_dir  = is_dir;
    axl_strlcpy(f->path, "/", sizeof(f->path));
    *root = &f->file;
    return EFI_SUCCESS;
}

// ===================================================================
// Publish / Unpublish
// ===================================================================

static int
provider_validate(const AxlFsProvider *p)
{
    if (p == NULL) return AXL_ERR;
    if (p->struct_size < sizeof(AxlFsProvider)) return AXL_ERR;
    if (p->version == 0 || p->version > AXL_FS_PROVIDER_VERSION) return AXL_ERR;
    if (p->open == NULL)     return AXL_ERR;
    if (p->close == NULL)    return AXL_ERR;
    if (p->read == NULL)     return AXL_ERR;
    if (p->read_dir == NULL) return AXL_ERR;
    if (p->seek == NULL)     return AXL_ERR;
    if (p->get_info == NULL) return AXL_ERR;
    return AXL_OK;
}

/**
 * @brief Assign a real "fsN:" shell mapping to a freshly-published volume.
 *
 * `axl_driver_connect_handle` (ConnectController) has no shell-map side
 * effect for a handle that already carries its own
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL (there is no driver left to bind — we
 * ARE the driver). The other lever, a `map -r` refresh, only works when
 * typed at the shell's own interactive prompt: driving it programmatically
 * via `EFI_SHELL_PROTOCOL.Execute("map -r")` spawns a NESTED shell
 * instance that rescans and updates only ITS OWN throwaway map table, then
 * discards it on exit — never the calling image's persistent one (see
 * axl_volume_set_map's docstring). `SetMap` is the one shell-map primitive
 * that writes the shell's persistent global map directly, so it is the
 * only lever that fulfills axl_fs_provider_publish's own documented
 * promise: a caller that publishes and immediately `dir`/`cd`/reads back
 * gets a working name with no extra step. Best-effort: a driver/headless
 * context with no shell (or no free slot) just leaves the volume unmapped,
 * same as any other shell-only feature — callers still reach it via the
 * returned handle / axl_protocol_enumerate.
 *
 * On success, writes the assigned name to @p pub->shell_name so unpublish
 * can remove it later; left as "" (the calloc'd default) otherwise.
 */
static void
publish_shell_map(Publication *pub)
{
    for (uint32_t i = 0; i < FS_PROVIDER_MAX_FS_SCAN; i++) {
        char name[16];
        axl_snprintf(name, sizeof(name), "fs%u", i);
        if (!axl_volume_map_taken(name)) {
            int rc = axl_volume_set_map(pub->device_path, name);
            if (rc == AXL_OK) {
                axl_strlcpy(pub->shell_name, name, sizeof(pub->shell_name));
            } else if (rc == AXL_UNSUPPORTED) {
                /* Expected in a headless/driver context (no EFI_SHELL_PROTOCOL
                   locatable) -- not a fault, so no WARN. Best-effort, matching
                   axl_driver_connect_handle above: the volume is still
                   reachable via the returned handle / axl_protocol_enumerate,
                   just not by an "fsN:" path. */
                axl_debug("no shell present; volume left unmapped");
            } else {
                /* A shell IS present but rejected SetMap for this name --
                   worth a WARN, unlike the plain no-shell case above. */
                axl_warning("shell map assignment failed (SetMap rejected)");
            }
            return;
        }
    }
}

int
axl_fs_provider_publish(
    const AxlFsProvider *provider,
    const AxlGuid       *vendor_guid,
    void               **out_handle
    )
{
    if (provider_validate(provider) != AXL_OK) return AXL_ERR;
    if (vendor_guid == NULL || out_handle == NULL) return AXL_ERR;

    Publication *pub = axl_calloc(1, sizeof(*pub));
    if (pub == NULL) return AXL_ERR;
    pub->provider = *provider;
    pub->vendor_guid = *vendor_guid;
    pub_list_init(pub);

    pub->sfs.Revision   = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    pub->sfs.OpenVolume = thunk_open_volume;

    if (axl_device_path_new_vendor(vendor_guid, &pub->device_path)
            != AXL_OK) {
        axl_free(pub);
        return AXL_ERR;
    }

    pub->handle = NULL;
    if (axl_protocol_register_multiple((void **)&pub->handle,
            "simple-fs",   &pub->sfs,
            "device-path", pub->device_path,
            NULL) != AXL_OK) {
        axl_free(pub->device_path);
        axl_free(pub);
        return AXL_ERR;
    }

    /* Best-effort: let any applicable driver bind (harmless no-op here since
       the protocols are already fully installed — see publish_shell_map's
       doc comment for why this alone does NOT make the volume reachable via
       an "fsN:" path), then assign the real shell mapping that does. */
    axl_driver_connect_handle(pub->handle);
    publish_shell_map(pub);

    *out_handle = pub->handle;
    return AXL_OK;
}

int
axl_fs_provider_unpublish(void *handle)
{
    if (handle == NULL) return AXL_OK;

    /* The opaque handle is the EFI_HANDLE we registered against; we
       discover the matching Publication by reading back our own
       interface pointer (the registry returns it on locate). */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_STATUS s = gBS->HandleProtocol(
        (EFI_HANDLE)handle,
        &gEfiSimpleFileSystemProtocolGuid,
        (void **)&sfs);
    if (s != EFI_SUCCESS || sfs == NULL) return AXL_ERR;

    Publication *pub = PUB_FROM_SFS(sfs);
    if (pub->dead) return AXL_ERR;

    /* Force-close every still-open file, then orphan it. After this
       loop each thunk has dead=true and pub=NULL, so the
       (self->dead || self->pub == NULL) guard at the top of every
       thunk_* path returns EFI_DEVICE_ERROR cleanly without
       dereferencing the soon-to-be-freed Publication. The thunk
       memory itself is NOT freed here — UEFI consumers may hold stale
       EFI_FILE_PROTOCOL pointers indefinitely (rare in practice, but
       the spec doesn't bound it), so the reclaim point is the
       consumer's eventual Close, which thunk_close performs whether
       the thunk is live or orphaned. A consumer that never Closes
       keeps the thunk; that is its choice, not a punt here. */
    while (pub->open_first != NULL) {
        FileThunk *f = pub->open_first;
        if (f->backing != NULL) {
            pub->provider.close(f->backing);
            f->backing = NULL;
        }
        f->dead = true;
        pub_list_remove(f);
        f->pub = NULL;       /* break the dangling-pointer trap */
    }

    axl_protocol_unregister("simple-fs", &pub->sfs, pub->handle);
    axl_protocol_unregister("device-path", pub->device_path, pub->handle);

    /* Drop the shell mapping BEFORE freeing device_path below -- otherwise
       the shell's map table is left holding a pointer into freed memory,
       and a later `dir`/`type fsN:` (or another axl_volume_enumerate
       walking every mapped entry's device path) dereferences it. */
    if (pub->shell_name[0] != '\0' && axl_volume_unmap(pub->shell_name) != AXL_OK) {
        /* Best-effort: the shell may already be gone (e.g. exiting alongside
           this unpublish), in which case there is no map left to leak. */
        axl_warning("shell map '%s' removal failed", pub->shell_name);
    }

    pub->dead = true;
    axl_free(pub->device_path);
    axl_free(pub);
    return AXL_OK;
}
