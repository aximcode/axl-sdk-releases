/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hii-internal.h
    Private types for the AxlHii module.

    Not shipped to SDK consumers. src/hii/axl-hii.c shares these
    definitions; everything else goes through the public header
    <axl/axl-hii.h>. The unit test (test/unit/axl-test-hii.c) also
    includes this to drive `_axl_hii_parse_form_package` against
    synthetic IFR byte streams — the only way to pin the malformed-opcode
    bounds contract, since live OVMF/AAVMF emit only well-formed IFR.
**/

#ifndef AXL_HII_INTERNAL_H
#define AXL_HII_INTERNAL_H

#include <uefi/axl-uefi.h>   /* EFI_GUID, EFI_HII_HANDLE, IFR structs (extra) */
#include <axl/axl-hii.h>

// ===================================================================
// Model sizing (mirrors the reference: bounded, file-scope arrays).
// ===================================================================

#define HII_MAX_FORM_SETS         32
#define HII_MAX_QUESTIONS_PER_FS  256
#define HII_MAX_VARSTORES         16
#define HII_MAX_VARSTORE_NAME     64   /* CHAR16 units */

// ===================================================================
// Internal model
// ===================================================================

/* A variable store an IFR question's value lives in. `name` is the
   CHAR16 EFI variable name; `is_efi_var` distinguishes an efivarstore
   (gRT variable) from a plain block varstore. */
typedef struct {
    uint16_t  var_store_id;
    EFI_GUID  guid;
    uint16_t  name[HII_MAX_VARSTORE_NAME];   /* CHAR16, NUL-terminated */
    uint16_t  size;
    bool      is_efi_var;
    uint32_t  attributes;
} HiiVarStore;

/* The public projected view plus the storage coordinates value I/O
   needs (kept private so the public struct stays storage-agnostic). */
typedef struct {
    AxlHiiQuestion  view;
    uint16_t        var_store_id;
    uint16_t        var_offset;
} HiiQuestion;

typedef struct {
    EFI_HII_HANDLE  hii_handle;
    AxlGuid         formset_guid;
    char            title[128];
    char            help[128];
    size_t          question_count;
    HiiQuestion    *questions;                    /* calloc'd array */
    size_t          varstore_count;
    HiiVarStore     varstores[HII_MAX_VARSTORES];
    uint8_t        *device_path;                  /* for ConfigRouting */
    size_t          device_path_len;
} HiiFormSet;

// ===================================================================
// Test seam
// ===================================================================

/* Parse one form package (IFR byte stream) into @p fs. Returns true if a
   titled form set was found. @p handle resolves string ids (NULL in the
   unit test, where there is no HII String protocol, so strings come back
   empty but the varstore/question model is still populated). The caller
   owns @p fs->questions and must axl_free it. Exposed for the unit test
   to drive synthetic / malformed IFR; production callers go through the
   public axl_hii_* API. */
bool
_axl_hii_parse_form_package(
    EFI_HII_HANDLE  handle,
    const uint8_t  *data,
    size_t          data_len,
    HiiFormSet     *fs
);

#endif /* AXL_HII_INTERNAL_H */
