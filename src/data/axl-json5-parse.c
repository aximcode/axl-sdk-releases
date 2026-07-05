/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json5-parse.c
    JSON5 (json5.org) lexer + recursive-descent parser. Emits a
    jsmntok_t-layout token array so the existing AxlJsonReader
    accessors (axl_json_get_string, axl_json_array_next, etc.)
    consume JSON5 documents without modification.

    JSON5 features over RFC 8259:
      - line comments (//) and block comments
      - trailing commas in objects and arrays
      - single-quoted strings
      - unquoted (identifier-name) object keys
      - hex number literals (0x...) and +/- number prefix
      - extended string escapes: \', \v, \0, \x##, line continuations

    Strict-mode parsing stays on jsmn — this file is reachable only
    via axl_json_parse_flags(..., AXL_JSON_PARSER_JSON5, ...).
**/

#include <axl/axl-json.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

#define JSMN_HEADER  /* types only, no impl symbols */
#include "jsmn.h"

AXL_LOG_DOMAIN("json5");

// ---------------------------------------------------------------------------
// Internal parser state
// ---------------------------------------------------------------------------

typedef struct {
    const char *json;
    size_t      len;
    size_t      pos;
    jsmntok_t  *tokens;
    size_t      tok_count;
    size_t      tok_cap;
    bool        oom;
} J5;

static jsmntok_t *
alloc_tok(J5 *p)
{
    if (p->tok_count >= p->tok_cap) {
        size_t new_cap = p->tok_cap ? p->tok_cap * 2 : 16;
        jsmntok_t *bigger = axl_malloc(new_cap * sizeof(jsmntok_t));
        if (bigger == NULL) {
            p->oom = true;
            return NULL;
        }
        if (p->tokens != NULL) {
            axl_memcpy(bigger, p->tokens,
                       p->tok_count * sizeof(jsmntok_t));
            axl_free(p->tokens);
        }
        p->tokens = bigger;
        p->tok_cap = new_cap;
    }
    jsmntok_t *t = &p->tokens[p->tok_count++];
    t->type  = JSMN_UNDEFINED;
    t->start = -1;
    t->end   = -1;
    t->size  = 0;
    return t;
}

// ---------------------------------------------------------------------------
// Whitespace + comment skipping
// ---------------------------------------------------------------------------

/**
 * Skip JSON5 insignificant text: whitespace + // line + / * block * /
 * comments. Returns 0 on success, -1 on unterminated block comment.
 */
static int
skip_ws(J5 *p)
{
    while (p->pos < p->len) {
        char c = p->json[p->pos];
        if (axl_isspace((unsigned char)c)) {
            p->pos++;
            continue;
        }
        if (c == '/' && p->pos + 1 < p->len) {
            char n = p->json[p->pos + 1];
            if (n == '/') {
                /* line comment: skip to newline (or EOF) */
                p->pos += 2;
                while (p->pos < p->len &&
                       p->json[p->pos] != '\n' &&
                       p->json[p->pos] != '\r') {
                    p->pos++;
                }
                continue;
            }
            if (n == '*') {
                /* block comment: skip to closing star-slash */
                p->pos += 2;
                bool closed = false;
                while (p->pos + 1 < p->len) {
                    if (p->json[p->pos] == '*' && p->json[p->pos + 1] == '/') {
                        p->pos += 2;
                        closed = true;
                        break;
                    }
                    p->pos++;
                }
                if (!closed) {
                    axl_debug("unterminated block comment");
                    return -1;
                }
                continue;
            }
        }
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Tokenizers
// ---------------------------------------------------------------------------

static bool
is_ident_start(char c)
{
    return axl_isalpha((unsigned char)c) || c == '_' || c == '$';
}

static bool
is_ident_cont(char c)
{
    return is_ident_start(c) || axl_isdigit((unsigned char)c);
}

/**
 * Lex a quoted string. @p quote is either '"' or '\''. Caller is
 * positioned on the opening quote; on success, p->pos is one past
 * the closing quote and a JSMN_STRING token has been emitted with
 * start/end bracketing the inner content.
 */
static int
parse_string(J5 *p, char quote)
{
    /* Caller guaranteed json[pos] == quote */
    p->pos++;  /* skip opening quote */

    int start = (int)p->pos;
    while (p->pos < p->len) {
        char c = p->json[p->pos];

        if (c == quote) {
            /* End of string */
            jsmntok_t *t = alloc_tok(p);
            if (t == NULL) return -1;
            t->type  = JSMN_STRING;
            t->start = start;
            t->end   = (int)p->pos;
            t->size  = 0;
            p->pos++;  /* skip closing quote */
            return 0;
        }

        if (c == '\\') {
            /* Backslash: validate the escape; we accept any single
               char (JSON5 allows \<anychar>), \x##, \u####, and a
               line continuation \<LF|CR>. Decoding happens later in
               decode_json_string — here we only validate. */
            if (p->pos + 1 >= p->len) {
                axl_debug("unterminated string escape at %zu", p->pos);
                return -1;
            }
            char e = p->json[p->pos + 1];
            if (e == 'x') {
                if (p->pos + 3 >= p->len ||
                    !axl_isxdigit((unsigned char)(p->json[p->pos + 2])) ||
                    !axl_isxdigit((unsigned char)(p->json[p->pos + 3]))) {
                    axl_debug("bad \\x escape at %zu", p->pos);
                    return -1;
                }
                p->pos += 4;
                continue;
            }
            if (e == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (p->pos + 2 + i >= p->len ||
                        !axl_isxdigit((unsigned char)(p->json[p->pos + 2 + i]))) {
                        axl_debug("bad \\u escape at %zu", p->pos);
                        return -1;
                    }
                }
                p->pos += 6;
                continue;
            }
            /* Anything else (including \', \", \n, \\, \v, \0,
               line continuation \<NL>) consumes two chars. */
            p->pos += 2;
            continue;
        }

        /* Forbid raw control chars (per RFC 8259, also enforced in
           JSON5). 0x09 (TAB) is the only allowed control char. */
        if ((unsigned char)c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            axl_debug("control char 0x%02x in string at %zu",
                      (unsigned)c, p->pos);
            return -1;
        }

        p->pos++;
    }
    axl_debug("unterminated string");
    return -1;
}

/**
 * Lex an unquoted identifier key. Caller has already verified that
 * json[pos] is an identifier-start character.
 */
static int
parse_identifier(J5 *p)
{
    int start = (int)p->pos;
    while (p->pos < p->len && is_ident_cont(p->json[p->pos])) {
        p->pos++;
    }
    jsmntok_t *t = alloc_tok(p);
    if (t == NULL) return -1;
    t->type  = JSMN_STRING;  /* keys are strings to the accessor layer */
    t->start = start;
    t->end   = (int)p->pos;
    t->size  = 0;
    return 0;
}

/**
 * Lex a number — decimal (with optional sign, fraction, exponent)
 * or hex (0x... with optional sign). Emits JSMN_PRIMITIVE.
 */
static int
parse_number(J5 *p)
{
    int start = (int)p->pos;

    /* Optional sign */
    if (p->json[p->pos] == '+' || p->json[p->pos] == '-') {
        p->pos++;
    }

    /* Hex? */
    if (p->pos + 1 < p->len &&
        p->json[p->pos] == '0' &&
        (p->json[p->pos + 1] == 'x' || p->json[p->pos + 1] == 'X'))
    {
        p->pos += 2;
        size_t hex_start = p->pos;
        while (p->pos < p->len && axl_isxdigit((unsigned char)(p->json[p->pos]))) {
            p->pos++;
        }
        if (p->pos == hex_start) {
            axl_debug("hex literal with no digits at %d", start);
            return -1;
        }
    } else {
        /* Decimal: int [. frac] [exp] — accept JSON5 leading/trailing dot */
        bool saw_digit = false;
        while (p->pos < p->len &&
               axl_isdigit((unsigned char)p->json[p->pos])) {
            saw_digit = true;
            p->pos++;
        }
        if (p->pos < p->len && p->json[p->pos] == '.') {
            p->pos++;
            while (p->pos < p->len &&
                   axl_isdigit((unsigned char)p->json[p->pos])) {
                saw_digit = true;
                p->pos++;
            }
        }
        if (!saw_digit) {
            axl_debug("number with no digits at %d", start);
            return -1;
        }
        if (p->pos < p->len &&
            (p->json[p->pos] == 'e' || p->json[p->pos] == 'E'))
        {
            p->pos++;
            if (p->pos < p->len &&
                (p->json[p->pos] == '+' || p->json[p->pos] == '-')) {
                p->pos++;
            }
            size_t exp_start = p->pos;
            while (p->pos < p->len &&
                   axl_isdigit((unsigned char)p->json[p->pos])) {
                p->pos++;
            }
            if (p->pos == exp_start) {
                axl_debug("exponent with no digits at %d", start);
                return -1;
            }
        }
    }

    jsmntok_t *t = alloc_tok(p);
    if (t == NULL) return -1;
    t->type  = JSMN_PRIMITIVE;
    t->start = start;
    t->end   = (int)p->pos;
    t->size  = 0;
    return 0;
}

/**
 * Lex a bare-word literal: true, false, null. Emits JSMN_PRIMITIVE.
 * Returns -1 if the word doesn't match a known literal.
 */
static int
parse_literal(J5 *p)
{
    static const struct {
        const char *kw;
        size_t      n;
    } kws[] = {
        { "true",  4 },
        { "false", 5 },
        { "null",  4 },
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        size_t n = kws[i].n;
        if (p->pos + n <= p->len &&
            axl_memcmp(&p->json[p->pos], kws[i].kw, n) == 0)
        {
            /* Reject "trueX" — must be followed by non-ident char or EOF */
            if (p->pos + n < p->len && is_ident_cont(p->json[p->pos + n])) {
                continue;
            }
            jsmntok_t *t = alloc_tok(p);
            if (t == NULL) return -1;
            t->type  = JSMN_PRIMITIVE;
            t->start = (int)p->pos;
            t->end   = (int)(p->pos + n);
            t->size  = 0;
            p->pos += n;
            return 0;
        }
    }
    axl_debug("unknown literal at %zu", p->pos);
    return -1;
}

// ---------------------------------------------------------------------------
// Recursive-descent parser
// ---------------------------------------------------------------------------

static int parse_value(J5 *p);

static int
parse_object(J5 *p)
{
    /* Caller has consumed '{' and emitted the OBJECT token; index is
       the most recently allocated. */
    size_t obj_idx = p->tok_count - 1;
    int    pair_count = 0;

    if (skip_ws(p) != 0) return -1;

    if (p->pos < p->len && p->json[p->pos] == '}') {
        p->tokens[obj_idx].end  = (int)(p->pos + 1);
        p->tokens[obj_idx].size = 0;
        p->pos++;
        return 0;
    }

    for (;;) {
        if (skip_ws(p) != 0) return -1;
        if (p->pos >= p->len) {
            axl_debug("unterminated object");
            return -1;
        }

        /* Key — must be a string (any quote) or unquoted identifier */
        char c = p->json[p->pos];
        if (c == '"' || c == '\'') {
            if (parse_string(p, c) != 0) return -1;
        } else if (is_ident_start(c)) {
            if (parse_identifier(p) != 0) return -1;
        } else {
            axl_debug("expected object key at %zu (got 0x%02x)",
                      p->pos, (unsigned)c);
            return -1;
        }
        pair_count++;

        if (skip_ws(p) != 0) return -1;
        if (p->pos >= p->len || p->json[p->pos] != ':') {
            axl_debug("expected ':' after key at %zu", p->pos);
            return -1;
        }
        p->pos++;  /* skip ':' */

        if (skip_ws(p) != 0) return -1;
        if (parse_value(p) != 0) return -1;

        if (skip_ws(p) != 0) return -1;
        if (p->pos >= p->len) {
            axl_debug("unterminated object after value");
            return -1;
        }
        if (p->json[p->pos] == ',') {
            p->pos++;
            if (skip_ws(p) != 0) return -1;
            /* Trailing comma: if next is '}', we're done */
            if (p->pos < p->len && p->json[p->pos] == '}') {
                p->tokens[obj_idx].end  = (int)(p->pos + 1);
                p->tokens[obj_idx].size = pair_count;
                p->pos++;
                return 0;
            }
            continue;
        }
        if (p->json[p->pos] == '}') {
            p->tokens[obj_idx].end  = (int)(p->pos + 1);
            p->tokens[obj_idx].size = pair_count;
            p->pos++;
            return 0;
        }
        axl_debug("expected ',' or '}' at %zu (got 0x%02x)",
                  p->pos, (unsigned)p->json[p->pos]);
        return -1;
    }
}

static int
parse_array(J5 *p)
{
    size_t arr_idx = p->tok_count - 1;
    int    elem_count = 0;

    if (skip_ws(p) != 0) return -1;

    if (p->pos < p->len && p->json[p->pos] == ']') {
        p->tokens[arr_idx].end  = (int)(p->pos + 1);
        p->tokens[arr_idx].size = 0;
        p->pos++;
        return 0;
    }

    for (;;) {
        if (skip_ws(p) != 0) return -1;
        if (parse_value(p) != 0) return -1;
        elem_count++;

        if (skip_ws(p) != 0) return -1;
        if (p->pos >= p->len) {
            axl_debug("unterminated array");
            return -1;
        }
        if (p->json[p->pos] == ',') {
            p->pos++;
            if (skip_ws(p) != 0) return -1;
            if (p->pos < p->len && p->json[p->pos] == ']') {
                p->tokens[arr_idx].end  = (int)(p->pos + 1);
                p->tokens[arr_idx].size = elem_count;
                p->pos++;
                return 0;
            }
            continue;
        }
        if (p->json[p->pos] == ']') {
            p->tokens[arr_idx].end  = (int)(p->pos + 1);
            p->tokens[arr_idx].size = elem_count;
            p->pos++;
            return 0;
        }
        axl_debug("expected ',' or ']' at %zu (got 0x%02x)",
                  p->pos, (unsigned)p->json[p->pos]);
        return -1;
    }
}

static int
parse_value(J5 *p)
{
    if (skip_ws(p) != 0) return -1;
    if (p->pos >= p->len) {
        axl_debug("expected value, got EOF");
        return -1;
    }
    char c = p->json[p->pos];

    if (c == '{') {
        jsmntok_t *t = alloc_tok(p);
        if (t == NULL) return -1;
        t->type  = JSMN_OBJECT;
        t->start = (int)p->pos;
        t->end   = -1;
        t->size  = 0;
        p->pos++;
        return parse_object(p);
    }
    if (c == '[') {
        jsmntok_t *t = alloc_tok(p);
        if (t == NULL) return -1;
        t->type  = JSMN_ARRAY;
        t->start = (int)p->pos;
        t->end   = -1;
        t->size  = 0;
        p->pos++;
        return parse_array(p);
    }
    if (c == '"' || c == '\'') {
        return parse_string(p, c);
    }
    if (c == '+' || c == '-' || axl_isdigit((unsigned char)c) || c == '.') {
        return parse_number(p);
    }
    if (is_ident_start(c)) {
        return parse_literal(p);
    }
    axl_debug("unexpected char 0x%02x at %zu", (unsigned)c, p->pos);
    return -1;
}

// ---------------------------------------------------------------------------
// Internal entry point — called from axl-json-parse.c when the JSON5
// flag is set. Reader fields are filled exactly as axl_json_parse fills
// them, so the existing accessors work unchanged.
// ---------------------------------------------------------------------------

bool
axl_json5_parse_internal(
    const char    *json,
    size_t         len,
    AxlJsonReader *r
);

bool
axl_json5_parse_internal(const char *json, size_t len, AxlJsonReader *r)
{
    J5 p = {
        .json      = json,
        .len       = len,
        .pos       = 0,
        .tokens    = NULL,
        .tok_count = 0,
        .tok_cap   = 0,
        .oom       = false,
    };

    if (parse_value(&p) != 0 || p.oom) {
        if (p.tokens != NULL) {
            axl_free(p.tokens);
        }
        return false;
    }

    /* Trailing junk (post-root) is allowed only if it's whitespace
       or comments. */
    skip_ws(&p);
    if (p.pos != p.len) {
        axl_debug("trailing junk at %zu", p.pos);
        axl_free(p.tokens);
        return false;
    }

    /* Root must be object or array — same restriction as the strict
       parser, so consumers see consistent behavior across modes. */
    if (p.tok_count < 1 ||
        (p.tokens[0].type != JSMN_OBJECT && p.tokens[0].type != JSMN_ARRAY))
    {
        axl_debug("root is not object or array");
        axl_free(p.tokens);
        return false;
    }

    r->json        = json;
    r->json_len    = len;
    r->tokens      = (int32_t *)p.tokens;
    r->token_count = (int32_t)p.tok_count;
    r->owns_tokens = true;
    return true;
}
