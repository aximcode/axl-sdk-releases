/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-efi1x.c
    Old EFI 1.x shell (EFI Toolkit "newshell") support for the native backend.

    The old shell has no EFI_SHELL_PROTOCOL, so none of the modern
    conveniences exist: no OpenFileByName, no GetMapFromDevicePath, no
    GetCurDir. What it does publish is SHELL_ENVIRONMENT, whose GetMap
    (name -> device path) and CurDir are enough to rebuild path resolution
    from first principles:

        NAME:\dir\file  ->  GetMap("NAME") -> device path
                        ->  LocateDevicePath(SimpleFileSystem) -> handle
                        ->  OpenVolume() -> root EFI_FILE_PROTOCOL
                        ->  root->Open("\dir\file")

    A relative path is joined onto CurDir() first and then resolved the same
    way. The resulting handle is an EFI_FILE_PROTOCOL *, which is exactly what
    a SHELL_FILE_HANDLE is on the modern shell — so the rest of the backend's
    file layer is shell-agnostic once the handle exists.
**/

#include "axl-backend-native-efi1x.h"
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("backend");

/* Longest path (in CHAR16, NUL included) we will build while joining a
   relative path onto the current directory. FAT's own limit is far below
   this; a longer request is a caller bug, not a filesystem the old shell
   could ever have mounted. */
#define AXL_EFI1X_PATH_MAX  512u

/* How many fs<n> names the reverse map lookup scans. */
#define AXL_EFI1X_FS_SCAN   64u

/* Longest volume name we resolve (`fs0`, `SCRATCH`, ...), NUL included. */
#define AXL_EFI1X_NAME_MAX  64u

// ===================================================================
// SHELL_ENVIRONMENT
// ===================================================================

static EFI_SHELL_ENVIRONMENT  *mShellEnv = NULL;
static bool                    mShellEnvLocated = false;

/* Module-owned copy of the current directory, so axl_efi1x_getcwd can honor
   the "caller does not free" contract while releasing the firmware buffer the
   old shell's CurDir hands back. See axl_efi1x_getcwd. */
static CHAR16                  mCwdCache[AXL_EFI1X_PATH_MAX];

EFI_SHELL_ENVIRONMENT *
axl_efi1x_shell_env(void)
{
    if (!mShellEnvLocated) {
        mShellEnvLocated = true;
        EFI_GUID guid = gEfiShellEnvironmentGuid;
        gBS->LocateProtocol(&guid, NULL, (VOID **)&mShellEnv);
    }
    return mShellEnv;
}

const unsigned short *
axl_efi1x_getcwd(void)
{
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (se == NULL || se->CurDir == NULL) {
        return NULL;
    }
    /* Unlike the modern shell's GetCurDir (firmware-owned, do-not-free), the
       old shell's CurDir StrDuplicate's a buffer the caller must FreePool
       (proven by the EFI Toolkit's own loadarg.c, which FreePool's it). But
       axl_backend_shell_getcwd's contract is "returns a pointer you do NOT
       free" — so copy into a module-owned cache (valid until the next call,
       exactly the modern shell's lifetime) and release the firmware buffer
       here. Single-threaded UEFI + a caller that converts immediately makes
       the single-slot cache safe. */
    CHAR16 *cwd = se->CurDir(NULL);
    if (cwd == NULL) {
        return NULL;
    }
    size_t n = axl_backend_wcslen(cwd);
    if (n + 1 > AXL_EFI1X_PATH_MAX) {
        axl_backend_free(cwd);
        return NULL;
    }
    for (size_t i = 0; i <= n; i++) {
        mCwdCache[i] = cwd[i];
    }
    axl_backend_free(cwd);
    return (const unsigned short *)mCwdCache;
}

const unsigned short *
axl_efi1x_getenv(
    const unsigned short  *name
    )
{
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (se == NULL || se->GetEnv == NULL || name == NULL) {
        return NULL;
    }
    /* GetEnv (unlike CurDir) returns a live pointer into the shell's own
       environment storage — NOT a copy — so it is firmware-owned and must
       NOT be freed, matching the modern shell's GetEnv contract. */
    return (const unsigned short *)se->GetEnv((CHAR16 *)name);
}

EFI_HANDLE
axl_efi1x_shell_parent(void)
{
    /* SHELL_ENVIRONMENT.Execute wants a ParentImageHandle carrying a
       SHELL_INTERFACE (a shell-launched image) so it can inherit the shell
       context; a resident driver's own image handle has none, so Execute
       rejects it with EFI_INVALID_PARAMETER. Prefer our own handle when it IS
       shell-launched (the plain shell-app case — unchanged behavior); else
       borrow a live shell-launched image's handle (the launcher that called
       into this resident driver is still alive and publishes one). */
    EFI_GUID  sig   = gEfiShellInterfaceGuid;
    void     *iface = NULL;
    if (gImageHandle != NULL
        && !EFI_ERROR(gBS->HandleProtocol(gImageHandle, &sig, &iface))) {
        return gImageHandle;
    }

    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    EFI_HANDLE  parent  = gImageHandle;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &sig, NULL,
                                           &count, &handles))) {
        if (handles != NULL && count > 0) {
            parent = handles[0];
        }
        if (handles != NULL) {
            gBS->FreePool(handles);
        }
    }
    return parent;
}

/* Longest `set` command line we build (CHAR16, NUL included). */
#define AXL_EFI1X_CMD_MAX  1024u

/* True for a character that can't be carried safely inside the quoted VALUE of
   a `set NAME "VALUE"` command: `"` closes the quote (a command-injection
   vector), `^` is the shell's escape character (a trailing `^` would escape the
   closing quote), `%` triggers the shell's %var% expansion (silent value
   mangling), and CR/LF/NUL split or truncate the command line. Space is fine —
   that's what the quoting is for. */
static bool
value_char_unsafe(CHAR16 c)
{
    return c == (CHAR16)'"' || c == (CHAR16)'^' || c == (CHAR16)'%'
        || c == (CHAR16)'\r' || c == (CHAR16)'\n' || c == 0;
}

/* True for a character not allowed in a variable NAME (kept strict — the name
   is unquoted on the command line). */
static bool
name_char_unsafe(CHAR16 c)
{
    bool ok = (c >= (CHAR16)'A' && c <= (CHAR16)'Z')
           || (c >= (CHAR16)'a' && c <= (CHAR16)'z')
           || (c >= (CHAR16)'0' && c <= (CHAR16)'9')
           || c == (CHAR16)'_';
    return !ok;
}

int
axl_efi1x_setenv(
    const unsigned short  *name,
    const unsigned short  *value,
    bool                   volatile_var
    )
{
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (se == NULL || se->Execute == NULL || name == NULL || name[0] == 0) {
        return AXL_ERR;
    }

    /* Reject anything the `set` command line can't carry unmangled. The old
       shell has no programmatic SetEnv and no quoting beyond `"..."`, so a
       clean AXL_ERR beats a corrupted or injected command. */
    for (size_t i = 0; name[i] != 0; i++) {
        if (name_char_unsafe((CHAR16)name[i])) {
            return AXL_ERR;
        }
    }
    if (value != NULL) {
        for (size_t i = 0; value[i] != 0; i++) {
            if (value_char_unsafe((CHAR16)value[i])) {
                return AXL_ERR;
            }
        }
    }

    /* An empty (or NULL) value DELETES the variable on the modern shell's
       SetEnv (UEFI Shell Spec 2.2), and axl_unsetenv relies on that. Match it:
       emit `@set -d NAME` (the old shell's delete form) rather than `@set NAME
       ""`, which would leave an empty var behind. */
    bool remove = (value == NULL || value[0] == 0);

    CHAR16 cmd[AXL_EFI1X_CMD_MAX];
    size_t len = 0;

    /* Lead with '@' to suppress the shell's echo of the synthesized command
       (the consumer expects a silent setenv, not a visible `set` line). */
    const CHAR16 *pre = remove       ? (const CHAR16 *)u"@set -d "
                      : volatile_var ? (const CHAR16 *)u"@set -v "
                                     : (const CHAR16 *)u"@set ";
    for (size_t i = 0; pre[i] != 0; i++) {
        cmd[len++] = pre[i];
    }
    for (size_t i = 0; name[i] != 0; i++) {
        if (len + 4 >= AXL_EFI1X_CMD_MAX) {
            return AXL_ERR;
        }
        cmd[len++] = (CHAR16)name[i];
    }
    if (!remove) {
        cmd[len++] = (CHAR16)' ';
        /* Quote the value so a space-bearing line (`do -f<file> var` assigns
           the whole line) is ONE argument to `set`, not several. */
        cmd[len++] = (CHAR16)'"';
        for (size_t i = 0; value[i] != 0; i++) {
            if (len + 3 >= AXL_EFI1X_CMD_MAX) {
                return AXL_ERR;
            }
            cmd[len++] = (CHAR16)value[i];
        }
        cmd[len++] = (CHAR16)'"';
    }
    cmd[len] = 0;

    /* Handle by value, per the EFI Toolkit ShellExecute convention. */
    EFI_STATUS st = se->Execute(axl_efi1x_shell_parent(), cmd, FALSE);
    return EFI_ERROR(st) ? AXL_ERR : AXL_OK;
}

// ===================================================================
// Small UCS-2 helpers
//
// UCS-2 strlen and ASCII uppercase reuse the backend primitives
// axl_backend_wcslen / axl_backend_towupper (same TU family). Only the ASCII
// lowercase fold (no backend equivalent) and the bounded path-append are
// local.
// ===================================================================

static CHAR16
w_lower(CHAR16 c)
{
    return (c >= (CHAR16)'A' && c <= (CHAR16)'Z') ? (CHAR16)(c + 32) : c;
}

/* Append @p src to @p dst (already @p *len chars long, capacity
   AXL_EFI1X_PATH_MAX). Returns false — and leaves *len unchanged — when the
   result would not fit, so a too-long path fails rather than silently
   resolving to a truncated one. */
static bool
w_append(CHAR16 *dst, size_t *len, const CHAR16 *src, size_t src_len)
{
    if (*len + src_len + 1 > AXL_EFI1X_PATH_MAX) {
        return false;
    }
    for (size_t i = 0; i < src_len; i++) {
        dst[*len + i] = src[i];
    }
    *len += src_len;
    dst[*len] = 0;
    return true;
}

// ===================================================================
// Path resolution
// ===================================================================

/* Split a leading `NAME:` off @p path. A ':' only counts as the volume
   separator when it precedes any path separator — `fs0:\a` has a prefix,
   `\a\b:c` does not. Writes the bare name (no colon) to @p name (which may be
   NULL to discard it) and points @p rest at the remainder.

   @return true when a prefix was found and fit in @p name. */
static bool
split_volume(
    const CHAR16  *path,
    CHAR16        *name,
    const CHAR16 **rest
    )
{
    size_t i = 0;
    while (path[i] != 0 && path[i] != (CHAR16)':' && path[i] != (CHAR16)'\\') {
        i++;
    }
    if (path[i] != (CHAR16)':' || i == 0 || i + 1 > AXL_EFI1X_NAME_MAX - 1) {
        return false;
    }
    if (name != NULL) {
        for (size_t j = 0; j < i; j++) {
            name[j] = path[j];
        }
        name[i] = 0;
    }
    *rest = path + i + 1;
    return true;
}

/* GetMap the volume @p name, trying the caller's spelling first and then the
   all-lower and all-upper foldings — the old shell stores fs<n> lowercase and
   `map`-assigned aliases in whatever case they were created with, and its
   GetMap is an exact match. */
static void *
map_device_path(
    const CHAR16  *name
    )
{
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (se == NULL || se->GetMap == NULL) {
        return NULL;
    }

    CHAR16 buf[AXL_EFI1X_NAME_MAX];
    size_t n = axl_backend_wcslen(name);
    if (n + 1 > AXL_EFI1X_NAME_MAX) {
        return NULL;
    }

    void *dp = se->GetMap((CHAR16 *)name);
    for (unsigned pass = 0; dp == NULL && pass < 2; pass++) {
        for (size_t i = 0; i <= n; i++) {
            buf[i] = (pass == 0) ? w_lower(name[i])
                                 : axl_backend_towupper(name[i]);
        }
        dp = se->GetMap(buf);
    }
    return dp;
}

/* Open the volume @p name (bare, no colon) and return its root directory.

   @return the root EFI_FILE_PROTOCOL (caller closes), or NULL when the name
       is unmapped or the volume has no EFI_SIMPLE_FILE_SYSTEM_PROTOCOL. */
static EFI_FILE_PROTOCOL *
open_volume_root(
    const CHAR16  *name
    )
{
    void *dp = map_device_path(name);
    if (dp == NULL) {
        return NULL;   /* no such mapping — the caller's clean "not found" */
    }

    /* LocateDevicePath advances the pointer past the matched portion; it
       walks our local copy, never the shell's device path. */
    EFI_DEVICE_PATH_PROTOCOL *walk = (EFI_DEVICE_PATH_PROTOCOL *)dp;
    EFI_HANDLE                handle = NULL;
    EFI_GUID                  sfs = gEfiSimpleFileSystemProtocolGuid;

    EFI_STATUS st = gBS->LocateDevicePath(&sfs, &walk, &handle);
    if (EFI_ERROR(st)) {
        axl_debug("efi1x: no filesystem on mapping (status=0x%llx)",
                  (unsigned long long)st);
        return NULL;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    st = gBS->HandleProtocol(handle, &sfs, (VOID **)&fs);
    if (EFI_ERROR(st) || fs == NULL) {
        return NULL;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) {
        axl_debug("efi1x: OpenVolume failed (status=0x%llx)",
                  (unsigned long long)st);
        return NULL;
    }
    return root;
}

/* CurDir for volume @p name (NULL = the current volume), retrying the
   lowercase folding the old shell stores its fs<n> names in. */
static CHAR16 *
cur_dir_of(
    const CHAR16  *name
    )
{
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (name == NULL) {
        return se->CurDir(NULL);
    }

    CHAR16 *cwd = se->CurDir((CHAR16 *)name);
    if (cwd != NULL) {
        return cwd;
    }
    CHAR16 buf[AXL_EFI1X_NAME_MAX];
    size_t n = axl_backend_wcslen(name);
    if (n + 1 > AXL_EFI1X_NAME_MAX) {
        return NULL;
    }
    for (size_t i = 0; i <= n; i++) {
        buf[i] = w_lower(name[i]);
    }
    return se->CurDir(buf);
}

/* Resolve @p path to (volume name, root-relative remainder).

   @p name receives the bare volume name; @p out receives the remainder,
   root-relative and separator-normalized (empty means the volume root).
   Both buffers are caller-owned; @p name must hold AXL_EFI1X_NAME_MAX chars
   and @p out AXL_EFI1X_PATH_MAX.

   @return AXL_OK on success; AXL_ERR when there is no shell environment, the
       path has no volume and no current directory to name one, or the joined
       path would overflow. */
static int
resolve_path(
    const CHAR16  *path,
    CHAR16        *name,
    CHAR16        *out
    )
{
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (se == NULL || se->CurDir == NULL) {
        return AXL_ERR;
    }

    const CHAR16 *rest     = path;
    bool          have_vol = split_volume(path, name, &rest);

    /* The directory the remainder hangs off: empty for a root-relative
       remainder (leading '\'), else the volume's current directory copied out
       of the (caller-owned, must-free) CurDir buffer into base_buf. */
    CHAR16 base_buf[AXL_EFI1X_PATH_MAX];
    size_t base_len = 0;

    /* A path with no `NAME:` needs the current directory to name its volume,
       even when the remainder is root-relative (`\dir\file`). */
    if (!have_vol || rest[0] != (CHAR16)'\\') {
        CHAR16 *cwd = cur_dir_of(have_vol ? name : NULL);
        if (cwd == NULL) {
            return AXL_ERR;
        }
        /* cwd is itself `NAME:\dir` — split it the same way. A shell that
           returns the bare `\dir` form leaves the volume unnamed, which is
           only fatal if the caller didn't supply one. */
        const CHAR16 *cwd_rest = cwd;
        if (!split_volume(cwd, have_vol ? NULL : name, &cwd_rest)) {
            cwd_rest = cwd;
        } else if (!have_vol) {
            have_vol = true;
        }
        int rc = AXL_OK;
        if (rest[0] != (CHAR16)'\\') {
            size_t bl = axl_backend_wcslen(cwd_rest);
            /* Drop trailing separators so the join adds exactly one. */
            while (bl > 0 && cwd_rest[bl - 1] == (CHAR16)'\\') {
                bl--;
            }
            if (bl + 1 > AXL_EFI1X_PATH_MAX) {
                rc = AXL_ERR;   /* base alone overflows the buffer */
            } else {
                for (size_t i = 0; i < bl; i++) {
                    base_buf[i] = cwd_rest[i];
                }
                base_buf[bl] = 0;
                base_len = bl;
            }
        }
        /* cwd is a StrDuplicate the old shell hands us; free it now that the
           base substring has been copied into base_buf (or rejected). */
        axl_backend_free(cwd);
        if (rc != AXL_OK) {
            return rc;
        }
    }

    if (!have_vol) {
        return AXL_ERR;   /* `\dir\file` and the cwd names no volume */
    }

    /* Join: <base> "\" <rest>, no doubled separators and no trailing one.
       An empty result means the volume root. */
    size_t len = 0;
    out[0] = 0;
    if (base_len > 0 && !w_append(out, &len, base_buf, base_len)) {
        return AXL_ERR;
    }
    while (rest[0] == (CHAR16)'\\') {
        rest++;   /* the separator is ours to add */
    }
    if (rest[0] != 0) {
        const CHAR16 sep = (CHAR16)'\\';
        if (!w_append(out, &len, &sep, 1)
            || !w_append(out, &len, rest, axl_backend_wcslen(rest))) {
            return AXL_ERR;
        }
    }
    /* A caller-supplied trailing separator (`fs0:\dir\`) survives into `rest`;
       strip it so the remainder never ends in '\' — some FAT drivers reject
       that, and the modern shell's OpenFileByName does not. Root stays empty. */
    while (len > 0 && out[len - 1] == (CHAR16)'\\') {
        out[--len] = 0;
    }
    return AXL_OK;
}

int
axl_efi1x_file_open(
    const unsigned short  *path,
    uint64_t               mode,
    uint64_t               attributes,
    EFI_FILE_PROTOCOL    **out
    )
{
    if (path == NULL || out == NULL) {
        return AXL_ERR;
    }

    CHAR16 name[AXL_EFI1X_NAME_MAX];
    CHAR16 rel[AXL_EFI1X_PATH_MAX];
    if (resolve_path((const CHAR16 *)path, name, rel) != AXL_OK) {
        return AXL_ERR;
    }

    EFI_FILE_PROTOCOL *root = open_volume_root(name);
    if (root == NULL) {
        return AXL_ERR;
    }

    if (rel[0] == 0) {
        /* The path named the volume itself (`fs0:`, `fs0:\`) — return the open
           root. This matches the modern shell's OpenFileByName, including its
           quirk that CREATE against an existing directory (e.g. `mkdir fs0:\`)
           reports success without creating anything: a shell-wide mkdir
           idempotency gap, tracked as a follow-up rather than diverged here. */
        *out = root;
        return AXL_OK;
    }

    /* EFI_FILE_PROTOCOL::Open only accepts READ, READ|WRITE, or
       READ|WRITE|CREATE. The shell's OpenFileByName is laxer, so normalize
       here rather than fail a write-only request the modern shell accepts. */
    if ((mode & AXL_FILE_MODE_WRITE) != 0) {
        mode |= AXL_FILE_MODE_READ;
    }

    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS st = root->Open(root, &file, rel, mode, attributes);
    root->Close(root);
    if (EFI_ERROR(st)) {
        /* EFI_NOT_FOUND is the ordinary "no such file" probe result and
           would flood the log; real errors still surface. */
        if (st != EFI_NOT_FOUND) {
            axl_debug("efi1x: open failed (status=0x%llx)",
                      (unsigned long long)st);
        }
        return AXL_ERR;
    }
    *out = file;
    return AXL_OK;
}

// ===================================================================
// Map reverse-lookup
//
// The old shell exposes only GetMap(name) -> device_path. To answer "what
// fs<n> is this device path mapped as", iterate fs0..fsN and byte-compare.
// ===================================================================

/* Total device-path size in bytes, including the trailing END node. Bounded
   walk; returns 0 on a malformed chain (Length < 4). */
static size_t
dp_total_bytes(const void *dp)
{
    const uint8_t *p = (const uint8_t *)dp;
    size_t total = 0;
    for (unsigned n = 0; n < 256 && p != NULL; n++) {
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));
        if (len < 4) {
            return 0;
        }
        total += len;
        if (p[0] == 0x7f) {   /* END node — included in the total */
            break;
        }
        p += len;
    }
    return total;
}

static bool
dp_bytes_equal(const void *a, const void *b)
{
    size_t sa = dp_total_bytes(a);
    if (sa == 0 || sa != dp_total_bytes(b)) {
        return false;
    }
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < sa; i++) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

int
axl_efi1x_map_fs_name_from_dp(
    void   *device_path,
    char   *out,
    size_t  out_size
    )
{
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (se == NULL || se->GetMap == NULL || device_path == NULL
        || out_size < 3) {
        return AXL_ERR;
    }
    for (unsigned i = 0; i < AXL_EFI1X_FS_SCAN; i++) {
        /* Query name "fs<i>" (the old shell stores lowercase). */
        CHAR16   qname[8];
        unsigned k = 0;
        qname[k++] = (CHAR16)'f';
        qname[k++] = (CHAR16)'s';
        if (i >= 10) {
            qname[k++] = (CHAR16)('0' + (i / 10));
        }
        qname[k++] = (CHAR16)('0' + (i % 10));
        qname[k] = 0;

        void *dp = se->GetMap(qname);
        if (dp == NULL || !dp_bytes_equal(dp, device_path)) {
            continue;
        }
        /* Match — build lowercase "fs<i>" (the old shell's own casing) in a
           local, then copy only if the full name + NUL fits. Rejecting a
           short buffer (rather than returning a truncated name as success)
           matches the map_alias truncation contract. */
        char     name[8];
        unsigned j = 0;
        name[j++] = 'f';
        name[j++] = 's';
        if (i >= 10) {
            name[j++] = (char)('0' + (i / 10));
        }
        name[j++] = (char)('0' + (i % 10));
        name[j] = '\0';
        if ((size_t)j + 1 > out_size) {
            return AXL_ERR;
        }
        for (unsigned c = 0; c <= j; c++) {
            out[c] = name[c];
        }
        return AXL_OK;
    }
    return AXL_ERR;
}

bool
axl_efi1x_map_exists(
    const unsigned short  *name
    )
{
    if (name == NULL) {
        return false;
    }
    /* GetMap wants the BARE name, so copy off any ':' the caller appended. */
    CHAR16 bare[AXL_EFI1X_NAME_MAX];
    size_t i = 0;
    while (name[i] != 0 && name[i] != (unsigned short)':'
           && i < AXL_EFI1X_NAME_MAX - 1) {
        bare[i] = (CHAR16)name[i];
        i++;
    }
    bare[i] = 0;
    return map_device_path(bare) != NULL;
}
