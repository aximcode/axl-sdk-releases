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
#include <axl/axl-url.h>

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
            if (axl_str_to_u64(value, 0, &v, NULL) != AXL_OK) {
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
                            "declared type - ignored",
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
        return AXL_ERR;
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
            return AXL_ERR;
        }
        if (rc == 1) {
            /* Callback handled it — store value for later retrieval */
            axl_hash_table_replace(cfg->values, axl_strdup(key),
                               value != NULL ? axl_strdup(value) : NULL);
            return AXL_OK;
        }
    }

    /* Descriptor lookup — reject unknown keys */
    desc = find_desc(cfg->descs, key);
    if (desc == NULL) {
        return AXL_ERR;
    }

    if (!validate_type(desc->type, value)) {
        return AXL_ERR;
    }

    /* Validate the value parses + fits the declared field width before
     * any side effect. Catches "port=99999" overflowing a uint16_t —
     * which used to silently truncate to 34463. */
    if (auto_apply(NULL, desc, value) != 0) {
        axl_warning("config: '%s' value '%s' out of range for declared type",
                    key, value);
        return AXL_ERR;
    }

    /* Handle MULTI: append to array */
    if (desc->type == AXL_CFG_MULTI) {
        MultiValues *mv = (MultiValues *)axl_hash_table_lookup(cfg->multi, key);
        if (mv == NULL) {
            mv = (MultiValues *)axl_calloc(1, sizeof(MultiValues));
            if (mv == NULL) {
                return AXL_ERR;
            }
            axl_hash_table_replace(cfg->multi, key, mv);
        }
        if (mv->count >= MAX_MULTI) {
            return AXL_ERR;
        }
        mv->values[mv->count++] = axl_strdup(value);
        return AXL_OK;
    }

    /* Store value (owned copy) */
    {
        char *stored = value != NULL ? axl_strdup(value) : NULL;
        axl_hash_table_replace(cfg->values, axl_strdup(key), stored);

        /* Auto-apply uses stored copy (safe for STRING pointer fields).
         * The value was validated above so this can't fail; ignore rc. */
        (void)auto_apply(cfg->target, desc, stored);
    }

    return AXL_OK;
}

int
axl_config_setv(AxlConfig *cfg, ...)
{
    va_list ap;
    const char *key;

    if (cfg == NULL) {
        return AXL_ERR;
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
        if (axl_config_set(cfg, key, value) != AXL_OK) {
            va_end(ap);
            return AXL_ERR;
        }
    }
    va_end(ap);

    return AXL_OK;
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
// Public API — cross-binary serialization
// ---------------------------------------------------------------------------

/* Append `key=value&` (URL-encoded) to *out, advancing *cursor and
   shrinking *remaining. Returns AXL_OK if the encode + concat fit,
   AXL_ERR on overflow. The trailing '&' is unconditional — caller
   strips it after the loop. */
static int
to_string_emit_pair(const char *key, const char *value,
                    char **cursor, size_t *remaining)
{
    int n;

    if (*remaining == 0) {
        return AXL_ERR;
    }

    n = axl_url_encode(key, *cursor, *remaining);
    if (n < 0) {
        return AXL_ERR;
    }
    *cursor    += n;
    *remaining -= (size_t)n;

    if (*remaining < 2) {  /* room for '=' + at least the trailing NUL */
        return AXL_ERR;
    }
    **cursor = '=';
    (*cursor)++;
    (*remaining)--;

    /* Empty value is legal (e.g. "key=") — encode produces 0 bytes. */
    n = axl_url_encode(value != NULL ? value : "", *cursor, *remaining);
    if (n < 0) {
        return AXL_ERR;
    }
    *cursor    += n;
    *remaining -= (size_t)n;

    if (*remaining < 2) {
        return AXL_ERR;
    }
    **cursor = '&';
    (*cursor)++;
    (*remaining)--;

    return AXL_OK;
}

typedef struct {
    char    *cursor;
    size_t   remaining;
    int      rc;
} ToStringCtx;

static void
to_string_scalar_cb(const void *key, void *value, void *data)
{
    ToStringCtx *ctx = (ToStringCtx *)data;

    if (ctx->rc != AXL_OK) {
        return;  /* short-circuit on first error */
    }
    ctx->rc = to_string_emit_pair((const char *)key,
                                  (const char *)value,
                                  &ctx->cursor, &ctx->remaining);
}

static void
to_string_multi_cb(const void *key, void *value, void *data)
{
    ToStringCtx       *ctx = (ToStringCtx *)data;
    const MultiValues *mv  = (const MultiValues *)value;

    for (size_t i = 0; i < mv->count; i++) {
        if (ctx->rc != AXL_OK) {
            return;
        }
        ctx->rc = to_string_emit_pair((const char *)key,
                                      mv->values[i],
                                      &ctx->cursor, &ctx->remaining);
    }
}

int
axl_config_to_string(AxlConfig *cfg, char *out, size_t out_size)
{
    if (cfg == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    ToStringCtx ctx;
    ctx.cursor    = out;
    ctx.remaining = out_size;
    ctx.rc        = AXL_OK;

    /* Scalar values (the multi map's keys also appear in `values`
       but pointing at MULTI sentinels — skip those, walk multi
       separately below). The scalar walk hits each non-multi key
       exactly once. */
    axl_hash_table_foreach(cfg->values, to_string_scalar_cb, &ctx);

    /* MULTI walk emits one `key=value&` for each value. */
    axl_hash_table_foreach(cfg->multi, to_string_multi_cb, &ctx);

    if (ctx.rc != AXL_OK) {
        return AXL_ERR;
    }

    /* Strip the trailing '&' if we emitted at least one pair, otherwise
       NUL-terminate the empty buffer. */
    if (ctx.cursor > out && ctx.cursor[-1] == '&') {
        ctx.cursor--;
        ctx.remaining++;
    }
    *ctx.cursor = '\0';
    return AXL_OK;
}

int
axl_config_target_to_string(
    const AxlConfigDesc *descs,
    const void          *target,
    char                *out,
    size_t               out_size
    )
{
    if (descs == NULL || target == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    char  *cursor    = out;
    size_t remaining = out_size;
    char   buf[64];   /* numeric/bool formatting */

    for (const AxlConfigDesc *d = descs; d->key != NULL; d++) {
        if (d->field_size == 0) {
            /* No auto-apply, no offsetof anchor — can't read from target. */
            continue;
        }

        const uint8_t *field = (const uint8_t *)target + d->offset;
        const char    *val   = NULL;

        switch (d->type) {
        case AXL_CFG_BOOL:
            if (d->field_size == sizeof(bool)) {
                val = (*(const bool *)field) ? "true" : "false";
            } else {
                axl_warning("config: target_to_string: '%s' BOOL has "
                            "field_size %zu, expected %zu - skipping",
                            d->key, d->field_size, sizeof(bool));
            }
            break;

        case AXL_CFG_INT: {
            int64_t v = 0;
            if (d->field_size == sizeof(int32_t)) {
                v = *(const int32_t *)field;
            } else if (d->field_size == sizeof(int64_t)) {
                v = *(const int64_t *)field;
            } else {
                axl_warning("config: target_to_string: '%s' INT has "
                            "field_size %zu, expected 4 or 8 - skipping",
                            d->key, d->field_size);
                continue;
            }
            axl_snprintf(buf, sizeof(buf), "%lld", (long long)v);
            val = buf;
            break;
        }

        case AXL_CFG_UINT: {
            uint64_t v = 0;
            if (d->field_size == sizeof(uint8_t)) {
                v = *(const uint8_t *)field;
            } else if (d->field_size == sizeof(uint16_t)) {
                v = *(const uint16_t *)field;
            } else if (d->field_size == sizeof(uint32_t)) {
                v = *(const uint32_t *)field;
            } else if (d->field_size == sizeof(uint64_t)) {
                v = *(const uint64_t *)field;
            } else {
                axl_warning("config: target_to_string: '%s' UINT has "
                            "field_size %zu, expected 1/2/4/8 - skipping",
                            d->key, d->field_size);
                continue;
            }
            axl_snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
            val = buf;
            break;
        }

        case AXL_CFG_STRING:
            if (d->field_size == sizeof(char *)) {
                const char *p = *(const char *const *)field;
                if (p == NULL) {
                    continue;  /* nothing set */
                }
                val = p;
            } else {
                /* Common consumer-side mistake: declaring a `char buf[N]`
                   field instead of `char *`. AXL_CFG_STRING auto-applies
                   a pointer to cfg-interned storage, not a memcpy. */
                axl_warning("config: target_to_string: '%s' STRING has "
                            "field_size %zu, expected %zu (sizeof(char*)) "
                            "- declare the target field as `const char *`, "
                            "not a char buffer; skipping",
                            d->key, d->field_size, sizeof(char *));
            }
            break;

        case AXL_CFG_MULTI:
        default:
            /* MULTI doesn't have an offset target; skip silently —
               this is the documented contract. */
            continue;
        }

        if (val == NULL) {
            continue;
        }

        if (to_string_emit_pair(d->key, val, &cursor, &remaining) != AXL_OK) {
            return AXL_ERR;
        }
    }

    /* Strip trailing '&' if any pair emitted, NUL-terminate either way. */
    if (cursor > out && cursor[-1] == '&') {
        cursor--;
    }
    *cursor = '\0';
    return AXL_OK;
}

int
axl_config_from_string(AxlConfig *cfg, const char *in)
{
    if (cfg == NULL || in == NULL) {
        return AXL_ERR;
    }

    /* Parse `key=value&key=value`. Decode each half via axl_url_decode
       into stack-bounded buffers (key + value capped to keep the
       parser stackable; over-cap pairs are an error rather than a
       silent truncate — matches the conservative split rest of AXL
       takes on URL parsing). */
    char key_buf[128];
    char val_buf[1024];

    const char *p = in;
    while (*p != '\0') {
        const char *amp = p;
        while (*amp != '\0' && *amp != '&') {
            amp++;
        }

        size_t pair_len = (size_t)(amp - p);
        if (pair_len == 0) {
            /* Empty pair (e.g. leading "&" or "&&") — skip. */
            if (*amp == '&') {
                amp++;
            }
            p = amp;
            continue;
        }

        /* Find '=' inside the pair. No '=' → value is empty string. */
        const char *eq = p;
        while (eq < amp && *eq != '=') {
            eq++;
        }

        size_t key_enc_len = (size_t)(eq - p);
        size_t val_enc_len = (eq < amp) ? (size_t)(amp - eq - 1) : 0;

        if (key_enc_len == 0) {
            axl_warning("axl_config_from_string: empty key at offset %zd",
                        (intptr_t)(p - in));
            return AXL_ERR;
        }
        if (key_enc_len >= sizeof(key_buf) ||
            val_enc_len >= sizeof(val_buf)) {
            axl_warning("axl_config_from_string: pair too large at offset %zd",
                        (intptr_t)(p - in));
            return AXL_ERR;
        }

        /* axl_url_decode reads a NUL-terminated string. Stage into
           a small buffer first to NUL-terminate the encoded slice. */
        char enc_key[sizeof(key_buf)];
        char enc_val[sizeof(val_buf)];
        axl_memcpy(enc_key, p, key_enc_len);
        enc_key[key_enc_len] = '\0';
        if (val_enc_len > 0) {
            axl_memcpy(enc_val, eq + 1, val_enc_len);
        }
        enc_val[val_enc_len] = '\0';

        if (axl_url_decode(enc_key, key_buf, sizeof(key_buf)) < 0) {
            axl_warning("axl_config_from_string: bad key encoding");
            return AXL_ERR;
        }
        if (axl_url_decode(enc_val, val_buf, sizeof(val_buf)) < 0) {
            axl_warning("axl_config_from_string: bad value encoding");
            return AXL_ERR;
        }

        if (axl_config_set(cfg, key_buf, val_buf) != AXL_OK) {
            axl_warning("axl_config_from_string: set('%s', '%s') failed",
                        key_buf, val_buf);
            return AXL_ERR;
        }

        if (*amp == '&') {
            amp++;
        }
        p = amp;
    }

    return AXL_OK;
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

// ---------------------------------------------------------------------------
// Public API — descriptor-table composition
// ---------------------------------------------------------------------------

size_t
axl_config_descs_append(
    AxlConfigDesc       *out,
    size_t               cap,
    const AxlConfigDesc *src)
{
    if (out == NULL || src == NULL) {
        return 0;
    }

    /* Count source entries (stopping at the {0} sentinel) before
       writing anything, so an under-capacity request is a clean
       no-op rather than a partial copy. */
    size_t n = 0;
    while (src[n].key != NULL) {
        n++;
    }

    if (n > cap) {
        axl_warning("axl_config_descs_append: cap=%zu cannot hold %zu entries",
                    cap, n);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = src[i];
    }
    return n;
}
