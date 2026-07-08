/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file sed.c
    Stream editor — POSIX sed(1) plus the common GNU extensions.

    Build with axl-cc:
      axl-cc sed.c -o sed.efi

    Usage:
      sed [-n] [-E|-r] [-s] [-z] [-e script]... [-f file]... [script] [file...]

    With no `-e`/`-f`, the first non-option argument is the script. With no
    file arguments (or `-`) input is read from stdin — so `sometool | sed …`
    works as the right-hand side of a UEFI Shell pipe.

    Regular expressions are POSIX Basic (BRE) by default, matched via
    AxlRegex's AXL_REGEX_BRE mode; `-E`/`-r` selects Extended (ERE). In
    addition to the standard escapes the regex and replacement lexers honor
    `\n \t \r \a \b \f \v` and the `\xHH` hex byte (a GNU/Dell-diagnostics
    extension), so a literal space can be written `\x20`.

    Commands: `{ }  s  y  d  D  p  P  n  N  h  H  g  G  x  b  t  T  :  a  i  c
    r  w  q  Q  =  l  z  #`. Addresses: line number, `$`, `/re/` (and
    `\cREc`), `first~step`, ranges `a1,a2`, `a1,+N`, `a1,~N`, `0,/re/`, and
    `!` negation. `s` flags: `g`, a count `N`, `p`, `i`/`I`, `m`/`M`, `w file`.

    Deliberately NOT implemented (full-GNU-only): the `e`/`F`/`R`/`W`
    commands, the case-conversion replacement escapes `\L \U \l \u \E`, and
    in-place `-i` (there is no ambient filesystem to rewrite under UEFI).

    Word-boundary assertions (`\b \B \< \>`) are not available — AxlRegex has
    no word-boundary opcode — so they are not honored here; use an explicit
    character class or anchor instead.
**/

#include <axl.h>

// ---------------------------------------------------------------------------
// Growable byte buffer — the pattern space, hold space, and output builder.
// Bytes are stored without a trailing NUL; callers pass explicit lengths.
// ---------------------------------------------------------------------------

typedef struct {
    char  *p;
    size_t len, cap;
} Buf;

static bool buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra) nc *= 2;
    char *np = axl_realloc(b->p, nc);
    if (np == NULL) return false;
    b->p = np; b->cap = nc;
    return true;
}

static bool buf_append(Buf *b, const char *s, size_t n)
{
    if (n == 0) return true;
    if (!buf_reserve(b, n)) return false;
    axl_memcpy(b->p + b->len, s, n);
    b->len += n;
    return true;
}

static bool buf_putc(Buf *b, char c) { return buf_append(b, &c, 1); }

static void buf_set(Buf *b, const char *s, size_t n) { b->len = 0; buf_append(b, s, n); }
static void buf_free(Buf *b) { axl_free(b->p); b->p = NULL; b->len = b->cap = 0; }

// ---------------------------------------------------------------------------
// Escape decoding (`fixquote` equivalent): `\n \t \r \a \b \f \v \e \xHH`,
// and `\<other>` -> that byte literally. Used when building regex pattern
// strings and `s`/`y` operands from the script. `*pp` points just past the
// backslash; on return it points past the consumed escape. Returns the byte.
// `meta_keep` non-NULL: when the escape is a STRUCTURAL backslash-meta that
// the regex engine must still see backslashed (BRE \( \) \{ \} \| \+ \? \<
// \>, or a digit backreference), it is reported via *meta_keep and the raw
// two chars are not collapsed (the caller re-emits `\` + the byte).
// ---------------------------------------------------------------------------

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_escape(const char **pp, const char *end)
{
    if (*pp >= end) return '\\';            /* trailing backslash -> literal */
    int c = (unsigned char)*(*pp)++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'e': return 27;
    case 'x': {
        if (*pp < end) {
            int h1 = hexval((unsigned char)**pp);
            int h2 = (*pp + 1 < end) ? hexval((unsigned char)*(*pp + 1)) : -1;
            if (h1 >= 0 && h2 >= 0) { *pp += 2; return (h1 << 4) | h2; }
            if (h1 >= 0)            { *pp += 1; return h1; }
        }
        return 'x';                         /* lone \x -> literal x */
    }
    default:  return c;                      /* \. \/ \space ... -> the byte */
    }
}

// ---------------------------------------------------------------------------
// Options / globals
// ---------------------------------------------------------------------------

static bool   opt_quiet      = false;   /* -n */
static bool   opt_separate   = false;   /* -s */
static bool   opt_null_data  = false;   /* -z */
static uint32_t re_syntax    = AXL_REGEX_BRE;   /* -E/-r flips to ERE */

/* Maximum bytes buffered per record (incl. NUL terminator). 64 MiB is
   generous for any real line while keeping the UEFI heap safe against
   a newline-free binary file. axl_readline_max uses the same limit. */
#define SED_LINE_MAX (64u * 1024u * 1024u)

static void fail(const char *msg, const char *arg)
{
    if (arg) axl_printerr("sed: %s: %s\n", msg, arg);
    else     axl_printerr("sed: %s\n", msg);
}

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------

typedef enum {
    A_NONE, A_LINE, A_LAST, A_RE, A_STEP, A_PLUS, A_TILDE, A_ZERO
} AddrType;

typedef struct {
    AddrType   type;
    long       n;        /* line (A_LINE), first (A_STEP), N (A_PLUS/A_TILDE) */
    long       step;     /* A_STEP */
    AxlRegex  *re;       /* A_RE (NULL => reuse the last regex at run time) */
} Addr;

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

typedef struct Cmd {
    Addr  a1, a2;
    bool  has_a2;
    bool  negate;
    char  cmd;

    /* range state (mutated during execution) */
    bool  active;
    long  range_end;     /* resolved end line for ,+N / ,~N (else 0) */

    /* s/// */
    AxlRegex *re;        /* compiled substitution regex */
    char     *rhs;       /* raw replacement text (decoded at apply time) */
    bool      s_global;
    bool      s_print;
    long      s_nth;     /* 1-based; replace from the Nth match */
    char     *s_wfile;

    /* a/i/c text, r/w/label filename, : / b / t / T label */
    char     *text;

    /* y/// */
    unsigned char *ymap; /* 256-entry translation table, or NULL */

    /* resolved targets */
    int   target;        /* '{' -> matching '}'+1 ; b/t/T -> label pc (-1 = end) */

    int   exit_code;      /* q/Q */
} Cmd;

static Cmd  *cmds = NULL;
static int   ncmds = 0, cap_cmds = 0;

static Cmd *cmd_new(void)
{
    if (ncmds == cap_cmds) {
        int nc = cap_cmds ? cap_cmds * 2 : 32;
        Cmd *np = axl_realloc(cmds, (size_t)nc * sizeof(Cmd));
        if (np == NULL) return NULL;
        cmds = np; cap_cmds = nc;
    }
    Cmd *c = &cmds[ncmds++];
    axl_memset(c, 0, sizeof *c);
    c->s_nth = 1;
    c->target = -1;
    return c;
}

/* Release every per-command allocation and the command array. */
static void free_program(void)
{
    for (int i = 0; i < ncmds; i++) {
        Cmd *c = &cmds[i];
        axl_regex_free(c->re);            /* NULL-safe */
        axl_regex_free(c->a1.re);
        axl_regex_free(c->a2.re);
        axl_free(c->rhs);
        axl_free(c->text);
        axl_free(c->s_wfile);
        axl_free(c->ymap);
    }
    axl_free(cmds);
    cmds = NULL; ncmds = cap_cmds = 0;
}

// ---------------------------------------------------------------------------
// Script parser
//
// The script text from all -e/-f sources is concatenated with newlines and
// parsed here. `sp` walks it. Commands are separated by ';' or newline; '{'
// and '}' nest; '#' to end-of-line is a comment.
// ---------------------------------------------------------------------------

typedef struct { const char *sp, *send; bool err; const char *errmsg; } P;

static void perr(P *p, const char *m) { if (!p->err) { p->err = true; p->errmsg = m; } }
static int  pp(P *p)  { return p->sp < p->send ? (unsigned char)*p->sp : -1; }
static int  pgc(P *p) { return p->sp < p->send ? (unsigned char)*p->sp++ : -1; }
static void skip_ws(P *p) { while (p->sp < p->send && (*p->sp == ' ' || *p->sp == '\t')) p->sp++; }
static void skip_seps(P *p)
{
    while (p->sp < p->send) {
        char c = *p->sp;
        if (c == ' ' || c == '\t' || c == '\n' || c == ';') { p->sp++; continue; }
        if (c == '#') { while (p->sp < p->send && *p->sp != '\n') p->sp++; continue; }
        break;
    }
}

/* Read a `/re/` (or `\cREc`) up to the unescaped delimiter, decoding escapes
   into a regex source string for axl_regex_new. Structural BRE/ERE backslash
   sequences are preserved; `\xHH`/`\n`/... collapse to literal bytes (escaped
   if they would otherwise be a regex metacharacter). The opening delimiter
   has already been consumed; `delim` is it. */
static char *read_regex_src(P *p, char delim, bool *out_empty)
{
    Buf b = {0};
    *out_empty = false;
    bool any = false;
    while (p->sp < p->send) {
        int c = pgc(p);
        if (c == delim) {
            if (!any) *out_empty = true;     /* `//` => reuse last regex */
            buf_putc(&b, '\0');
            return b.p;
        }
        any = true;
        if (c == '\n') { perr(p, "unterminated regex"); buf_free(&b); return NULL; }
        if (c == '\\') {
            int n = pp(p);
            if (n == delim) { pgc(p); buf_putc(&b, (char)delim); continue; }  /* \delim -> literal */
            /* Preserve structural backslash-metas + backrefs for the engine. */
            if (n == '(' || n == ')' || n == '{' || n == '}' || n == '|' ||
                n == '+' || n == '?' || n == '<' || n == '>' || n == '.' ||
                n == '*' || n == '[' || n == ']' || n == '^' || n == '$' ||
                (n >= '1' && n <= '9') ||
                n == 'w' || n == 'W' || n == 's' || n == 'S' || n == 'd' || n == 'D') {
                buf_putc(&b, '\\'); buf_putc(&b, (char)pgc(p)); continue;
            }
            const char *q = p->sp;
            int byte = decode_escape(&q, p->send);
            p->sp = q;
            /* Escape it if the decoded byte is itself a regex metacharacter so
               it stays a literal. */
            if (axl_strchr(".[]()*+?{}|^$\\", byte) != NULL) buf_putc(&b, '\\');
            buf_putc(&b, (char)byte);
            continue;
        }
        buf_putc(&b, (char)c);
    }
    perr(p, "unterminated regex");
    buf_free(&b);
    return NULL;
}

/* Parse one address into `a`. Returns true if an address was present. */
static bool parse_addr(P *p, Addr *a)
{
    skip_ws(p);
    int c = pp(p);
    if (c == '$') { pgc(p); a->type = A_LAST; return true; }
    if (c >= '0' && c <= '9') {
        long v = 0;
        while (pp(p) >= '0' && pp(p) <= '9') v = v * 10 + (pgc(p) - '0');
        if (pp(p) == '~') {                  /* first~step (GNU) */
            pgc(p);
            long st = 0;
            while (pp(p) >= '0' && pp(p) <= '9') st = st * 10 + (pgc(p) - '0');
            a->type = A_STEP; a->n = v; a->step = st;
            return true;
        }
        a->type = (v == 0) ? A_ZERO : A_LINE; a->n = v;
        return true;
    }
    if (c == '/' || c == '\\') {
        char delim = '/';
        pgc(p);
        if (c == '\\') { delim = (char)pgc(p); }      /* \cREc custom delimiter */
        bool empty = false;
        char *src = read_regex_src(p, delim, &empty);
        if (p->err) return false;
        uint32_t f = re_syntax;
        while (pp(p) == 'I' || pp(p) == 'M') {        /* addr regex flags */
            if (pgc(p) == 'I') f |= AXL_REGEX_CASELESS; else f |= AXL_REGEX_MULTILINE;
        }
        a->type = A_RE;
        if (empty) { a->re = NULL; axl_free(src); }
        else {
            a->re = axl_regex_new(src, f);
            axl_free(src);
            if (a->re == NULL) { perr(p, "bad regex in address"); return false; }
        }
        return true;
    }
    return false;
}

/* Collect text for a/i/c: the rest of the line(s). Supports both the
   one-liner `a text` (GNU) and the classic `a\<newline>text` forms, with
   `\<newline>` continuations. Returns a heap string (caller owns). */
static char *parse_text(P *p)
{
    Buf b = {0};
    skip_ws(p);
    if (pp(p) == '\\') {                      /* a\  — text starts next line */
        pgc(p);
        if (pp(p) == '\n') pgc(p);
        else skip_ws(p);                      /* `a\ text` on the same line */
    }
    while (p->sp < p->send) {
        int c = pgc(p);
        if (c == '\\') {
            if (pp(p) == '\n') { pgc(p); buf_putc(&b, '\n'); continue; }  /* continuation */
            if (p->sp < p->send) { buf_putc(&b, (char)pgc(p)); continue; }
            break;
        }
        if (c == '\n') break;
        buf_putc(&b, (char)c);
    }
    buf_putc(&b, '\0');
    return b.p;
}

/* Read a label / filename token (to end of line, trimmed). */
static char *parse_token(P *p)
{
    skip_ws(p);
    const char *start = p->sp;
    while (p->sp < p->send && *p->sp != '\n' && *p->sp != ';' && *p->sp != '}') p->sp++;
    size_t n = (size_t)(p->sp - start);
    while (n > 0 && (start[n-1] == ' ' || start[n-1] == '\t')) n--;
    return axl_strndup(start, n);
}
static char *parse_filename(P *p)
{
    skip_ws(p);
    const char *start = p->sp;
    while (p->sp < p->send && *p->sp != '\n') p->sp++;   /* filenames run to EOL */
    size_t n = (size_t)(p->sp - start);
    while (n > 0 && (start[n-1] == ' ' || start[n-1] == '\t')) n--;
    return axl_strndup(start, n);
}

/* Parse `s/re/repl/flags`. The opening `s` is consumed; next char is delim. */
static bool parse_s(P *p, Cmd *c)
{
    int delim = pgc(p);
    if (delim == -1 || delim == '\n' || delim == '\\') { perr(p, "bad s/// delimiter"); return false; }
    bool empty = false;
    char *src = read_regex_src(p, (char)delim, &empty);
    if (p->err) return false;
    /* replacement: copy raw up to the unescaped delimiter, keeping `\` pairs
       intact (expanded at apply time). */
    Buf r = {0};
    bool closed = false;
    while (p->sp < p->send) {
        int ch = pgc(p);
        if (ch == delim) { closed = true; break; }
        if (ch == '\n') { perr(p, "unterminated s command"); axl_free(src); buf_free(&r); return false; }
        if (ch == '\\') {
            if (p->sp < p->send) { buf_putc(&r, '\\'); buf_putc(&r, (char)pgc(p)); continue; }
            perr(p, "trailing backslash in replacement"); axl_free(src); buf_free(&r); return false;
        }
        buf_putc(&r, (char)ch);
    }
    if (!closed) { perr(p, "unterminated s command"); axl_free(src); buf_free(&r); return false; }
    buf_putc(&r, '\0');
    c->rhs = r.p;

    uint32_t f = re_syntax;
    for (;;) {
        int fl = pp(p);
        if (fl == 'g') { pgc(p); c->s_global = true; }
        else if (fl == 'p') { pgc(p); c->s_print = true; }
        else if (fl == 'i' || fl == 'I') { pgc(p); f |= AXL_REGEX_CASELESS; }
        else if (fl == 'm' || fl == 'M') { pgc(p); f |= AXL_REGEX_MULTILINE; }
        else if (fl >= '0' && fl <= '9') {
            long v = 0; while (pp(p) >= '0' && pp(p) <= '9') v = v * 10 + (pgc(p) - '0');
            c->s_nth = v ? v : 1;
        }
        else if (fl == 'w') { pgc(p); c->s_wfile = parse_filename(p); break; }
        else break;
    }
    if (empty) { c->re = NULL; axl_free(src); }       /* s//repl/ reuses last regex */
    else {
        c->re = axl_regex_new(src, f);
        axl_free(src);
        if (c->re == NULL) { perr(p, "bad regex in s command"); return false; }
    }
    return true;
}

/* Parse `y/abc/xyz/` into a 256-byte translation table. */
static bool parse_y(P *p, Cmd *c)
{
    int delim = pgc(p);
    if (delim == -1 || delim == '\\' || delim == '\n') { perr(p, "bad y/// delimiter"); return false; }
    Buf from = {0}, to = {0};
    Buf *cur = &from;
    int seen_delims = 1;
    while (p->sp < p->send && seen_delims < 3) {
        int ch = pgc(p);
        if (ch == delim) { seen_delims++; if (seen_delims == 2) cur = &to; continue; }
        if (ch == '\n') { perr(p, "unterminated y command"); buf_free(&from); buf_free(&to); return false; }
        if (ch == '\\') {
            int n = pp(p);
            if (n == delim) { pgc(p); buf_putc(cur, (char)delim); continue; }
            if (n == '\\')  { pgc(p); buf_putc(cur, '\\'); continue; }
            const char *q = p->sp; int byte = decode_escape(&q, p->send); p->sp = q;
            buf_putc(cur, (char)byte); continue;
        }
        buf_putc(cur, (char)ch);
    }
    if (seen_delims < 3 || from.len != to.len) {
        perr(p, "y strings differ in length"); buf_free(&from); buf_free(&to); return false;
    }
    c->ymap = axl_malloc(256);
    if (c->ymap == NULL) { buf_free(&from); buf_free(&to); return false; }
    for (int i = 0; i < 256; i++) c->ymap[i] = (unsigned char)i;
    for (size_t i = 0; i < from.len; i++) c->ymap[(unsigned char)from.p[i]] = (unsigned char)to.p[i];
    buf_free(&from); buf_free(&to);
    return true;
}

static bool parse_script(const char *text, size_t len)
{
    P p = { text, text + len, false, NULL };
    int brace_stack[64];
    int bdepth = 0;

    for (;;) {
        skip_seps(&p);
        if (p.sp >= p.send) break;

        Cmd *c = cmd_new();
        if (c == NULL) { fail("out of memory", NULL); return false; }

        /* addresses */
        if (parse_addr(&p, &c->a1)) {
            skip_ws(&p);
            if (pp(&p) == ',') {
                pgc(&p); skip_ws(&p);
                if (pp(&p) == '+') {                  /* a1,+N */
                    pgc(&p); long v = 0;
                    while (pp(&p) >= '0' && pp(&p) <= '9') v = v * 10 + (pgc(&p) - '0');
                    c->a2.type = A_PLUS; c->a2.n = v;
                } else if (pp(&p) == '~') {            /* a1,~N */
                    pgc(&p); long v = 0;
                    while (pp(&p) >= '0' && pp(&p) <= '9') v = v * 10 + (pgc(&p) - '0');
                    c->a2.type = A_TILDE; c->a2.n = v;
                } else if (!parse_addr(&p, &c->a2)) {
                    perr(&p, "expected address after ','");
                }
                c->has_a2 = true;
            }
        }
        if (p.err) { fail(p.errmsg, NULL); return false; }

        /* Line address 0 is only legal as the start of a `0,/re/` range. */
        if (c->a1.type == A_ZERO && (!c->has_a2 || c->a2.type != A_RE)) {
            fail("invalid use of line address 0", NULL);
            return false;
        }

        skip_ws(&p);
        while (pp(&p) == '!') { pgc(&p); c->negate = !c->negate; skip_ws(&p); }

        int cmd = pgc(&p);
        if (cmd == -1) { perr(&p, "expected command"); fail(p.errmsg, NULL); return false; }
        c->cmd = (char)cmd;

        switch (cmd) {
        case '{':
            if (bdepth >= 64) { fail("blocks nested too deep", NULL); return false; }
            brace_stack[bdepth++] = ncmds - 1;
            break;
        case '}':
            if (bdepth == 0) { fail("unexpected `}'", NULL); return false; }
            cmds[brace_stack[--bdepth]].target = ncmds;   /* '{' jumps past '}' */
            break;
        case 's': if (!parse_s(&p, c))  { fail(p.errmsg, NULL); return false; } break;
        case 'y': if (!parse_y(&p, c))  { fail(p.errmsg, NULL); return false; } break;
        case 'a': case 'i': case 'c':
            c->text = parse_text(&p);
            break;
        case ':': c->text = parse_token(&p); break;       /* label name */
        case 'b': case 't': case 'T':
            c->text = parse_token(&p);                    /* may be empty => end */
            break;
        case 'r': case 'R': case 'w': case 'W':
            c->text = parse_filename(&p);
            break;
        case 'q': case 'Q': {
            skip_ws(&p); long v = 0; bool any = false;
            while (pp(&p) >= '0' && pp(&p) <= '9') { v = v * 10 + (pgc(&p) - '0'); any = true; }
            c->exit_code = any ? (int)v : 0;
            break;
        }
        case 'd': case 'D': case 'p': case 'P': case 'n': case 'N':
        case 'h': case 'H': case 'g': case 'G': case 'x': case '=':
        case 'l': case 'z':
            break;                                         /* no operands */
        default:
            fail("unknown command", NULL);
            axl_printerr("sed: near offset %ld\n", (long)(p.sp - text));
            return false;
        }
        if (p.err) { fail(p.errmsg, NULL); return false; }
    }
    if (bdepth != 0) { fail("unmatched `{'", NULL); return false; }
    return true;
}

/* Resolve b/t/T label targets to command indices. */
static bool resolve_labels(void)
{
    for (int i = 0; i < ncmds; i++) {
        char k = cmds[i].cmd;
        if (k != 'b' && k != 't' && k != 'T') continue;
        const char *lbl = cmds[i].text;
        if (lbl == NULL || lbl[0] == '\0') { cmds[i].target = ncmds; continue; }  /* -> end */
        int found = -1;
        for (int j = 0; j < ncmds; j++)
            if (cmds[j].cmd == ':' && cmds[j].text && axl_strcmp(cmds[j].text, lbl) == 0) { found = j; break; }
        if (found < 0) { fail("can't find label", lbl); return false; }
        cmds[i].target = found;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

static Buf ps;             /* pattern space */
static Buf hold;           /* hold space */
static long line_no;       /* current input line number (1-based) */
static bool last_line;     /* current line is the last */
static bool sub_made;      /* a substitution happened since last line / t */
static AxlRegex *last_re;  /* most recently used regex (for `//` reuse) */
static Buf append_q;       /* queued a/r output, flushed after auto-print */
static bool had_newline;   /* input line ended with a newline */
static int  exit_status;   /* from q/Q */

static AxlStream *out;      /* axl_stdout */

static void emit(const char *s, size_t n) { axl_write(out, s, n); }
static void emit_sep(void) { emit(opt_null_data ? "\0" : "\n", 1); }   /* line terminator (-z = NUL) */
static void emit_line(const char *s, size_t n) { emit(s, n); emit_sep(); }

/* Run a regex over the pattern space, returning the match + groups. */
static bool ps_search(AxlRegex *re, size_t from, AxlMatch *groups, size_t ng)
{
    AxlMemReader mr;
    axl_mem_reader_init(&mr, ps.p, ps.len);
    return axl_regex_search_captures(re, &mr.reader, from, AXL_REGEX_MATCH_DEFAULT, groups, ng);
}

static bool addr_re_match(AxlRegex *re)
{
    AxlRegex *r = re ? re : last_re;
    if (r == NULL) return false;
    last_re = r;
    AxlMatch m;
    AxlMemReader mr;
    axl_mem_reader_init(&mr, ps.p, ps.len);
    return axl_regex_search_captures(r, &mr.reader, 0, AXL_REGEX_MATCH_DEFAULT, &m, 1);
}

static bool addr_one_match(const Addr *a)
{
    switch (a->type) {
    case A_LINE: return line_no == a->n;
    case A_LAST: return last_line;
    case A_RE:   return addr_re_match(a->re);
    case A_STEP: return a->step > 0 ? (line_no >= a->n && (line_no - a->n) % a->step == 0)
                                    : (line_no == a->n);
    case A_ZERO: return false;                 /* only meaningful as a range start */
    default:     return false;
    }
}

/* Decide whether `c` applies to the current line, updating range state. */
static bool cmd_selected(Cmd *c)
{
    bool sel;
    if (!c->has_a2) {
        sel = (c->a1.type == A_NONE) ? true : addr_one_match(&c->a1);
    } else if (!c->active) {
        bool start = (c->a1.type == A_ZERO) ? true : addr_one_match(&c->a1);
        if (start) {
            c->active = true;
            /* resolve relative / numeric ends; close immediately for a
               degenerate range that can't extend past this line. */
            if (c->a2.type == A_PLUS)  c->range_end = line_no + c->a2.n;
            else if (c->a2.type == A_TILDE) {
                long m = c->a2.n;
                c->range_end = (m <= 0) ? line_no : ((line_no / m) + 1) * m;
            }
            else c->range_end = 0;
            if (c->a1.type == A_ZERO) {
                /* 0,/re/ : the end may match on line 1; re-check below. */
                c->active = true;
            }
            if (c->a2.type == A_LINE && c->a2.n <= line_no) c->active = false;
            if ((c->a2.type == A_PLUS || c->a2.type == A_TILDE) && c->range_end <= line_no)
                c->active = false;
            sel = true;
            /* A_ZERO start: still test the end on THIS line. */
            if (c->a1.type == A_ZERO) {
                if ((c->a2.type == A_RE && addr_re_match(c->a2.re)) ||
                    (c->a2.type == A_LINE && line_no >= c->a2.n) || c->a2.type == A_LAST)
                    c->active = false;
            }
        } else sel = false;
    } else {
        sel = true;                              /* inside an open range */
        switch (c->a2.type) {
        case A_LINE:  if (line_no >= c->a2.n) c->active = false; break;
        case A_LAST:  if (last_line)          c->active = false; break;
        case A_RE:    if (addr_re_match(c->a2.re)) c->active = false; break;
        case A_PLUS: case A_TILDE: if (line_no >= c->range_end) c->active = false; break;
        default: c->active = false; break;
        }
    }
    return c->negate ? !sel : sel;
}

/* Expand an `s` replacement into `b`, using the captured groups over ps. */
static void expand_rhs(Buf *b, const char *rhs, const AxlMatch *g, size_t ng)
{
    for (const char *q = rhs; *q; ) {
        char ch = *q++;
        if (ch == '&') {
            buf_append(b, ps.p + g[0].start, g[0].length);
        } else if (ch == '\\') {
            char n = *q;
            if (n >= '0' && n <= '9') {
                size_t gi = (size_t)(n - '0');
                q++;
                if (gi < ng && g[gi].start != AXL_REGEX_NO_MATCH)
                    buf_append(b, ps.p + g[gi].start, g[gi].length);
            } else if (n == '&') { q++; buf_putc(b, '&'); }
            else if (n == '\\')  { q++; buf_putc(b, '\\'); }
            else if (n == '\0')  { buf_putc(b, '\\'); }
            else {
                const char *qq = q; int byte = decode_escape(&qq, rhs + axl_strlen(rhs)); q = qq;
                buf_putc(b, (char)byte);
            }
        } else {
            buf_putc(b, ch);
        }
    }
}

/* Apply an `s` command to the pattern space. Returns true if any sub made. */
static bool do_subst(Cmd *c)
{
    AxlRegex *re = c->re ? c->re : last_re;
    if (re == NULL) { fail("no previous regular expression", NULL); exit_status = 1; return false; }
    last_re = re;

    size_t ncap = axl_regex_capture_count(re);
    size_t ng = ncap + 1; if (ng > 10) ng = 10;
    AxlMatch g[10];

    Buf out_b = {0};
    size_t pos = 0;
    long matchno = 0;
    bool any = false;

    while (pos <= ps.len) {
        if (!ps_search(re, pos, g, ng)) break;
        matchno++;
        size_t ms = g[0].start, ml = g[0].length;
        bool replace_this = c->s_global ? (matchno >= c->s_nth) : (matchno == c->s_nth);

        buf_append(&out_b, ps.p + pos, ms - pos);   /* unmatched prefix */
        if (replace_this) {
            expand_rhs(&out_b, c->rhs, g, ng);
            any = true;
        } else {
            buf_append(&out_b, ps.p + ms, ml);       /* keep original */
        }

        if (ml == 0) {                               /* zero-width: emit one byte, advance */
            if (ms < ps.len) buf_putc(&out_b, ps.p[ms]);
            pos = ms + 1;
        } else {
            pos = ms + ml;
        }
        if (!c->s_global && matchno >= c->s_nth) break;
    }
    if (!any) { buf_free(&out_b); return false; }

    /* tail — guard the subtraction: a zero-width match at end-of-line leaves
       pos == ps.len + 1, so an unguarded `ps.len - pos` underflows size_t. */
    if (pos < ps.len) buf_append(&out_b, ps.p + pos, ps.len - pos);
    buf_free(&ps);
    ps = out_b;

    if (c->s_print) emit_line(ps.p, ps.len);
    if (c->s_wfile && c->s_wfile[0]) {
        AxlStream *w = axl_fopen(c->s_wfile, "a");
        if (w) { axl_write(w, ps.p, ps.len); axl_write(w, "\n", 1); axl_fclose(w); }
    }
    return true;
}

static void output_l(void)
{
    /* `l`: unambiguous form — escape non-printables, end with '$'. */
    Buf b = {0};
    for (size_t i = 0; i < ps.len; i++) {
        unsigned char ch = (unsigned char)ps.p[i];
        switch (ch) {
        case '\\': buf_append(&b, "\\\\", 2); break;
        case '\a': buf_append(&b, "\\a", 2); break;
        case '\b': buf_append(&b, "\\b", 2); break;
        case '\t': buf_append(&b, "\\t", 2); break;
        case '\n': buf_append(&b, "\\n", 2); break;
        case '\v': buf_append(&b, "\\v", 2); break;
        case '\f': buf_append(&b, "\\f", 2); break;
        case '\r': buf_append(&b, "\\r", 2); break;
        default:
            if (ch < 32 || ch >= 127) {
                char tmp[5]; axl_snprintf(tmp, sizeof tmp, "\\%03o", ch);
                buf_append(&b, tmp, 4);
            } else buf_putc(&b, (char)ch);
        }
    }
    buf_putc(&b, '$');
    emit(b.p, b.len); emit("\n", 1);
    buf_free(&b);
}

/* Read one record from `s` up to `delim` ('\0' under -z) or EOF, bounded at
   SED_LINE_MAX bytes.  The trailing delimiter is consumed and NOT stored.
   Sets *had_delim to true when the record ended on the delimiter (not EOF).
   Returns a heap-allocated NUL-terminated string, or NULL at clean EOF. */
static char *read_record(AxlStream *s, char delim, bool *had_delim)
{
    Buf b = {0};
    bool any = false;
    *had_delim = false;
    for (;;) {
        char ch;
        axl_ssize_t n = axl_read(s, &ch, 1);
        if (n <= 0) break;              /* EOF or error */
        any = true;
        if (ch == delim) { *had_delim = true; break; }
        if (b.len < SED_LINE_MAX) buf_putc(&b, ch);
        /* At cap: keep consuming until delimiter so the next call starts
           cleanly at the next record boundary. */
    }
    if (!any) { buf_free(&b); return NULL; }
    buf_putc(&b, '\0');
    return b.p;
}

/* Input source: a list of files (or stdin). One-line lookahead lets us flag
   the last line ($) and implement N/n correctly.
   In NUL mode (-z) curtxt is NULL and records are read byte-by-byte from cur
   via read_record; in normal mode curtxt is a text-stream wrapper and records
   are read via axl_readline_max. */
typedef struct {
    char       **files; int nfiles, fidx;
    AxlStream   *cur, *curtxt; bool owns;
    char        *pending;           /* next record (owned; NL mode: includes trailing \n) */
    bool         pending_had_delim; /* NUL mode: pending record was terminated by NUL */
    int          pending_fidx;      /* file index the pending record came from */
    bool         reading_had_delim; /* NUL mode: most recent read_record hit NUL */
    int          reading_fidx;      /* file index input_raw_line is currently in */
    int          cur_fidx;          /* file index of the record now in the pattern space */
} Input;

static bool input_open_next(Input *in)
{
    while (in->fidx < in->nfiles) {
        const char *f = in->files[in->fidx++];
        if (axl_strcmp(f, "-") == 0) { in->cur = axl_stdin; in->owns = false; }
        else {
            in->cur = axl_fopen(f, "r");
            if (in->cur == NULL) {
                axl_printerr("sed: cannot open '%s'\n", f);
                exit_status = 2;
                continue;
            }
            in->owns = true;
        }
        if (!opt_null_data) {
            /* Normal mode: wrap for encoding detection and readline. */
            in->curtxt = axl_text_stream_wrap(in->cur);
            if (in->curtxt == NULL) { if (in->owns) axl_fclose(in->cur); continue; }
        }
        in->reading_fidx = in->fidx - 1;      /* the file we just opened */
        return true;
    }
    return false;
}

static char *input_raw_line(Input *in)
{
    for (;;) {
        if (opt_null_data) {
            /* NUL mode: read from the raw stream; no text-wrap needed. */
            if (in->cur == NULL && !input_open_next(in)) return NULL;
            char *l = read_record(in->cur, '\0', &in->reading_had_delim);
            if (l != NULL) return l;
            /* EOF on this file — close and try next. */
            if (in->owns) axl_fclose(in->cur);
            in->cur = NULL;
        } else {
            /* Normal mode: bounded readline via text-stream wrapper. */
            if (in->curtxt == NULL && !input_open_next(in)) return NULL;
            char *l = axl_readline_max(in->curtxt, SED_LINE_MAX + 1);
            if (l != NULL) return l;
            axl_fclose(in->curtxt); in->curtxt = NULL;
            if (in->owns) axl_fclose(in->cur);
            in->cur = NULL;
        }
        if (!input_open_next(in)) return NULL;
    }
}

/* Close any still-open input stream (early `q`/`Q`) and free the lookahead. */
static void input_close(Input *in)
{
    if (in->curtxt) axl_fclose(in->curtxt);
    if (in->owns && in->cur) axl_fclose(in->cur);
    in->curtxt = NULL; in->cur = NULL;
    axl_free(in->pending); in->pending = NULL;
}

/* Fetch the next logical line into ps; set line_no/last_line/had_newline.
   Returns false at EOF. */
static bool next_line(Input *in)
{
    char *l; int lf; bool cur_had_delim;
    if (in->pending != NULL) {
        l = in->pending; lf = in->pending_fidx;
        cur_had_delim = in->pending_had_delim;
    } else {
        l = input_raw_line(in); lf = in->reading_fidx;
        cur_had_delim = in->reading_had_delim;
    }
    if (l == NULL) return false;
    in->pending = input_raw_line(in);
    in->pending_fidx = in->reading_fidx;
    in->pending_had_delim = in->reading_had_delim;

    /* Last line of the stream — or, under -s, of the current file. */
    last_line = (in->pending == NULL) || (opt_separate && in->pending_fidx != lf);
    /* Continuous numbering, reset per file under -s. */
    if (opt_separate && lf != in->cur_fidx) line_no = 0;
    in->cur_fidx = lf;

    size_t n = axl_strlen(l);
    if (opt_null_data) {
        had_newline = cur_had_delim;
    } else {
        had_newline = (n > 0 && l[n-1] == '\n');
        if (had_newline) n--;
        if (n > 0 && l[n-1] == '\r') n--;    /* ignore CR (CRLF / shell pipe), as legacy sed does */
    }
    buf_set(&ps, l, n);
    axl_free(l);
    line_no++;
    return true;
}

/* Append the next input line to ps (the N command). Returns false if none.
   Under -s, refuses to pull a line across a file boundary. */
static bool append_next_line(Input *in)
{
    if (in->pending == NULL) {
        in->pending = input_raw_line(in);
        in->pending_fidx = in->reading_fidx;
        in->pending_had_delim = in->reading_had_delim;
    }
    if (in->pending == NULL) return false;
    if (opt_separate && in->pending_fidx != in->cur_fidx) return false;

    char *l = in->pending;
    bool cur_had_delim = in->pending_had_delim;
    in->pending = input_raw_line(in);
    in->pending_fidx = in->reading_fidx;
    in->pending_had_delim = in->reading_had_delim;
    last_line = (in->pending == NULL) || (opt_separate && in->pending_fidx != in->cur_fidx);

    size_t n = axl_strlen(l);
    if (opt_null_data) {
        had_newline = cur_had_delim;
    } else {
        if (n > 0 && l[n-1] == '\n') { had_newline = true; n--; } else had_newline = false;
        if (n > 0 && l[n-1] == '\r') n--;    /* ignore CR (CRLF / shell pipe) */
    }
    buf_putc(&ps, '\n');
    buf_append(&ps, l, n);
    axl_free(l);
    line_no++;
    return true;
}

static void flush_appends(void)
{
    if (append_q.len) { emit(append_q.p, append_q.len); append_q.len = 0; }
}

static void auto_print(void)
{
    if (!opt_quiet) {
        emit(ps.p, ps.len);
        if (had_newline) emit(opt_null_data ? "\0" : "\n", 1);
    }
}

typedef enum { CYC_NORMAL, CYC_DELETE, CYC_RESTART, CYC_QUIT, CYC_QUIT_SILENT } CycleResult;

/* Run the program over the current pattern space. Returns how the cycle ends. */
static CycleResult run_cycle(Input *in)
{
    int pc = 0;
    while (pc < ncmds) {
        Cmd *c = &cmds[pc];

        if (c->cmd == ':') { pc++; continue; }
        if (c->cmd == '}') { pc++; continue; }

        bool sel = cmd_selected(c);
        if (c->cmd == '{') { pc = sel ? pc + 1 : c->target; continue; }
        if (!sel) { pc++; continue; }

        switch (c->cmd) {
        case 's': if (do_subst(c)) sub_made = true; break;
        case 'y': for (size_t i = 0; i < ps.len; i++) ps.p[i] = (char)c->ymap[(unsigned char)ps.p[i]]; break;
        case 'd': return CYC_DELETE;
        case 'D': {
            char *nl = axl_memchr(ps.p, '\n', ps.len);
            if (nl != NULL) {
                size_t keep = ps.len - (size_t)(nl + 1 - ps.p);
                axl_memmove(ps.p, nl + 1, keep); ps.len = keep;
                return CYC_RESTART;                   /* restart without reading */
            }
            return CYC_DELETE;
        }
        case 'p': emit_line(ps.p, ps.len); break;
        case 'P': {
            char *nl = axl_memchr(ps.p, '\n', ps.len);
            size_t n = nl ? (size_t)(nl - ps.p) : ps.len;
            emit_line(ps.p, n);
            break;
        }
        case 'n':
            auto_print();
            flush_appends();
            if (!next_line(in)) return CYC_QUIT_SILENT;
            break;
        case 'N':
            if (!append_next_line(in)) {
                /* GNU: print pattern space and quit (POSIX would delete). */
                return CYC_NORMAL;
            }
            break;
        case 'h': buf_set(&hold, ps.p, ps.len); break;
        case 'H': buf_putc(&hold, '\n'); buf_append(&hold, ps.p, ps.len); break;
        case 'g': buf_set(&ps, hold.p, hold.len); break;
        case 'G': buf_putc(&ps, '\n'); buf_append(&ps, hold.p, hold.len); break;
        case 'x': { Buf t = ps; ps = hold; hold = t; break; }
        case 'b': pc = c->target; continue;
        case 't': if (sub_made) { sub_made = false; pc = c->target; continue; } break;
        case 'T': if (!sub_made) { pc = c->target; continue; } sub_made = false; break;
        case 'a': buf_append(&append_q, c->text, axl_strlen(c->text)); buf_putc(&append_q, opt_null_data ? '\0' : '\n'); break;
        case 'i': emit(c->text, axl_strlen(c->text)); emit_sep(); break;
        case 'c':
            /* Print text once, at the end of the range (or for a single-line
               address). A range that never closes before EOF prints nothing —
               this matches GNU sed. */
            if (!c->has_a2 || !c->active) { emit(c->text, axl_strlen(c->text)); emit_sep(); }
            return CYC_DELETE;
        case 'r': {                                  /* queue file contents after the cycle */
            AxlStream *f = axl_fopen(c->text, "r");
            if (f != NULL) {
                AxlStream *t = axl_text_stream_wrap(f);
                if (t != NULL) {
                    char *ln;
                    while ((ln = axl_readline_max(t, SED_LINE_MAX + 1)) != NULL) {
                        buf_append(&append_q, ln, axl_strlen(ln));
                        axl_free(ln);
                    }
                    axl_fclose(t);
                }
                axl_fclose(f);
            }
            break;
        }
        case 'w': { AxlStream *w = axl_fopen(c->text, "a");
                    if (w) { axl_write(w, ps.p, ps.len); axl_write(w, "\n", 1); axl_fclose(w); } break; }
        case '=': { char tmp[24]; axl_snprintf(tmp, sizeof tmp, "%ld", line_no); emit(tmp, axl_strlen(tmp)); emit_sep(); break; }
        case 'l': output_l(); break;
        case 'z': ps.len = 0; break;
        case 'q': exit_status = c->exit_code; return CYC_QUIT;
        case 'Q': exit_status = c->exit_code; return CYC_QUIT_SILENT;
        default: break;
        }
        pc++;
    }
    return CYC_NORMAL;
}

static int run(Input *in)
{
    out = axl_stdout;
    while (next_line(in)) {
        sub_made = false;
restart:;
        CycleResult r = run_cycle(in);
        switch (r) {
        case CYC_NORMAL:      auto_print(); flush_appends(); break;
        case CYC_DELETE:      flush_appends(); break;
        case CYC_RESTART:     flush_appends(); goto restart;
        case CYC_QUIT:        auto_print(); flush_appends(); return exit_status;
        case CYC_QUIT_SILENT: flush_appends(); return exit_status;
        }
    }
    return exit_status;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

AXL_TOOL_MAIN(sed)
{
    Buf script = {0};
    bool have_script = false;
    char *files[256]; int nfiles = 0;
    bool no_more_opts = false;

    int i = 1;
    /* Options + -e/-f scripts. */
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (no_more_opts || a[0] != '-' || a[1] == '\0') {
            /* positional: first is the script if none given via -e/-f */
            if (!have_script) { buf_append(&script, a, axl_strlen(a)); have_script = true; }
            else if (nfiles < 256) files[nfiles++] = (char *)a;
            continue;
        }
        if (axl_strcmp(a, "--") == 0) { no_more_opts = true; continue; }
        for (int k = 1; a[k]; k++) {
            char o = a[k];
            if (o == 'n') opt_quiet = true;
            else if (o == 'E' || o == 'r') re_syntax = AXL_REGEX_DEFAULT;
            else if (o == 's') opt_separate = true;
            else if (o == 'z') opt_null_data = true;
            else if (o == 'e' || o == 'f') {
                const char *arg = a[k+1] ? a + k + 1 : (++i < argc ? argv[i] : NULL);
                if (arg == NULL) { fail("option requires an argument", a); return 2; }
                if (script.len) buf_putc(&script, '\n');
                if (o == 'e') buf_append(&script, arg, axl_strlen(arg));
                else {
                    AxlStream *f = axl_fopen(arg, "r");
                    if (f == NULL) { fail("can't read script file", arg); return 2; }
                    AxlStream *t = axl_text_stream_wrap(f);
                    if (t != NULL) {
                        char *ln;
                        while ((ln = axl_readline_max(t, SED_LINE_MAX + 1)) != NULL) { buf_append(&script, ln, axl_strlen(ln)); axl_free(ln); }
                        axl_fclose(t);
                    }
                    axl_fclose(f);
                }
                have_script = true;
                break;                                  /* rest of arg consumed */
            }
            else { fail("invalid option", a); return 2; }
        }
    }

    if (!have_script) { fail("no script", NULL); return 2; }

    buf_putc(&script, '\0');
    if (!parse_script(script.p, script.len - 1) || !resolve_labels()) {
        free_program(); buf_free(&script);
        return 2;
    }

    Input in = {0};
    if (nfiles == 0) { static char *stdin_only[] = { (char *)"-" }; in.files = stdin_only; in.nfiles = 1; }
    else { in.files = files; in.nfiles = nfiles; }

    int rc = run(&in);

    input_close(&in);
    free_program();
    buf_free(&ps); buf_free(&hold); buf_free(&append_q); buf_free(&script);
    return rc;
}
