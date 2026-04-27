/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-parse.c
    JSMN-based JSON parser with typed extraction helpers.
**/

#include <axl/axl-json.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
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
    if (p < end && *p == '-') {
        negative = true;
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') {
        return false;
    }

    v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }

    *value = negative ? -v : v;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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
