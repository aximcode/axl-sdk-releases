/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-config-file.c
    Free-form key=value config-file map — the open-vocabulary counterpart
    to the descriptor-bound AxlConfig. Parses a text file into a flat
    string->string hash map and serves typed getters with caller defaults.

    A map created with AXL_CONFIG_FILE_OPENSSH parses OpenSSH sshd_config(5)
    lines instead and also keeps every directive, in order, as an entry.
**/

#include <axl/axl-config-file.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-fs.h>
#include <axl/axl-mem.h>
#include <axl/axl-array.h>
#include "../fs/axl-fs-internal.h"

/* The per-line cap is public (AXL_CONFIG_FILE_LINE_MAX). Real keys and
   values are well under it; a key=value line at or beyond the cap is
   treated as malformed and skipped rather than half-parsed, and an
   OpenSSH line there is refused. */

/* OpenSSH whitespace. A CR survives line splitting only when it is not the
   one before an LF, and argv_split's isspace() treats it as a separator. */
#define CFO_IS_WS(c)  ((c) == ' ' || (c) == '\t' || (c) == '\r')

/* Most arguments one line can hold: each needs one byte and one separator,
   and a line is shorter than the cap. */
#define CFO_ARGS_MAX  (AXL_CONFIG_FILE_LINE_MAX / 2u + 1u)

struct AxlConfigFile {
    AxlHashTable        *map;      /* owned char* key -> owned char* value */
    AxlConfigFileSyntax  syntax;
    AxlArray            *entries;  /* OpenSSH only: AxlConfigFileEntry*, each
                                      one allocation with its strings */
};

static const char CFO_UNTERMINATED[] = "unterminated quoted string";
static const char CFO_TOO_LONG[]     = "line too long";
static const char CFO_NO_KEYWORD[]   = "missing keyword";
static const char CFO_BAD_UTF16[]    = "invalid UTF-16";
static const char CFO_NUL[]          = "NUL byte";

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/* sshd_config keywords are case-insensitive, so an OpenSSH map hashes and
   compares its keys ASCII-folded (FNV-1a over the folded bytes). */
static size_t
cfo_ci_hash(const void *key)
{
    size_t h = (size_t)2166136261u;
    for (const char *p = (const char *)key; *p != '\0'; p++) {
        h = (h ^ (size_t)(unsigned char)axl_tolower((unsigned char)*p))
            * (size_t)16777619u;
    }
    return h;
}

static bool
cfo_ci_equal(const void *a, const void *b)
{
    return axl_strcasecmp((const char *)a, (const char *)b) == 0;
}

static AxlConfigFile *
config_file_alloc(AxlConfigFileSyntax syntax)
{
    AxlConfigFile *cf = axl_calloc(1, sizeof(*cf));
    if (cf == NULL) {
        return NULL;
    }
    cf->syntax = syntax;
    if (syntax == AXL_CONFIG_FILE_OPENSSH) {
        cf->map = axl_hash_table_new_full(cfo_ci_hash, cfo_ci_equal,
                                          axl_free_impl, axl_free_impl);
        cf->entries = axl_array_new(sizeof(void *));
        if (cf->entries != NULL
            && axl_array_set_ptr_free_func(cf->entries, axl_free_impl)
                   != AXL_OK) {
            axl_array_free(cf->entries);
            cf->entries = NULL;
        }
        if (cf->map == NULL || cf->entries == NULL) {
            axl_hash_table_free(cf->map);
            axl_array_free(cf->entries);
            axl_free(cf);
            return NULL;
        }
        return cf;
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
    return config_file_alloc(AXL_CONFIG_FILE_KEY_VALUE);
}

AxlConfigFile *
axl_config_file_new_syntax(AxlConfigFileSyntax syntax)
{
    if (syntax != AXL_CONFIG_FILE_KEY_VALUE
        && syntax != AXL_CONFIG_FILE_OPENSSH) {
        return NULL;
    }
    return config_file_alloc(syntax);
}

void
axl_config_file_free(AxlConfigFile *cf)
{
    if (cf == NULL) {
        return;
    }
    axl_hash_table_free(cf->map);
    axl_array_free(cf->entries);
    axl_free(cf);
}

size_t
axl_config_file_entry_count(AxlConfigFile *cf)
{
    return (cf != NULL && cf->entries != NULL) ? axl_array_len(cf->entries)
                                               : 0;
}

const AxlConfigFileEntry *
axl_config_file_entry_at(AxlConfigFile *cf, size_t index)
{
    if (index >= axl_config_file_entry_count(cf)) {
        return NULL;
    }
    return (const AxlConfigFileEntry *)axl_array_get_ptr(cf->entries, index);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/* Parse one line (already stripped of its newline) into the map: split on
   the first '=', trim both halves, skip blanks and '#' comments.

   @return false only when a well-formed line could not be stored (OOM). */
static bool
config_file_parse_line(
    AxlConfigFile *cf,
    const char    *line,
    size_t         line_len
    )
{
    if (line_len == 0 || line_len >= AXL_CONFIG_FILE_LINE_MAX) {
        return true;  /* blank or over-long (malformed) */
    }

    char tmp[AXL_CONFIG_FILE_LINE_MAX];
    axl_memcpy(tmp, line, line_len);
    tmp[line_len] = '\0';

    /* Leading whitespace decides comment / blank without disturbing the
       split (axl_strstrip on each half handles the rest). */
    char *p = tmp;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '#') {
        return true;
    }

    char *eq = axl_strchr(p, '=');
    if (eq == NULL) {
        return true;  /* no '=' — not a key=value line */
    }
    *eq = '\0';
    char *key   = axl_strstrip(p);
    char *value = axl_strstrip(eq + 1);
    if (*key == '\0') {
        return true;  /* empty key after trimming */
    }

    return axl_config_file_set(cf, key, value) == AXL_OK;
}

/* Shared by axl_config_file_load (file bytes) and axl_config_file_parse_string
   (an in-memory NUL-terminated string): split @p buf/@p len into lines on
   '\n', strip a trailing '\r', and hand each line to config_file_parse_line.

   @return false when some line could not be stored (OOM); the remaining
       lines are still parsed, as they always were. */
static bool
config_file_parse_buf(AxlConfigFile *cf, const char *buf, size_t len)
{
    bool   stored = true;
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
        if (!config_file_parse_line(cf, buf + start, end - start)) {
            stored = false;
        }
    }
    return stored;
}

// ---------------------------------------------------------------------------
// OpenSSH sshd_config(5) grammar
// ---------------------------------------------------------------------------

/* Copy one token starting at line[*pos] into work[*w], quotes and escapes
   removed, NUL-terminated. The loop is OpenSSH 9.9p1 misc.c argv_split()'s
   inner loop; @p stop_at_eq also ends an unquoted token at '=', which is
   how the keyword ends (strdelim). Advances *pos past the token.

   @return false on an unterminated quote. */
static bool
cfo_token(
    const char *line,
    size_t      len,
    size_t     *pos,
    char       *work,
    size_t     *w,
    bool        stop_at_eq
    )
{
    size_t i     = *pos;
    char   quote = 0;
    for (; i < len; i++) {
        char c = line[i];
        if (c == '\\') {
            char n = (i + 1 < len) ? line[i + 1] : '\0';
            if (n == '\'' || n == '"' || n == '\\'
                || (quote == 0 && n == ' ')) {
                i++;
                work[(*w)++] = n;
            } else {
                work[(*w)++] = c;
            }
        } else if (quote == 0 && (CFO_IS_WS(c) || (stop_at_eq && c == '='))) {
            break;
        } else if (quote == 0 && (c == '"' || c == '\'')) {
            quote = c;
        } else if (quote != 0 && c == quote) {
            quote = 0;
        } else {
            work[(*w)++] = c;
        }
    }
    *pos = i;
    work[(*w)++] = '\0';
    return quote == 0;
}

/* Tokenize one OpenSSH line (terminator already removed) into a new entry:
   one allocation holding the AxlConfigFileEntry, its argv array and every
   string, so an entry is freed with one axl_free. The argument loop follows
   OpenSSH 9.9p1 misc.c argv_split() with terminate_on_comment set.

   @return AXL_OK with *out set, or with *out NULL for a line that holds no
       directive; AXL_INVALID with *reason set; AXL_NO_RESOURCES. */
static AxlStatus
cfo_tokenize_line(
    const char           *line,
    size_t                len,
    size_t                lineno,
    AxlConfigFileEntry  **out,
    const char          **reason
    )
{
    *out = NULL;
    for (size_t k = 0; k < len; k++) {
        if (line[k] == '\0') {
            *reason = CFO_NUL;
            return AXL_INVALID;
        }
    }
    if (len >= AXL_CONFIG_FILE_LINE_MAX) {
        *reason = CFO_TOO_LONG;
        return AXL_INVALID;
    }

    char   work[AXL_CONFIG_FILE_LINE_MAX + 1];   /* keyword\0arg\0arg\0... */
    size_t starts[CFO_ARGS_MAX];
    size_t w = 0;
    size_t i = 0;

    while (i < len && CFO_IS_WS(line[i])) {
        i++;
    }
    if (i == len || line[i] == '#') {
        return AXL_OK;   /* blank or comment */
    }

    if (!cfo_token(line, len, &i, work, &w, true)) {
        *reason = CFO_UNTERMINATED;
        return AXL_INVALID;
    }
    if (w == 1) {   /* only the NUL: nothing before '=', or `""` */
        *reason = CFO_NO_KEYWORD;
        return AXL_INVALID;
    }

    while (i < len && CFO_IS_WS(line[i])) {
        i++;
    }
    if (i < len && line[i] == '=') {
        i++;
    }

    size_t argc = 0;
    while (i < len) {
        if (CFO_IS_WS(line[i])) {
            i++;
            continue;
        }
        if (line[i] == '#') {
            break;   /* a comment starts where an argument would */
        }
        starts[argc++] = w;
        if (!cfo_token(line, len, &i, work, &w, false)) {
            *reason = CFO_UNTERMINATED;
            return AXL_INVALID;
        }
    }

    size_t head = sizeof(AxlConfigFileEntry) + argc * sizeof(char *);
    char  *block = axl_malloc(head + w);
    if (block == NULL) {
        return AXL_NO_RESOURCES;
    }
    AxlConfigFileEntry *e    = (AxlConfigFileEntry *)block;
    const char        **argv = (const char **)(block + sizeof(*e));
    char               *strs = block + head;
    axl_memcpy(strs, work, w);
    for (size_t a = 0; a < argc; a++) {
        argv[a] = strs + starts[a];
    }
    e->keyword = strs;
    e->argc    = argc;
    e->argv    = argv;
    e->line    = lineno;
    *out = e;
    return AXL_OK;
}

/* Copy every new entry's FIRST-occurrence value into the map: keyword ->
   arguments joined by one space. A keyword already present (an earlier
   parse, an earlier line, axl_config_file_set) is left alone. */
static AxlStatus
cfo_apply_values(
    AxlConfigFile *cf,
    size_t         base
    )
{
    size_t n = axl_array_len(cf->entries);
    for (size_t i = base; i < n; i++) {
        const AxlConfigFileEntry *e =
            (const AxlConfigFileEntry *)axl_array_get_ptr(cf->entries, i);
        if (axl_hash_table_lookup(cf->map, e->keyword) != NULL) {
            continue;
        }
        size_t vlen = 0;
        for (size_t a = 0; a < e->argc; a++) {
            vlen += axl_strlen(e->argv[a]) + 1;
        }
        char *k = axl_strdup(e->keyword);
        char *v = axl_malloc(vlen > 0 ? vlen : 1);
        if (k == NULL || v == NULL) {
            axl_free(k);
            axl_free(v);
            return AXL_NO_RESOURCES;
        }
        size_t off = 0;
        for (size_t a = 0; a < e->argc; a++) {
            if (a > 0) {
                v[off++] = ' ';
            }
            size_t al = axl_strlen(e->argv[a]);
            axl_memcpy(v + off, e->argv[a], al);
            off += al;
        }
        v[off] = '\0';
        if (axl_hash_table_insert(cf->map, k, v) == AXL_HASH_TABLE_ERR) {
            axl_free(k);
            axl_free(v);
            return AXL_NO_RESOURCES;
        }
    }
    return AXL_OK;
}

/* Parse @p len bytes of OpenSSH text into @p cf, all or nothing: every line
   tokenizes into new entries first; on an error they are dropped again and
   the map is not touched. Only then are the values applied.

   @p allow_utf16 is false for axl_config_file_parse_string, whose
   NUL-terminated text cannot carry UTF-16. */
static AxlStatus
cfo_parse(
    AxlConfigFile      *cf,
    const char         *data,
    size_t              len,
    bool                allow_utf16,
    AxlConfigFileError *err
    )
{
    char  *owned    = NULL;
    size_t bad_line = 0;

    if (allow_utf16) {
        /* A file: UTF-16LE + BOM (what the Shell writes) or UTF-8 with or
           without a BOM, as axl_text_to_utf8 reads every text file. UTF-16
           that does not convert is reported at the line its first bad unit
           falls on; the lines before it still parse. An encoded U+0000
           becomes a NUL byte, which the tokenizer refuses on its own line. */
        size_t tlen = 0;
        size_t bad  = 0;
        int    trc  = axl_text_to_utf8(data, len, &owned, &tlen, &bad);
        if (owned == NULL) {
            return trc == AXL_INVALID ? AXL_ERR : trc;
        }
        if (trc == AXL_INVALID) {
            bad_line = 1;
            for (size_t k = 0; k < bad; k++) {
                bad_line += owned[k] == '\n';
            }
        }
        data = owned;
        len  = tlen;
    } else if (len >= 3 && (unsigned char)data[0] == 0xEF
               && (unsigned char)data[1] == 0xBB
               && (unsigned char)data[2] == 0xBF) {
        data += 3;
        len  -= 3;
    }

    size_t      base   = axl_array_len(cf->entries);
    size_t      lineno = 0;
    size_t      i      = 0;
    AxlStatus   rc     = AXL_OK;
    const char *reason = NULL;

    while (i < len && rc == AXL_OK) {
        size_t start = i;
        while (i < len && data[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;
        }
        if (end > start && data[end - 1] == '\r') {
            end--;
        }
        lineno++;
        if (bad_line != 0 && lineno >= bad_line) {
            break;
        }
        AxlConfigFileEntry *e = NULL;
        rc = cfo_tokenize_line(data + start, end - start, lineno, &e, &reason);
        if (rc == AXL_OK && e != NULL
            && axl_array_append_ptr(cf->entries, e) != AXL_OK) {
            axl_free(e);
            rc = AXL_NO_RESOURCES;
        }
    }
    if (rc == AXL_OK && bad_line != 0) {
        rc     = AXL_INVALID;
        reason = CFO_BAD_UTF16;
        lineno = bad_line;
    }
    if (rc == AXL_OK) {
        rc = cfo_apply_values(cf, base);
    }
    if (rc != AXL_OK) {
        /* Shrinking runs the pointer-free hook: the new entries go away. */
        axl_array_set_size(cf->entries, base);
        if (rc == AXL_INVALID && err != NULL) {
            err->line   = lineno;
            err->reason = reason;
        }
    }
    axl_free(owned);
    return rc;
}

AxlConfigFile *
axl_config_file_load(const char *path)
{
    if (path == NULL) {
        return NULL;
    }

    AxlConfigFile *cf = config_file_alloc(AXL_CONFIG_FILE_KEY_VALUE);
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

    /* A line that could not be stored is dropped, as it always was: this
       loader has no channel to report it. axl_config_file_read() does. */
    config_file_parse_buf(cf, buf, len);

    axl_free(buf);
    return cf;
}

int
axl_config_file_parse_string(AxlConfigFile *cf, const char *text)
{
    if (cf == NULL || text == NULL) {
        return AXL_ERR;
    }
    if (cf->syntax == AXL_CONFIG_FILE_OPENSSH) {
        return (cfo_parse(cf, text, axl_strlen(text), false, NULL) == AXL_OK)
            ? AXL_OK : AXL_ERR;
    }
    config_file_parse_buf(cf, text, axl_strlen(text));
    return AXL_OK;
}

/* Clear @p err to the no-error state the header promises. */
static void
cfo_err_clear(AxlConfigFileError *err)
{
    if (err != NULL) {
        err->line   = 0;
        err->reason = "";
    }
}

AxlStatus
axl_config_file_parse_bytes(
    AxlConfigFile      *cf,
    const void         *data,
    size_t              len,
    AxlConfigFileError *err
    )
{
    cfo_err_clear(err);
    if (cf == NULL || (data == NULL && len != 0)) {
        return AXL_INVALID;
    }
    if (len == 0) {
        return AXL_OK;
    }
    if (cf->syntax == AXL_CONFIG_FILE_OPENSSH) {
        return cfo_parse(cf, (const char *)data, len, true, err);
    }
    return config_file_parse_buf(cf, (const char *)data, len)
        ? AXL_OK : AXL_NO_RESOURCES;
}

AxlStatus
axl_config_file_read(
    AxlConfigFile      *cf,
    const char         *path,
    AxlConfigFileError *err
    )
{
    cfo_err_clear(err);
    if (cf == NULL || path == NULL || path[0] == '\0') {
        return AXL_INVALID;
    }
    void     *buf = NULL;
    size_t    len = 0;
    AxlStatus rc  = axl_file_get_contents_status(path, true, &buf, &len);
    if (rc != AXL_OK) {
        return rc;
    }
    rc = axl_config_file_parse_bytes(cf, buf, len, err);
    axl_free(buf);
    return rc;
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

    /* key=value: replace destroys any old key+value and keeps our k/v.
       OpenSSH: insert keeps the key as first written and frees ours. On OOM
       the table took neither, so we free both. */
    AxlHashTableInsertResult r = (cf->syntax == AXL_CONFIG_FILE_OPENSSH)
        ? axl_hash_table_insert(cf->map, k, v)
        : axl_hash_table_replace(cf->map, k, v);
    if (r == AXL_HASH_TABLE_ERR) {
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

/* Shared by axl_config_file_save (file) and axl_config_file_to_string
   (caller buffer): build the full "key=value\n"-per-entry text. Returns a
   new AxlString the caller frees with axl_string_free, or NULL on OOM /
   NULL @p cf. */
static AxlString *
config_file_build_string(AxlConfigFile *cf)
{
    /* The serializer writes key=value only; an OpenSSH map would not
       round-trip through it. */
    if (cf == NULL || cf->syntax == AXL_CONFIG_FILE_OPENSSH) {
        return NULL;
    }

    AxlString *out = axl_string_new("");
    if (out == NULL) {
        return NULL;
    }

    ConfigFileSaveCtx ctx = { out, true };
    axl_hash_table_foreach(cf->map, config_file_save_cb, &ctx);
    if (!ctx.ok) {
        axl_string_free(out);
        return NULL;
    }
    return out;
}

int
axl_config_file_save(AxlConfigFile *cf, const char *path)
{
    if (path == NULL) {
        return AXL_ERR;
    }

    AxlString *out = config_file_build_string(cf);
    if (out == NULL) {
        return AXL_ERR;
    }

    int rc = axl_file_set_contents(path, axl_string_str(out),
                                   axl_string_len(out));
    axl_string_free(out);
    return rc;
}

int
axl_config_file_to_string(AxlConfigFile *cf, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return AXL_ERR;
    }

    AxlString *out = config_file_build_string(cf);
    if (out == NULL) {
        return AXL_ERR;
    }

    size_t len = axl_string_len(out);
    if (len + 1 > cap) {
        axl_string_free(out);
        return AXL_ERR;   /* would not fit -- buf left unmodified */
    }
    axl_memcpy(buf, axl_string_str(out), len);
    buf[len] = '\0';
    axl_string_free(out);
    return AXL_OK;
}
