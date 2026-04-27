/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-build.c
    Streaming JSON writer, AxlString-backed, with optional pretty-print.
    Orthogonal calls — containers, keys, atoms — driven by a single
    state machine that tracks depth + object-vs-array context per
    level + comma + expecting-value.
**/

#include <axl/axl-json.h>
#include <axl/axl-string.h>
#include <axl/axl-log.h>

#define JSMN_HEADER
#include "jsmn.h"

AXL_LOG_DOMAIN("json");

// ---------------------------------------------------------------------------
// Number formatting helpers (no allocation)
// ---------------------------------------------------------------------------

static size_t
u64_to_str(char *buf, size_t buf_size, uint64_t val)
{
    char tmp[21];
    int  pos = 0;

    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0) {
            tmp[pos++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    if ((size_t)pos >= buf_size) {
        pos = (int)buf_size - 1;
    }
    for (int i = 0; i < pos; i++) {
        buf[i] = tmp[pos - 1 - i];
    }
    buf[pos] = '\0';
    return (size_t)pos;
}

static size_t
i64_to_str(char *buf, size_t buf_size, int64_t val)
{
    if (val < 0) {
        buf[0] = '-';
        return 1 + u64_to_str(buf + 1, buf_size - 1,
                              (uint64_t)(~(uint64_t)val + 1));
    }
    return u64_to_str(buf, buf_size, (uint64_t)val);
}

static size_t
u64_to_hex(char *buf, size_t buf_size, uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[17];
    int  pos = 0;

    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0) {
            tmp[pos++] = hex[val & 0xf];
            val >>= 4;
        }
    }
    if ((size_t)pos + 2 >= buf_size) {
        /* Truncation guard: emit a syntactically valid placeholder so callers
         * that pass the buffer to emit_quoted never read uninitialized data. */
        if (buf_size >= 4) {
            buf[0] = '0'; buf[1] = 'x'; buf[2] = '0'; buf[3] = '\0';
            return 3;
        }
        if (buf_size > 0) buf[0] = '\0';
        return 0;
    }
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < pos; i++) {
        buf[2 + i] = tmp[pos - 1 - i];
    }
    buf[2 + pos] = '\0';
    return (size_t)2 + (size_t)pos;
}

// ---------------------------------------------------------------------------
// JSON String Escaping (utility, public)
// ---------------------------------------------------------------------------

int
axl_json_escape_string(const char *src, char *out, size_t size)
{
    size_t pos = 0;

    if (src == NULL || out == NULL || size < 3) {
        return -1;
    }

#define ESC_APPEND_CHAR(c) do {           \
    if (pos >= size - 1) return -1;       \
    out[pos++] = (c);                     \
} while (0)

#define ESC_APPEND_STR(s) do {                          \
    for (const char *_p = (s); *_p != '\0'; _p++) {     \
        ESC_APPEND_CHAR(*_p);                           \
    }                                                   \
} while (0)

    ESC_APPEND_CHAR('"');

    while (*src != '\0') {
        char ch = *src++;
        if (ch == '"')       { ESC_APPEND_STR("\\\""); }
        else if (ch == '\\') { ESC_APPEND_STR("\\\\"); }
        else if (ch == '\n') { ESC_APPEND_STR("\\n");  }
        else if (ch == '\r') { ESC_APPEND_STR("\\r");  }
        else if (ch == '\t') { ESC_APPEND_STR("\\t");  }
        else if (ch < 0x20)  { /* skip */               }
        else                 { ESC_APPEND_CHAR(ch);     }
    }

    ESC_APPEND_CHAR('"');
    out[pos] = '\0';

#undef ESC_APPEND_CHAR
#undef ESC_APPEND_STR

    return (int)pos;
}

// ---------------------------------------------------------------------------
// Internal: low-level append (sets sticky error on AxlString OOM)
// ---------------------------------------------------------------------------

static void
wr_chr(AxlJsonWriter *w, char c)
{
    if (w->error) return;
    if (axl_string_append_c(w->out, c) != 0) w->error = true;
}

static void
wr_str(AxlJsonWriter *w, const char *s)
{
    if (w->error) return;
    if (axl_string_append(w->out, s) != 0) w->error = true;
}

static void
wr_strn(AxlJsonWriter *w, const char *s, size_t n)
{
    if (w->error) return;
    if (axl_string_append_len(w->out, s, n) != 0) w->error = true;
}

// ---------------------------------------------------------------------------
// Internal: state-machine helpers
// ---------------------------------------------------------------------------

static bool
is_pretty(const AxlJsonWriter *w)
{
    return (w->flags & AXL_JSON_WRITER_PRETTY) != 0;
}

static bool
current_is_array(const AxlJsonWriter *w)
{
    if (w->depth == 0) return false;
    return (w->in_array_bits & (1u << (w->depth - 1))) != 0;
}

static void
emit_indent(AxlJsonWriter *w, uint32_t depth)
{
    if (!is_pretty(w)) return;
    wr_chr(w, '\n');
    for (uint32_t i = 0; i < depth; i++) {
        wr_strn(w, "  ", 2);
    }
}

/* Emit comma + indent before the next item. Returns true if the caller
 * should proceed; false if the writer is in an error state. Honors the
 * "value-after-key is inline" rule (no comma, no indent). */
static bool
begin_item(AxlJsonWriter *w)
{
    if (w->error) return false;

    if (w->expecting_value) {
        return true;
    }

    /* At depth 0, a second value isn't valid JSON. */
    if (w->depth == 0 && w->needs_comma) {
        w->error = true;
        return false;
    }

    if (w->needs_comma) {
        wr_chr(w, ',');
    }
    if (w->depth > 0) {
        emit_indent(w, w->depth);
    }
    return true;
}

/* Mark that a value was just emitted. */
static void
finish_value(AxlJsonWriter *w)
{
    w->needs_comma     = true;
    w->expecting_value = false;
}

/* Push a new container at depth+1. Caller has already validated context
 * and emitted the opening brace/bracket. */
static void
push_container(AxlJsonWriter *w, bool is_array)
{
    if (w->depth >= AXL_JSON_WRITER_MAX_DEPTH) {
        w->error = true;
        return;
    }
    w->depth++;
    if (is_array) {
        w->in_array_bits |= (1u << (w->depth - 1));
    } else {
        w->in_array_bits &= ~(1u << (w->depth - 1));
    }
    w->needs_comma     = false;
    w->expecting_value = false;
}

/* Pop a container. After pop, we're back in the outer container; the
 * just-closed container counts as a value at the outer level. */
static void
pop_container(AxlJsonWriter *w)
{
    if (w->depth == 0) {
        w->error = true;
        return;
    }
    /* Pretty: dedent before close brace, but only if container had items. */
    if (is_pretty(w) && w->needs_comma) {
        emit_indent(w, w->depth - 1);
    }
    w->depth--;
    finish_value(w);
}

/* Emit a quoted, escaped string. */
static void
emit_quoted(AxlJsonWriter *w, const char *s)
{
    wr_chr(w, '"');
    if (w->error) return;
    char ch;
    while ((ch = *s++) != '\0') {
        if (ch == '"')       wr_strn(w, "\\\"", 2);
        else if (ch == '\\') wr_strn(w, "\\\\", 2);
        else if (ch == '\n') wr_strn(w, "\\n",  2);
        else if (ch == '\r') wr_strn(w, "\\r",  2);
        else if (ch == '\t') wr_strn(w, "\\t",  2);
        else if (ch < 0x20)  { /* skip control chars */ }
        else                 wr_chr(w, ch);
    }
    wr_chr(w, '"');
}

static void
emit_quoted_n(AxlJsonWriter *w, const char *s, size_t n)
{
    wr_chr(w, '"');
    if (w->error) return;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '"')       wr_strn(w, "\\\"", 2);
        else if (ch == '\\') wr_strn(w, "\\\\", 2);
        else if (ch == '\n') wr_strn(w, "\\n",  2);
        else if (ch == '\r') wr_strn(w, "\\r",  2);
        else if (ch == '\t') wr_strn(w, "\\t",  2);
        else if (ch < 0x20)  { /* skip */ }
        else                 wr_chr(w, ch);
    }
    wr_chr(w, '"');
}

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

void
axl_json_writer_init(AxlJsonWriter *w, AxlString *out, uint32_t flags)
{
    if (w == NULL) return;
    w->out             = out;
    w->flags           = flags;
    w->depth           = 0;
    w->in_array_bits   = 0;
    w->needs_comma     = false;
    w->expecting_value = false;
    w->error           = (out == NULL);
}

size_t
axl_json_writer_finish(AxlJsonWriter *w)
{
    if (w == NULL || w->out == NULL) return 0;
    if (w->depth != 0) {
        /* Unclosed container(s) at finish — sticky error. */
        w->error = true;
    }
    return axl_string_len(w->out);
}

bool
axl_json_writer_error(const AxlJsonWriter *w)
{
    return w == NULL || w->error;
}

// ---------------------------------------------------------------------------
// Public API: containers
// ---------------------------------------------------------------------------

void
axl_json_obj_begin(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    /* In object context, a value can only follow a key. */
    if (w->depth > 0 && !current_is_array(w) && !w->expecting_value) {
        w->error = true;
        return;
    }
    if (!begin_item(w)) return;
    wr_chr(w, '{');
    push_container(w, false);
}

void
axl_json_obj_end(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    if (w->depth == 0 || current_is_array(w) || w->expecting_value) {
        w->error = true;
        return;
    }
    pop_container(w);
    wr_chr(w, '}');
}

void
axl_json_arr_begin(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    if (w->depth > 0 && !current_is_array(w) && !w->expecting_value) {
        w->error = true;
        return;
    }
    if (!begin_item(w)) return;
    wr_chr(w, '[');
    push_container(w, true);
}

void
axl_json_arr_end(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    if (w->depth == 0 || !current_is_array(w)) {
        w->error = true;
        return;
    }
    pop_container(w);
    wr_chr(w, ']');
}

// ---------------------------------------------------------------------------
// Public API: keys
// ---------------------------------------------------------------------------

/* Internal: validate object-key context, emit comma + indent. Returns
 * true if the caller should proceed to emit the key bytes themselves. */
static bool
key_prefix(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return false;
    if (w->depth == 0 || current_is_array(w) || w->expecting_value) {
        w->error = true;
        return false;
    }
    if (w->needs_comma) {
        wr_chr(w, ',');
    }
    emit_indent(w, w->depth);
    return true;
}

/* Internal: emit the colon and (pretty) space after a key's quoted form,
 * and update state so the next call expects a value. */
static void
key_suffix(AxlJsonWriter *w)
{
    wr_chr(w, ':');
    if (is_pretty(w)) {
        wr_chr(w, ' ');
    }
    w->needs_comma     = false;
    w->expecting_value = true;
}

void
axl_json_key(AxlJsonWriter *w, const char *key)
{
    if (key == NULL) {
        if (w != NULL) w->error = true;
        return;
    }
    if (!key_prefix(w)) return;
    emit_quoted(w, key);
    key_suffix(w);
}

void
axl_json_keyn(AxlJsonWriter *w, const char *key, size_t n)
{
    if (key == NULL) {
        if (w != NULL) w->error = true;
        return;
    }
    if (!key_prefix(w)) return;
    emit_quoted_n(w, key, n);
    key_suffix(w);
}

/* Internal: emit a key whose bytes are spliced verbatim from a parsed
 * source (jsmn keeps escape sequences in source form). Used by the
 * parse→write bridge. */
static void
key_raw(AxlJsonWriter *w, const char *src, size_t n)
{
    if (!key_prefix(w)) return;
    wr_chr(w, '"');
    wr_strn(w, src, n);
    wr_chr(w, '"');
    key_suffix(w);
}

// ---------------------------------------------------------------------------
// Public API: atoms
// ---------------------------------------------------------------------------

static bool
check_atom_context(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return false;
    /* Bare-primitive root is rejected to mirror axl_json_parse, which
     * requires the root token to be an object or array. */
    if (w->depth == 0) {
        w->error = true;
        return false;
    }
    if (!current_is_array(w) && !w->expecting_value) {
        w->error = true;
        return false;
    }
    return true;
}

void
axl_json_str(AxlJsonWriter *w, const char *s)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    if (s == NULL) {
        wr_strn(w, "null", 4);
    } else {
        emit_quoted(w, s);
    }
    finish_value(w);
}

void
axl_json_strn(AxlJsonWriter *w, const char *s, size_t n)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    if (s == NULL) {
        wr_strn(w, "null", 4);
    } else {
        emit_quoted_n(w, s, n);
    }
    finish_value(w);
}

void
axl_json_int(AxlJsonWriter *w, int64_t v)
{
    char buf[24];
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    i64_to_str(buf, sizeof(buf), v);
    wr_str(w, buf);
    finish_value(w);
}

void
axl_json_uint(AxlJsonWriter *w, uint64_t v)
{
    char buf[24];
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    u64_to_str(buf, sizeof(buf), v);
    wr_str(w, buf);
    finish_value(w);
}

void
axl_json_bool(AxlJsonWriter *w, bool v)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    wr_str(w, v ? "true" : "false");
    finish_value(w);
}

void
axl_json_null(AxlJsonWriter *w)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    wr_strn(w, "null", 4);
    finish_value(w);
}

void
axl_json_hex(AxlJsonWriter *w, uint64_t v)
{
    char buf[20];
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    u64_to_hex(buf, sizeof(buf), v);
    emit_quoted(w, buf);
    finish_value(w);
}

void
axl_json_raw(AxlJsonWriter *w, const char *fragment)
{
    if (!check_atom_context(w)) return;
    if (fragment == NULL) {
        w->error = true;
        return;
    }
    if (!begin_item(w)) return;
    wr_str(w, fragment);
    finish_value(w);
}

// ---------------------------------------------------------------------------
// Public API: convenience kv pairs
// ---------------------------------------------------------------------------

void
axl_json_kv_str(AxlJsonWriter *w, const char *key, const char *value)
{
    axl_json_key(w, key);
    axl_json_str(w, value);
}

void
axl_json_kv_strn(AxlJsonWriter *w, const char *key,
                 const char *value, size_t value_n)
{
    axl_json_key(w, key);
    axl_json_strn(w, value, value_n);
}

void
axl_json_kv_int(AxlJsonWriter *w, const char *key, int64_t value)
{
    axl_json_key(w, key);
    axl_json_int(w, value);
}

void
axl_json_kv_uint(AxlJsonWriter *w, const char *key, uint64_t value)
{
    axl_json_key(w, key);
    axl_json_uint(w, value);
}

void
axl_json_kv_bool(AxlJsonWriter *w, const char *key, bool value)
{
    axl_json_key(w, key);
    axl_json_bool(w, value);
}

void
axl_json_kv_null(AxlJsonWriter *w, const char *key)
{
    axl_json_key(w, key);
    axl_json_null(w);
}

void
axl_json_kv_hex(AxlJsonWriter *w, const char *key, uint64_t value)
{
    axl_json_key(w, key);
    axl_json_hex(w, value);
}

// ---------------------------------------------------------------------------
// Public API: parse → write bridge
// ---------------------------------------------------------------------------

/* Walk one token and emit its JSON form. Returns the index of the next
 * token after the subtree rooted at @p idx. */
static int
write_token_walk(AxlJsonWriter *w, const AxlJsonReader *r, int idx)
{
    if (w->error) return idx;
    if (idx < 0 || idx >= r->token_count) {
        w->error = true;
        return idx;
    }
    const jsmntok_t *toks = (const jsmntok_t *)r->tokens;
    const jsmntok_t *t    = &toks[idx];

    if (t->type == JSMN_OBJECT) {
        axl_json_obj_begin(w);
        int next = idx + 1;
        for (int i = 0; i < t->size; i++) {
            const jsmntok_t *kt = &toks[next];
            if (kt->type != JSMN_STRING) {
                w->error = true;
                return next;
            }
            key_raw(w, r->json + kt->start, (size_t)(kt->end - kt->start));
            next = write_token_walk(w, r, next + 1);
        }
        axl_json_obj_end(w);
        return next;
    }

    if (t->type == JSMN_ARRAY) {
        axl_json_arr_begin(w);
        int next = idx + 1;
        for (int i = 0; i < t->size; i++) {
            next = write_token_walk(w, r, next);
        }
        axl_json_arr_end(w);
        return next;
    }

    if (t->type == JSMN_STRING) {
        if (!check_atom_context(w)) return idx + 1;
        if (!begin_item(w)) return idx + 1;
        /* Splice string bytes verbatim, surrounded by quotes — jsmn keeps
         * escape sequences in the source so this preserves the original
         * representation without re-escaping. */
        wr_chr(w, '"');
        wr_strn(w, r->json + t->start, (size_t)(t->end - t->start));
        wr_chr(w, '"');
        finish_value(w);
        return idx + 1;
    }

    if (t->type == JSMN_PRIMITIVE) {
        if (!check_atom_context(w)) return idx + 1;
        if (!begin_item(w)) return idx + 1;
        wr_strn(w, r->json + t->start, (size_t)(t->end - t->start));
        finish_value(w);
        return idx + 1;
    }

    w->error = true;
    return idx + 1;
}

void
axl_json_write_token(AxlJsonWriter *w, const AxlJsonReader *r, int tok_idx)
{
    if (w == NULL || w->error || r == NULL || r->tokens == NULL) {
        if (w != NULL) w->error = true;
        return;
    }
    if (tok_idx < 0 || tok_idx >= r->token_count) {
        w->error = true;
        return;
    }
    write_token_walk(w, r, tok_idx);
}
