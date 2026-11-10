/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-var-internal.h — the one UEFI variable walk, shared.
 *
 * INTERNAL. Not installed, not part of the public API.
 *
 * GetNextVariableName is fiddly in a way that is easy to get subtly
 * wrong twice: the name buffer is in/out, EFI_BUFFER_TOO_SMALL has to
 * grow it while PRESERVING the current name (that name is the cursor —
 * lose it and the walk restarts or stops), and the UTF-8 rendering has
 * to be sized from the UCS-2 length rather than assumed. So there is
 * exactly one implementation, in axl-var.c, and both callers use it:
 *
 *   - axl_var_enumerate()  — yields every variable
 *   - axl_nvstore_iter()   — the same walk behind a vendor-GUID predicate
 */

#ifndef AXL_VAR_INTERNAL_H
#define AXL_VAR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-sys.h>

/*
 * Called once per variable found.
 *
 * @a wname is the raw UCS-2 name, passed through because GetVariable
 * needs exactly those code units — round-tripping the UTF-8 rendering
 * back to UCS-2 would be a needless chance to corrupt a name we already
 * hold correctly. @a name is the UTF-8 rendering for API surfaces.
 *
 * Returns true to keep walking, false to stop.
 *
 * It returns a BOOL, not a status, on purpose. An earlier shape had it
 * return the caller's own int and had axl_var_walk pass that back --
 * which meant one return value carried either an AXL_* status or an
 * arbitrary caller value, indistinguishably. A callback stopping with
 * -5 was unreadable from AXL_NOT_FOUND, and both callers had to invent
 * a private workaround (a sentinel here, a flag there). A callback that
 * wants to report WHY it stopped puts that in its own ctx, where it is
 * unambiguous.
 */
typedef bool (*AxlVarWalkFn)(
    const unsigned short  *wname,   /* raw UCS-2 name (NUL-terminated) */
    const char            *name,    /* UTF-8 rendering of the same */
    const AxlGuid         *vendor,  /* vendor GUID for this variable */
    void                  *ctx
);

/*
 * Walk every UEFI variable on the machine.
 *
 * @return AXL_OK when the walk ran to completion OR the callback asked
 *     it to stop (both are successful walks -- the callback records its
 *     own reason); AXL_NO_RESOURCES on allocation failure; AXL_ERR if
 *     the firmware walk failed.
 */
int
axl_var_walk(
    AxlVarWalkFn  cb,
    void         *ctx
);

/* EFI_VARIABLE_* bits -> AXL_NV_* flags. Shared so the two surfaces
   cannot drift in how they report the same variable's attributes. */
uint32_t
axl_var_attrs_from_efi(
    uint32_t  efi_attrs
);

/* axl_var_read() with the attributes as the firmware reported them -- every
   EFI_VARIABLE_* bit, including the deprecated AUTHENTICATED_WRITE_ACCESS
   (0x10) and any bit no AXL_NV_* / AXL_VAR_* flag names. For a caller that
   must refuse a variable whose attributes are anything but an exact set: a
   mapped word drops what it does not model, so "exactly NV+BS" would accept
   NV+BS+0x10. Returns as axl_var_read(). */
int
axl_var_read_raw(
    const char     *name,
    const AxlGuid  *vendor,
    uint32_t       *efi_attrs,   /* [out] raw EFI_VARIABLE_* word, or NULL */
    void          **data,
    size_t         *size
);

/* SetVariable with a raw attribute word; @a size 0 deletes. Deliberately not
   public: axl-var.h is read-only by design, and writes outside a registered
   namespace belong to a caller that has a reason (AxlSsh's host key, whose
   GUID is fixed and whose refusals must be told apart).
   AXL_OK; AXL_NOT_FOUND for a delete of an absent variable; AXL_DENIED when
   the firmware refuses it as write-protected or a security violation;
   AXL_INVALID on a bad argument; AXL_NO_RESOURCES; AXL_ERR otherwise. */
int
axl_var_write_raw(
    const char     *name,
    const AxlGuid  *vendor,
    uint32_t        efi_attrs,
    const void     *data,
    size_t          size
);

#endif /* AXL_VAR_INTERNAL_H */
