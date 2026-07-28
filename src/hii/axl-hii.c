/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hii.c
    UEFI HII setup-form reader.

    Locates the HII database/string/config-routing protocols, exports
    every form package, walks the IFR opcode stream into a cached typed
    model (form sets -> questions + variable stores), and projects each
    question as an AxlHiiQuestion. Value read/write resolves a question's
    variable store and goes through GetVariable/SetVariable, with an HII
    config-routing/config-access fallback for driver-private block
    stores.

    The IFR walk and value I/O are a port of the SoftBMC HiiParse
    reference (de-EDK2'd onto axl primitives): the IFR structs are cast
    directly onto the raw on-disk byte stream, so the layouts in
    axl-uefi-extra.h are byte-packed and load-bearing.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* HII protocols + IFR structs (extra) */
#include <axl/axl-hii.h>
#include <axl/axl-mem.h>     /* axl_malloc / axl_calloc / axl_free */
#include <axl/axl-str.h>     /* axl_memcpy / axl_memset */
#include <axl/axl-atexit.h>
#include "axl-hii-internal.h"   /* model types + the parse test seam */

// ===================================================================
// Module state (parsed lazily, once, then cached)
// ===================================================================

static EFI_HII_DATABASE_PROTOCOL        *g_hii_db;
static EFI_HII_STRING_PROTOCOL          *g_hii_string;
static EFI_HII_CONFIG_ROUTING_PROTOCOL  *g_config_routing;

static HiiFormSet  g_formsets[HII_MAX_FORM_SETS];
static size_t      g_formset_count;
static bool        g_parsed;

// ===================================================================
// String resolution
// ===================================================================

/* Resolve an HII string id to ASCII into @p buf (NUL-terminated,
   truncated to @p buf_size - 1). Two-pass GetString; CHAR16 masked to
   7-bit ASCII. Leaves @p buf empty on any failure. */
static void
resolve_string(
    EFI_HII_HANDLE  handle,
    EFI_STRING_ID   string_id,
    char           *buf,
    size_t          buf_size
    )
{
    buf[0] = '\0';
    if (string_id == 0 || g_hii_string == NULL || buf_size == 0) {
        return;
    }

    UINTN size = 0;
    EFI_STATUS status = g_hii_string->GetString(
        g_hii_string, "en-US", handle, string_id, NULL, &size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        return;
    }

    uint16_t *str16 = axl_malloc(size);
    if (str16 == NULL) {
        return;
    }

    status = g_hii_string->GetString(
        g_hii_string, "en-US", handle, string_id,
        (EFI_STRING)str16, &size, NULL);
    if (!EFI_ERROR(status)) {
        size_t i;
        for (i = 0; i < buf_size - 1 && str16[i] != 0; i++) {
            buf[i] = (char)(str16[i] & 0x7F);
        }
        buf[i] = '\0';
    }

    axl_free(str16);
}

// ===================================================================
// IFR helpers
// ===================================================================

/* Value width (bytes) encoded in a ONE_OF/NUMERIC flags byte. */
static uint8_t
numeric_width(uint8_t flags)
{
    switch (flags & EFI_IFR_NUMERIC_SIZE) {
    case EFI_IFR_NUMERIC_SIZE_1: return 1;
    case EFI_IFR_NUMERIC_SIZE_2: return 2;
    case EFI_IFR_NUMERIC_SIZE_4: return 4;
    case EFI_IFR_NUMERIC_SIZE_8: return 8;
    default:                     return 1;
    }
}

/* Fold the IFR question flags into the projected read-only / reset
   fields. A callback-driven question is treated as read-only: its
   value is not safely writable as a plain variable. */
static void
apply_question_flags(AxlHiiQuestion *view, uint8_t flags)
{
    view->read_only =
        (flags & EFI_IFR_FLAG_READ_ONLY) != 0 ||
        (flags & EFI_IFR_FLAG_CALLBACK) != 0;
    view->reset_required = (flags & EFI_IFR_FLAG_RESET_REQUIRED) != 0;
}

/* Common prologue for the four question opcodes: stamp id/varstore/
   offset/flags/prompt/help onto a fresh HiiQuestion. */
static void
init_question(
    HiiFormSet                    *fs,
    HiiQuestion                   *q,
    const EFI_IFR_QUESTION_HEADER *qh,
    AxlHiiQuestionType             type
    )
{
    axl_memset(q, 0, sizeof(*q));
    q->view.id   = qh->QuestionId;
    q->view.type = type;
    q->var_store_id = qh->VarStoreId;
    q->var_offset   = qh->VarStoreInfo.VarOffset;
    apply_question_flags(&q->view, qh->Flags);
    resolve_string(fs->hii_handle, qh->Header.Prompt,
                   q->view.prompt, sizeof(q->view.prompt));
    resolve_string(fs->hii_handle, qh->Header.Help,
                   q->view.help, sizeof(q->view.help));
}

// ===================================================================
// IFR opcode parser
// ===================================================================

/* Parse one form package (IFR byte stream) into @p fs. Returns true if
   a titled form set was found. Declared in axl-hii-internal.h as the
   unit-test seam; not static so the test can drive synthetic IFR. */
bool
_axl_hii_parse_form_package(
    EFI_HII_HANDLE  handle,
    const uint8_t  *data,
    size_t          data_len,
    HiiFormSet     *fs
    )
{
    axl_memset(fs, 0, sizeof(*fs));
    fs->hii_handle = handle;

    fs->questions = axl_calloc(HII_MAX_QUESTIONS_PER_FS, sizeof(HiiQuestion));
    if (fs->questions == NULL) {
        return false;
    }

    const uint8_t *ptr = data;
    const uint8_t *end = data + data_len;
    size_t scope_depth = 0;
    bool in_form_set = false;
    HiiQuestion *current = NULL;

    while (ptr + sizeof(EFI_IFR_OP_HEADER) <= end) {
        const EFI_IFR_OP_HEADER *op = (const EFI_IFR_OP_HEADER *)ptr;

        if (op->Length < sizeof(EFI_IFR_OP_HEADER) || ptr + op->Length > end) {
            break;
        }

        switch (op->OpCode) {
        case EFI_IFR_FORM_SET_OP: {
            if (ptr + sizeof(EFI_IFR_FORM_SET) > end) {
                break;
            }
            const EFI_IFR_FORM_SET *o = (const EFI_IFR_FORM_SET *)ptr;
            axl_memcpy(&fs->formset_guid, &o->Guid, sizeof(fs->formset_guid));
            resolve_string(handle, o->FormSetTitle, fs->title, sizeof(fs->title));
            resolve_string(handle, o->Help, fs->help, sizeof(fs->help));
            in_form_set = true;
            break;
        }

        case EFI_IFR_VARSTORE_OP: {
            if (!in_form_set || fs->varstore_count >= HII_MAX_VARSTORES) {
                break;
            }
            if (op->Length < sizeof(EFI_IFR_VARSTORE) ||
                ptr + sizeof(EFI_IFR_VARSTORE) > end) {
                break;
            }
            const EFI_IFR_VARSTORE *o = (const EFI_IFR_VARSTORE *)ptr;
            HiiVarStore *vs = &fs->varstores[fs->varstore_count];

            axl_memcpy(&vs->guid, &o->Guid, sizeof(EFI_GUID));
            vs->var_store_id = o->VarStoreId;
            vs->size = o->Size;
            vs->is_efi_var = false;

            /* Name is trailing ASCII; widen to CHAR16. */
            const uint8_t *name = ptr + sizeof(EFI_IFR_VARSTORE);
            size_t name_len = op->Length - sizeof(EFI_IFR_VARSTORE);
            for (size_t i = 0; i < name_len && i < HII_MAX_VARSTORE_NAME - 1
                               && name[i] != 0; i++) {
                vs->name[i] = name[i];
            }

            fs->varstore_count++;
            break;
        }

        case EFI_IFR_VARSTORE_EFI_OP: {
            if (!in_form_set || fs->varstore_count >= HII_MAX_VARSTORES) {
                break;
            }
            if (op->Length < sizeof(EFI_IFR_VARSTORE_EFI) ||
                ptr + sizeof(EFI_IFR_VARSTORE_EFI) > end) {
                break;
            }
            const EFI_IFR_VARSTORE_EFI *o = (const EFI_IFR_VARSTORE_EFI *)ptr;
            HiiVarStore *vs = &fs->varstores[fs->varstore_count];

            axl_memcpy(&vs->guid, &o->Guid, sizeof(EFI_GUID));
            vs->var_store_id = o->VarStoreId;
            vs->size = o->Size;
            vs->is_efi_var = true;
            vs->attributes = o->Attributes;

            const uint8_t *name = ptr + sizeof(EFI_IFR_VARSTORE_EFI);
            size_t name_len = op->Length - sizeof(EFI_IFR_VARSTORE_EFI);
            for (size_t i = 0; i < name_len && i < HII_MAX_VARSTORE_NAME - 1
                               && name[i] != 0; i++) {
                vs->name[i] = name[i];
            }

            fs->varstore_count++;
            break;
        }

        case EFI_IFR_ONE_OF_OP: {
            if (!in_form_set || fs->question_count >= HII_MAX_QUESTIONS_PER_FS) {
                break;
            }
            if (ptr + sizeof(EFI_IFR_ONE_OF) > end) {
                break;
            }
            const EFI_IFR_ONE_OF *o = (const EFI_IFR_ONE_OF *)ptr;
            HiiQuestion *q = &fs->questions[fs->question_count];

            init_question(fs, q, &o->Question, AXL_HII_ONE_OF);
            q->view.width = numeric_width(o->Flags);

            current = q;
            fs->question_count++;
            break;
        }

        case EFI_IFR_CHECKBOX_OP: {
            if (!in_form_set || fs->question_count >= HII_MAX_QUESTIONS_PER_FS) {
                break;
            }
            if (ptr + sizeof(EFI_IFR_CHECKBOX) > end) {
                break;
            }
            const EFI_IFR_CHECKBOX *o = (const EFI_IFR_CHECKBOX *)ptr;
            HiiQuestion *q = &fs->questions[fs->question_count];

            init_question(fs, q, &o->Question, AXL_HII_CHECKBOX);
            q->view.width = 1;   /* BOOLEAN */

            current = q;
            fs->question_count++;
            break;
        }

        case EFI_IFR_NUMERIC_OP: {
            if (!in_form_set || fs->question_count >= HII_MAX_QUESTIONS_PER_FS) {
                break;
            }
            if (ptr + sizeof(EFI_IFR_NUMERIC) > end) {
                break;
            }
            const EFI_IFR_NUMERIC *o = (const EFI_IFR_NUMERIC *)ptr;
            HiiQuestion *q = &fs->questions[fs->question_count];

            init_question(fs, q, &o->Question, AXL_HII_NUMERIC);
            q->view.width = numeric_width(o->Flags);

            switch (q->view.width) {
            case 1:
                q->view.u.numeric.min  = o->data.u8.MinValue;
                q->view.u.numeric.max  = o->data.u8.MaxValue;
                q->view.u.numeric.step = o->data.u8.Step;
                break;
            case 2:
                q->view.u.numeric.min  = o->data.u16.MinValue;
                q->view.u.numeric.max  = o->data.u16.MaxValue;
                q->view.u.numeric.step = o->data.u16.Step;
                break;
            case 4:
                q->view.u.numeric.min  = o->data.u32.MinValue;
                q->view.u.numeric.max  = o->data.u32.MaxValue;
                q->view.u.numeric.step = o->data.u32.Step;
                break;
            case 8:
                q->view.u.numeric.min  = o->data.u64.MinValue;
                q->view.u.numeric.max  = o->data.u64.MaxValue;
                q->view.u.numeric.step = o->data.u64.Step;
                break;
            default:
                break;
            }

            current = q;
            fs->question_count++;
            break;
        }

        case EFI_IFR_STRING_OP: {
            if (!in_form_set || fs->question_count >= HII_MAX_QUESTIONS_PER_FS) {
                break;
            }
            if (ptr + sizeof(EFI_IFR_STRING) > end) {
                break;
            }
            const EFI_IFR_STRING *o = (const EFI_IFR_STRING *)ptr;
            HiiQuestion *q = &fs->questions[fs->question_count];

            init_question(fs, q, &o->Question, AXL_HII_STRING);
            q->view.width = 0;   /* string — width not applicable */
            q->view.u.string.min_size = o->MinSize;
            q->view.u.string.max_size = o->MaxSize;

            current = q;
            fs->question_count++;
            break;
        }

        case EFI_IFR_ONE_OF_OPTION_OP: {
            if (current == NULL || current->view.type != AXL_HII_ONE_OF) {
                break;
            }
            if (current->view.u.one_of.option_count >= 16) {
                break;
            }
            if (ptr + sizeof(EFI_IFR_ONE_OF_OPTION) > end) {
                break;
            }
            const EFI_IFR_ONE_OF_OPTION *o = (const EFI_IFR_ONE_OF_OPTION *)ptr;
            AxlHiiOption *opt =
                &current->view.u.one_of.options[current->view.u.one_of.option_count];

            switch (o->Type) {
            case EFI_IFR_TYPE_NUM_SIZE_8:  opt->value = o->Value.u8;  break;
            case EFI_IFR_TYPE_NUM_SIZE_16: opt->value = o->Value.u16; break;
            case EFI_IFR_TYPE_NUM_SIZE_32: opt->value = o->Value.u32; break;
            case EFI_IFR_TYPE_NUM_SIZE_64: opt->value = o->Value.u64; break;
            case EFI_IFR_TYPE_BOOLEAN:     opt->value = o->Value.b;   break;
            default:                       opt->value = o->Value.u64; break;
            }

            resolve_string(handle, o->Option, opt->label, sizeof(opt->label));
            current->view.u.one_of.option_count++;
            break;
        }

        case EFI_IFR_END_OP:
            if (scope_depth > 0) {
                scope_depth--;
            }
            /* When the question's scope closes, stop attaching options. */
            if (current != NULL && scope_depth == 0) {
                current = NULL;
            }
            break;

        default:
            break;
        }

        if (op->Scope) {
            scope_depth++;
        }

        ptr += op->Length;
    }

    return in_form_set && fs->title[0] != '\0';
}

// ===================================================================
// Package-list export + form-set discovery
// ===================================================================

/* Scan a package list's device-path package (type 0x08). Returns an
   axl_malloc'd copy (caller frees) and sets @p out_len, or NULL. */
static uint8_t *
find_device_path(
    const uint8_t *pkg_ptr,
    const uint8_t *pkg_end,
    size_t        *out_len
    )
{
    *out_len = 0;
    const uint8_t *scan = pkg_ptr;
    while (scan + sizeof(EFI_HII_PACKAGE_HEADER) <= pkg_end) {
        const EFI_HII_PACKAGE_HEADER *hdr = (const EFI_HII_PACKAGE_HEADER *)scan;
        if (hdr->Length < sizeof(EFI_HII_PACKAGE_HEADER) ||
            scan + hdr->Length > pkg_end) {
            break;
        }
        if (hdr->Type == EFI_HII_PACKAGE_END) {
            break;
        }
        if (hdr->Type == EFI_HII_PACKAGE_DEVICE_PATH) {
            size_t len = hdr->Length - sizeof(EFI_HII_PACKAGE_HEADER);
            if (len > 0) {
                uint8_t *copy = axl_malloc(len);
                if (copy != NULL) {
                    axl_memcpy(copy, scan + sizeof(EFI_HII_PACKAGE_HEADER), len);
                    *out_len = len;
                    return copy;
                }
            }
        }
        scan += hdr->Length;
    }
    return NULL;
}

/* Export one HII handle's package list and parse every form package in
   it into the model, attaching the handle's device path. */
static void
parse_handle(EFI_HII_HANDLE handle)
{
    UINTN pkg_size = 0;
    EFI_STATUS status = g_hii_db->ExportPackageLists(
        g_hii_db, handle, &pkg_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || pkg_size == 0) {
        return;
    }

    EFI_HII_PACKAGE_LIST_HEADER *pkg_list = axl_malloc(pkg_size);
    if (pkg_list == NULL) {
        return;
    }

    status = g_hii_db->ExportPackageLists(g_hii_db, handle, &pkg_size, pkg_list);
    if (EFI_ERROR(status)) {
        axl_free(pkg_list);
        return;
    }

    /* PackageLength is firmware-supplied; never trust it past the buffer
       the export actually filled (pkg_size). */
    size_t list_len = pkg_list->PackageLength;
    if (list_len > pkg_size) {
        list_len = pkg_size;
    }
    const uint8_t *pkg_ptr =
        (const uint8_t *)pkg_list + sizeof(EFI_HII_PACKAGE_LIST_HEADER);
    const uint8_t *pkg_end = (const uint8_t *)pkg_list + list_len;

    size_t dev_path_len = 0;
    uint8_t *dev_path = find_device_path(pkg_ptr, pkg_end, &dev_path_len);

    while (pkg_ptr + sizeof(EFI_HII_PACKAGE_HEADER) <= pkg_end) {
        const EFI_HII_PACKAGE_HEADER *hdr = (const EFI_HII_PACKAGE_HEADER *)pkg_ptr;
        uint32_t pkg_len = hdr->Length;

        if (pkg_len < sizeof(EFI_HII_PACKAGE_HEADER) || pkg_ptr + pkg_len > pkg_end) {
            break;
        }
        if (hdr->Type == EFI_HII_PACKAGE_END) {
            break;
        }

        if (hdr->Type == EFI_HII_PACKAGE_FORMS &&
            g_formset_count < HII_MAX_FORM_SETS) {
            const uint8_t *form_data = pkg_ptr + sizeof(EFI_HII_PACKAGE_HEADER);
            size_t form_len = pkg_len - sizeof(EFI_HII_PACKAGE_HEADER);

            HiiFormSet *fs = &g_formsets[g_formset_count];
            if (_axl_hii_parse_form_package(handle, form_data, form_len, fs)) {
                if (dev_path != NULL) {
                    fs->device_path = axl_malloc(dev_path_len);
                    if (fs->device_path != NULL) {
                        axl_memcpy(fs->device_path, dev_path, dev_path_len);
                        fs->device_path_len = dev_path_len;
                    }
                }
                g_formset_count++;
            } else if (fs->questions != NULL) {
                /* Reject: reclaim the question array so the slot is reusable. */
                axl_free(fs->questions);
                fs->questions = NULL;
            }
        }

        pkg_ptr += pkg_len;
    }

    if (dev_path != NULL) {
        axl_free(dev_path);
    }
    axl_free(pkg_list);
}

static void
hii_cleanup(void *unused)
{
    (void)unused;
    for (size_t i = 0; i < g_formset_count; i++) {
        if (g_formsets[i].questions != NULL) {
            axl_free(g_formsets[i].questions);
            g_formsets[i].questions = NULL;
        }
        if (g_formsets[i].device_path != NULL) {
            axl_free(g_formsets[i].device_path);
            g_formsets[i].device_path = NULL;
        }
    }
    g_formset_count = 0;
}

/* Locate the HII protocols and parse every form set. Idempotent: runs
   the full enumeration exactly once, on the first public call. */
static void
ensure_parsed(void)
{
    if (g_parsed) {
        return;
    }
    g_parsed = true;

    EFI_STATUS status = axl_efi_call(
        axl_bs()->LocateProtocol, 3,
        &EFI_HII_DATABASE_PROTOCOL_GUID, NULL, (void **)&g_hii_db);
    if (EFI_ERROR(status) || g_hii_db == NULL) {
        g_hii_db = NULL;
        return;
    }

    /* String + config-routing are optional: without strings we still
       enumerate (no labels); without routing we lose block-store I/O. */
    status = axl_efi_call(
        axl_bs()->LocateProtocol, 3,
        &EFI_HII_STRING_PROTOCOL_GUID, NULL, (void **)&g_hii_string);
    if (EFI_ERROR(status)) {
        g_hii_string = NULL;
    }
    status = axl_efi_call(
        axl_bs()->LocateProtocol, 3,
        &EFI_HII_CONFIG_ROUTING_PROTOCOL_GUID, NULL, (void **)&g_config_routing);
    if (EFI_ERROR(status)) {
        g_config_routing = NULL;
    }

    /* Two-pass: enumerate the handles that carry form packages. */
    UINTN handle_bytes = 0;
    status = g_hii_db->ListPackageLists(
        g_hii_db, EFI_HII_PACKAGE_FORMS, NULL, &handle_bytes, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || handle_bytes == 0) {
        return;
    }

    size_t handle_count = handle_bytes / sizeof(EFI_HII_HANDLE);
    EFI_HII_HANDLE *handles = axl_malloc(handle_bytes);
    if (handles == NULL) {
        return;
    }

    status = g_hii_db->ListPackageLists(
        g_hii_db, EFI_HII_PACKAGE_FORMS, NULL, &handle_bytes, handles);
    if (EFI_ERROR(status)) {
        axl_free(handles);
        return;
    }

    for (size_t i = 0; i < handle_count && g_formset_count < HII_MAX_FORM_SETS; i++) {
        parse_handle(handles[i]);
    }

    axl_free(handles);
    axl_atexit(hii_cleanup, NULL);
}

// ===================================================================
// Internal accessors
// ===================================================================

static HiiFormSet *
formset_at(size_t index)
{
    ensure_parsed();
    if (index >= g_formset_count) {
        return NULL;
    }
    return &g_formsets[index];
}

static HiiQuestion *
question_at(size_t formset_index, size_t question_index)
{
    HiiFormSet *fs = formset_at(formset_index);
    if (fs == NULL || question_index >= fs->question_count) {
        return NULL;
    }
    return &fs->questions[question_index];
}

// ===================================================================
// Public API — enumeration
// ===================================================================

bool
axl_hii_available(void)
{
    ensure_parsed();
    return g_formset_count > 0;
}

size_t
axl_hii_formset_count(void)
{
    ensure_parsed();
    return g_formset_count;
}

/* Copy @p src into @p dst (NUL-terminated, truncated), skipping a NULL
   buffer or zero capacity. */
static void
copy_field(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    size_t i;
    for (i = 0; i < cap - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int
axl_hii_formset_get(
    size_t   index,
    char    *title,
    size_t   title_cap,
    char    *help,
    size_t   help_cap,
    AxlGuid *guid,
    size_t  *question_count
    )
{
    HiiFormSet *fs = formset_at(index);
    if (fs == NULL) {
        return AXL_ERR;
    }
    copy_field(title, title_cap, fs->title);
    copy_field(help, help_cap, fs->help);
    if (guid != NULL) {
        *guid = fs->formset_guid;
    }
    if (question_count != NULL) {
        *question_count = fs->question_count;
    }
    return AXL_OK;
}

int
axl_hii_question_get(
    size_t          formset_index,
    size_t          question_index,
    AxlHiiQuestion *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    HiiQuestion *q = question_at(formset_index, question_index);
    if (q == NULL) {
        return AXL_ERR;
    }
    *out = q->view;
    return AXL_OK;
}

// ===================================================================
// Value I/O — variable store resolution
// ===================================================================

static HiiVarStore *
find_varstore(HiiFormSet *fs, uint16_t var_store_id)
{
    for (size_t i = 0; i < fs->varstore_count; i++) {
        if (fs->varstores[i].var_store_id == var_store_id) {
            return &fs->varstores[i];
        }
    }
    return NULL;
}

/* Free a buffer the firmware allocated (ConfigRouting/ConfigAccess
   output strings) — these come from the UEFI pool, NOT axl_malloc, so
   they must go back via FreePool. */
static void
free_pool(void *p)
{
    if (p != NULL) {
        axl_efi_call(axl_bs()->FreePool, 1, p);  /* axl-pool-direct: ConfigRouting/ConfigAccess output string */
    }
}

/* Locate the ConfigAccess protocol on the driver that owns this form
   set's HII package list. */
static EFI_HII_CONFIG_ACCESS_PROTOCOL *
find_config_access(HiiFormSet *fs)
{
    if (g_hii_db == NULL || fs->hii_handle == NULL) {
        return NULL;
    }
    EFI_HANDLE driver = NULL;
    EFI_STATUS status =
        g_hii_db->GetPackageListHandle(g_hii_db, fs->hii_handle, &driver);
    if (EFI_ERROR(status) || driver == NULL) {
        return NULL;
    }
    EFI_HII_CONFIG_ACCESS_PROTOCOL *ca = NULL;
    status = axl_efi_call(
        axl_bs()->HandleProtocol, 3,
        driver, &EFI_HII_CONFIG_ACCESS_PROTOCOL_GUID, (void **)&ca);
    if (EFI_ERROR(status)) {
        return NULL;
    }
    return ca;
}

// ===================================================================
// Value I/O — ConfigRouting request building (block varstores)
// ===================================================================

static const char HII_HEX[] = "0123456789abcdef";

/* Length of a NUL-terminated CHAR16 string, capped at @p cap. */
static size_t
ucs16_len(const uint16_t *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n] != 0) {
        n++;
    }
    return n;
}

/* Widen an ASCII literal into the CHAR16 buffer. Returns the new pos. */
static size_t
append_ascii(uint16_t *b, size_t pos, const char *s)
{
    while (*s != '\0') {
        b[pos++] = (uint8_t)*s++;
    }
    return pos;
}

/* Append raw bytes as lowercase hex pairs. */
static size_t
append_hex(uint16_t *b, size_t pos, const uint8_t *d, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        b[pos++] = (uint8_t)HII_HEX[d[i] >> 4];
        b[pos++] = (uint8_t)HII_HEX[d[i] & 0x0F];
    }
    return pos;
}

/* Append a UINT16 as 4 hex digits (big-endian nibble order). */
static size_t
append_hex16(uint16_t *b, size_t pos, uint16_t v)
{
    b[pos++] = (uint8_t)HII_HEX[(v >> 12) & 0x0F];
    b[pos++] = (uint8_t)HII_HEX[(v >> 8) & 0x0F];
    b[pos++] = (uint8_t)HII_HEX[(v >> 4) & 0x0F];
    b[pos++] = (uint8_t)HII_HEX[v & 0x0F];
    return pos;
}

/* Append a CHAR16 name as 4 hex digits per character (EDK2 NAME=). */
static size_t
append_name(uint16_t *b, size_t pos, const uint16_t *name, size_t name_len)
{
    for (size_t i = 0; i < name_len; i++) {
        pos = append_hex16(b, pos, name[i]);
    }
    return pos;
}

/* Build a ConfigRouting request CHAR16 string for one value:
   GUID=..&NAME=..&PATH=..&OFFSET=..&WIDTH=.. Caller frees with axl_free.
   Returns NULL if the form set has no device path / the varstore is
   anonymous. */
static uint16_t *
build_config_request(
    HiiFormSet  *fs,
    HiiVarStore *vs,
    uint16_t     offset,
    uint16_t     width    /* bytes; > 255 for a STRING field (max_size * 2) */
    )
{
    if (fs->device_path == NULL || fs->device_path_len == 0 || vs->name[0] == 0) {
        return NULL;
    }

    size_t name_len = ucs16_len(vs->name, HII_MAX_VARSTORE_NAME);

    /* "GUID="(5) + 32 + "&NAME="(6) + name_len*4 + "&PATH="(6) +
       dp*2 + "&OFFSET="(8) + 4 + "&WIDTH="(7) + 4 + NUL(1) */
    size_t buf_chars = 5 + 32 + 6 + name_len * 4 + 6 +
                       fs->device_path_len * 2 + 8 + 4 + 7 + 4 + 1;
    uint16_t *buf = axl_malloc(buf_chars * sizeof(uint16_t));
    if (buf == NULL) {
        return NULL;
    }

    size_t pos = 0;
    pos = append_ascii(buf, pos, "GUID=");
    pos = append_hex(buf, pos, (const uint8_t *)&vs->guid, sizeof(EFI_GUID));
    pos = append_ascii(buf, pos, "&NAME=");
    pos = append_name(buf, pos, vs->name, name_len);
    pos = append_ascii(buf, pos, "&PATH=");
    pos = append_hex(buf, pos, fs->device_path, fs->device_path_len);
    pos = append_ascii(buf, pos, "&OFFSET=");
    pos = append_hex16(buf, pos, offset);
    pos = append_ascii(buf, pos, "&WIDTH=");
    pos = append_hex16(buf, pos, width);
    buf[pos] = 0;
    return buf;
}

/* Decode a ConfigResp string into a block and extract the value at
   @p offset using ConfigRouting->ConfigToBlock. */
static EFI_STATUS
extract_value_from_resp(
    uint16_t *resp,
    uint16_t  offset,
    uint8_t   width,
    uint64_t *value
    )
{
    if (g_config_routing == NULL) {
        return EFI_UNSUPPORTED;
    }
    UINTN block_size = (UINTN)offset + width;
    uint8_t *block = axl_calloc(1, block_size);
    if (block == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    uint16_t *progress = NULL;
    EFI_STATUS status = g_config_routing->ConfigToBlock(
        g_config_routing, (EFI_STRING)resp, block, &block_size,
        (EFI_STRING *)&progress);
    if (!EFI_ERROR(status)) {
        uint64_t v = 0;
        axl_memcpy(&v, block + offset, width);
        *value = v;
    }
    axl_free(block);
    return status;
}

/* Read a block-varstore value via ConfigAccess->ExtractConfig (falling
   back to ConfigRouting->ExtractConfig), then decode it. */
static EFI_STATUS
read_via_config_access(
    HiiFormSet  *fs,
    HiiVarStore *vs,
    uint16_t     offset,
    uint8_t      width,
    uint64_t    *value
    )
{
    if (g_config_routing == NULL) {
        return EFI_UNSUPPORTED;
    }
    uint16_t *request = build_config_request(fs, vs, offset, width);
    if (request == NULL) {
        return EFI_UNSUPPORTED;
    }

    uint16_t *progress = NULL;
    uint16_t *results = NULL;
    EFI_HII_CONFIG_ACCESS_PROTOCOL *ca = find_config_access(fs);
    EFI_STATUS status;
    if (ca != NULL) {
        status = ca->ExtractConfig(ca, (EFI_STRING)request,
                                   (EFI_STRING *)&progress,
                                   (EFI_STRING *)&results);
    } else {
        status = g_config_routing->ExtractConfig(
            g_config_routing, (EFI_STRING)request,
            (EFI_STRING *)&progress, (EFI_STRING *)&results);
    }

    if (!EFI_ERROR(status) && results != NULL) {
        status = extract_value_from_resp(results, offset, width, value);
    }

    free_pool(results);   /* firmware-allocated */
    axl_free(request);
    return status;
}

/* Write a block-varstore value: encode the block with
   ConfigRouting->BlockToConfig, then route it via
   ConfigAccess->RouteConfig. */
static EFI_STATUS
write_via_config_access(
    HiiFormSet  *fs,
    HiiVarStore *vs,
    uint16_t     offset,
    uint8_t      width,
    uint64_t     value
    )
{
    EFI_HII_CONFIG_ACCESS_PROTOCOL *ca = find_config_access(fs);
    if (ca == NULL || g_config_routing == NULL) {
        return EFI_UNSUPPORTED;
    }
    uint16_t *request = build_config_request(fs, vs, offset, width);
    if (request == NULL) {
        return EFI_UNSUPPORTED;
    }

    UINTN block_size = (UINTN)offset + width;
    uint8_t *block = axl_calloc(1, block_size);
    if (block == NULL) {
        axl_free(request);
        return EFI_OUT_OF_RESOURCES;
    }
    axl_memcpy(block + offset, &value, width);

    uint16_t *config_resp = NULL;
    uint16_t *progress = NULL;
    EFI_STATUS status = g_config_routing->BlockToConfig(
        g_config_routing, (EFI_STRING)request, block, block_size,
        (EFI_STRING *)&config_resp, (EFI_STRING *)&progress);
    axl_free(block);
    axl_free(request);

    if (EFI_ERROR(status) || config_resp == NULL) {
        free_pool(config_resp);
        return EFI_ERROR(status) ? status : EFI_UNSUPPORTED;
    }

    progress = NULL;
    status = ca->RouteConfig(ca, (EFI_STRING)config_resp,
                             (EFI_STRING *)&progress);
    free_pool(config_resp);
    return status;
}

// ===================================================================
// Value I/O — read/write core
// ===================================================================

static EFI_STATUS
read_value(HiiFormSet *fs, HiiQuestion *q, uint64_t *value)
{
    *value = 0;
    HiiVarStore *vs = find_varstore(fs, q->var_store_id);
    if (vs == NULL) {
        return EFI_NOT_FOUND;
    }
    if (vs->name[0] == 0) {
        return EFI_UNSUPPORTED;
    }
    uint8_t width = q->view.width;

    /* gRT->GetVariable first (efivarstore and some block varstores). */
    UINTN var_size = 0;
    EFI_STATUS status = axl_rt()->GetVariable(
        vs->name, &vs->guid, NULL, &var_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        /* Not variable-backed — try ConfigAccess for block varstores. */
        if (width > 0) {
            return read_via_config_access(fs, vs, q->var_offset, width, value);
        }
        return status;
    }

    uint8_t *data = axl_malloc(var_size);
    if (data == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    status = axl_rt()->GetVariable(vs->name, &vs->guid, NULL, &var_size, data);
    if (EFI_ERROR(status)) {
        axl_free(data);
        return status;
    }
    if ((size_t)q->var_offset + width > var_size) {
        axl_free(data);
        return EFI_BUFFER_TOO_SMALL;
    }
    uint64_t v = 0;
    axl_memcpy(&v, data + q->var_offset, width);
    *value = v;
    axl_free(data);
    return EFI_SUCCESS;
}

static EFI_STATUS
write_value(HiiFormSet *fs, HiiQuestion *q, uint64_t value)
{
    if (q->view.read_only) {
        return EFI_WRITE_PROTECTED;
    }
    HiiVarStore *vs = find_varstore(fs, q->var_store_id);
    if (vs == NULL) {
        return EFI_NOT_FOUND;
    }
    if (vs->name[0] == 0) {
        return EFI_UNSUPPORTED;
    }
    uint8_t width = q->view.width;

    UINTN var_size = 0;
    uint32_t attrs = 0;
    EFI_STATUS status = axl_rt()->GetVariable(
        vs->name, &vs->guid, &attrs, &var_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        if (width > 0) {
            return write_via_config_access(fs, vs, q->var_offset, width, value);
        }
        return status;
    }

    uint8_t *data = axl_malloc(var_size);
    if (data == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    status = axl_rt()->GetVariable(vs->name, &vs->guid, &attrs, &var_size, data);
    if (EFI_ERROR(status)) {
        axl_free(data);
        return status;
    }
    if ((size_t)q->var_offset + width > var_size) {
        axl_free(data);
        return EFI_BUFFER_TOO_SMALL;
    }
    axl_memcpy(data + q->var_offset, &value, width);
    if (attrs == 0 && vs->is_efi_var) {
        attrs = vs->attributes;
    }
    status = axl_rt()->SetVariable(vs->name, &vs->guid, attrs, var_size, data);
    axl_free(data);
    return status;
}

// ===================================================================
// Value I/O — STRING text (CHAR16 field at the question's offset)
// ===================================================================
//
// A STRING question's value is a CHAR16 array of `max_size` characters at
// VarOffset (not a 1/2/4/8 integer — that is why width is 0 and the u64
// path rejects it). max_size is a uint8_t, so the field is at most 510
// bytes; a 256-CHAR16 stack buffer covers any of it.

/* Number of CHAR16 units in a NUL-terminated wide string. */
static size_t
wstr_len(const uint16_t *s)
{
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

/* Decode a CHAR16 block read out of a varstore/config block into @p buf
   as UTF-8. @p n_chars CHAR16 units at @p src (may be unaligned / not
   NUL-terminated within the field). */
static void
string_block_to_utf8(const uint8_t *src, size_t n_chars, char *buf, size_t buf_cap)
{
    uint16_t wbuf[256];                 /* max_size <= 255 */
    axl_memcpy(wbuf, src, n_chars * 2); /* aligned copy; src may be odd */
    wbuf[n_chars] = 0;                  /* bound the string to the field */
    axl_ucs2_to_utf8_buf(wbuf, buf, buf_cap);
}

/* Read a STRING value through the config-routing/access path (block
   varstores). Mirrors read_via_config_access but extracts a CHAR16 run. */
static int
read_string_via_config(
    HiiFormSet  *fs,
    HiiVarStore *vs,
    uint16_t     offset,
    uint16_t     byte_len,
    size_t       max_chars,
    char        *buf,
    size_t       buf_cap
    )
{
    if (g_config_routing == NULL) {
        return AXL_ERR;
    }
    uint16_t *request = build_config_request(fs, vs, offset, byte_len);
    if (request == NULL) {
        return AXL_ERR;
    }

    uint16_t *progress = NULL;
    uint16_t *results = NULL;
    EFI_HII_CONFIG_ACCESS_PROTOCOL *ca = find_config_access(fs);
    EFI_STATUS status;
    if (ca != NULL) {
        status = ca->ExtractConfig(ca, (EFI_STRING)request,
                                   (EFI_STRING *)&progress,
                                   (EFI_STRING *)&results);
    } else {
        status = g_config_routing->ExtractConfig(
            g_config_routing, (EFI_STRING)request,
            (EFI_STRING *)&progress, (EFI_STRING *)&results);
    }

    int rc = AXL_ERR;
    if (!EFI_ERROR(status) && results != NULL) {
        UINTN block_size = (UINTN)offset + byte_len;
        uint8_t *block = axl_calloc(1, block_size);
        if (block != NULL) {
            uint16_t *prog2 = NULL;
            status = g_config_routing->ConfigToBlock(
                g_config_routing, (EFI_STRING)results, block, &block_size,
                (EFI_STRING *)&prog2);
            if (!EFI_ERROR(status)) {
                string_block_to_utf8(block + offset, max_chars, buf, buf_cap);
                rc = AXL_OK;
            }
            axl_free(block);
        }
    }

    free_pool(results);
    axl_free(request);
    return rc;
}

/* Write a STRING value through config-routing/access (block varstores).
   @p wstr is the CHAR16 text, @p n_chars its length (<= field width). */
static int
write_string_via_config(
    HiiFormSet     *fs,
    HiiVarStore    *vs,
    uint16_t        offset,
    uint16_t        byte_len,
    const uint16_t *wstr,
    size_t          n_chars
    )
{
    EFI_HII_CONFIG_ACCESS_PROTOCOL *ca = find_config_access(fs);
    if (ca == NULL || g_config_routing == NULL) {
        return AXL_ERR;
    }
    uint16_t *request = build_config_request(fs, vs, offset, byte_len);
    if (request == NULL) {
        return AXL_ERR;
    }

    UINTN block_size = (UINTN)offset + byte_len;
    uint8_t *block = axl_calloc(1, block_size);   /* NUL-pads the field */
    if (block == NULL) {
        axl_free(request);
        return AXL_ERR;
    }
    axl_memcpy(block + offset, wstr, n_chars * 2);

    uint16_t *config_resp = NULL;
    uint16_t *progress = NULL;
    EFI_STATUS status = g_config_routing->BlockToConfig(
        g_config_routing, (EFI_STRING)request, block, block_size,
        (EFI_STRING *)&config_resp, (EFI_STRING *)&progress);
    axl_free(block);
    axl_free(request);

    if (EFI_ERROR(status) || config_resp == NULL) {
        free_pool(config_resp);
        return AXL_ERR;
    }

    progress = NULL;
    status = ca->RouteConfig(ca, (EFI_STRING)config_resp,
                             (EFI_STRING *)&progress);
    free_pool(config_resp);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

static int
read_string_value(HiiFormSet *fs, HiiQuestion *q, char *buf, size_t buf_cap)
{
    HiiVarStore *vs = find_varstore(fs, q->var_store_id);
    if (vs == NULL || vs->name[0] == 0) {
        return AXL_ERR;
    }
    size_t max_chars = q->view.u.string.max_size;
    if (max_chars == 0 || max_chars > 255) {
        return AXL_ERR;
    }
    uint16_t offset = q->var_offset;
    uint16_t byte_len = (uint16_t)(max_chars * 2);

    UINTN var_size = 0;
    EFI_STATUS status = axl_rt()->GetVariable(
        vs->name, &vs->guid, NULL, &var_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        return read_string_via_config(fs, vs, offset, byte_len, max_chars,
                                      buf, buf_cap);
    }

    uint8_t *data = axl_malloc(var_size);
    if (data == NULL) {
        return AXL_ERR;
    }
    status = axl_rt()->GetVariable(vs->name, &vs->guid, NULL, &var_size, data);
    if (EFI_ERROR(status) || (size_t)offset + 2 > var_size) {
        axl_free(data);
        return AXL_ERR;
    }
    /* Clamp to what the variable actually holds. */
    size_t avail = (var_size - offset) / 2;
    size_t n = avail < max_chars ? avail : max_chars;
    string_block_to_utf8(data + offset, n, buf, buf_cap);
    axl_free(data);
    return AXL_OK;
}

static int
write_string_value(HiiFormSet *fs, HiiQuestion *q, const char *value)
{
    if (q->view.read_only) {
        return AXL_ERR;
    }
    HiiVarStore *vs = find_varstore(fs, q->var_store_id);
    if (vs == NULL || vs->name[0] == 0) {
        return AXL_ERR;
    }
    size_t max_chars = q->view.u.string.max_size;
    if (max_chars == 0 || max_chars > 255) {
        return AXL_ERR;
    }
    uint16_t offset = q->var_offset;
    uint16_t byte_len = (uint16_t)(max_chars * 2);

    /* Convert and reject any input longer than the field (rather than
       silently truncating). */
    unsigned short *wstr = axl_utf8_to_ucs2(value);
    if (wstr == NULL) {
        return AXL_ERR;
    }
    size_t n = wstr_len(wstr);
    if (n > max_chars) {
        axl_free(wstr);
        return AXL_ERR;
    }

    UINTN var_size = 0;
    uint32_t attrs = 0;
    EFI_STATUS status = axl_rt()->GetVariable(
        vs->name, &vs->guid, &attrs, &var_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        int rc = write_string_via_config(fs, vs, offset, byte_len, wstr, n);
        axl_free(wstr);
        return rc;
    }

    uint8_t *data = axl_malloc(var_size);
    if (data == NULL) {
        axl_free(wstr);
        return AXL_ERR;
    }
    status = axl_rt()->GetVariable(vs->name, &vs->guid, &attrs, &var_size, data);
    if (EFI_ERROR(status) || (size_t)offset + byte_len > var_size) {
        axl_free(data);
        axl_free(wstr);
        return AXL_ERR;
    }
    /* Zero the whole field (NUL-pads), then write the text. */
    axl_memset(data + offset, 0, byte_len);
    axl_memcpy(data + offset, wstr, n * 2);
    if (attrs == 0 && vs->is_efi_var) {
        attrs = vs->attributes;
    }
    status = axl_rt()->SetVariable(vs->name, &vs->guid, attrs, var_size, data);
    axl_free(data);
    axl_free(wstr);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

// ===================================================================
// Public API — value I/O
// ===================================================================

/* Resolve a question for value I/O: in range and not STRING (string
   values are out of scope for the u64 API). Returns NULL otherwise. */
static HiiQuestion *
io_question(size_t formset_index, size_t question_index, HiiFormSet **out_fs)
{
    HiiFormSet *fs = formset_at(formset_index);
    if (fs == NULL || question_index >= fs->question_count) {
        return NULL;
    }
    HiiQuestion *q = &fs->questions[question_index];
    if (q->view.type == AXL_HII_STRING) {
        return NULL;
    }
    *out_fs = fs;
    return q;
}

int
axl_hii_question_read(
    size_t    formset_index,
    size_t    question_index,
    uint64_t *value
    )
{
    if (value == NULL) {
        return AXL_ERR;
    }
    HiiFormSet *fs;
    HiiQuestion *q = io_question(formset_index, question_index, &fs);
    if (q == NULL) {
        return AXL_ERR;
    }
    return EFI_ERROR(read_value(fs, q, value)) ? AXL_ERR : AXL_OK;
}

int
axl_hii_question_write(
    size_t   formset_index,
    size_t   question_index,
    uint64_t value
    )
{
    HiiFormSet *fs;
    HiiQuestion *q = io_question(formset_index, question_index, &fs);
    if (q == NULL) {
        return AXL_ERR;
    }
    return EFI_ERROR(write_value(fs, q, value)) ? AXL_ERR : AXL_OK;
}

/* Resolve a STRING question for the string API: in range and AXL_HII_STRING
   (the type the u64 path rejects). Returns NULL otherwise. */
static HiiQuestion *
string_question(size_t formset_index, size_t question_index, HiiFormSet **out_fs)
{
    HiiFormSet *fs = formset_at(formset_index);
    if (fs == NULL || question_index >= fs->question_count) {
        return NULL;
    }
    HiiQuestion *q = &fs->questions[question_index];
    if (q->view.type != AXL_HII_STRING) {
        return NULL;
    }
    *out_fs = fs;
    return q;
}

int
axl_hii_question_read_string(
    size_t  formset_index,
    size_t  question_index,
    char   *buf,
    size_t  buf_cap
    )
{
    if (buf == NULL || buf_cap == 0) {
        return AXL_ERR;
    }
    HiiFormSet *fs;
    HiiQuestion *q = string_question(formset_index, question_index, &fs);
    if (q == NULL) {
        return AXL_ERR;
    }
    buf[0] = '\0';
    return read_string_value(fs, q, buf, buf_cap);
}

int
axl_hii_question_write_string(
    size_t      formset_index,
    size_t      question_index,
    const char *value
    )
{
    if (value == NULL) {
        return AXL_ERR;
    }
    HiiFormSet *fs;
    HiiQuestion *q = string_question(formset_index, question_index, &fs);
    if (q == NULL) {
        return AXL_ERR;
    }
    return write_string_value(fs, q, value);
}
