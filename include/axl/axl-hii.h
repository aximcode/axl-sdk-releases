/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hii.h
    UEFI HII setup-form reader — enumerate Setup form sets and
    get/read/write their questions.

    The platform's BIOS Setup menus are published as HII form sets: a
    tree of IFR (Internal Forms Representation) opcodes in the HII
    database, backed by EFI variables or driver-private config storage.
    This module locates the HII protocols, walks every form package once
    into a cached typed model, and projects each setting as an
    AxlHiiQuestion. A consumer (e.g. a remote BIOS-setup UI) can then
    enumerate form sets, read a question's current value, and write a new
    one — without touching the raw HII protocols or decoding IFR.

    @code
    if (axl_hii_available()) {
        size_t n = axl_hii_formset_count();
        for (size_t f = 0; f < n; f++) {
            char title[128], help[128];
            size_t qcount;
            if (axl_hii_formset_get(f, title, sizeof title,
                                    help, sizeof help, NULL, &qcount) != AXL_OK) {
                continue;
            }
            for (size_t q = 0; q < qcount; q++) {
                AxlHiiQuestion question;
                if (axl_hii_question_get(f, q, &question) != AXL_OK) {
                    continue;
                }
                uint64_t value;
                if (axl_hii_question_read(f, q, &value) == AXL_OK) {
                    // ... report question.prompt = value ...
                }
            }
        }
    }
    @endcode

    Scope: ONE_OF, CHECKBOX, NUMERIC, and STRING questions. STRING
    questions are enumerated (prompt/help/min/max size) but their values
    are not read or written through the u64 API — `axl_hii_question_read`
    and `_write` return AXL_ERR for them (a string-valued variant may be
    added later). Forms, grefs, suppression/grayout expressions, and
    default stores are out of scope; this is a flat list of the
    answerable questions in each form set, not a form-browser.

    The model is parsed lazily on the first `axl_hii_*` call and cached
    for the life of the process (freed via an internal atexit handler).
    Values are read live on each `axl_hii_question_read`, so they reflect
    writes made by this or any other agent.
**/

#ifndef AXL_HII_H
#define AXL_HII_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>    /* AxlGuid */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Question (setting) kind.
 *
 * The four IFR statement kinds this module surfaces. The active union
 * arm of AxlHiiQuestion is selected by this type:
 * AXL_HII_ONE_OF -> `u.one_of`, AXL_HII_NUMERIC -> `u.numeric`,
 * AXL_HII_STRING -> `u.string`. AXL_HII_CHECKBOX has no extra data
 * (its value is 0 or 1).
 */
typedef enum {
    AXL_HII_ONE_OF,    ///< enumerated choice from a fixed option list
    AXL_HII_CHECKBOX,  ///< boolean (value 0 or 1, width 1)
    AXL_HII_NUMERIC,   ///< integer within a min/max range
    AXL_HII_STRING     ///< free text (value not read via the u64 API)
} AxlHiiQuestionType;

/**
 * @brief One selectable choice of a ONE_OF question.
 *
 * `value` is what `axl_hii_question_read` returns when this option is
 * selected (and what to pass to `_write` to select it). `label` is the
 * resolved, NUL-terminated display string (truncated to fit).
 */
typedef struct {
    uint64_t value;     ///< the option's stored value
    char     label[128];///< resolved display label (NUL-terminated)
} AxlHiiOption;

/**
 * @brief A single setup question, projected from IFR.
 *
 * `id` is the IFR question id, stable within its form set across boots
 * for a given firmware. `width` is the value's byte width (1/2/4/8) for
 * the integer kinds, and 0 for AXL_HII_STRING. `read_only` folds in both
 * the IFR read-only and callback flags (a callback-driven question is
 * not safely writable as a plain variable). `reset_required` is the
 * IFR hint that changing this setting needs a reboot to take effect.
 *
 * The `u` union is valid only for the matching `type`:
 * - AXL_HII_ONE_OF: `u.one_of.options[0..option_count)` lists the
 *   allowed value/label pairs (capped at 16).
 * - AXL_HII_NUMERIC: `u.numeric` gives the inclusive min/max and the
 *   step (0 means "no step constraint").
 * - AXL_HII_STRING: `u.string` gives the min/max character counts.
 * - AXL_HII_CHECKBOX: no union arm is used.
 */
typedef struct {
    uint32_t            id;             ///< IFR question id (stable within a form set)
    AxlHiiQuestionType  type;           ///< question kind; selects the `u` arm
    char                prompt[128];    ///< resolved prompt string (NUL-terminated)
    char                help[128];      ///< resolved help string (NUL-terminated)
    bool                read_only;      ///< read-only or callback-driven (not plainly writable)
    bool                reset_required; ///< changing it needs a reboot to take effect
    uint8_t             width;          ///< value width 1/2/4/8 (0 for STRING)
    union {
        struct {
            AxlHiiOption options[16];   ///< allowed choices
            size_t       option_count;  ///< number of valid entries in `options`
        } one_of;
        struct {
            uint64_t min;               ///< inclusive minimum
            uint64_t max;               ///< inclusive maximum
            uint64_t step;              ///< increment (0 = unconstrained)
        } numeric;
        struct {
            uint8_t min_size;           ///< minimum character count
            uint8_t max_size;           ///< maximum character count
        } string;
    } u;
} AxlHiiQuestion;

/**
 * @brief Report whether any HII setup form sets are present.
 *
 * Triggers the lazy parse on first call. A cheap gate a consumer can
 * branch on before enumerating: true means the HII database is published
 * and at least one form set with a title was parsed. False covers both a
 * firmware with no HII database (rare) and one that published no form
 * packages. The parse result is cached, so this is cheap after the first
 * call.
 *
 * @return true if at least one form set is available, false otherwise.
 */
bool
axl_hii_available(void);

/**
 * @brief Number of discovered HII form sets.
 *
 * Triggers the lazy parse on first call. Valid form-set indices are
 * `[0, axl_hii_formset_count())`.
 *
 * @return the form-set count (0 if none / no HII database).
 */
size_t
axl_hii_formset_count(void);

/**
 * @brief Read a form set's metadata by index.
 *
 * Copies the form set's resolved title and help into the caller's
 * buffers (NUL-terminated, truncated to fit), its form-set GUID, and how
 * many questions it has. Any of @p title, @p help, @p guid, or
 * @p question_count may be NULL to skip that field; a 0 capacity also
 * skips the matching string copy. The GUID is the IFR form-set class
 * GUID — a stable identity a consumer can use to recognize a well-known
 * form set (or label it) regardless of the localized title.
 *
 * @return AXL_OK on success, AXL_ERR if @p index is out of range.
 */
AXL_WARN_UNUSED int
axl_hii_formset_get(
    size_t   index,           ///< form-set index in [0, count)
    char    *title,           ///< [out] title buffer, or NULL to skip
    size_t   title_cap,       ///< capacity of @p title in bytes
    char    *help,            ///< [out] help buffer, or NULL to skip
    size_t   help_cap,        ///< capacity of @p help in bytes
    AxlGuid *guid,            ///< [out] form-set GUID, or NULL to skip
    size_t  *question_count   ///< [out] number of questions, or NULL to skip
);

/**
 * @brief Read a question's static description.
 *
 * Projects question @p question_index of form set @p formset_index into
 * @p out: type, prompt/help, width, read-only/reset-required flags, and
 * the per-type constraints (options, min/max/step, or string sizes).
 * This is the static IFR description; the live value is read separately
 * via `axl_hii_question_read`.
 *
 * @return AXL_OK on success, AXL_ERR if either index is out of range or
 *     @p out is NULL.
 */
AXL_WARN_UNUSED int
axl_hii_question_get(
    size_t          formset_index,   ///< form-set index in [0, count)
    size_t          question_index,  ///< question index in [0, question_count)
    AxlHiiQuestion *out              ///< [out] populated on success
);

/**
 * @brief Read a question's current value.
 *
 * Resolves the question's variable store and reads the value at its
 * offset — first via `GetVariable` (EFI-backed stores), falling back to
 * the HII config-routing/config-access path for driver-private block
 * stores. The result is the raw integer, width-extended to 64 bits (for
 * CHECKBOX, 0 or 1; for ONE_OF, the selected option's value).
 *
 * @return AXL_OK on success. AXL_ERR if either index is out of range,
 *     @p value is NULL, the question is AXL_HII_STRING (not supported by
 *     the u64 API), or the value cannot be read (no backing store /
 *     unreadable variable).
 */
AXL_WARN_UNUSED int
axl_hii_question_read(
    size_t    formset_index,   ///< form-set index in [0, count)
    size_t    question_index,  ///< question index in [0, question_count)
    uint64_t *value            ///< [out] current value on success
);

/**
 * @brief Write a question's value.
 *
 * Writes @p value into the question's variable store at its offset —
 * via `GetVariable`/patch/`SetVariable` for EFI-backed stores, or the
 * config-routing `BlockToConfig` -> config-access `RouteConfig` path for
 * driver-private block stores. The caller is responsible for passing a
 * value the firmware accepts (a valid ONE_OF option, or an in-range
 * NUMERIC); this module does not range-check against the question's
 * constraints.
 *
 * @return AXL_OK on success. AXL_ERR if either index is out of range,
 *     the question is read-only or AXL_HII_STRING, or the underlying
 *     write fails.
 */
AXL_WARN_UNUSED int
axl_hii_question_write(
    size_t   formset_index,   ///< form-set index in [0, count)
    size_t   question_index,  ///< question index in [0, question_count)
    uint64_t value            ///< value to store
);

/**
 * @brief Read an AXL_HII_STRING question's current text.
 *
 * The string counterpart of `axl_hii_question_read` (which rejects
 * STRING questions). Resolves the question's variable store, reads the
 * `CHAR16` text stored at its offset (bounded by the question's
 * `max_size`), and converts it to UTF-8 into @p buf — NUL-terminated and
 * truncated to @p buf_cap. The same `GetVariable` / config-routing
 * resolution as the integer reader is used.
 *
 * @return AXL_OK on success. AXL_ERR if either index is out of range,
 *     @p buf is NULL or @p buf_cap is 0, the question is not
 *     AXL_HII_STRING, or the value cannot be read (no backing store /
 *     unreadable variable).
 */
AXL_WARN_UNUSED int
axl_hii_question_read_string(
    size_t  formset_index,   ///< form-set index in [0, count)
    size_t  question_index,  ///< question index in [0, question_count)
    char   *buf,             ///< [out] UTF-8 text, NUL-terminated
    size_t  buf_cap          ///< capacity of @p buf in bytes
);

/**
 * @brief Write an AXL_HII_STRING question's text.
 *
 * The string counterpart of `axl_hii_question_write`. Converts the UTF-8
 * @p value to `CHAR16`, requires its length not exceed the question's
 * `max_size` (the field is then NUL-padded to that width), and stores it
 * at the question's offset — `GetVariable`/patch/`SetVariable` for
 * EFI-backed stores, or the config-routing path for block stores. The
 * `min_size` constraint is left to the firmware to enforce.
 *
 * @return AXL_OK on success. AXL_ERR if either index is out of range,
 *     @p value is NULL, the question is read-only or not AXL_HII_STRING,
 *     @p value is longer than `max_size`, or the underlying write fails.
 */
AXL_WARN_UNUSED int
axl_hii_question_write_string(
    size_t      formset_index,   ///< form-set index in [0, count)
    size_t      question_index,  ///< question index in [0, question_count)
    const char *value            ///< UTF-8 text to store
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HII_H */
