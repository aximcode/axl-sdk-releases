/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-handle-iter.c
    Shared protocol-handle enumeration cursor — see axl-handle-iter.h.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>
#include <axl/axl-atexit.h>
#include <axl/axl-log.h>
#include "axl-handle-iter.h"

AXL_LOG_DOMAIN("handle-iter");

static void
iter_cleanup(
    void *ctx
    )
{
    AxlHandleIter *it = (AxlHandleIter *)ctx;
    if (it->handles != NULL) {
        axl_backend_free(it->handles);
        it->handles = NULL;
    }
    it->count = 0;
}

static void
iter_ensure(
    AxlHandleIter *it
    )
{
    if (it->inited) {
        return;
    }

    EFI_HANDLE *handles = NULL;
    size_t      count   = 0;
    EFI_STATUS  status  = axl_efi_call(
        axl_bs()->LocateHandleBuffer, 5,
        ByProtocol, (EFI_GUID *)it->guid, NULL,
        &count, &handles);
    if (EFI_ERROR(status) || count == 0) {
        axl_debug("no %s handles installed", it->what);
        it->inited = true;   /* successful "none present" */
        return;
    }

    it->handles = (void **)handles;
    it->count   = count;
    axl_atexit(iter_cleanup, it);
    axl_debug("%s: %zu handle(s) enumerated", it->what, it->count);
    it->inited = true;
}

AxlHandle
axl_handle_iter_next(
    AxlHandleIter *it,
    AxlHandle      prev
    )
{
    iter_ensure(it);
    if (it->count == 0) {
        return NULL;
    }
    if (prev == NULL) {
        return (AxlHandle)it->handles[0];
    }
    for (size_t i = 0; i < it->count; i++) {
        if ((AxlHandle)it->handles[i] == prev) {
            return (i + 1 < it->count)
                ? (AxlHandle)it->handles[i + 1]
                : NULL;
        }
    }
    /* prev is not one of our handles: rewind to the first. */
    return (AxlHandle)it->handles[0];
}
