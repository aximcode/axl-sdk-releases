/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-build.c
    Buffer-based JSON builder. No dynamic allocation.
**/

#include <axl/axl-json.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("json");

// ---------------------------------------------------------------------------
// Local helpers (no EDK2 dependencies)
// ---------------------------------------------------------------------------

static size_t
u64_to_str(char *buf, size_t buf_size, uint64_t val)
{
    char tmp[21];
    int pos = 0;

    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0) {
            tmp[pos++] = '0' + (val % 10);
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
        return 1 + u64_to_str(buf + 1, buf_size - 1, (uint64_t)(~(uint64_t)val + 1));
    }
    return u64_to_str(buf, buf_size, (uint64_t)val);
}

static size_t
u64_to_hex(char *buf, size_t buf_size, uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[17];
    int pos = 0;

    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0) {
            tmp[pos++] = hex[val & 0xf];
            val >>= 4;
        }
    }
    if ((size_t)pos + 2 >= buf_size) {
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
// Internal Helpers
// ---------------------------------------------------------------------------

static void
j_append(AxlJsonBuilder *j, const char *str)
{
    while (*str != '\0' && j->pos < j->size - 1) {
        j->buffer[j->pos++] = *str++;
    }
    if (*str != '\0') {
        j->overflow = true;
    }
}

static void
j_append_char(AxlJsonBuilder *j, char ch)
{
    if (j->pos < j->size - 1) {
        j->buffer[j->pos++] = ch;
    } else {
        j->overflow = true;
    }
}

static void
j_comma(AxlJsonBuilder *j)
{
    if (j->need_comma) {
        j_append_char(j, ',');
    }
}

static void
j_escaped(AxlJsonBuilder *j, const char *str)
{
    char ch;

    j_append_char(j, '"');
    while ((ch = *str++) != '\0') {
        if (ch == '"') {
            j_append(j, "\\\"");
        } else if (ch == '\\') {
            j_append(j, "\\\\");
        } else if (ch == '\n') {
            j_append(j, "\\n");
        } else if (ch == '\r') {
            j_append(j, "\\r");
        } else if (ch == '\t') {
            j_append(j, "\\t");
        } else if (ch < 0x20) {
            /* Skip other control characters */
        } else {
            j_append_char(j, ch);
        }
    }
    j_append_char(j, '"');
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_json_escape_string(const char *src, char *out, size_t size)
{
    size_t pos = 0;

    if (src == NULL || out == NULL || size < 3) {
        return -1;
    }

#define ESC_APPEND_CHAR(c) do { \
    if (pos >= size - 1) return -1; \
    out[pos++] = (c); \
} while (0)

#define ESC_APPEND_STR(s) do { \
    for (const char *_p = (s); *_p != '\0'; _p++) { \
        ESC_APPEND_CHAR(*_p); \
    } \
} while (0)

    ESC_APPEND_CHAR('"');

    while (*src != '\0') {
        char ch = *src++;
        if (ch == '"') {
            ESC_APPEND_STR("\\\"");
        } else if (ch == '\\') {
            ESC_APPEND_STR("\\\\");
        } else if (ch == '\n') {
            ESC_APPEND_STR("\\n");
        } else if (ch == '\r') {
            ESC_APPEND_STR("\\r");
        } else if (ch == '\t') {
            ESC_APPEND_STR("\\t");
        } else if (ch < 0x20) {
            /* skip control characters */
        } else {
            ESC_APPEND_CHAR(ch);
        }
    }

    ESC_APPEND_CHAR('"');
    out[pos] = '\0';

#undef ESC_APPEND_CHAR
#undef ESC_APPEND_STR

    return (int)pos;
}

void
axl_json_init(AxlJsonBuilder *j, char *buffer, size_t size)
{
    j->buffer = buffer;
    j->size = (size < 2) ? 2 : size;
    j->pos = 0;
    j->need_comma = false;
    j->overflow = false;
    buffer[0] = '\0';
}

void
axl_json_object_start(AxlJsonBuilder *j)
{
    j_comma(j);
    j_append_char(j, '{');
    j->need_comma = false;
}

void
axl_json_object_end(AxlJsonBuilder *j)
{
    j_append_char(j, '}');
    j->buffer[j->pos] = '\0';
    j->need_comma = true;
}

void
axl_json_object_start_named(AxlJsonBuilder *j, const char *key)
{
    j_comma(j);
    j_escaped(j, key);
    j_append_char(j, ':');
    j_append_char(j, '{');
    j->need_comma = false;
}

void
axl_json_array_start(AxlJsonBuilder *j, const char *key)
{
    j_comma(j);
    j_escaped(j, key);
    j_append_char(j, ':');
    j_append_char(j, '[');
    j->need_comma = false;
}

void
axl_json_array_end(AxlJsonBuilder *j)
{
    j_append_char(j, ']');
    j->buffer[j->pos] = '\0';
    j->need_comma = true;
}

void
axl_json_array_object_start(AxlJsonBuilder *j)
{
    j_comma(j);
    j_append_char(j, '{');
    j->need_comma = false;
}

void
axl_json_array_add_string(AxlJsonBuilder *j, const char *value)
{
    j_comma(j);
    if (value != NULL) {
        j_escaped(j, value);
    } else {
        j_append(j, "null");
    }
    j->need_comma = true;
}

void
axl_json_add_string(AxlJsonBuilder *j, const char *key, const char *value)
{
    j_comma(j);
    j_escaped(j, key);
    j_append_char(j, ':');
    if (value != NULL) {
        j_escaped(j, value);
    } else {
        j_append(j, "null");
    }
    j->need_comma = true;
}

void
axl_json_add_uint(AxlJsonBuilder *j, const char *key, uint64_t value)
{
    char num[24];

    j_comma(j);
    j_escaped(j, key);
    j_append_char(j, ':');
    u64_to_str(num, sizeof (num), value);
    j_append(j, num);
    j->need_comma = true;
}

void
axl_json_add_int(AxlJsonBuilder *j, const char *key, int64_t value)
{
    char num[24];

    j_comma(j);
    j_escaped(j, key);
    j_append_char(j, ':');
    i64_to_str(num, sizeof (num), value);
    j_append(j, num);
    j->need_comma = true;
}

void
axl_json_add_bool(AxlJsonBuilder *j, const char *key, bool value)
{
    j_comma(j);
    j_escaped(j, key);
    j_append_char(j, ':');
    j_append(j, value ? "true" : "false");
    j->need_comma = true;
}

void
axl_json_add_hex(AxlJsonBuilder *j, const char *key, uint64_t value)
{
    char buf[20];

    u64_to_hex(buf, sizeof (buf), value);
    axl_json_add_string(j, key, buf);
}

void
axl_json_add_null(AxlJsonBuilder *j, const char *key)
{
    j_comma(j);
    j_escaped(j, key);
    j_append_char(j, ':');
    j_append(j, "null");
    j->need_comma = true;
}

size_t
axl_json_finish(AxlJsonBuilder *j)
{
    j->buffer[j->pos] = '\0';
    return j->pos;
}
