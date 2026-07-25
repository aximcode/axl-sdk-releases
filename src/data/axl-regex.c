/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-regex.c
    Regular-expression matcher: recursive-descent parse -> bytecode ->
    Pike VM (Thompson NFA with submatch). Linear time in input × program
    for every pattern — no backtracking, hence no "ReDoS" blowup.

    Unanchored leftmost search is a lazy `.*?` prefix compiled ahead of
    the pattern, so the VM runs anchored from a single start state while
    still finding the leftmost match. See docs/AXL-Design.md / axl-regex.h.
**/

#include <axl/axl-regex.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("regex");

#define MAX_GROUPS 31              /* group 0 + 31 captures = 64 save slots */
#define MAX_SLOTS  (2 * (MAX_GROUPS + 1))

// ---------------------------------------------------------------------------
// Character-class bitmap (256 bits)
// ---------------------------------------------------------------------------

typedef struct { uint8_t b[32]; } ClassSet;
static void cs_add(ClassSet *s, uint8_t c) { s->b[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static bool cs_has(const ClassSet *s, uint8_t c) { return (s->b[c >> 3] >> (c & 7)) & 1u; }
static void cs_neg(ClassSet *s) { for (int i = 0; i < 32; i++) s->b[i] ^= 0xFF; }
static void cs_add_d(ClassSet *s) { for (int c = '0'; c <= '9'; c++) cs_add(s, (uint8_t)c); }
static void cs_add_w(ClassSet *s) {
    for (int c = 'a'; c <= 'z'; c++) cs_add(s, (uint8_t)c);
    for (int c = 'A'; c <= 'Z'; c++) cs_add(s, (uint8_t)c);
    cs_add_d(s); cs_add(s, '_');
}
static void cs_add_s(ClassSet *s) {
    static const char ws[] = " \t\n\r\f\v";
    for (const char *p = ws; *p; p++) cs_add(s, (uint8_t)*p);
}
/* Fold ASCII case: ensure both cases of every letter are present. */
static void cs_fold_case(ClassSet *s) {
    for (int c = 'a'; c <= 'z'; c++) {
        if (cs_has(s, (uint8_t)c)) cs_add(s, (uint8_t)(c - 'a' + 'A'));
        if (cs_has(s, (uint8_t)(c - 'a' + 'A'))) cs_add(s, (uint8_t)c);
    }
}
static uint8_t ascii_lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c; }

// ---------------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------------

typedef enum { N_LIT, N_ANY, N_CLASS, N_CAT, N_ALT, N_STAR, N_PLUS, N_QUEST,
               N_GROUP, N_BOL, N_EOL, N_EMPTY } Kind;
typedef struct Node {
    Kind          kind;
    uint8_t       ch;       /* N_LIT */
    ClassSet      cls;      /* N_CLASS */
    struct Node  *a, *b;    /* children */
    bool          greedy;   /* quantifiers */
    int           group;    /* N_GROUP capture index (1..N) */
} Node;

// ---------------------------------------------------------------------------
// Parser (recursive descent)
// ---------------------------------------------------------------------------

typedef struct {
    const char *start, *p, *end;
    int         ngroups;     /* groups assigned so far */
    bool        bre;         /* POSIX Basic RE syntax (AXL_REGEX_BRE) */
    bool        err;
    size_t      err_off;
    const char *err_msg;
    Node      **pool;        /* every allocated node — flat ownership list */
    size_t      pool_n, pool_cap;
    Node        sentinel;    /* returned on OOM; never freed, never compiled */
} Parser;

static void perr(Parser *s, const char *msg) {
    if (!s->err) { s->err = true; s->err_off = (size_t)(s->p - s->start); s->err_msg = msg; }
}

/* Allocate a node, tracking it in the parser pool so cleanup is a flat
   free (no recursion, no double-free on OOM-corrupted trees). On any
   allocation failure, flag the error and return the embedded sentinel so
   callers never dereference NULL. */
static Node *node_new(Parser *s, Kind k) {
    if (s->err) return &s->sentinel;
    Node *n = axl_calloc(1, sizeof *n);
    if (n == NULL) { perr(s, "out of memory"); return &s->sentinel; }
    if (s->pool_n == s->pool_cap) {
        size_t nc = s->pool_cap ? s->pool_cap * 2 : 16;
        Node **np = axl_realloc(s->pool, nc * sizeof(Node *));
        if (np == NULL) { axl_free(n); perr(s, "out of memory"); return &s->sentinel; }
        s->pool = np; s->pool_cap = nc;
    }
    s->pool[s->pool_n++] = n;
    n->kind = k; n->greedy = true;
    return n;
}

static void parser_free_nodes(Parser *s) {
    for (size_t i = 0; i < s->pool_n; i++) axl_free(s->pool[i]);
    axl_free(s->pool);
}
static int ppeek(Parser *s) { return s->p < s->end ? (unsigned char)*s->p : -1; }
static int pget(Parser *s) { return s->p < s->end ? (unsigned char)*s->p++ : -1; }

/* Metacharacter token layer for the BRE/ERE split. The grouping, interval,
   alternation, and `+`/`?` metacharacters are bare in ERE (`(`, `{`, `|`, …)
   but backslashed in BRE (`\(`, `\{`, `\|`, …) — with the bare forms literal.
   at_meta() asks "does the input here start the meta `m`?" in the current
   syntax; take_meta() consumes it (1 byte in ERE, 2 in BRE). The recursive
   descent reads identically in both modes by going through these. The bytes
   `m` routed through here are exactly the ones that invert: ( ) { } | + ? */
static bool at_meta(Parser *s, char m) {
    if (s->bre)
        return s->p + 1 < s->end && s->p[0] == '\\' && s->p[1] == m;
    return s->p < s->end && s->p[0] == m;
}
static void take_meta(Parser *s) { s->p += s->bre ? 2 : 1; }

static Node *parse_alt(Parser *s);

static int esc_char(int c) {
    switch (c) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
    case 'f': return '\f'; case 'v': return '\v'; default: return c;
    }
}

static Node *parse_class(Parser *s) {
    pget(s);  /* '[' */
    Node *n = node_new(s, N_CLASS);
    bool neg = false;
    if (ppeek(s) == '^') { pget(s); neg = true; }
    while (ppeek(s) != ']' && ppeek(s) != -1) {
        int c = pget(s);
        if (c == '\\') {
            int e = pget(s);
            if (e == -1) { perr(s, "trailing backslash in class"); break; }
            if (e == 'd') { cs_add_d(&n->cls); continue; }
            if (e == 'w') { cs_add_w(&n->cls); continue; }
            if (e == 's') { cs_add_s(&n->cls); continue; }
            c = esc_char(e);
        }
        if (ppeek(s) == '-' && s->p + 1 < s->end && s->p[1] != ']') {
            pget(s);  /* '-' */
            int hi = pget(s);
            if (hi == '\\') hi = esc_char(pget(s));
            for (int x = c; x <= hi; x++) cs_add(&n->cls, (uint8_t)x);
        } else {
            cs_add(&n->cls, (uint8_t)c);
        }
    }
    if (ppeek(s) == ']') pget(s); else perr(s, "unterminated character class");
    if (neg) cs_neg(&n->cls);
    return n;
}

static Node *parse_atom(Parser *s, bool at_start) {
    /* Grouping: `( )` in ERE, `\( \)` in BRE. */
    if (at_meta(s, '(')) {
        take_meta(s);
        int grp = ++s->ngroups;
        if (grp > MAX_GROUPS) { perr(s, "too many capture groups"); }
        Node *body = parse_alt(s);
        if (at_meta(s, ')')) take_meta(s); else perr(s, "unbalanced parenthesis");
        Node *g = node_new(s, N_GROUP); g->group = grp; g->a = body;
        return g;
    }
    if (at_meta(s, ')')) { perr(s, "unbalanced parenthesis"); return node_new(s, N_EMPTY); }
    int c = ppeek(s);
    if (c == '[') return parse_class(s);
    if (c == '.') { pget(s); return node_new(s, N_ANY); }
    if (c == '^') {
        /* ERE: `^` is always an anchor. BRE: an anchor only at the start of
           the expression/subexpression; a literal `^` anywhere else. */
        pget(s);
        if (!s->bre || at_start) return node_new(s, N_BOL);
        Node *n = node_new(s, N_LIT); n->ch = (uint8_t)'^'; return n;
    }
    if (c == '$') {
        /* ERE: `$` is always an anchor. BRE: an anchor only at the end of the
           expression/subexpression (next is end-of-pattern, `\)`, or `\|`). */
        pget(s);
        if (!s->bre || s->p >= s->end || at_meta(s, ')') || at_meta(s, '|'))
            return node_new(s, N_EOL);
        Node *n = node_new(s, N_LIT); n->ch = (uint8_t)'$'; return n;
    }
    if (c == '\\') {
        pget(s);
        int e = pget(s);
        if (e == -1) { perr(s, "trailing backslash"); return node_new(s, N_EMPTY); }
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            Node *n = node_new(s, N_CLASS);
            if (e == 'd' || e == 'D') cs_add_d(&n->cls);
            if (e == 'w' || e == 'W') cs_add_w(&n->cls);
            if (e == 's' || e == 'S') cs_add_s(&n->cls);
            if (e == 'D' || e == 'W' || e == 'S') cs_neg(&n->cls);
            return n;
        }
        Node *n = node_new(s, N_LIT); n->ch = (uint8_t)esc_char(e); return n;
    }
    Node *n = node_new(s, N_LIT); n->ch = (uint8_t)pget(s); return n;
}

/* Deep-copy a subtree into the parser pool (so bounded-repetition `{n}`
   desugaring can emit independent instances of the repeated atom). A capture
   group inside the copy keeps its index — an interval-repeated group resolves
   to its LAST match (POSIX-style), which is fine for the non-capturing
   char-class / literal patterns intervals are normally used with. */
static Node *node_clone(Parser *s, const Node *n) {
    if (s->err || n == NULL) return node_new(s, N_EMPTY);
    Node *c = node_new(s, n->kind);
    if (s->err) return c;
    c->ch = n->ch; c->cls = n->cls; c->greedy = n->greedy; c->group = n->group;
    if (n->a) c->a = node_clone(s, n->a);
    if (n->b) c->b = node_clone(s, n->b);
    return c;
}

/* Parse a `{n}` / `{n,}` / `{n,m}` / `{,m}` interval. The caller has already
   confirmed the next char is '{' and SAVED s->p; on any non-interval shape
   this returns false (consuming input that the caller then rolls back), so a
   literal '{' still parses as a plain character. Counts clamp to REP_MAX so
   `a{999999}` can't blow up the AST. @hi == -1 means unbounded. */
static bool parse_interval(Parser *s, int *lo, int *hi) {
    enum { REP_MAX = 1024 };
    take_meta(s);                         /* consume '{' (ERE) or '\{' (BRE) */
    int min = 0, max = 0, c;
    bool hmin = false, hmax = false, comma = false;
    while ((c = ppeek(s)) >= '0' && c <= '9') {
        pget(s); hmin = true; min = min * 10 + (c - '0'); if (min > REP_MAX) min = REP_MAX;
    }
    if (ppeek(s) == ',') {
        pget(s); comma = true;
        while ((c = ppeek(s)) >= '0' && c <= '9') {
            pget(s); hmax = true; max = max * 10 + (c - '0'); if (max > REP_MAX) max = REP_MAX;
        }
    }
    if (!at_meta(s, '}')) return false;   /* not an interval (e.g. a literal '{') */
    take_meta(s);                         /* consume '}' (ERE) or '\}' (BRE) */
    if (!hmin && !hmax) return false;     /* {} or {,} — treat '{' literally */
    if (!comma)     { *lo = min;            *hi = min; }   /* {n}   */
    else if (!hmax) { *lo = hmin ? min : 0; *hi = -1;  }   /* {n,}  */
    else            { *lo = hmin ? min : 0; *hi = max; }   /* {n,m} / {,m} */
    if (*hi >= 0 && *hi < *lo) return false;               /* {3,1} invalid */
    return true;
}

/* Concatenate two nodes (NULL-left returns right). */
static Node *node_cat(Parser *s, Node *left, Node *right) {
    if (!left) return right;
    Node *c = node_new(s, N_CAT); c->a = left; c->b = right;
    return c;
}

static Node *parse_repeat(Parser *s, bool at_start) {
    Node *a = parse_atom(s, at_start);
    for (;;) {
        /* Interval: `{n,m}` in ERE, `\{n,m\}` in BRE. */
        if (at_meta(s, '{')) {
            const char *save = s->p;
            int lo = 0, hi = 0;
            if (!parse_interval(s, &lo, &hi)) { s->p = save; break; }
            /* Desugar onto the existing quantifier compilation: `lo` required
               copies, then either `a*` (unbounded) or `hi-lo` optional `a?`. */
            Node *seq = NULL;
            for (int i = 0; i < lo; i++) seq = node_cat(s, seq, node_clone(s, a));
            if (hi < 0) {
                Node *star = node_new(s, N_STAR); star->a = node_clone(s, a);
                seq = node_cat(s, seq, star);
            } else {
                for (int i = lo; i < hi; i++) {
                    Node *q = node_new(s, N_QUEST); q->a = node_clone(s, a);
                    seq = node_cat(s, seq, q);
                }
            }
            a = seq ? seq : node_new(s, N_EMPTY);   /* {0} matches empty */
            continue;
        }
        /* `*` is bare in both syntaxes; `+` and `?` are bare in ERE and the
           backslashed `\+` / `\?` in BRE (the common GNU-BRE extension). */
        Kind k;
        if (ppeek(s) == '*')      { pget(s);      k = N_STAR;  }
        else if (at_meta(s, '+')) { take_meta(s); k = N_PLUS;  }
        else if (at_meta(s, '?')) { take_meta(s); k = N_QUEST; }
        else break;
        Node *q = node_new(s, k); q->a = a;
        /* Lazy suffix `?` is an ERE-only convenience — POSIX BRE has no
           non-greedy form, so don't consume a following `\?` as a modifier. */
        if (!s->bre && ppeek(s) == '?') { pget(s); q->greedy = false; }
        a = q;
    }
    return a;
}

static Node *parse_cat(Parser *s) {
    Node *left = NULL;
    bool first = true;   /* first atom of the branch — where a BRE `^` anchors */
    while (ppeek(s) != -1 && !at_meta(s, '|') && !at_meta(s, ')')) {
        Node *r = parse_repeat(s, first);
        first = false;
        if (s->err) break;   /* r is pool-owned; freed in parser_free_nodes */
        if (!left) left = r;
        else { Node *c = node_new(s, N_CAT); c->a = left; c->b = r; left = c; }
    }
    return left ? left : node_new(s, N_EMPTY);
}

static Node *parse_alt(Parser *s) {
    Node *left = parse_cat(s);
    while (at_meta(s, '|')) {   /* `|` in ERE, `\|` in BRE (GNU extension) */
        take_meta(s);
        Node *right = parse_cat(s);
        Node *a = node_new(s, N_ALT); a->a = left; a->b = right; left = a;
    }
    return left;
}

// ---------------------------------------------------------------------------
// Bytecode
// ---------------------------------------------------------------------------

typedef enum { I_CHAR, I_ANY, I_ANYALL, I_CLASS, I_MATCH, I_JMP, I_SPLIT,
               I_SAVE, I_BOL, I_EOL } Op;
typedef struct { Op op; uint8_t ch; ClassSet cls; int x, y, n; } Inst;
typedef struct { Inst *v; int n, cap; bool oom; } Prog;

static int emit(Prog *p, Inst in) {
    if (p->oom) return p->n;
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 32;
        Inst *nv = axl_realloc(p->v, (size_t)nc * sizeof(Inst));
        if (nv == NULL) { p->oom = true; return p->n; }
        p->v = nv; p->cap = nc;
    }
    p->v[p->n] = in;
    return p->n++;
}

static void compile(Prog *p, Node *n, uint32_t flags) {
    Inst z = { 0 };
    if (p->oom) return;   /* stop emitting once an allocation has failed */
    switch (n->kind) {
    case N_EMPTY: break;
    case N_LIT:  { Inst i = z; i.op = I_CHAR; i.ch = n->ch; emit(p, i); break; }
    case N_ANY:  { Inst i = z; i.op = I_ANY; emit(p, i); break; }
    case N_CLASS:{ Inst i = z; i.op = I_CLASS; i.cls = n->cls;
                   if (flags & AXL_REGEX_CASELESS) cs_fold_case(&i.cls);
                   emit(p, i); break; }
    case N_BOL:  { Inst i = z; i.op = I_BOL; emit(p, i); break; }
    case N_EOL:  { Inst i = z; i.op = I_EOL; emit(p, i); break; }
    case N_CAT:  compile(p, n->a, flags); compile(p, n->b, flags); break;
    case N_GROUP: {
        Inst s0 = z; s0.op = I_SAVE; s0.n = 2 * n->group;     emit(p, s0);
        compile(p, n->a, flags);
        Inst s1 = z; s1.op = I_SAVE; s1.n = 2 * n->group + 1; emit(p, s1);
        break; }
    case N_ALT: {
        Inst sp = z; sp.op = I_SPLIT; int s = emit(p, sp);
        if (p->oom) break;   /* s is one-past-end on OOM — don't write p->v[s] */
        p->v[s].x = p->n; compile(p, n->a, flags);
        Inst jm = z; jm.op = I_JMP; int j = emit(p, jm);
        if (p->oom) break;
        p->v[s].y = p->n; compile(p, n->b, flags);
        p->v[j].x = p->n;
        break; }
    case N_STAR: {
        Inst sp = z; sp.op = I_SPLIT; int s = emit(p, sp);
        if (p->oom) break;
        int body = p->n; compile(p, n->a, flags);
        Inst jm = z; jm.op = I_JMP; jm.x = s; emit(p, jm);
        int after = p->n;
        if (n->greedy) { p->v[s].x = body; p->v[s].y = after; }
        else           { p->v[s].x = after; p->v[s].y = body; }
        break; }
    case N_PLUS: {
        int body = p->n; compile(p, n->a, flags);
        Inst sp = z; sp.op = I_SPLIT; int s = emit(p, sp);
        if (p->oom) break;
        if (n->greedy) { p->v[s].x = body; p->v[s].y = p->n; }
        else           { p->v[s].x = p->n; p->v[s].y = body; }
        break; }
    case N_QUEST: {
        Inst sp = z; sp.op = I_SPLIT; int s = emit(p, sp);
        if (p->oom) break;
        int body = p->n; compile(p, n->a, flags);
        int after = p->n;
        if (n->greedy) { p->v[s].x = body; p->v[s].y = after; }
        else           { p->v[s].x = after; p->v[s].y = body; }
        break; }
    }
}

// ---------------------------------------------------------------------------
// Compiled regex
// ---------------------------------------------------------------------------

struct AxlRegex {
    Inst    *prog;
    int      prog_len;
    int      enter_pc;   /* the Save0 inst — the anchored entry past the .*? prefix */
    int      ncap;       /* number of capture groups (excl. group 0) */
    uint32_t flags;
};

/* Build: SPLIT(enter, consume) ; enter: SAVE0 <pat> SAVE1 MATCH ;
          consume: ANYALL ; JMP 0 */
static AxlRegex *build(Node *ast, int ngroups, uint32_t flags) {
    Prog p = { 0 };
    Inst z = { 0 };
    Inst sp = z; sp.op = I_SPLIT; int s = emit(&p, sp);
    int enter = p.n;
    Inst sv0 = z; sv0.op = I_SAVE; sv0.n = 0; emit(&p, sv0);
    compile(&p, ast, flags);
    Inst sv1 = z; sv1.op = I_SAVE; sv1.n = 1; emit(&p, sv1);
    Inst mt = z; mt.op = I_MATCH; emit(&p, mt);
    int consume = p.n;
    Inst any = z; any.op = I_ANYALL; emit(&p, any);
    Inst jm = z; jm.op = I_JMP; jm.x = s; emit(&p, jm);
    if (p.oom) { axl_free(p.v); return NULL; }
    p.v[s].x = enter;
    p.v[s].y = consume;

    AxlRegex *re = axl_calloc(1, sizeof *re);
    if (re == NULL) { axl_free(p.v); return NULL; }
    re->prog = p.v; re->prog_len = p.n; re->enter_pc = enter;
    re->ncap = ngroups; re->flags = flags;
    return re;
}

AxlRegex *
axl_regex_new_full(const char *pattern, uint32_t flags, AxlRegexError *err)
{
    if (pattern == NULL) {
        axl_warning("NULL pattern");
        return NULL;
    }
    Parser s = { 0 };
    s.start = pattern; s.p = pattern; s.end = pattern + axl_strlen(pattern);
    s.bre = (flags & AXL_REGEX_BRE) != 0;
    Node *ast = parse_alt(&s);
    if (!s.err && s.p != s.end) perr(&s, "unexpected character");
    if (s.err) {
        if (err != NULL) { err->offset = s.err_off; err->message = s.err_msg; }
        axl_warning("bad pattern at %zu: %s", s.err_off, s.err_msg ? s.err_msg : "?");
        parser_free_nodes(&s);
        return NULL;
    }
    AxlRegex *re = build(ast, s.ngroups, flags);
    parser_free_nodes(&s);
    return re;
}

AxlRegex *
axl_regex_new(const char *pattern, uint32_t flags)
{
    return axl_regex_new_full(pattern, flags, NULL);
}

void
axl_regex_free(AxlRegex *re)
{
    if (re == NULL) return;
    axl_free(re->prog);
    axl_free(re);
}

size_t
axl_regex_capture_count(const AxlRegex *re)
{
    return (re != NULL) ? (size_t)re->ncap : 0;
}

// ---------------------------------------------------------------------------
// Pike VM
// ---------------------------------------------------------------------------

typedef struct {
    int    *pc;       /* [cap] */
    size_t *saved;    /* [cap * nslots] */
    int     n;
} TList;

typedef struct {
    const AxlRegex *re;
    const uint8_t  *in;
    size_t          len;
    int             nslots;
    bool            caseless, multiline, dotall;
    bool            notbol, noteol;  /* from_offset is mid-stream / end is mid-stream */
    int            *seen;     /* [prog_len] last listid each pc was added */
    int             listid;
} VM;

static void addthread(VM *vm, TList *l, int pc, const size_t *saved, size_t sp) {
    if (vm->seen[pc] == vm->listid) return;     /* dedup => linear */
    vm->seen[pc] = vm->listid;
    Inst *I = &vm->re->prog[pc];
    switch (I->op) {
    case I_JMP:   addthread(vm, l, I->x, saved, sp); break;
    case I_SPLIT: addthread(vm, l, I->x, saved, sp);
                  addthread(vm, l, I->y, saved, sp); break;
    case I_SAVE: {
        size_t s2[MAX_SLOTS] = {0};
        for (int i = 0; i < vm->nslots; i++) s2[i] = saved[i];
        if (I->n < vm->nslots) s2[I->n] = sp;
        addthread(vm, l, pc + 1, s2, sp);
        break; }
    case I_BOL: if ((sp == 0 && !vm->notbol)
                    || (vm->multiline && sp > 0 && vm->in[sp - 1] == '\n'))
                    addthread(vm, l, pc + 1, saved, sp);
                break;
    case I_EOL: if ((sp == vm->len && !vm->noteol)
                    || (vm->multiline && sp < vm->len && vm->in[sp] == '\n'))
                    addthread(vm, l, pc + 1, saved, sp);
                break;
    default: {
        int idx = l->n++;
        l->pc[idx] = pc;
        for (int i = 0; i < vm->nslots; i++) l->saved[idx * vm->nslots + i] = saved[i];
        break; }
    }
}

/* Run over in[0,len), starting the search at `from`. Anchored seeds only
   the post-prefix entry. On match, fills result[0..nslots) (absolute
   offsets; SIZE_MAX = slot unset). */
static bool vm_run(const AxlRegex *re, const uint8_t *in, size_t len, size_t from,
                   bool anchored, bool notbol, bool noteol,
                   size_t *result, int nslots) {
    VM vm = { re, in, len, nslots,
              (re->flags & AXL_REGEX_CASELESS) != 0,
              (re->flags & AXL_REGEX_MULTILINE) != 0,
              (re->flags & AXL_REGEX_DOTALL) != 0,
              notbol, noteol,
              NULL, 0 };
    int plen = re->prog_len;
    vm.seen = axl_malloc((size_t)plen * sizeof(int));
    TList cl = { axl_malloc((size_t)plen * sizeof(int)),
                 axl_malloc((size_t)plen * (size_t)nslots * sizeof(size_t)), 0 };
    TList nl = { axl_malloc((size_t)plen * sizeof(int)),
                 axl_malloc((size_t)plen * (size_t)nslots * sizeof(size_t)), 0 };
    if (!vm.seen || !cl.pc || !cl.saved || !nl.pc || !nl.saved) {
        axl_free(vm.seen); axl_free(cl.pc); axl_free(cl.saved);
        axl_free(nl.pc); axl_free(nl.saved);
        return false;
    }
    for (int i = 0; i < plen; i++) vm.seen[i] = -1;

    size_t init[MAX_SLOTS] = {0};
    for (int i = 0; i < nslots; i++) init[i] = (size_t)-1;

    bool matched = false;
    size_t msv[MAX_SLOTS] = {0};

    vm.listid++;
    addthread(&vm, &cl, anchored ? re->enter_pc : 0, init, from);

    for (size_t sp = from; sp <= len; sp++) {
        if (cl.n == 0) break;
        vm.listid++;
        nl.n = 0;
        for (int i = 0; i < cl.n; i++) {
            int pc = cl.pc[i];
            const size_t *sv = &cl.saved[i * nslots];
            Inst *I = &re->prog[pc];
            uint8_t b = (sp < len) ? in[sp] : 0;
            bool consume = false;
            switch (I->op) {
            case I_CHAR:
                consume = (sp < len) &&
                          (vm.caseless ? ascii_lower(b) == ascii_lower(I->ch) : b == I->ch);
                break;
            case I_ANY:    consume = (sp < len) && (vm.dotall || b != '\n'); break;
            case I_ANYALL: consume = (sp < len); break;
            case I_CLASS:  consume = (sp < len) && cs_has(&I->cls, b); break;
            case I_MATCH:
                matched = true;
                for (int k = 0; k < nslots; k++) msv[k] = sv[k];
                i = cl.n;  /* cut lower-priority threads */
                break;
            default: break;
            }
            if (consume) addthread(&vm, &nl, pc + 1, sv, sp + 1);
        }
        TList t = cl; cl = nl; nl = t;
    }

    if (matched) for (int i = 0; i < nslots; i++) result[i] = msv[i];
    axl_free(vm.seen); axl_free(cl.pc); axl_free(cl.saved);
    axl_free(nl.pc); axl_free(nl.saved);
    return matched;
}

// ---------------------------------------------------------------------------
// Public search
// ---------------------------------------------------------------------------

static bool search_buf(const AxlRegex *re, const uint8_t *data, size_t len,
                       size_t from, uint32_t mf, AxlMatch *groups, size_t ng) {
    if (re == NULL || data == NULL || from > len) return false;
    int nslots = 2 * (re->ncap + 1);
    size_t result[MAX_SLOTS] = {0};
    bool anchored = (mf & AXL_REGEX_MATCH_ANCHORED) != 0;
    bool notbol   = (mf & AXL_REGEX_MATCH_NOTBOL)   != 0;
    bool noteol   = (mf & AXL_REGEX_MATCH_NOTEOL)   != 0;
    if (!vm_run(re, data, len, from, anchored, notbol, noteol, result, nslots))
        return false;

    for (size_t g = 0; g < ng; g++) {
        size_t s = result[2 * g], e = result[2 * g + 1];
        if (s == (size_t)-1 || e == (size_t)-1) {
            groups[g].start = AXL_REGEX_NO_MATCH; groups[g].length = 0;
        } else {
            groups[g].start = s; groups[g].length = e - s;
        }
    }
    return true;
}

bool
axl_regex_search_buf(const AxlRegex *re, const void *data, size_t len,
                     size_t from_offset, uint32_t match_flags, AxlMatch *out)
{
    if (out == NULL) return false;
    return search_buf(re, data, len, from_offset, match_flags, out, 1);
}

/* Obtain a contiguous view of the whole source (peek fast path, else
   materialize), run the matcher, fill up to ng groups. */
static bool search_reader(const AxlRegex *re, const AxlByteReader *reader,
                          size_t from, uint32_t mf, AxlMatch *groups, size_t ng) {
    if (re == NULL || reader == NULL || groups == NULL || ng == 0) return false;
    size_t len = reader->length(reader);
    if (from > len) return false;

    const char *view = (reader->peek != NULL) ? reader->peek(reader, 0, len) : NULL;
    if (view != NULL) {
        return search_buf(re, (const uint8_t *)view, len, from, mf, groups, ng);
    }
    /* Materialize the whole source (documented O(len) fallback). */
    uint8_t *buf = axl_malloc(len ? len : 1);
    if (buf == NULL) return false;
    size_t got = reader->read(reader, 0, buf, len);
    bool r = search_buf(re, buf, got, from, mf, groups, ng);
    axl_free(buf);
    return r;
}

bool
axl_regex_search(const AxlRegex *re, const AxlByteReader *reader,
                 size_t from_offset, uint32_t match_flags, AxlMatch *out)
{
    if (out == NULL) return false;
    return search_reader(re, reader, from_offset, match_flags, out, 1);
}

bool
axl_regex_search_captures(const AxlRegex *re, const AxlByteReader *reader,
                          size_t from_offset, uint32_t match_flags,
                          AxlMatch *groups, size_t n_groups)
{
    return search_reader(re, reader, from_offset, match_flags, groups, n_groups);
}
