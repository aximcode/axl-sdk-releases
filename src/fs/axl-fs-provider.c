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
#include "axl-fs-internal.h"

AXL_LOG_DOMAIN("fs-provider");

bool axl_fs_provider_test_unfiltered;
bool axl_fs_provider_test_unterminated;

/* Longest path the thunk hands a provider, in UTF-8 bytes with the NUL. */
#define MAX_PATH_BYTES  AXL_FS_PROVIDER_PATH_MAX
/* The same path in UCS-2 units with the NUL: each unit is at most 3 bytes. */
#define MAX_PATH_UNITS  (AXL_FS_PROVIDER_PATH_MAX / 3u)
/* Longest name, in UTF-8 bytes, that AxlFsEntry.name holds whole. */
#define ENTRY_NAME_BYTES  (sizeof(((AxlFsEntry *)0)->name) - 1u)
/* Longest name in UTF-16 units: FAT's, which is what UEFI readers size
   their buffers for. A provider name must fit both limits. */
#define NAME_UNITS_MAX  255u
/* Latest Unix second an EFI_TIME can hold: 9999-12-31 23:59:59 UTC. */
#define EFI_TIME_UNIX_MAX  253402300799ULL
/* EFI_TIME.Daylight: EFI_TIME_ADJUST_DAYLIGHT (0x01) and EFI_TIME_IN_DAYLIGHT
   (0x02), UEFI 2.11 section 8.3; no other bit is defined. */
#define EFI_TIME_DAYLIGHT_BITS  0x03u

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
    bool                 writable;    ///< opened with EFI_FILE_MODE_WRITE
    bool                 has_pending; ///< `pending` holds an entry a Read could not take
    AxlFsEntry           pending;     ///< directory entry kept for the next Read
    bool                 dead;        ///< set on unpublish or force-close

    /* Linked-list pointers in pub->open_files. */
    FileThunk           *prev;
    FileThunk           *next;
};

// ===================================================================
// Forward decls (attr converters used by thunk_open before defn)
// ===================================================================

static uint32_t attr_efi_to_axl(uint64_t efi_attr);
static size_t utf16_units(const char *s, size_t n);
static EFI_STATUS set_info_resolved(FileThunk *self, const EFI_FILE_INFO *fi,
                                    size_t units, const AxlFsEntry *cur,
                                    char *new_path, char *parent);

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

    /* A name longer than MAX_PATH_UNITS is refused, never cut: below it
       the UTF-8 fits MAX_PATH_BYTES whole. */
    size_t units = 0;
    while (efi_name[units] != 0) {
        if (++units >= MAX_PATH_UNITS) return AXL_ERR;
    }
    /* On the heap: Open and SetInfo run on whatever stack their caller has.
       Out of memory is AXL_NO_RESOURCES -- EFI_OUT_OF_RESOURCES to the
       caller -- never the AXL_ERR that says the NAME is bad. */
    char *name = axl_malloc(MAX_PATH_BYTES);
    if (name == NULL) return AXL_NO_RESOURCES;
    axl_ucs2_to_utf8_buf((const unsigned short *)efi_name, name, MAX_PATH_BYTES);

    /* Normalize backslashes to forward slashes. */
    for (char *p = name; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    int rc = AXL_OK;
    if (name[0] == '\0' || (name[0] == '.' && name[1] == '\0')) {
        /* "" / "." → caller's path verbatim. */
        if (axl_strlen(base) >= out_size) {
            rc = AXL_ERR;
        } else {
            axl_strlcpy(out, base, out_size);
        }
    } else if (axl_path_resolve(base, name, out, out_size) != AXL_OK) {
        rc = AXL_ERR;   /* too long, or ".." above the root */
    } else {
        /* Every component must fit AxlFsEntry.name, or a handle's name
           could not be reported whole, and be one FAT could create (at most
           255 UTF-16 units), as every listed name is. */
        const char *comp = out;
        for (const char *p = out; rc == AXL_OK; p++) {
            if (*p == '/' || *p == '\0') {
                size_t len = (size_t)(p - comp);
                if (len > ENTRY_NAME_BYTES || utf16_units(comp, len) > NAME_UNITS_MAX) {
                    rc = AXL_ERR;
                }
                comp = p + 1;
            }
            if (*p == '\0') {
                break;
            }
        }
    }
    axl_free(name);
    return rc;
}

/* UTF-16 units the first @p n bytes of UTF-8 @p s take: one per character,
   two for one past the BMP (a four-byte sequence). */
static size_t
utf16_units(const char *s, size_t n)
{
    size_t units = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)s[i];
        if ((b & 0xC0u) != 0x80u) {
            units += (b >= 0xF0u) ? 2u : 1u;
        }
    }
    return units;
}

/* The directory part of absolute @p path into @p out ("/" for an entry at
   the root). */
static void
path_parent(const char *path, char *out, size_t cap)
{
    size_t cut = 0;
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            cut = i;
        }
    }
    if (cut == 0) {
        axl_strlcpy(out, "/", cap);
        return;
    }
    size_t n = (cut < cap) ? cut : cap - 1;
    axl_memcpy(out, path, n);
    out[n] = '\0';
}

/* The last component of absolute @p path. */
static const char *
path_last(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            last = p + 1;
        }
    }
    return last;
}

/* Is @p path strictly below @p dir, on a component boundary? */
static bool
path_below(const char *path, const char *dir)
{
    size_t n = axl_strlen(dir);
    return axl_strncmp(path, dir, n) == 0 && path[n] == '/';
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
 * Hard-caps the source at sizeof(AxlFsEntry.name) so a provider that
 * hands back a non-NUL-terminated buffer can't make us walk off the end.
 * axl_utf8_to_ucs2_buf is itself NUL-driven but we use a stack-local
 * NUL-terminated copy to enforce the cap. Every byte is at most one unit,
 * so a scratch of that many units holds any name whole.
 */
static size_t
efi_file_info_size(const char *utf8_name)
{
    char bounded[sizeof(((AxlFsEntry *)0)->name)];
    axl_strlcpy(bounded, utf8_name, sizeof(bounded));
    unsigned short scratch[sizeof(bounded)];
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

/* Is @p t a valid EFI_TIME (UEFI 2.11 section 8.3, as the EDK2-derived FAT
   driver's FatIsValidTime checks it)? The calendar fields must name a real
   second, TimeZone must be within a day of UTC or unspecified, and only the
   two defined Daylight bits may be set. */
static bool
efi_time_is_valid(const EFI_TIME *t)
{
    static const uint8_t days[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (t->Year < 1900 || t->Year > 9999 || t->Month < 1 || t->Month > 12
        || t->Day < 1 || t->Day > days[t->Month - 1]
        || t->Hour > 23 || t->Minute > 59 || t->Second > 59
        || t->Nanosecond > 999999999u) {
        return false;
    }
    bool leap = (t->Year % 4u == 0 && t->Year % 100u != 0) || t->Year % 400u == 0;
    if (t->Month == 2 && t->Day == 29 && !leap) {
        return false;
    }
    if (t->TimeZone != 0x07FF && (t->TimeZone < -1440 || t->TimeZone > 1440)) {
        return false;
    }
    return (t->Daylight & ~EFI_TIME_DAYLIGHT_BITS) == 0;
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
    /* The test hook leaves the NUL's unit as the caller's buffer had it. */
    axl_memset(fi, 0, axl_fs_provider_test_unterminated
                      ? needed - sizeof(CHAR16) : needed);
    fi->Size         = needed;
    fi->FileSize     = info->size;
    /* Spec PhysicalSize is "the amount of physical space allocated on
       the volume for the file." Providers that distinguish logical
       from physical (sparse, compressed, dedup'd) fill alloc_size;
       providers that don't, leave it zero and we fall back to
       FileSize so consumers always see a sane non-zero value. */
    fi->PhysicalSize = (info->alloc_size != 0) ? info->alloc_size : info->size;
    fi->Attribute    = attr_axl_to_efi(info->attributes);
    /* ModificationTime in UTC calendar fields, zone unspecified, as AXL
       writes every file time; 0 (unknown) or a time EFI_TIME cannot hold
       stays all zero. CreateTime and LastAccessTime stay zero: AxlFsEntry
       carries one time and it is not duplicated across all three. */
    if (info->mtime_unix != 0 && info->mtime_unix <= EFI_TIME_UNIX_MAX) {
        axl_backend_unix_to_efi_time(info->mtime_unix, &fi->ModificationTime);
    }

    /* Trailer: UCS-2 name + NUL into FileName[]. */
    size_t cells_cap = (needed - SIZE_OF_EFI_FILE_INFO) / sizeof(CHAR16);
    if (axl_fs_provider_test_unterminated) {
        /* The name without its NUL, the info sized to match: a driver
           that omits it. Converted aside so the NUL is not written. */
        unsigned short w[sizeof(info->name)];
        size_t units = axl_utf8_to_ucs2_buf(info->name, w,
                                            sizeof(w) / sizeof(w[0]));
        axl_memcpy(fi->FileName, w, units * sizeof(CHAR16));
        fi->Size    = SIZE_OF_EFI_FILE_INFO + units * sizeof(CHAR16);
        *inout_size = fi->Size;
        return EFI_SUCCESS;
    }
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
    int rrc = resolve_efi_path(self->path, file_name, resolved, sizeof(resolved));
    if (rrc != AXL_OK) {
        return (rrc == AXL_NO_RESOURCES) ? EFI_OUT_OF_RESOURCES : EFI_INVALID_PARAMETER;
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
    child->backing  = backing;
    child->is_dir   = is_dir;
    child->writable = (open_mode & EFI_FILE_MODE_WRITE) != 0u;
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

/* Is @p e's name ONE entry a caller can join onto a path
   (axl_fs_name_is_one_component)? "." and ".." are dropped quietly: any
   directory may list them. The rest are dropped with the provider and the
   name, escaped -- at warning for the first in directory @p dir (the ring
   axl_fs_report_first keeps), at debug after that, so a remote directory
   full of such names does not flood the log. */
static bool
entry_name_is_one(
    const Publication  *pub,
    const char         *dir,
    const AxlFsEntry   *e
    )
{
    const char *n = e->name;
    if (axl_strcmp(n, ".") == 0 || axl_strcmp(n, "..") == 0) {
        return false;
    }
    /* One entry name, and one FAT could create: at most 255 UTF-16 units.
       UEFI readers size their buffers for FAT's names (EDK2's
       FileHandleFindNextFile asserts no EFI_BUFFER_TOO_SMALL), and a longer
       name could not be opened through a path anyway. */
    if (axl_fs_name_is_one_component(n)
        && utf16_units(n, axl_strlen(n)) <= NAME_UNITS_MAX) {
        return true;
    }
    char esc[64];
    axl_fs_escape_name(n, esc, sizeof(esc));
    const char *label = (pub->provider.default_label != NULL)
                        ? pub->provider.default_label : "";
    if (axl_fs_report_first(axl_fs_report_key(pub, dir))) {
        /* log-level: the walker the entry is hidden from sees only a shorter
           listing; nothing it is returned says an entry was skipped. */
        axl_warning("provider '%s' listed \"%s\", which is not one entry name, or "
                    "is over 255 UTF-16 units; skipped (more in this directory are "
                    "logged at debug)", label, esc);
    } else {
        axl_debug("provider '%s' listed \"%s\", which is not one entry name, or is "
                  "over 255 UTF-16 units; skipped", label, esc);
    }
    return false;
}

static EFI_STATUS EFIAPI
thunk_read(EFI_FILE_PROTOCOL *this, UINTN *buffer_size, VOID *buffer)
{
    FileThunk *self = FILE_FROM_EFI(this);
    if (self->dead || self->pub == NULL) return EFI_DEVICE_ERROR;
    if (buffer_size == NULL) return EFI_INVALID_PARAMETER;

    if (self->is_dir) {
        /* An entry a too-small buffer could not take is kept and handed
           back on the next Read, as the FAT driver keeps it: a reader that
           probes for the size and retries must get the SAME entry. */
        if (!self->has_pending) {
            for (;;) {
                axl_memset(&self->pending, 0, sizeof(self->pending));
                self->pending.struct_size = sizeof(self->pending);
                self->pending.version     = AXL_FS_ENTRY_VERSION;
                bool end = false;
                AxlFsStatus s = self->pub->provider.read_dir(self->backing,
                                                              &self->pending, &end);
                if (s != AXL_FS_OK) return status_to_efi(s);
                if (end) {
                    /* End-of-directory: spec says return success with
                       *BufferSize = 0. */
                    *buffer_size = 0;
                    return EFI_SUCCESS;
                }
                if (axl_fs_provider_test_unfiltered
                    || entry_name_is_one(self->pub, self->path, &self->pending)) {
                    break;
                }
            }
            self->has_pending = true;
        }
        EFI_STATUS rc = write_efi_file_info(&self->pending, buffer_size, buffer);
        if (rc != EFI_BUFFER_TOO_SMALL) {
            self->has_pending = false;
        }
        return rc;
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
    if (s == AXL_FS_OK) {
        self->has_pending = false;   /* a rewind starts the listing over */
    }
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
    if (buffer_size < SIZE_OF_EFI_FILE_INFO) return EFI_INVALID_PARAMETER;

    Publication   *pub = self->pub;
    EFI_FILE_INFO *fi  = buffer;

    /* The FileName must end inside the buffer and convert whole. */
    size_t name_max = (buffer_size - SIZE_OF_EFI_FILE_INFO) / sizeof(CHAR16);
    size_t units    = 0;
    while (units < name_max && fi->FileName[units] != 0) {
        units++;
    }
    if (units == name_max) return EFI_INVALID_PARAMETER;

    AxlFsEntry cur = {
        .struct_size = sizeof(cur),
        .version     = AXL_FS_ENTRY_VERSION,
    };
    AxlFsStatus s = pub->provider.get_info(self->backing, &cur);
    if (s != AXL_FS_OK) return status_to_efi(s);

    /* The new path, resolved as the EDK2-derived FAT driver resolves it: a
       leading separator from the volume root, anything else from the
       file's PARENT directory (the Shell's mv sends "..\x"). An empty
       FileName keeps the name. The path buffers live on the heap: SetInfo
       runs on whatever stack its caller has. */
    char *new_path = axl_malloc(2u * MAX_PATH_BYTES);
    if (new_path == NULL) return EFI_OUT_OF_RESOURCES;
    EFI_STATUS rc = set_info_resolved(self, fi, units, &cur, new_path,
                                      new_path + MAX_PATH_BYTES);
    axl_free(new_path);
    return rc;
}

/* thunk_set_info once the current metadata is read: @p new_path and
   @p parent are MAX_PATH_BYTES of heap each. */
static EFI_STATUS
set_info_resolved(
    FileThunk           *self,
    const EFI_FILE_INFO *fi,
    size_t               units,
    const AxlFsEntry    *cur,
    char                *new_path,
    char                *parent
    )
{
    Publication *pub      = self->pub;
    AxlFsStatus  s;
    bool         renaming = false;
    new_path[0] = '\0';
    if (units > 0) {
        path_parent(self->path, parent, MAX_PATH_BYTES);
        int rrc = resolve_efi_path(parent, fi->FileName, new_path, MAX_PATH_BYTES);
        if (rrc != AXL_OK) {
            /* too long, or above the root -- or out of memory */
            return (rrc == AXL_NO_RESOURCES) ? EFI_OUT_OF_RESOURCES
                                             : EFI_INVALID_PARAMETER;
        }
        renaming = axl_strcmp(new_path, self->path) != 0;
    }

    /* A non-zero time must be a valid EFI_TIME after 1970-01-01 00:00:00
       UTC: 0 already means "unknown". All zero is UEFI's "leave it". */
    static const EFI_TIME zero_time;
    bool     time_given = axl_memcmp(&fi->ModificationTime, &zero_time,
                                     sizeof(zero_time)) != 0;
    uint64_t want_mtime = 0;
    if (time_given) {
        if (!efi_time_is_valid(&fi->ModificationTime)) {
            return EFI_INVALID_PARAMETER;
        }
        want_mtime = axl_backend_efi_time_to_unix(&fi->ModificationTime);
        if (want_mtime == 0) return EFI_INVALID_PARAMETER;
    }

    uint32_t want_attr    = attr_efi_to_axl(fi->Attribute);
    bool     size_changed = !self->is_dir && fi->FileSize != cur->size;
    bool     time_set     = time_given
                            && (size_changed || want_mtime != cur->mtime_unix);
    bool     attr_changed = want_attr != cur->attributes;

    if (((want_attr ^ cur->attributes) & AXL_FS_ATTR_DIRECTORY) != 0u) {
        return EFI_ACCESS_DENIED;   /* a file cannot become a directory */
    }
    /* UEFI 2.11 13.5.14: through a read-only handle only the Attribute
       may change. */
    if (!self->writable && (renaming || size_changed || time_set)) {
        return EFI_ACCESS_DENIED;
    }
    /* Refused as the FAT driver refuses them, whether or not the provider
       has `rename`: the root has no name to change and nothing can be
       renamed onto it; a directory cannot move into itself or below
       itself; and a directory with any handle open below it does not move
       at all -- those handles' paths would keep the old name. */
    if (renaming && (axl_strcmp(self->path, "/") == 0
                     || axl_strcmp(new_path, "/") == 0)) {
        return EFI_ACCESS_DENIED;
    }
    if (renaming && self->is_dir) {
        if (path_below(new_path, self->path)) {
            return EFI_ACCESS_DENIED;
        }
        for (const FileThunk *o = pub->open_first; o != NULL; o = o->next) {
            if (o != self && !o->dead && path_below(o->path, self->path)) {
                return EFI_ACCESS_DENIED;
            }
        }
    }

    AxlFsProviderRename rename_cb = pub->provider.rename;
    bool                name_via_set_info = false;
    if (renaming && rename_cb != NULL) {
        s = rename_cb(self->backing, new_path);
        if (s != AXL_FS_OK) return status_to_efi(s);
        axl_strlcpy(self->path, new_path, sizeof(self->path));
    } else if (renaming) {
        /* Without `rename` a provider renames within the directory only:
           a move elsewhere is EFI_UNSUPPORTED, nothing changed, which the
           caller reads as "copy instead" (axl_file_move). The parents are
           compared in place: everything before each path's last name. */
        size_t from = (size_t)(path_last(self->path) - self->path);
        size_t to   = (size_t)(path_last(new_path) - new_path);
        if (from != to || axl_memcmp(self->path, new_path, from) != 0) {
            return EFI_UNSUPPORTED;
        }
        name_via_set_info = true;
    }

    if (!(attr_changed || size_changed || time_set || name_via_set_info)) {
        return EFI_SUCCESS;
    }
    if (pub->provider.set_info == NULL) return EFI_WRITE_PROTECTED;

    AxlFsEntry info = {
        .struct_size = sizeof(info),
        .version     = AXL_FS_ENTRY_VERSION,
        .size        = fi->FileSize,
        .mtime_unix  = time_set ? want_mtime : 0,
        .attributes  = want_attr,
    };
    axl_strlcpy(info.name, renaming ? path_last(new_path) : cur->name,
                sizeof(info.name));
    s = pub->provider.set_info(self->backing, &info);
    if (s == AXL_FS_OK && name_via_set_info) {
        axl_strlcpy(self->path, new_path, sizeof(self->path));
    }
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
    f->backing  = backing;
    f->is_dir   = is_dir;
    /* The FAT driver opens a volume's root read-write. */
    f->writable = true;
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
    /* Exactly version 1's size (ending at backend_ctx), or one that covers
       all of `rename`: a size ending INSIDE it would leave the thunk part
       of a function pointer to call (axl_fs_provider_publish copies only
       struct_size bytes). */
    if (p->struct_size != offsetof(AxlFsProvider, rename)
        && p->struct_size < offsetof(AxlFsProvider, rename) + sizeof(p->rename)) {
        return AXL_ERR;
    }
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
 * Unpublish removes it again by device path, not by name, so a name that has
 * since moved to another volume stays with that volume.
 */
static void
publish_shell_map(Publication *pub)
{
    /* FS20 and up, not the lowest free name: the shell's `map -u` hands out
       low numbers from a counter that never checks the map, and would take
       this volume's name for the next volume it names (AXL_VOLUME_FSN_FLOOR). */
    char name[16];
    if (axl_volume_pick_free_fsn(name, sizeof(name)) != AXL_OK) {
        axl_warning("no free shell name from FS%u to FS%u; volume left unmapped",
                    (unsigned)AXL_VOLUME_FSN_FLOOR,
                    (unsigned)(AXL_VOLUME_FSN_LIMIT - 1));
        return;
    }
    int rc = axl_volume_set_map(pub->device_path, name);
    if (rc == AXL_UNSUPPORTED) {
        /* Expected in a headless/driver context (no EFI_SHELL_PROTOCOL
           locatable) -- not a fault, so no WARN. Best-effort, matching
           axl_driver_connect_handle above: the volume is still reachable via
           the returned handle / axl_protocol_enumerate, just not by an
           "fsN:" path. */
        axl_debug("no shell present; volume left unmapped");
    } else if (rc != AXL_OK) {
        /* A shell IS present but rejected SetMap for this name -- worth a
           WARN, unlike the plain no-shell case above. */
        axl_warning("shell map assignment failed (SetMap rejected)");
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
    /* Copy only the bytes the provider declares and leave the rest zero, so
       a vtable built against an older header neither over-reads nor
       supplies a member it does not have. */
    size_t have = (provider->struct_size < sizeof(pub->provider))
                  ? provider->struct_size : sizeof(pub->provider);
    axl_memcpy(&pub->provider, provider, have);
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

    /* Drop every shell name for this volume while device_path is still ours
       to match on. The shell keeps its own copy of a mapped device path, so
       a name left behind outlives the volume: it resolves to a path with no
       volume behind it, or to the next publication with the same vendor
       GUID. Match by device path, not by the name publish assigned: that
       name may have moved to another volume since (the user, or `map -u`,
       gave it away), and removing it would take it from that volume; and a
       user or a `map -r` may have added names of their own. The shell
       cannot tell apart two publications with the same vendor GUID either
       (identical device paths), so their names go together. */
    if (axl_volume_unmap_device(pub->device_path, NULL) != AXL_OK) {
        /* Best-effort: a shell with no programmatic unmap (the old EFI 1.x
           shell) keeps a name its own `map -r` gave the volume. */
        axl_warning("the shell still maps a name to the unpublished volume; "
                    "'map -r' clears it");
    }

    pub->dead = true;
    axl_free(pub->device_path);
    axl_free(pub);
    return AXL_OK;
}
