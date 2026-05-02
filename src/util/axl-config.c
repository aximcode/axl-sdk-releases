/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-config.c
    Unified configuration framework.
**/

#include <stdarg.h>
#include "../backend/axl-backend.h"
#include <axl/axl-config.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-stream.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("config");

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

#define MAX_MULTI  16

// ---------------------------------------------------------------------------
// Internal structures
// ---------------------------------------------------------------------------

typedef struct {
    const char *values[MAX_MULTI];
    size_t      count;
} MultiValues;

struct AxlConfig {
    const AxlConfigDesc  *descs;
    AxlConfigApplyFunc    apply_fn;
    void                 *target;
    AxlConfig            *parent;
    AxlHashTable         *values;     /* key → char* (owned copies) */
    AxlHashTable         *multi;      /* key → MultiValues* (owned) */
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const AxlConfigDesc *
find_desc(const AxlConfigDesc *descs, const char *key)
{
    for (int i = 0; descs[i].key != NULL; i++) {
        if (axl_streql(descs[i].key, key)) {
            return &descs[i];
        }
    }
    return NULL;
}

static bool
str_to_bool(const char *s)
{
    if (s == NULL) {
        return false;
    }
    return s[0] == 't' || s[0] == 'T' || s[0] == '1' || s[0] == 'y' || s[0] == 'Y';
}

static bool
validate_type(int type, const char *value)
{
    if (value == NULL) {
        return true;
    }

    switch (type) {
    case AXL_CFG_BOOL:
        return value[0] == 't' || value[0] == 'T' ||
               value[0] == 'f' || value[0] == 'F' ||
               value[0] == '0' || value[0] == '1' ||
               value[0] == 'y' || value[0] == 'Y' ||
               value[0] == 'n' || value[0] == 'N';

    case AXL_CFG_INT: {
        const char *p = value;
        if (*p == '-' || *p == '+') {
            p++;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        return true;
    }

    case AXL_CFG_UINT: {
        const char *p = value;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            /* After 0x prefix, require hex digit */
            if (!((*p >= '0' && *p <= '9') ||
                  (*p >= 'a' && *p <= 'f') ||
                  (*p >= 'A' && *p <= 'F'))) {
                return false;
            }
        } else {
            if (*p < '0' || *p > '9') {
                return false;
            }
        }
        return true;
    }

    case AXL_CFG_STRING:
    case AXL_CFG_MULTI:
        return true;

    default:
        return false;
    }
}

/* Validate + (optionally) write an INT/UINT/BOOL/STRING field.
 *
 * Two modes:
 *   target == NULL          → validate only (parse + range check); no write.
 *   target != NULL          → validate AND write to target+desc->offset, if
 *                             desc->field_size matches a known width. If the
 *                             width is 0 (no auto-apply field declared),
 *                             we still validate so callers see range errors.
 *
 * For numeric types the declared field_size narrows the accepted range:
 * a uint16_t field rejects 99999, a uint32_t field rejects 2^33, etc.
 *
 * Returns 0 on success, -1 on parse error or out-of-range. */
static int
auto_apply(void *target, const AxlConfigDesc *desc, const char *value)
{
    if (value == NULL) {
        return 0;
    }

    const size_t width = desc->field_size;
    uint8_t *field = (target != NULL && width > 0)
        ? (uint8_t *)target + desc->offset
        : NULL;

    switch (desc->type) {
    case AXL_CFG_BOOL: {
        bool v = str_to_bool(value);
        if (field != NULL && width == sizeof(bool)) {
            *(bool *)field = v;
        }
        return 0;
    }

    case AXL_CFG_INT: {
        /* sizeof(int) is 4 on every supported AXL target (UEFI x64,
         * AARCH64). The static assert means we can map `int` and
         * `int32_t` paths to the same axl_str_to_s32 call without the
         * runtime guard the previous version had. */
        static_assert(sizeof(int) == sizeof(int32_t),
                      "AXL assumes sizeof(int) == 4");
        int64_t v;
        if (width == sizeof(int32_t)) {
            int32_t v32;
            if (axl_str_to_s32(value, 0, &v32, NULL) != 0) {
                return -1;
            }
            v = v32;
        } else {
            if (axl_str_to_s64(value, 0, &v, NULL) != 0) {
                return -1;
            }
        }
        if (field != NULL) {
            if (width == sizeof(int64_t)) {
                *(int64_t *)field = v;
            } else if (width == sizeof(int32_t)) {
                *(int32_t *)field = (int32_t)v;
            }
        }
        return 0;
    }

    case AXL_CFG_UINT: {
        uint64_t v;
        if (width == sizeof(uint16_t)) {
            uint16_t v16;
            if (axl_str_to_u16(value, 0, &v16, NULL) != 0) {
                return -1;
            }
            v = v16;
        } else if (width == sizeof(uint32_t)) {
            uint32_t v32;
            if (axl_str_to_u32(value, 0, &v32, NULL) != 0) {
                return -1;
            }
            v = v32;
        } else {
            if (axl_str_to_u64(value, 0, &v, NULL) != 0) {
                return -1;
            }
        }
        if (field != NULL) {
            if (width == sizeof(uint64_t)) {
                *(uint64_t *)field = v;
            } else if (width == sizeof(uint32_t)) {
                *(uint32_t *)field = (uint32_t)v;
            } else if (width == sizeof(uint16_t)) {
                *(uint16_t *)field = (uint16_t)v;
            }
        }
        return 0;
    }

    case AXL_CFG_STRING:
        if (field != NULL && width == sizeof(char *)) {
            *(const char **)field = value;
        }
        return 0;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Public API — lifecycle
// ---------------------------------------------------------------------------

AxlConfig *
axl_config_new(
    const AxlConfigDesc  *descs,
    AxlConfigApplyFunc    apply_fn,
    void                 *target)
{
    AxlConfig *cfg;

    if (descs == NULL) {
        return NULL;
    }

    cfg = (AxlConfig *)axl_calloc(1, sizeof(AxlConfig));
    if (cfg == NULL) {
        return NULL;
    }

    cfg->descs = descs;
    cfg->apply_fn = apply_fn;
    cfg->target = target;
    cfg->parent = NULL;

    cfg->values = axl_hash_table_new_full(
        NULL, NULL, axl_free_impl, axl_free_impl);
    cfg->multi = axl_hash_table_new_str();
    if (cfg->values == NULL || cfg->multi == NULL) {
        axl_hash_table_free(cfg->values);
        axl_hash_table_free(cfg->multi);
        axl_free(cfg);
        return NULL;
    }

    /* Apply defaults. A default that overflows the declared width is a
     * descriptor bug — log it but don't fail axl_config_new (we still
     * have a usable config; the field just won't auto-apply). */
    for (int i = 0; descs[i].key != NULL; i++) {
        if (descs[i].default_value != NULL) {
            axl_hash_table_replace(cfg->values,
                               axl_strdup(descs[i].key),
                               axl_strdup(descs[i].default_value));
            if (auto_apply(target, &descs[i], descs[i].default_value) != 0) {
                axl_warning("config: default '%s'='%s' out of range for "
                            "declared type — ignored",
                            descs[i].key, descs[i].default_value);
            }
        }
    }

    return cfg;
}

static void
free_multi(const void *key, void *val, void *ctx)
{
    MultiValues *mv = (MultiValues *)val;
    (void)key;
    (void)ctx;
    for (size_t i = 0; i < mv->count; i++) {
        axl_free((void *)mv->values[i]);
    }
    axl_free(mv);
}

void
axl_config_free(AxlConfig *cfg)
{
    if (cfg == NULL) {
        return;
    }

    if (cfg->values != NULL) {
        axl_hash_table_free(cfg->values);
    }

    if (cfg->multi != NULL) {
        axl_hash_table_foreach(cfg->multi, free_multi, NULL);
        axl_hash_table_free(cfg->multi);
    }

    axl_free(cfg);
}

// ---------------------------------------------------------------------------
// Public API — set / get
// ---------------------------------------------------------------------------

int
axl_config_set(AxlConfig *cfg, const char *key, const char *value)
{
    const AxlConfigDesc *desc;

    if (cfg == NULL || key == NULL) {
        return -1;
    }

    /*
     * Callback runs FIRST — handles dynamic keys (e.g. "header.*")
     * that aren't in the descriptor table.
     *   return  0: accepted, proceed with descriptor lookup + auto-apply
     *   return  1: handled (stored by callback), skip auto-apply
     *   return -1: rejected
     */
    if (cfg->apply_fn != NULL) {
        int rc = cfg->apply_fn(cfg->target, key, value);
        if (rc == -1) {
            return -1;
        }
        if (rc == 1) {
            /* Callback handled it — store value for later retrieval */
            axl_hash_table_replace(cfg->values, axl_strdup(key),
                               value != NULL ? axl_strdup(value) : NULL);
            return 0;
        }
    }

    /* Descriptor lookup — reject unknown keys */
    desc = find_desc(cfg->descs, key);
    if (desc == NULL) {
        return -1;
    }

    if (!validate_type(desc->type, value)) {
        return -1;
    }

    /* Validate the value parses + fits the declared field width before
     * any side effect. Catches "port=99999" overflowing a uint16_t —
     * which used to silently truncate to 34463. */
    if (auto_apply(NULL, desc, value) != 0) {
        axl_warning("config: '%s' value '%s' out of range for declared type",
                    key, value);
        return -1;
    }

    /* Handle MULTI: append to array */
    if (desc->type == AXL_CFG_MULTI) {
        MultiValues *mv = (MultiValues *)axl_hash_table_lookup(cfg->multi, key);
        if (mv == NULL) {
            mv = (MultiValues *)axl_calloc(1, sizeof(MultiValues));
            if (mv == NULL) {
                return -1;
            }
            axl_hash_table_replace(cfg->multi, key, mv);
        }
        if (mv->count >= MAX_MULTI) {
            return -1;
        }
        mv->values[mv->count++] = axl_strdup(value);
        return 0;
    }

    /* Store value (owned copy) */
    {
        char *stored = value != NULL ? axl_strdup(value) : NULL;
        axl_hash_table_replace(cfg->values, axl_strdup(key), stored);

        /* Auto-apply uses stored copy (safe for STRING pointer fields).
         * The value was validated above so this can't fail; ignore rc. */
        (void)auto_apply(cfg->target, desc, stored);
    }

    return 0;
}

int
axl_config_setv(AxlConfig *cfg, ...)
{
    va_list ap;
    const char *key;

    if (cfg == NULL) {
        return -1;
    }

    va_start(ap, cfg);
    /* clang-tidy 18's clang-analyzer-valist.Uninitialized loses track
       of ap inside this loop and reports a false positive. Fixed
       upstream by LLVM PR #156682 (merged 2025-09-08, valist checker
       consolidation); remove the NOLINT once the CI runner's
       clang-tidy is new enough. */
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    while ((key = va_arg(ap, const char *)) != NULL) {
        const char *value = va_arg(ap, const char *);
        if (axl_config_set(cfg, key, value) != 0) {
            va_end(ap);
            return -1;
        }
    }
    va_end(ap);

    return 0;
}

const char *
axl_config_get(AxlConfig *cfg, const char *key)
{
    const char *val;

    if (cfg == NULL || key == NULL) {
        return NULL;
    }

    val = (const char *)axl_hash_table_lookup(cfg->values, key);
    if (val != NULL) {
        return val;
    }

    /* Fall through to parent */
    if (cfg->parent != NULL) {
        return axl_config_get(cfg->parent, key);
    }

    return NULL;
}

bool
axl_config_get_bool(AxlConfig *cfg, const char *key)
{
    return str_to_bool(axl_config_get(cfg, key));
}

int64_t
axl_config_get_int(AxlConfig *cfg, const char *key)
{
    const char *val = axl_config_get(cfg, key);
    if (val == NULL) {
        return 0;
    }

    int64_t result = 0;
    bool negative = false;
    const char *p = val;

    if (*p == '-') { negative = true; p++; }
    else if (*p == '+') { p++; }

    while (*p >= '0' && *p <= '9') {
        result = result * 10 + (*p - '0');
        p++;
    }

    return negative ? -result : result;
}

uint64_t
axl_config_get_uint(AxlConfig *cfg, const char *key)
{
    const char *val = axl_config_get(cfg, key);
    if (val == NULL) {
        return 0;
    }
    return axl_strtou64(val);
}

size_t
axl_config_get_multi_count(AxlConfig *cfg, const char *key)
{
    MultiValues *mv;

    if (cfg == NULL || key == NULL) {
        return 0;
    }

    mv = (MultiValues *)axl_hash_table_lookup(cfg->multi, key);
    if (mv == NULL) {
        return 0;
    }

    return mv->count;
}

const char *
axl_config_get_multi(AxlConfig *cfg, const char *key, size_t index)
{
    MultiValues *mv;

    if (cfg == NULL || key == NULL) {
        return NULL;
    }

    mv = (MultiValues *)axl_hash_table_lookup(cfg->multi, key);
    if (mv == NULL || index >= mv->count) {
        return NULL;
    }

    return mv->values[index];
}

// ---------------------------------------------------------------------------
// Public API — command-line parsing
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Public API — inheritance
// ---------------------------------------------------------------------------

void
axl_config_set_parent(AxlConfig *cfg, AxlConfig *parent)
{
    if (cfg == NULL) {
        return;
    }
    cfg->parent = parent;
}
