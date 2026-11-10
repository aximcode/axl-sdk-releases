/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-shell.c
    Path resolution under the modern UEFI Shell that parks nothing on the
    Shell's per-command free list. The why, and the shape of the
    resolution, are in axl-backend-native-shell.h.

    Measured before this existed (both arches, per 400 operations inside one
    shell command): a read, write, stat or directory listing parked 25 pages
    absolute and 50 relative, a create+delete 50/100, a mkdir+rmdir 75/175,
    axl_fs_path_canonical 50/125, axl_volume_enumerate 25, getcwd 25 -- all
    of it held until the command ended, which for a resident driver is
    never. After: 0 for every one (test-shell-buffer-growth-qemu.sh).

    Two answers the Shell can only give by parking are found another way:

    - The current directory (GetCurDir). The Shell mirrors `cwd` into a
      volatile UEFI variable, and reading it (GetVariable) parks nothing.
      Every SetCurDir writes both -- this image's, a child image's, the
      user's `cd` at the prompt while a resident driver runs -- so an answer
      is reused while the variable still holds exactly that answer, and no
      list of invalidation points has to be kept or can be missed. An answer
      the variable does not vouch for is never kept, so a change that goes
      through SetCurDir is always seen. What the variable CANNOT show is a
      change that bypasses it: the list moving to B and the variable then
      being put back to A without SetCurDir (EDK2's Execute with an
      Environment does that; AXL passes none) leaves the cache answering A.
      On the EDK2 Shell the two have not been seen to differ otherwise --
      not across Execute's nested shells, not across a child image's
      SetCurDir.

    The cache is static and unguarded. A file operation at TPL_CALLBACK that
    preempts one at TPL_APPLICATION can rewrite it mid-copy; both write the
    same answer unless the cwd changed in exactly that window.
    - A volume's fs<n> name (GetMapFromDevicePath, the first fs<n> on the
      volume's ';'-list). Not cached: no cache of that list can be
      validated without parking, because the Shell's SetMap of a name the
      volume already has deletes and re-appends it -- a reorder no other
      call shows. Instead `FS0:`..`FS63:` are probed through
      GetDevicePathFromMap (which parks nothing): when exactly ONE of them
      maps to the volume, the order cannot matter and that is the answer;
      otherwise the Shell is asked. The first name of all
      (axl_volume_map_alias) is always asked of the Shell for the same
      reason.

    The cache lives in static storage: an image's pool allocations outlive
    it, so a cache that allocated would leak on every launch.
**/

#include "axl-backend-native-shell.h"
#include "axl-backend-native-nosh.h"   /* axl_nosh_open_root */

/* Longest current directory (CHAR16, NUL included) the cache keeps: AXL's
   own volume-relative path ceiling (AXL_BACKEND_VOL_PATH_MAX; AxlFs README,
   "Path length"). A longer one is answered straight from the Shell,
   uncached, and parks a buffer per call -- but from such a directory a
   relative operation fails at resolution anyway, so only getcwd sees it.
   FAT cannot reach it (260 for a whole path); a non-FAT volume can. */
#define AXL_SHELL_CWD_MAX       512u

/* Longest `NAME:` (CHAR16, NUL included) resolved from a stack buffer; a
   longer one is copied to the heap. */
#define AXL_SHELL_NAME_MAX      64u

/* The fs<n> names probed for a volume: FS0..FS63. The Shell numbers them
   densely from 0 by volume, so a machine with fewer than 64 volumes has
   every generated one in range. */
#define AXL_SHELL_FS_PROBE      64u

typedef struct {
    bool     valid;
    size_t   len;                       /* characters in `cwd` */
    CHAR16   cwd[AXL_SHELL_CWD_MAX];    /* what GetCurDir answered */
    CHAR16   map[AXL_SHELL_NAME_MAX];   /* cwd up to its first ':' */
} CwdCache;

static CwdCache   mCwd;
static CHAR16     mCwdVar[AXL_SHELL_CWD_MAX];     /* scratch for the variable read */

static const CHAR16 mRootRest[] = { (CHAR16)'\\', 0 };

// ===================================================================
// Small helpers
// ===================================================================

static const CHAR16 *
first_colon(
    const CHAR16  *s
    )
{
    for (; *s != 0; s++) {
        if (*s == (CHAR16)':') {
            return s;
        }
    }
    return NULL;
}

/* BaseLib's PathRemoveLastItem, which the Shell runs on `cwd\` to climb to
   the root for a path that starts with '\'. */
static bool
path_remove_last_item(
    CHAR16  *path
    )
{
    CHAR16 *last = NULL;
    for (CHAR16 *w = path; *w != 0; w++) {
        if (*w == (CHAR16)'\\' && w[1] != 0) {
            last = w + 1;
        } else if (*w == (CHAR16)':' && w[1] != (CHAR16)'\\' && w[1] != 0) {
            last = w + 1;
        }
    }
    if (last == NULL) {
        return false;
    }
    *last = 0;
    return true;
}

static bool
bytes_equal(
    const void  *a,
    const void  *b,
    size_t       n
    )
{
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

// ===================================================================
// The current directory
// ===================================================================

/* The Shell's `cwd` variable mirror into mCwdVar. @return false when there
   is none to read (not an EDK2-derived Shell, or a value too long). */
static bool
read_cwd_variable(
    UINTN  *size
    )
{
    EFI_GUID guid = gShellVariableGuid;
    UINTN    sz   = sizeof(mCwdVar);
    if (gRT == NULL || gRT->GetVariable == NULL) {
        return false;
    }
    EFI_STATUS st = gRT->GetVariable((CHAR16 *)L"cwd", &guid, NULL, &sz, mCwdVar);
    if (EFI_ERROR(st)) {
        return false;
    }
    *size = sz;
    return true;
}

/* Does the `cwd` variable just read (@p size bytes in mCwdVar) hold
   exactly @p cwd? The Shell stores the string without its NUL. */
static bool
variable_holds(
    const CHAR16  *cwd,
    size_t         len,
    UINTN          size
    )
{
    return size == len * sizeof(CHAR16) && bytes_equal(cwd, mCwdVar, size);
}

const CHAR16 *
axl_shell_cwd(
    EFI_SHELL_PROTOCOL  *shell
    )
{
    UINTN size    = 0;
    bool  stamped = read_cwd_variable(&size);

    if (shell->GetCurDir == NULL || shell->GetDevicePathFromMap == NULL) {
        return NULL;
    }
    /* The mapping check is what the variable cannot see: the Shell answers
       GetCurDir from its own list, which reports nothing once the map list
       is empty, and the variable stays. A cwd on a volume that is no longer
       mapped is re-read every time -- rare, and never stale. */
    if (stamped && mCwd.valid && variable_holds(mCwd.cwd, mCwd.len, size)
        && shell->GetDevicePathFromMap(mCwd.map) != NULL) {
        return mCwd.cwd;
    }
    mCwd.valid = false;

    const CHAR16 *cwd = shell->GetCurDir(NULL);   /* parks one buffer */
    if (cwd == NULL || !axl_backend_wcs_copy(mCwd.cwd, AXL_SHELL_CWD_MAX, cwd)) {
        return cwd;
    }
    /* Kept only when the variable agrees with the answer: then any change
       to either shows as a change to the variable. When they disagree, or
       there is no variable (a Shell that keeps none), the answer is given
       but not kept, and the next call asks again. */
    size_t        len   = axl_backend_wcslen(mCwd.cwd);
    const CHAR16 *colon = first_colon(mCwd.cwd);
    size_t        n     = (colon != NULL) ? (size_t)(colon - mCwd.cwd) + 1 : 0;
    if (!stamped || !variable_holds(mCwd.cwd, len, size)
        || n == 0 || n + 1 > AXL_SHELL_NAME_MAX) {
        return mCwd.cwd;
    }
    for (size_t i = 0; i < n; i++) {
        mCwd.map[i] = mCwd.cwd[i];
    }
    mCwd.map[n] = 0;
    mCwd.len    = len;
    mCwd.valid  = true;
    return mCwd.cwd;
}

// ===================================================================
// Path resolution (EfiShellGetDevicePathFromFilePath, string for string)
// ===================================================================

void
axl_shell_path_done(
    AxlShellPath  *p
    )
{
    if (p != NULL) {
        axl_backend_free(p->joined);
        p->joined = NULL;
    }
}

int
axl_shell_path_resolve(
    EFI_SHELL_PROTOCOL  *shell,
    const CHAR16        *path,
    AxlShellPath        *out
    )
{
    out->volume = NULL;
    out->rest   = NULL;
    out->joined = NULL;
    if (path == NULL || shell->GetDevicePathFromMap == NULL) {
        return AXL_NOT_FOUND;
    }

    /* No ':' ANYWHERE in the path: join it onto the current directory --
       `cwd` + '\' + path, or, for a path starting with '\', the current
       volume's root (the Shell climbs `cwd\` with PathRemoveLastItem). */
    const CHAR16 *full = path;
    if (first_colon(path) == NULL) {
        const CHAR16 *cwd = axl_shell_cwd(shell);
        if (cwd == NULL) {
            return AXL_NOT_FOUND;
        }
        size_t  cl = axl_backend_wcslen(cwd);
        size_t  pl = axl_backend_wcslen(path);
        CHAR16 *j  = axl_backend_alloc((cl + pl + 2) * sizeof(CHAR16));
        if (j == NULL) {
            return AXL_NOT_FOUND;
        }
        for (size_t i = 0; i < cl; i++) {
            j[i] = cwd[i];
        }
        j[cl]     = (CHAR16)'\\';
        j[cl + 1] = 0;
        const CHAR16 *p = path;
        if (*p == (CHAR16)'\\') {
            p++;
            while (path_remove_last_item(j)) {
            }
        }
        size_t jl = axl_backend_wcslen(j);
        for (size_t i = 0; p[i] != 0; i++) {
            j[jl++] = p[i];
        }
        j[jl] = 0;
        out->joined = j;
        full        = j;
        if (first_colon(full) == NULL) {
            /* A current directory with no volume: the Shell would recurse
               on it forever. Not a real Shell state. */
            axl_shell_path_done(out);
            return AXL_NOT_FOUND;
        }
    }

    /* The map name: everything up to and including the FIRST ':'. */
    size_t  nl = (size_t)(first_colon(full) - full) + 1;
    CHAR16  small[AXL_SHELL_NAME_MAX];
    CHAR16 *name = small;
    if (nl + 1 > AXL_SHELL_NAME_MAX) {
        name = axl_backend_alloc((nl + 1) * sizeof(CHAR16));
        if (name == NULL) {
            axl_shell_path_done(out);
            return AXL_NOT_FOUND;
        }
    }
    for (size_t i = 0; i < nl; i++) {
        name[i] = full[i];
    }
    name[nl] = 0;
    /* The map list's own device path: firmware-owned, never parked. */
    const EFI_DEVICE_PATH_PROTOCOL *dp = shell->GetDevicePathFromMap(name);
    if (name != small) {
        axl_backend_free(name);
    }

    EFI_DEVICE_PATH_PROTOCOL *walk   = (EFI_DEVICE_PATH_PROTOCOL *)dp;
    EFI_HANDLE                handle = NULL;
    EFI_GUID                  sfs    = gEfiSimpleFileSystemProtocolGuid;
    if (dp == NULL || EFI_ERROR(gBS->LocateDevicePath(&sfs, &walk, &handle))) {
        axl_shell_path_done(out);
        return AXL_NOT_FOUND;
    }

    /* The Shell's own quirk, kept: a remainder of exactly ONE character
       names the root, whatever the character (`fs0:\`, and `fs0:a` too). */
    const CHAR16 *rest = full + nl;
    if (rest[0] != 0 && rest[1] == 0) {
        rest = mRootRest;
    }
    out->volume = handle;
    out->rest   = rest;
    return AXL_OK;
}

EFI_STATUS
axl_shell_path_open(
    EFI_SHELL_PROTOCOL   *shell,
    const CHAR16         *path,
    UINT64                mode,
    UINT64                attributes,
    EFI_FILE_PROTOCOL   **out
    )
{
    AxlShellPath sp;

    *out = NULL;
    if (axl_shell_path_resolve(shell, path, &sp) != AXL_OK) {
        return EFI_NOT_FOUND;   /* GetDevicePathFromFilePath answered NULL */
    }

    /* InternalOpenFileDevicePath on the one file-path node the Shell builds:
       open the root, Open the remainder from it with the caller's mode and
       attributes, close the root. */
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS         st   = axl_nosh_open_root(sp.volume, &root);
    if (!EFI_ERROR(st)) {
        EFI_FILE_PROTOCOL *file = NULL;
        st = root->Open(root, &file, (CHAR16 *)sp.rest, mode, attributes);
        root->Close(root);
        if (!EFI_ERROR(st)) {
            *out = file;
        } else if (file != NULL) {
            file->Close(file);
        }
    }
    axl_shell_path_done(&sp);
    return st;
}

// ===================================================================
// A volume's fs<n> name
// ===================================================================

unsigned
axl_shell_fs_probe(
    EFI_SHELL_PROTOCOL  *shell,
    void                *device_path,
    unsigned            *index
    )
{
    unsigned found = 0;

    if (shell->GetDevicePathFromMap == NULL) {
        return 0;
    }
    for (unsigned k = 0; k < AXL_SHELL_FS_PROBE; k++) {
        CHAR16   name[8];
        unsigned i = 0;
        name[i++] = (CHAR16)'F';
        name[i++] = (CHAR16)'S';
        if (k >= 10) {
            name[i++] = (CHAR16)('0' + k / 10);
        }
        name[i++] = (CHAR16)('0' + k % 10);
        name[i++] = (CHAR16)':';
        name[i]   = 0;
        const EFI_DEVICE_PATH_PROTOCOL *dp = shell->GetDevicePathFromMap(name);
        if (dp != NULL && axl_backend_dp_equal(dp, device_path)) {
            *index = k;
            if (++found > 1) {
                break;      /* two already decide it: the Shell is asked */
            }
        }
    }
    return found;
}
