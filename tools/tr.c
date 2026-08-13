/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file tr.c
    Translate, squeeze, and delete bytes. A port of POSIX tr(1) to
    UEFI (the UEFI Shell has no tr of its own).

    Usage:
      tr [-c|-C] [-t] SET1 SET2      translate SET1 -> SET2
      tr [-c|-C] -d SET1             delete SET1
      tr [-c|-C] -s SET1             squeeze repeats of SET1
      tr [-c|-C] -d -s SET1 SET2     delete SET1, then squeeze SET2
      tr [-c|-C] -s SET1 SET2        translate, then squeeze SET2

    Reads stdin, writes stdout — byte-oriented, exactly like GNU tr (so it
    operates on bytes, not UTF-8 codepoints; feeding it multibyte text
    translates the underlying bytes).

    SET syntax: literal bytes, ranges `a-z`, C escapes
    (`\\ \a \b \f \n \r \t \v` and `\ooo` octal), and POSIX character
    classes `[:alnum:] [:alpha:] [:blank:] [:cntrl:] [:digit:] [:graph:]
    [:lower:] [:print:] [:punct:] [:space:] [:upper:] [:xdigit:]`.
    (The rarer `[c*n]` repeat and `[=c=]` equivalence forms are not
    supported — see the axl-sdk cut/tr scope note.)
**/

#include <axl.h>

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "delete",          .short_name = 'd', .type = AXL_ARG_BOOL,
      .help = "Delete characters in SET1" },
    { .name = "squeeze-repeats", .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Squeeze repeats of the last-operated set into one" },
    { .name = "complement",      .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Use the complement of SET1" },
    { .name = "complement-C",    .short_name = 'C', .type = AXL_ARG_BOOL,
      .help = "Same as -c", .hidden = true },
    { .name = "truncate-set1",   .short_name = 't', .type = AXL_ARG_BOOL,
      .help = "Truncate SET1 to the length of SET2 before translating" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "set1", .type = AXL_ARG_STRING, .required = true,
      .help = "First set of characters" },
    { .name = "set2", .type = AXL_ARG_STRING,
      .help = "Second set (translate target)" },
    {0}
};

// ---------------------------------------------------------------------------
// SET expansion — expand a SET string into an explicit byte sequence
// ---------------------------------------------------------------------------

/* A POSIX class name → membership predicate. Byte-oriented (0..255). */
typedef struct {
    const char *name;
    bool (*member)(int c);
} ClassDef;

/* C-locale byte semantics (GNU tr is byte-oriented). Defined explicitly rather
   than via axl_is* so the ranges are unambiguous and self-contained; only a
   few axl_is* helpers exist anyway. */
static bool cls_upper(int c) { return c >= 'A' && c <= 'Z'; }
static bool cls_lower(int c) { return c >= 'a' && c <= 'z'; }
static bool cls_alpha(int c) { return cls_upper(c) || cls_lower(c); }
static bool cls_digit(int c) { return c >= '0' && c <= '9'; }
static bool cls_alnum(int c) { return cls_alpha(c) || cls_digit(c); }
static bool cls_blank(int c) { return c == ' ' || c == '\t'; }
static bool cls_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static bool cls_cntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
static bool cls_print(int c) { return c >= 0x20 && c < 0x7F; }
static bool cls_graph(int c) { return c > 0x20 && c < 0x7F; }
static bool cls_punct(int c) { return cls_graph(c) && !cls_alnum(c); }
static bool cls_xdigit(int c)
{
    return cls_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static const ClassDef CLASSES[] = {
    { "alnum",  cls_alnum },  { "alpha", cls_alpha }, { "blank", cls_blank },
    { "cntrl",  cls_cntrl },  { "digit", cls_digit }, { "graph", cls_graph },
    { "lower",  cls_lower },  { "print", cls_print }, { "punct", cls_punct },
    { "space",  cls_space },  { "upper", cls_upper }, { "xdigit", cls_xdigit },
};

/* A dynamically grown byte sequence. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} ByteVec;

static bool
bv_push(ByteVec *v, uint8_t b)
{
    if (v->len == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 64;
        uint8_t *nb = axl_realloc(v->buf, nc);
        if (nb == NULL) {
            return false;
        }
        v->buf = nb;
        v->cap = nc;
    }
    v->buf[v->len++] = b;
    return true;
}

/* Parse one octal escape body of up to 3 digits after a backslash. */
static uint8_t
parse_octal(const char **p)
{
    const char *s = *p;
    unsigned v = 0;
    int n = 0;
    while (n < 3 && *s >= '0' && *s <= '7') {
        unsigned nv = v * 8 + (unsigned)(*s - '0');
        if (n == 2 && nv > 255) {
            break;   /* GNU: a 3-digit value > 255 backs off to 2 digits, the
                        third digit becoming a literal char */
        }
        v = nv;
        s++;
        n++;
    }
    *p = s;
    return (uint8_t)v;
}

/* Decode a backslash escape at @p p (which points AT the backslash). Advances
   @p p past the escape and returns the byte. A trailing lone backslash, or an
   unknown escape, yields the literal following char (GNU warns; we pass it
   through). */
static uint8_t
parse_escape(const char **p)
{
    const char *s = *p + 1;   /* past '\' */
    uint8_t out;
    switch (*s) {
    case 'a': out = '\a'; s++; break;
    case 'b': out = '\b'; s++; break;
    case 'f': out = '\f'; s++; break;
    case 'n': out = '\n'; s++; break;
    case 'r': out = '\r'; s++; break;
    case 't': out = '\t'; s++; break;
    case 'v': out = '\v'; s++; break;
    case '\\': out = '\\'; s++; break;
    case '\0': out = '\\'; break;   /* trailing backslash → literal '\' */
    default:
        if (*s >= '0' && *s <= '7') {
            out = parse_octal(&s);
        } else {
            out = (uint8_t)*s;      /* unknown escape → the literal char */
            s++;
        }
        break;
    }
    *p = s;
    return out;
}

/* Read one "element" (an escaped or literal byte) at @p p, advancing it.
   Class/range handling is done by the caller. */
static uint8_t
next_byte(const char **p)
{
    if (**p == '\\') {
        return parse_escape(p);
    }
    uint8_t b = (uint8_t)**p;
    (*p)++;
    return b;
}

/* Match a "[:class:]" at @p p. Returns:
     1  matched — member bytes appended, @p p advanced past the closing "]"
     0  not a class — the caller treats '[' as a literal byte
    -1  a well-formed "[:name:]" whose name is unknown — a hard error (GNU
        rejects an invalid character class rather than silently taking it
        literally); the unknown name is written to @p bad (caller reports it). */
static int
try_class(const char **p, ByteVec *out, const char **bad, size_t *bad_len)
{
    if ((*p)[0] != '[' || (*p)[1] != ':') {
        return 0;
    }
    const char *name = *p + 2;
    const char *end = name;
    while (*end != '\0' && *end != ':') {
        end++;
    }
    if (end[0] != ':' || end[1] != ']') {
        return 0;   /* not a well-formed class — treat '[' literally */
    }
    size_t nlen = (size_t)(end - name);
    for (size_t i = 0; i < sizeof(CLASSES) / sizeof(CLASSES[0]); i++) {
        if (axl_strlen(CLASSES[i].name) == nlen
            && axl_strncmp(CLASSES[i].name, name, nlen) == 0)
        {
            for (int c = 0; c < 256; c++) {
                if (CLASSES[i].member(c) && !bv_push(out, (uint8_t)c)) {
                    return 1;   /* OOM: stop, best effort */
                }
            }
            *p = end + 2;
            return 1;
        }
    }
    *bad     = name;
    *bad_len = nlen;
    return -1;   /* well-formed syntax, unknown class name */
}

/* Expand a full SET string into @p out. Returns AXL_OK or AXL_ERR (OOM). */
static int
expand_set(const char *set, ByteVec *out)
{
    const char *p = set;
    while (*p != '\0') {
        const char *bad = NULL;
        size_t      bad_len = 0;
        int         cls = try_class(&p, out, &bad, &bad_len);
        if (cls == 1) {
            continue;
        }
        if (cls == -1) {
            axl_printerr("tr: invalid character class '%.*s'\n",
                         (int)bad_len, bad);
            return AXL_ERR;
        }
        /* A range is `X-Y` where neither X nor Y is itself the whole range
           dash. Read X, then peek for '-' followed by a real endpoint. */
        const char *save = p;
        uint8_t x = next_byte(&p);
        if (*p == '-' && *(p + 1) != '\0') {
            const char *q = p + 1;
            uint8_t y = next_byte(&q);
            if (y >= x) {
                for (int c = x; c <= y; c++) {
                    if (!bv_push(out, (uint8_t)c)) {
                        return AXL_ERR;
                    }
                }
                p = q;
                continue;
            }
            /* Decreasing "range" isn't one — fall through to literal X. */
            p = save;
            x = next_byte(&p);
        }
        if (!bv_push(out, x)) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Translation build + stream
// ---------------------------------------------------------------------------

static AxlStream *g_out;

static void
emit_byte(uint8_t b)
{
    axl_write(g_out, &b, 1);
}

static int
run_tr(AxlArgs *a)
{
    bool del      = axl_args_get_bool(a, "delete");
    bool squeeze  = axl_args_get_bool(a, "squeeze-repeats");
    bool compl_set1 = axl_args_get_bool(a, "complement")
                  || axl_args_get_bool(a, "complement-C");
    bool truncate = axl_args_get_bool(a, "truncate-set1");

    const char *set1s = axl_args_get_string(a, "set1");
    const char *set2s = axl_args_get_string(a, "set2");

    if (!del && set2s == NULL && !squeeze) {
        axl_printerr("tr: missing operand after '%s'\n"
                     "Two strings must be given when translating.\n",
                     set1s ? set1s : "");
        return 1;
    }
    if (del && !squeeze && set2s != NULL) {
        axl_printerr("tr: extra operand '%s'\n", set2s);
        return 1;
    }
    if (!del && set2s != NULL && set2s[0] == '\0' && !truncate) {
        axl_printerr("tr: when not truncating set1, string2 must be "
                     "non-empty\n");
        return 1;
    }

    ByteVec s1 = { 0 };
    ByteVec s2 = { 0 };
    if (expand_set(set1s, &s1) != AXL_OK) {
        axl_free(s1.buf);
        return 1;
    }
    if (set2s != NULL && expand_set(set2s, &s2) != AXL_OK) {
        axl_free(s1.buf);
        axl_free(s2.buf);
        return 1;
    }

    /* Build the SET1 membership map (byte -> in SET1), honoring --complement. */
    bool in1[256] = { false };
    for (size_t i = 0; i < s1.len; i++) {
        in1[s1.buf[i]] = true;
    }
    if (compl_set1) {
        for (int c = 0; c < 256; c++) {
            in1[c] = !in1[c];
        }
    }

    /* Translation map (only when translating). SET2 is extended by repeating
       its last byte to cover all of SET1 (GNU), unless -t truncates SET1 to
       SET2's length. With --complement the SET1 members are all bytes not
       listed; GNU maps them ALL to SET2's last byte. */
    uint8_t map[256];
    for (int c = 0; c < 256; c++) {
        map[c] = (uint8_t)c;
    }
    bool translating = (!del && set2s != NULL);
    if (translating && s2.len > 0) {
        if (compl_set1) {
            /* The effective SET1 is the complement bytes in ascending order;
               pair them with SET2 by index, repeating SET2's last byte once it
               runs out (GNU). NOT all-to-last — that only matches when SET2 is
               a single byte. -t does not apply to a complemented SET1. */
            size_t idx = 0;
            for (int c = 0; c < 256; c++) {
                if (in1[c]) {
                    map[c] = (idx < s2.len) ? s2.buf[idx] : s2.buf[s2.len - 1];
                    idx++;
                }
            }
        } else {
            size_t limit = s1.len;
            if (truncate && s2.len < limit) {
                limit = s2.len;
            }
            for (size_t i = 0; i < limit; i++) {
                uint8_t src = s1.buf[i];
                uint8_t dst = (i < s2.len) ? s2.buf[i]
                                           : s2.buf[s2.len - 1];
                map[src] = dst;
            }
        }
    }

    /* The squeezed set is SET2 whenever a SET2 was given — both when
       translating (`-s SET1 SET2`) and when deleting (`-d -s SET1 SET2`, where
       GNU squeezes SET2). With only SET1 (`-s SET1`) squeeze SET1 itself,
       honoring any --complement already folded into in1. */
    bool sq[256] = { false };
    if (squeeze) {
        if (set2s != NULL) {
            for (size_t i = 0; i < s2.len; i++) {
                sq[s2.buf[i]] = true;
            }
        } else {
            for (int c = 0; c < 256; c++) {
                sq[c] = in1[c];
            }
        }
    }

    g_out = axl_stdout;
    if (g_out == NULL) {
        axl_free(s1.buf);
        axl_free(s2.buf);
        return 1;
    }

    /* The Shell delivers a pipe as UCS-2; wrap stdin in a text-decoding view
       so we translate the DECODED UTF-8 bytes (reading raw axl_stdin would
       operate on UCS-2 with interleaved NULs). tr stays byte-oriented on that
       UTF-8, exactly like GNU tr on a UTF-8 stream. */
    AxlStream *in = axl_stdin_text();
    if (in == NULL) {
        axl_free(s1.buf);
        axl_free(s2.buf);
        return 1;
    }

    /* Byte stream: delete, translate, then squeeze — GNU's order.

       axl_read, not axl_fread: a filter translates whatever has arrived and
       moves on. axl_fread would block until the whole 4 KiB is in hand (it
       loops, as fread must), which on an interactive console means nothing
       comes out until the user has typed a page. GNU tr reads raw for the
       same reason. */
    int  prev = -1;    /* last byte EMITTED, for squeeze */
    uint8_t inbuf[4096];
    axl_ssize_t got;
    while ((got = axl_read(in, inbuf, sizeof(inbuf))) > 0) {
        for (size_t i = 0; i < (size_t)got; i++) {
            uint8_t b = inbuf[i];
            if (del && in1[b]) {
                continue;
            }
            uint8_t o = translating ? map[b] : b;
            if (squeeze && sq[o] && prev == (int)o) {
                continue;
            }
            emit_byte(o);
            prev = (int)o;
        }
    }

    axl_fclose(in);   /* the wrapper; axl_stdin itself stays open */
    axl_free(s1.buf);
    axl_free(s2.buf);
    return 0;
}

AXL_TOOL_MAIN(tr)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "tr",
        .help        = "Translate, squeeze, or delete bytes (UEFI tr(1) equivalent)",
        .flags       = flags,
        .positionals = positional,
        .handler     = run_tr,
    });
}
