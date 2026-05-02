/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-parse.c
    JSMN-based JSON parser with typed extraction helpers.
**/

#include <axl/axl-json.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("json");

#define JSMN_STATIC
#include "jsmn.h"

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

static bool
token_equals(const char *json, const jsmntok_t *tok, const char *key)
{
    size_t len;

    if (tok->type != JSMN_STRING) {
        return false;
    }

    len = (size_t)(tok->end - tok->start);
    if (len != axl_strlen(key)) {
        return false;
    }

    return (axl_strncmp(json + tok->start, key, len) == 0);
}

static int32_t
find_value_token(const char *json, const jsmntok_t *tokens,
                 int32_t token_count, const char *key)
{
    int32_t pairs;
    int32_t idx;
    int32_t i;

    if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
        return -1;
    }

    pairs = tokens[0].size;
    idx = 1;
    for (i = 0; i < pairs && idx + 1 < token_count; i++) {
        if (token_equals(json, &tokens[idx], key)) {
            return idx + 1;
        }
        /* Skip key token */
        idx++;
        /* Skip value token and all its children(handles nested objects/arrays) */
        {
            int32_t val_end = tokens[idx].end;
            idx++;
            while (idx < token_count && tokens[idx].start < val_end) {
                idx++;
            }
        }
    }

    return -1;
}

static bool
decode_json_string(const char *src, size_t src_len,
                   char *dst, size_t dst_size)
{
    size_t out = 0;
    size_t i;

    for (i = 0; i < src_len && out < dst_size - 1; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
            case '"':  dst[out++] = '"';  break;
            case '\\': dst[out++] = '\\'; break;
            case '/':  dst[out++] = '/';  break;
            case 'b':  dst[out++] = '\b'; break;
            case 'f':  dst[out++] = '\f'; break;
            case 'n':  dst[out++] = '\n'; break;
            case 'r':  dst[out++] = '\r'; break;
            case 't':  dst[out++] = '\t'; break;
            /* JSON5 additions — strict-JSON parsers reject these
               at parse time, so they only reach this decoder for
               JSON5-flagged readers. Decoding the superset is safe
               in either mode. */
            case '\'': dst[out++] = '\''; break;
            case 'v':  dst[out++] = '\v'; break;
            case '0':  dst[out++] = '\0'; break;
            case 'x': {
                int hi = (i + 1 < src_len) ? axl_hex_nibble(src[i + 1]) : -1;
                int lo = (i + 2 < src_len) ? axl_hex_nibble(src[i + 2]) : -1;
                if (hi >= 0 && lo >= 0) {
                    dst[out++] = (char)((hi << 4) | lo);
                    i += 2;
                } else {
                    dst[out++] = src[i];
                }
                break;
            }
            case '\n':  /* line continuation: \<LF>     — emit nothing */
                break;
            case '\r':  /* line continuation: \<CR>[LF] — emit nothing */
                if (i + 1 < src_len && src[i + 1] == '\n') {
                    i++;
                }
                break;
            default:   dst[out++] = src[i]; break;
            }
        } else {
            dst[out++] = src[i];
        }
    }

    dst[out] = '\0';
    return true;
}

static bool
parse_int64(const char *json, const jsmntok_t *tok, int64_t *value)
{
    const char *p;
    const char *end;
    bool negative;
    int64_t v;

    p = json + tok->start;
    end = json + tok->end;

    negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = (*p == '-');
        p++;
    }

    /* JSON5: 0x... hex literal */
    if (p + 1 < end && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (p >= end) return false;
        v = 0;
        while (p < end) {
            int n = axl_hex_nibble(*p);
            if (n < 0) return false;
            v = (v << 4) | n;
            p++;
        }
        *value = negative ? -v : v;
        return true;
    }

    if (p >= end || !axl_isdigit(*p)) {
        return false;
    }

    v = 0;
    while (p < end && axl_isdigit(*p)) {
        v = v * 10 + (*p - '0');
        p++;
    }

    *value = negative ? -v : v;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/* Defined in axl-json5-parse.c — separate file so the strict path
   carries no JSON5 code when only strict consumers link in. */
bool axl_json5_parse_internal(const char *json, size_t len, AxlJsonReader *r);

bool
axl_json_parse_flags(const char *json, size_t len,
                     uint32_t flags, AxlJsonReader *r)
{
    if (json == NULL || len == 0 || r == NULL) {
        return false;
    }

    r->json        = json;
    r->json_len    = len;
    r->tokens      = NULL;
    r->token_count = 0;
    r->owns_tokens = false;

    if (flags & AXL_JSON_PARSER_JSON5) {
        return axl_json5_parse_internal(json, len, r);
    }
    return axl_json_parse(json, len, r);
}

bool
axl_json_parse(const char *json, size_t len, AxlJsonReader *ctx)
{
    jsmn_parser parser;
    int         count;

    if (json == NULL || len == 0 || ctx == NULL) {
        return false;
    }

    ctx->json = json;
    ctx->json_len = len;
    ctx->tokens = NULL;
    ctx->token_count = 0;
    ctx->owns_tokens = false;

    /* Counting pass: determine how many tokens we need */
    jsmn_init(&parser);
    count = jsmn_parse(&parser, json, len, NULL, 0);
    if (count < 1) {
        axl_debug("JSON parse failed (token count %d)", count);
        return false;
    }

    /* Allocate exact-fit token array */
    AXL_AUTO_FREE jsmntok_t *tokens =
        (jsmntok_t *)axl_malloc((size_t)count * sizeof(jsmntok_t));
    if (tokens == NULL) {
        axl_warning("JSON token allocation failed (%d tokens)", count);
        return false;
    }

    /* Real parse */
    jsmn_init(&parser);
    ctx->token_count = jsmn_parse(&parser, json, len,
                                  tokens, (unsigned int)count);
    if (ctx->token_count < 1) {
        ctx->token_count = 0;
        axl_debug("JSON parse failed on second pass");
        return false;
    }

    /* Require root token to be an object or array(reject bare primitives) */
    if (tokens[0].type != JSMN_OBJECT && tokens[0].type != JSMN_ARRAY) {
        axl_debug("JSON root is not object or array(type %d)", tokens[0].type);
        ctx->token_count = 0;
        return false;
    }

    /* Transfer ownership of the token array to ctx. */
    ctx->tokens = (int32_t *)axl_steal_pointer(&tokens);
    ctx->owns_tokens = true;
    return true;
}

void
axl_json_free(AxlJsonReader *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->owns_tokens && ctx->tokens != NULL) {
        axl_free(ctx->tokens);
    }

    ctx->tokens = NULL;
    ctx->token_count = 0;
    ctx->owns_tokens = false;
}

bool
axl_json_load_file_flags(const char *path, uint32_t flags,
                         AxlJsonReader *r,
                         void **out_buf, size_t *out_len)
{
    if (path == NULL || r == NULL || out_buf == NULL) {
        return false;
    }
    *out_buf = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }

    void   *raw = NULL;
    size_t  raw_len = 0;
    if (axl_file_get_contents(path, &raw, &raw_len) != 0) {
        return false;
    }
    if (!axl_json_parse_flags((const char *)raw, raw_len, flags, r)) {
        axl_free(raw);
        return false;
    }
    *out_buf = raw;
    if (out_len != NULL) {
        *out_len = raw_len;
    }
    return true;
}

bool
axl_json_load_file(const char *path, AxlJsonReader *r,
                   void **out_buf, size_t *out_len)
{
    return axl_json_load_file_flags(path, AXL_JSON_PARSER_DEFAULT,
                                    r, out_buf, out_len);
}

bool
axl_json_get_string(const AxlJsonReader *ctx, const char *key,
                    char *value, size_t value_size)
{
    int32_t vi;
    const jsmntok_t *tok;

    if (ctx == NULL || key == NULL || value == NULL) {
        return false;
    }

    vi = find_value_token(ctx->json, (const jsmntok_t *)ctx->tokens,
                          ctx->token_count, key);
    if (vi < 0) {
        return false;
    }

    tok = &((const jsmntok_t *)ctx->tokens)[vi];
    if (tok->type != JSMN_STRING) {
        return false;
    }

    return decode_json_string(ctx->json + tok->start,
                              (size_t)(tok->end - tok->start),
                              value, value_size);
}

bool
axl_json_get_int(const AxlJsonReader *ctx, const char *key, int64_t *value)
{
    int32_t vi;
    const jsmntok_t *tok;

    if (ctx == NULL || key == NULL || value == NULL) {
        return false;
    }

    vi = find_value_token(ctx->json, (const jsmntok_t *)ctx->tokens,
                          ctx->token_count, key);
    if (vi < 0) {
        return false;
    }

    tok = &((const jsmntok_t *)ctx->tokens)[vi];
    if (tok->type != JSMN_PRIMITIVE) {
        return false;
    }

    return parse_int64(ctx->json, tok, value);
}

bool
axl_json_get_uint(const AxlJsonReader *ctx, const char *key, uint64_t *value)
{
    int64_t v;

    if (!axl_json_get_int(ctx, key, &v)) {
        return false;
    }
    if (v < 0) {
        return false;
    }

    *value = (uint64_t)v;
    return true;
}

bool
axl_json_get_bool(const AxlJsonReader *ctx, const char *key, bool *value)
{
    int32_t vi;
    const jsmntok_t *tok;
    char first;

    if (ctx == NULL || key == NULL || value == NULL) {
        return false;
    }

    vi = find_value_token(ctx->json, (const jsmntok_t *)ctx->tokens,
                          ctx->token_count, key);
    if (vi < 0) {
        return false;
    }

    tok = &((const jsmntok_t *)ctx->tokens)[vi];
    if (tok->type != JSMN_PRIMITIVE) {
        return false;
    }

    first = ctx->json[tok->start];
    if (first == 't') { *value = true;  return true; }
    if (first == 'f') { *value = false; return true; }

    return false;
}

bool
axl_json_extract_string(const char *json, size_t len, const char *key,
                        char *value, size_t value_size)
{
    AxlJsonReader ctx;

    if (!axl_json_parse(json, len, &ctx)) {
        return false;
    }

    bool found = axl_json_get_string(&ctx, key, value, value_size);
    axl_json_free(&ctx);
    return found;
}

bool
axl_json_array_begin(const AxlJsonReader *ctx, const char *key,
                     AxlJsonArrayIter *iter)
{
    int32_t vi;
    const jsmntok_t *tok;

    if (ctx == NULL || key == NULL || iter == NULL) {
        return false;
    }

    vi = find_value_token(ctx->json, (const jsmntok_t *)ctx->tokens,
                          ctx->token_count, key);
    if (vi < 0) {
        return false;
    }

    tok = &((const jsmntok_t *)ctx->tokens)[vi];
    if (tok->type != JSMN_ARRAY) {
        return false;
    }

    iter->reader = ctx;
    iter->array_idx = vi;
    iter->pos = vi + 1;
    iter->remaining = tok->size;

    return true;
}

bool
axl_json_root_array_begin(const AxlJsonReader *ctx, AxlJsonArrayIter *iter)
{
    const jsmntok_t *tok;

    if (ctx == NULL || iter == NULL || ctx->token_count == 0) {
        return false;
    }

    tok = &((const jsmntok_t *)ctx->tokens)[0];
    if (tok->type != JSMN_ARRAY) {
        return false;
    }

    iter->reader = ctx;
    iter->array_idx = 0;
    iter->pos = 1;
    iter->remaining = tok->size;

    return true;
}

bool
axl_json_array_next(AxlJsonArrayIter *iter, AxlJsonReader *element)
{
    const jsmntok_t *tokens;
    const jsmntok_t *tok;
    int32_t skip_count;
    int32_t i;

    if (iter == NULL || element == NULL || iter->remaining <= 0) {
        return false;
    }

    if (iter->pos >= iter->reader->token_count) {
        return false;
    }

    tokens = (const jsmntok_t *)iter->reader->tokens;
    tok = &tokens[iter->pos];

    /* Element borrows parent's token array at offset — no copy needed */
    element->json = iter->reader->json;
    element->json_len = iter->reader->json_len;
    element->tokens = &iter->reader->tokens[iter->pos * (int32_t)(sizeof(jsmntok_t) / sizeof(int32_t))];
    element->token_count = iter->reader->token_count - iter->pos;
    element->owns_tokens = false;

    /* Count tokens to skip(element + all its children) */
    skip_count = 1;
    for (i = iter->pos + 1; i < iter->reader->token_count; i++) {
        if (tokens[i].start >= tok->end) {
            break;
        }
        skip_count++;
    }

    iter->pos += skip_count;
    iter->remaining--;

    return true;
}
