/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-var.c — raw UEFI variable inspection (unscoped, read-only).
 *
 * Contract in include/axl/axl-var.h. The shared walk that both this and
 * axl-nvstore.c run on is declared in axl-var-internal.h.
 */

#include "../backend/axl-backend.h"
#include "axl-var-internal.h"
#include <axl/axl-var.h>
#include <axl/axl-nvstore.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-macros.h>

AXL_LOG_DOMAIN("var");

/* This file moves GUIDs between AxlGuid (public, standard C types) and
   EFI_GUID (internal UEFI type) by memcpy. The layout match is
   documented in <axl/axl-sys.h>; assert it here so any future drift
   surfaces at compile time, not as a 16-byte GUID-corruption bug at
   runtime. Same reasoning as src/util/axl-protocol.c. */
_Static_assert(sizeof(AxlGuid) == sizeof(EFI_GUID),
               "AxlGuid must be ABI-compatible with EFI_GUID");

// ---------------------------------------------------------------------------
// Shared attribute mapping
// ---------------------------------------------------------------------------

uint32_t
axl_var_attrs_from_efi(
    uint32_t  efi_attrs
    )
{
    uint32_t flags = 0;

    /* The portable three: meaningful on any nvstore backend. */
    if (efi_attrs & EFI_VARIABLE_NON_VOLATILE) {
        flags |= AXL_NV_PERSISTENT;
    }
    if (efi_attrs & EFI_VARIABLE_BOOTSERVICE_ACCESS) {
        flags |= AXL_NV_BOOT;
    }
    if (efi_attrs & EFI_VARIABLE_RUNTIME_ACCESS) {
        flags |= AXL_NV_RUNTIME;
    }

    /* The UEFI-only rest. Dropping these made an inspection API unable
       to distinguish an authenticated variable from an ordinary one --
       PK / KEK / db / dbx all carry TIME_BASED_AUTHENTICATED_WRITE, so
       a caller could not describe Secure Boot state, which is exactly
       what axl-var.h advertises. Reported here rather than in a second
       mapping so the two surfaces still cannot drift; nvstore callers
       testing individual AXL_NV_* bits are unaffected. */
    if (efi_attrs & EFI_VARIABLE_HARDWARE_ERROR_RECORD) {
        flags |= AXL_VAR_HW_ERROR_RECORD;
    }
    if (efi_attrs & EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) {
        flags |= AXL_VAR_TIME_AUTH_WRITE;
    }
    if (efi_attrs & EFI_VARIABLE_APPEND_WRITE) {
        flags |= AXL_VAR_APPEND_WRITE;
    }
    if (efi_attrs & EFI_VARIABLE_ENHANCED_AUTHENTICATED_ACCESS) {
        flags |= AXL_VAR_ENHANCED_AUTH;
    }
    return flags;
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------

/* Starting name capacity, in UCS-2 code units. The loop grows on
   demand, so this is a latency knob and not a limit.

   Deliberately SMALL. At 256 no real variable name ever reached the
   grow path, so the fiddliest block in this file -- the one that has
   to preserve the iteration cursor across a realloc -- ran zero times
   in CI and was covered only by hand. At 16 ordinary names like
   "SecureBoot" fit while longer ones grow, so every suite run
   exercises it. The cost is a couple of extra allocations on an
   inventory call that already does one firmware round-trip per
   variable. */
#define VAR_NAME_CHARS_INIT  16

/* UTF-8 needs at most 3 bytes per UCS-2 code unit (a BMP code point),
   plus a NUL. Sizing from the actual capacity rather than a fixed
   buffer is what keeps a long name from being silently truncated --
   which in an enumeration API would be a data bug, not a cosmetic one. */
static size_t
utf8_cap_for(
    size_t  name_chars
    )
{
    return (name_chars * 3) + 1;
}

int
axl_var_walk(
    AxlVarWalkFn  cb,
    void         *ctx
    )
{
    if (cb == NULL) {
        return AXL_ERR;
    }

    size_t          name_chars = VAR_NAME_CHARS_INIT;
    unsigned short *wname      = axl_malloc(name_chars * sizeof(unsigned short));
    char           *utf8       = axl_malloc(utf8_cap_for(name_chars));
    if (wname == NULL || utf8 == NULL) {
        axl_free(wname);
        axl_free(utf8);
        return AXL_NO_RESOURCES;
    }

    /* An empty name plus a zeroed GUID is the documented "start at the
       beginning" cursor for GetNextVariableName. */
    wname[0] = 0;
    EFI_GUID iter_guid = { 0 };

    int rc = AXL_OK;

    while (1) {
        UINTN      name_size = name_chars * sizeof(unsigned short);
        EFI_STATUS status = axl_rt()->GetNextVariableName(   /* axl-uefi-direct: runtime variable services */
            &name_size,
            wname,
            &iter_guid);

        if (status == EFI_NOT_FOUND) {
            break;                     /* walked the whole store */
        }

        if (status == EFI_BUFFER_TOO_SMALL) {
            /* name_size is now the REQUIRED size in bytes. Grow, and
               carry the current name across: it is the walk cursor, so
               dropping it would restart or stall the enumeration. */
            size_t new_chars = (name_size / sizeof(unsigned short)) + 1;

            /* name_size is firmware-controlled, and the memcpy below
               copies the OLD capacity into the new buffer. Trust it to
               grow and a driver that reports a smaller size overflows
               the heap; trust it to differ and one that reports the
               SAME size spins here forever, allocating and freeing with
               no progress. A hang is not the cheap failure it looks
               like: test-axl.sh runs every binary in one QEMU boot
               under one timeout, so one stuck walk starves the rest of
               the suite. Refuse both. */
            if (new_chars <= name_chars) {
                axl_error("firmware asked for %zu name chars, not more "
                          "than the %zu it already had", new_chars, name_chars);
                rc = AXL_ERR;
                break;
            }

            unsigned short *bigger    = axl_malloc(new_chars * sizeof(unsigned short));
            char           *bigger_u8 = axl_malloc(utf8_cap_for(new_chars));
            if (bigger == NULL || bigger_u8 == NULL) {
                axl_free(bigger);
                axl_free(bigger_u8);
                rc = AXL_NO_RESOURCES;
                break;
            }
            axl_memcpy(bigger, wname, name_chars * sizeof(unsigned short));
            axl_free(wname);
            axl_free(utf8);
            wname      = bigger;
            utf8       = bigger_u8;
            name_chars = new_chars;
            continue;                  /* retry the same step, bigger */
        }

        if (EFI_ERROR(status)) {
            rc = AXL_ERR;
            break;
        }

        axl_ucs2_to_utf8_buf(wname, utf8, utf8_cap_for(name_chars));

        AxlGuid vendor;
        axl_memcpy(&vendor, &iter_guid, sizeof(vendor));

        if (!cb(wname, utf8, &vendor, ctx)) {
            break;                     /* callback asked to stop */
        }
    }

    axl_free(wname);
    axl_free(utf8);
    return rc;
}

// ---------------------------------------------------------------------------
// Public API — enumeration
// ---------------------------------------------------------------------------

/* One accumulated record. `name` is owned here and released once the
   packed result has copied it. */
typedef struct {
    char    *name;
    AxlGuid  vendor;
    uint32_t attrs;
    size_t   size;
} VarRec;

typedef struct {
    VarRec *recs;
    size_t  count;
    size_t  cap;
    bool    oom;
} VarAccum;

/*
 * Attributes + payload SIZE without transferring the payload.
 *
 * GetVariable(Data=NULL, DataSize=0) answers both: EFI_BUFFER_TOO_SMALL
 * with DataSize set is the normal case. EFI_SUCCESS is accepted too, on
 * the reading that a zero-length variable has nothing to truncate --
 * though EDK2's VariableServiceGetVariable takes the Data==NULL branch
 * to EFI_INVALID_PARAMETER, and a zero-length SetVariable is a delete,
 * so that outcome may be unreachable in practice. Accepting it costs
 * nothing and avoids depending on which reading a given firmware took.
 * Anything else is a read failure for this variable.
 */
static bool
probe_attrs_size(
    const unsigned short *wname,
    const EFI_GUID       *vendor,
    uint32_t             *out_attrs,
    size_t               *out_size
    )
{
    UINT32     efi_attrs = 0;
    UINTN      data_size = 0;
    EFI_STATUS status = axl_rt()->GetVariable(   /* axl-uefi-direct: runtime variable services */
        (CHAR16 *)wname,
        (EFI_GUID *)vendor,
        &efi_attrs,
        &data_size,
        NULL);

    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status)) {
        return false;
    }
    *out_attrs = axl_var_attrs_from_efi(efi_attrs);
    *out_size  = (size_t)data_size;
    return true;
}

static bool
accum_cb(
    const unsigned short *wname,
    const char           *name,
    const AxlGuid        *vendor,
    void                 *ctx
    )
{
    VarAccum *a = (VarAccum *)ctx;

    if (a->count == a->cap) {
        size_t  new_cap = (a->cap == 0) ? 64 : (a->cap * 2);
        VarRec *bigger  = axl_malloc(new_cap * sizeof(VarRec));
        if (bigger == NULL) {
            a->oom = true;
            return false;              /* stop the walk */
        }
        if (a->recs != NULL) {
            axl_memcpy(bigger, a->recs, a->count * sizeof(VarRec));
            axl_free(a->recs);
        }
        a->recs = bigger;
        a->cap  = new_cap;
    }

    EFI_GUID efi_vendor;
    axl_memcpy(&efi_vendor, vendor, sizeof(efi_vendor));

    uint32_t attrs = 0;
    size_t   size  = 0;
    if (!probe_attrs_size(wname, &efi_vendor, &attrs, &size)) {
        /* A variable the walk named but we cannot describe. Skipping it
           beats failing the whole inventory over one entry -- firmware
           can and does expose names whose reads are refused. Logged
           rather than dropped in silence: "my variable is missing from
           the list" is otherwise undiagnosable from the outside. */
        axl_debug("'%s' enumerated but not readable - skipped", name);
        return true;                   /* skip this one, keep walking */
    }

    size_t  len  = axl_strlen(name);
    char   *copy = axl_malloc(len + 1);
    if (copy == NULL) {
        a->oom = true;
        return false;
    }
    axl_memcpy(copy, name, len + 1);

    a->recs[a->count].name   = copy;
    a->recs[a->count].vendor = *vendor;
    a->recs[a->count].attrs  = attrs;
    a->recs[a->count].size   = size;
    a->count++;
    return true;
}

static void
accum_free(
    VarAccum *a
    )
{
    for (size_t i = 0; i < a->count; i++) {
        axl_free(a->recs[i].name);
    }
    axl_free(a->recs);
    a->recs  = NULL;
    a->count = 0;
    a->cap   = 0;
}

int
axl_var_enumerate(
    AxlVarInfo **vars,
    size_t      *count
    )
{
    /* Clear whichever out param we were actually given, so a caller
       that frees unconditionally is safe on every reject path. */
    if (vars != NULL) {
        *vars = NULL;
    }
    if (count != NULL) {
        *count = 0;
    }
    if (vars == NULL || count == NULL) {
        return AXL_INVALID;
    }

    VarAccum acc = { NULL, 0, 0, false };
    int      rc  = axl_var_walk(accum_cb, &acc);

    if (acc.oom) {
        accum_free(&acc);
        return AXL_NO_RESOURCES;
    }
    if (rc != AXL_OK) {
        accum_free(&acc);
        return rc;
    }
    if (acc.count == 0) {
        /* Still free the accumulator: the record array is grown BEFORE
           each probe, so a machine whose every variable fails to probe
           leaves recs allocated with count 0. Returning here without
           this leaks it, and no test can see that -- under QEMU every
           probe succeeds, so this path is only reachable on firmware
           the suite never runs against. */
        accum_free(&acc);
        return AXL_OK;                 /* empty store is not an error */
    }

    /* Pack into ONE allocation: the array, then the name bytes behind
       it. That is what lets the caller release everything with a single
       axl_free() and never think about the strings. */
    size_t names_bytes = 0;
    for (size_t i = 0; i < acc.count; i++) {
        names_bytes += axl_strlen(acc.recs[i].name) + 1;
    }

    size_t array_bytes = acc.count * sizeof(AxlVarInfo);
    void  *block       = axl_malloc(array_bytes + names_bytes);
    if (block == NULL) {
        accum_free(&acc);
        return AXL_NO_RESOURCES;
    }

    AxlVarInfo *out    = (AxlVarInfo *)block;
    char       *cursor = (char *)block + array_bytes;

    for (size_t i = 0; i < acc.count; i++) {
        size_t len = axl_strlen(acc.recs[i].name);
        axl_memcpy(cursor, acc.recs[i].name, len + 1);
        out[i].name   = cursor;
        out[i].vendor = acc.recs[i].vendor;
        out[i].attrs  = acc.recs[i].attrs;
        out[i].size   = acc.recs[i].size;
        cursor += len + 1;
    }

    /* Read the count out BEFORE releasing the accumulator -- accum_free
       zeroes it. */
    size_t n = acc.count;
    accum_free(&acc);

    *vars  = out;
    *count = n;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public API — single read
// ---------------------------------------------------------------------------

int
axl_var_read(
    const char     *name,
    const AxlGuid  *vendor,
    uint32_t       *attrs,
    void          **data,
    size_t         *size
    )
{
    if (data != NULL) {
        *data = NULL;
    }
    if (size != NULL) {
        *size = 0;
    }
    if (name == NULL || name[0] == '\0' || vendor == NULL) {
        /* An empty name is rejected here rather than passed down: the
           firmware answers it with EFI_INVALID_PARAMETER, which would
           surface as AXL_ERR and read as a firmware failure instead of
           the bad argument it is. */
        return AXL_INVALID;
    }
    if (data != NULL && size == NULL) {
        return AXL_INVALID;
    }

    /* UCS-2 name buffer sized from the UTF-8 length: a UCS-2 name is
       never longer in code units than the UTF-8 form is in bytes. */
    size_t          wcap  = axl_strlen(name) + 1;
    unsigned short *wname = axl_malloc(wcap * sizeof(unsigned short));
    if (wname == NULL) {
        return AXL_NO_RESOURCES;
    }
    axl_utf8_to_ucs2_buf(name, wname, wcap);

    EFI_GUID efi_vendor;
    axl_memcpy(&efi_vendor, vendor, sizeof(efi_vendor));

    UINT32     efi_attrs = 0;
    UINTN      data_size = 0;
    EFI_STATUS status = axl_rt()->GetVariable(   /* axl-uefi-direct: runtime variable services */
        (CHAR16 *)wname,
        &efi_vendor,
        &efi_attrs,
        &data_size,
        NULL);

    if (status == EFI_NOT_FOUND) {
        axl_free(wname);
        return AXL_NOT_FOUND;
    }
    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status)) {
        axl_free(wname);
        return AXL_ERR;
    }

    /* Out params are written only on paths that return AXL_OK. Setting
       them here and failing below would hand back *data == NULL beside
       a populated *size -- a pairing a caller could act on, and the
       opposite of the clearing discipline axl_var_enumerate documents. */
    if (data == NULL) {
        if (attrs != NULL) {
            *attrs = axl_var_attrs_from_efi(efi_attrs);
        }
        if (size != NULL) {
            *size = (size_t)data_size;
        }
        axl_free(wname);
        return AXL_OK;                 /* size-only form */
    }

    /* One byte past the payload, zeroed, so a text variable can be used
       as a C string without a copy -- same guarantee as
       axl_nvstore_get_alloc. Not counted in *size. */
    void *buf = axl_malloc((size_t)data_size + 1);
    if (buf == NULL) {
        axl_free(wname);
        return AXL_NO_RESOURCES;
    }
    ((uint8_t *)buf)[data_size] = 0;

    size_t final_size = (size_t)data_size;

    if (data_size > 0) {
        UINTN read_size = data_size;
        status = axl_rt()->GetVariable(   /* axl-uefi-direct: runtime variable services */
            (CHAR16 *)wname,
            &efi_vendor,
            &efi_attrs,
            &read_size,
            buf);
        if (EFI_ERROR(status)) {
            axl_free(buf);
            axl_free(wname);
            return AXL_ERR;
        }
        final_size = (size_t)read_size;
    }

    axl_free(wname);

    /* Every failure exit above is behind us, so the out params commit
       together with the payload. */
    if (attrs != NULL) {
        *attrs = axl_var_attrs_from_efi(efi_attrs);
    }
    *size = final_size;
    *data = buf;
    return AXL_OK;
}
