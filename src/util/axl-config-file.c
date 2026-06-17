/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-config-file.c
    Free-form key=value config-file map — the open-vocabulary counterpart
    to the descriptor-bound AxlConfig. Parses a text file into a flat
    string->string hash map and serves typed getters with caller defaults.
**/

#include <axl/axl-config-file.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-fs.h>
#include <axl/axl-mem.h>

/* Generous per-line cap. SoftBMC's real limits are ~64-char keys /
   256-char values; a line at or beyond this is treated as malformed and
   skipped rather than half-parsed. */
#define CONFIG_FILE_LINE_MAX  1024

struct AxlConfigFile {
    AxlHashTable *map;   /* owned char* key -> owned char* value */
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static AxlConfigFile *
config_file_alloc(void)
{
    AxlConfigFile *cf = axl_calloc(1, sizeof(*cf));
    if (cf == NULL) {
        return NULL;
    }
    cf->map = axl_hash_table_new_full(axl_str_hash, axl_str_equal,
                                      axl_free_impl, axl_free_impl);
    if (cf->map == NULL) {
        axl_free(cf);
        return NULL;
    }
    return cf;
}

AxlConfigFile *
axl_config_file_new(void)
{
    return config_file_alloc();
}

void
axl_config_file_free(AxlConfigFile *cf)
{
    if (cf == NULL) {
        return;
    }
    axl_hash_table_free(cf->map);
    axl_free(cf);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/* Parse one line (already stripped of its newline) into the map: split on
   the first '=', trim both halves, skip blanks and '#' comments. */
static void
config_file_parse_line(
    AxlConfigFile *cf,
    const char    *line,
    size_t         line_len
    )
{
    if (line_len == 0 || line_len >= CONFIG_FILE_LINE_MAX) {
        return;  /* blank or over-long (malformed) */
    }

    char tmp[CONFIG_FILE_LINE_MAX];
    axl_memcpy(tmp, line, line_len);
    tmp[line_len] = '\0';

    /* Leading whitespace decides comment / blank without disturbing the
       split (axl_strstrip on each half handles the rest). */
    char *p = tmp;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '#') {
        return;
    }

    char *eq = axl_strchr(p, '=');
    if (eq == NULL) {
        return;  /* no '=' — not a key=value line */
    }
    *eq = '\0';
    char *key   = axl_strstrip(p);
    char *value = axl_strstrip(eq + 1);
    if (*key == '\0') {
        return;  /* empty key after trimming */
    }

    axl_config_file_set(cf, key, value);
}

AxlConfigFile *
axl_config_file_load(const char *path)
{
    if (path == NULL) {
        return NULL;
    }

    AxlConfigFile *cf = config_file_alloc();
    if (cf == NULL) {
        return NULL;
    }

    char  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(path, (void **)&buf, &len) != AXL_OK
        || buf == NULL) {
        /* Missing / unreadable file is not an error — an empty map means
           every lookup returns its caller default. */
        return cf;
    }

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        size_t end = i;          /* exclusive, at '\n' or EOF */
        if (i < len) {
            i++;                 /* consume '\n' */
        }
        if (end > start && buf[end - 1] == '\r') {
            end--;               /* strip CRLF's '\r' */
        }
        config_file_parse_line(cf, buf + start, end - start);
    }

    axl_free(buf);
    return cf;
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

const char *
axl_config_file_get(AxlConfigFile *cf, const char *key, const char *def)
{
    if (cf == NULL || key == NULL) {
        return def;
    }
    const char *v = (const char *)axl_hash_table_lookup(cf->map, key);
    return (v != NULL) ? v : def;
}

uint64_t
axl_config_file_get_uint(AxlConfigFile *cf, const char *key, uint64_t def)
{
    const char *v = axl_config_file_get(cf, key, NULL);
    if (v == NULL) {
        return def;
    }
    uint64_t out = 0;
    return (axl_str_to_u64(v, 0, &out, NULL) == AXL_OK) ? out : def;
}

int64_t
axl_config_file_get_int(AxlConfigFile *cf, const char *key, int64_t def)
{
    const char *v = axl_config_file_get(cf, key, NULL);
    if (v == NULL) {
        return def;
    }
    int64_t out = 0;
    return (axl_str_to_s64(v, 0, &out, NULL) == AXL_OK) ? out : def;
}

bool
axl_config_file_get_bool(AxlConfigFile *cf, const char *key, bool def)
{
    const char *v = axl_config_file_get(cf, key, NULL);
    if (v == NULL) {
        return def;
    }
    if (axl_strcasecmp(v, "true") == 0 || axl_strcasecmp(v, "yes") == 0
        || axl_strcasecmp(v, "on") == 0 || axl_strcmp(v, "1") == 0) {
        return true;
    }
    if (axl_strcasecmp(v, "false") == 0 || axl_strcasecmp(v, "no") == 0
        || axl_strcasecmp(v, "off") == 0 || axl_strcmp(v, "0") == 0) {
        return false;
    }
    return def;
}

// ---------------------------------------------------------------------------
// Mutation + persistence
// ---------------------------------------------------------------------------

int
axl_config_file_set(AxlConfigFile *cf, const char *key, const char *value)
{
    if (cf == NULL || key == NULL || value == NULL || key[0] == '\0') {
        return AXL_ERR;
    }

    char *k = axl_strdup(key);
    char *v = axl_strdup(value);
    if (k == NULL || v == NULL) {
        axl_free(k);
        axl_free(v);
        return AXL_ERR;
    }

    /* replace: destroys any old key+value, keeps our k/v. On OOM the table
       took neither, so we free both. */
    if (axl_hash_table_replace(cf->map, k, v) == AXL_HASH_TABLE_ERR) {
        axl_free(k);
        axl_free(v);
        return AXL_ERR;
    }
    return AXL_OK;
}

typedef struct {
    AxlString *out;
    bool       ok;
} ConfigFileSaveCtx;

static void
config_file_save_cb(const void *key, void *value, void *data)
{
    ConfigFileSaveCtx *ctx = (ConfigFileSaveCtx *)data;
    if (!ctx->ok) {
        return;
    }
    if (axl_string_append_printf(ctx->out, "%s=%s\n",
                                 (const char *)key,
                                 (const char *)value) != AXL_OK) {
        ctx->ok = false;
    }
}

int
axl_config_file_save(AxlConfigFile *cf, const char *path)
{
    if (cf == NULL || path == NULL) {
        return AXL_ERR;
    }

    AxlString *out = axl_string_new("");
    if (out == NULL) {
        return AXL_ERR;
    }

    ConfigFileSaveCtx ctx = { out, true };
    axl_hash_table_foreach(cf->map, config_file_save_cb, &ctx);
    if (!ctx.ok) {
        axl_string_free(out);
        return AXL_ERR;
    }

    int rc = axl_file_set_contents(path, axl_string_str(out),
                                   axl_string_len(out));
    axl_string_free(out);
    return rc;
}
