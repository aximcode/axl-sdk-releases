/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-xml-parse.c
    AxlXmlReader — pull-token XML reader. Each
    @ref axl_xml_reader_next call advances through the next
    significant construct (START_ELEMENT, END_ELEMENT, TEXT,
    END_DOCUMENT). Comments, processing instructions, and DOCTYPE
    declarations are skipped silently; CDATA sections deliver as
    TEXT with `is_cdata` set. Attribute lookup on the current
    START_ELEMENT is via @ref axl_xml_reader_attr; the values are
    entity-decoded into a reader-owned scratch buffer that's
    invalidated on the next @ref axl_xml_reader_next call.
**/

#include <axl/axl-xml.h>
#include <axl/axl-string.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

#define MAX_DEPTH 64

typedef struct {
    const char *name;        ///< slice into r->buf
    size_t      name_len;
    size_t      value_off;   ///< offset into r->scratch
    size_t      value_len;
} AttrSlot;

/* One xmlns declaration in scope. Pushed by parse_start_element when
   it sees `xmlns=...` or `xmlns:prefix=...`; popped by parse_end_element
   (real END) or the synthetic-END branch (self-closing) when the
   declaring element closes. `depth` is the element-depth this binding
   was declared at, so the pop-walk is "drop while top.depth == d". */
typedef struct {
    uint32_t    depth;
    const char *prefix;      ///< slice into r->buf; NULL = default ns
    size_t      prefix_len;
    char       *uri;         ///< owned decoded copy; NUL-terminated
    size_t      uri_len;
} NsBinding;

struct AxlXmlReader {
    const char *buf;
    size_t      len;
    size_t      pos;
    uint32_t    line, col;   ///< 1-based parse position

    /* Tag-balance stack: qname slices into r->buf. */
    const char *stack_name[MAX_DEPTH];
    size_t      stack_name_len[MAX_DEPTH];
    uint32_t    depth;

    /* Namespace bindings in scope. Grows / shrinks across element
       opens and closes. Each entry's `uri` is a heap-owned strdup of
       the decoded attribute value — lifetime survives next() calls
       (unlike scratch). Bindings are LIFO by depth, so a
       linear-scan-from-top resolves prefixes with shadowing correctly. */
    NsBinding  *ns_bindings;
    size_t      ns_count;       ///< logically active (in-scope) count
    size_t      ns_alive_count; ///< count whose `uri` is still heap-owned;
                                ///<  ns_alive_count >= ns_count. Entries in
                                ///<  [ns_count, ns_alive_count) were popped
                                ///<  during the LAST token's emit and remain
                                ///<  alive for the caller's read; the next
                                ///<  reset_token_state frees them.
    size_t      ns_cap;

    /* When a self-closing element (`<foo/>`) is encountered we emit
       START_ELEMENT immediately and stash the END_ELEMENT pending so
       the next call returns it without re-parsing. */
    bool        emit_end_next;
    const char *pending_end_name;
    size_t      pending_end_name_len;

    /* Scratch buffer for decoded TEXT body + attribute values.
       Reset on every successful @ref axl_xml_reader_next. */
    char       *scratch;
    size_t      scratch_cap;
    size_t      scratch_len;

    /* Attribute table for the current START_ELEMENT (valid until
       the next next() call). */
    AttrSlot   *attrs;
    size_t      attr_count;
    size_t      attr_cap;
    bool        attrs_valid;

    /* Document state. */
    bool        root_emitted;     ///< root START has fired
    bool        root_closed;      ///< root END has fired
    bool        eof_delivered;    ///< END_DOCUMENT has fired

    /* Error state. */
    bool        error;
    uint32_t    error_line;
    uint32_t    error_col;
    const char *error_msg;
};

// ---------------------------------------------------------------------------
// Low-level scanning helpers
// ---------------------------------------------------------------------------

static int
peek(const AxlXmlReader *r)
{
    if (r->pos >= r->len) {
        return -1;
    }
    return (unsigned char)r->buf[r->pos];
}

static void
advance(AxlXmlReader *r)
{
    if (r->pos >= r->len) {
        return;
    }
    char c = r->buf[r->pos++];
    if (c == '\n') {
        r->line++;
        r->col = 1;
    } else {
        r->col++;
    }
}

/* Compare the buffer slice starting at the current position against
   a NUL-terminated literal. Does not advance. */
static bool
starts_with(const AxlXmlReader *r, const char *literal)
{
    size_t i = 0;
    while (literal[i] != '\0') {
        if (r->pos + i >= r->len || r->buf[r->pos + i] != literal[i]) {
            return false;
        }
        i++;
    }
    return true;
}

/* As starts_with, but advance pos by the matched length. */
static bool
consume(AxlXmlReader *r, const char *literal)
{
    if (!starts_with(r, literal)) {
        return false;
    }
    while (*literal != '\0') {
        advance(r);
        literal++;
    }
    return true;
}

static bool
is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* XML name char set is broad in the spec (any Unicode letter, plus
   `_`, `:`, `-`, `.`, digits except as the first char). We restrict
   to ASCII letters / digits / `_` / `-` / `.` / `:` — covers every
   qname we ever expect in v1's consumer set (WebDAV `D:foo`, Dell
   diag XML, etc.). UTF-8 names would round-trip OK by the
   byte-slice approach but the strictness check rejects them; loosen
   when a consumer needs them. */
static bool
is_name_start(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '_' || c == ':';
}

static bool
is_name_char(int c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') ||
           c == '-' || c == '.';
}

// ---------------------------------------------------------------------------
// Error helper
// ---------------------------------------------------------------------------

static void
set_error(AxlXmlReader *r, const char *msg)
{
    if (r->error) {
        return;  /* keep the first error */
    }
    r->error      = true;
    r->error_line = r->line;
    r->error_col  = r->col;
    r->error_msg  = msg;
}

// ---------------------------------------------------------------------------
// Scratch buffer
// ---------------------------------------------------------------------------

static bool
scratch_grow_to(AxlXmlReader *r, size_t need)
{
    if (need <= r->scratch_cap) {
        return true;
    }
    size_t new_cap = (r->scratch_cap == 0) ? 64 : r->scratch_cap;
    while (new_cap < need) {
        new_cap *= 2;
    }
    char *p = axl_realloc(r->scratch, new_cap);
    if (p == NULL) {
        set_error(r, "out of memory growing scratch buffer");
        return false;
    }
    r->scratch     = p;
    r->scratch_cap = new_cap;
    return true;
}

static bool
scratch_append_byte(AxlXmlReader *r, char c)
{
    if (!scratch_grow_to(r, r->scratch_len + 1)) {
        return false;
    }
    r->scratch[r->scratch_len++] = c;
    return true;
}

/* NUL-terminate the scratch so attribute lookups can return C
   strings. The terminator is NOT counted in any value_len.
   Increases scratch_len by 1; the next append starts after it. */
static bool
scratch_terminate(AxlXmlReader *r)
{
    return scratch_append_byte(r, '\0');
}

static bool
encode_utf8(AxlXmlReader *r, uint32_t cp)
{
    /* XML 1.0 §4.1 forbids char refs to U+0000 (data corruption /
       premature NUL termination of C-string views) and to the UTF-16
       surrogate range U+D800..U+DFFF (invalid as standalone
       codepoints; encoding them as 3-byte UTF-8 produces invalid
       UTF-8). Reject both. */
    if (cp == 0) {
        set_error(r, "character reference to U+0000 is forbidden");
        return false;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        set_error(r, "character reference to UTF-16 surrogate is forbidden");
        return false;
    }
    if (cp < 0x80) {
        return scratch_append_byte(r, (char)cp);
    } else if (cp < 0x800) {
        return scratch_append_byte(r, (char)(0xC0 | (cp >> 6))) &&
               scratch_append_byte(r, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        return scratch_append_byte(r, (char)(0xE0 | (cp >> 12))) &&
               scratch_append_byte(r, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
               scratch_append_byte(r, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        return scratch_append_byte(r, (char)(0xF0 | (cp >> 18))) &&
               scratch_append_byte(r, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
               scratch_append_byte(r, (char)(0x80 | ((cp >> 6)  & 0x3F))) &&
               scratch_append_byte(r, (char)(0x80 | (cp & 0x3F)));
    }
    set_error(r, "numeric character reference out of Unicode range");
    return false;
}

// ---------------------------------------------------------------------------
// Entity decoding
// ---------------------------------------------------------------------------

/* Decode a `&entity;` reference starting AT the `&` and append the
   decoded byte(s) into scratch. Advances pos past the trailing `;`.
   Sticky on error.

   Supported: `&amp;` `&lt;` `&gt;` `&quot;` `&apos;`, `&#N;`,
   `&#xH;`. Reject everything else. */
static bool
decode_entity(AxlXmlReader *r)
{
    /* Caller has already verified peek() == '&'. */
    advance(r);  /* eat '&' */
    if (peek(r) == '#') {
        advance(r);  /* eat '#' */
        bool     hex = false;
        if (peek(r) == 'x' || peek(r) == 'X') {
            hex = true;
            advance(r);
        }
        uint32_t cp     = 0;
        bool     any    = false;
        while (peek(r) != ';' && peek(r) != -1) {
            int c = peek(r);
            int d;
            if (hex) {
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
                else { set_error(r, "malformed hex character reference"); return false; }
            } else {
                if (c >= '0' && c <= '9') d = c - '0';
                else { set_error(r, "malformed decimal character reference"); return false; }
            }
            cp = cp * (hex ? 16 : 10) + (uint32_t)d;
            any = true;
            if (cp > 0x10FFFF) {
                set_error(r, "numeric character reference out of Unicode range");
                return false;
            }
            advance(r);
        }
        if (!any || peek(r) != ';') {
            set_error(r, "unterminated character reference");
            return false;
        }
        advance(r);  /* eat ';' */
        return encode_utf8(r, cp);
    }

    /* Named entity. Match against the 5 we support. */
    struct { const char *name; char ch; } table[] = {
        { "amp;",  '&'  },
        { "lt;",   '<'  },
        { "gt;",   '>'  },
        { "quot;", '"'  },
        { "apos;", '\'' },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (consume(r, table[i].name)) {
            return scratch_append_byte(r, table[i].ch);
        }
    }
    set_error(r, "unknown entity reference (not in the 5 named XML entities)");
    return false;
}

// ---------------------------------------------------------------------------
// Per-construct parsers
// ---------------------------------------------------------------------------

/* Skip whitespace at the current position. */
static void
skip_ws(AxlXmlReader *r)
{
    while (is_space(peek(r))) {
        advance(r);
    }
}

/* Parse a name starting at pos. Returns slice (pointer + length).
   Slice points into r->buf — caller must not free.
   On bad first char or empty, sets error. */
static bool
parse_name(AxlXmlReader *r, const char **out_name, size_t *out_len)
{
    if (!is_name_start(peek(r))) {
        set_error(r, "expected element/attribute name");
        return false;
    }
    size_t start = r->pos;
    while (is_name_char(peek(r))) {
        advance(r);
    }
    *out_name = r->buf + start;
    *out_len  = r->pos - start;
    return true;
}

/* Parse an attribute value enclosed in single or double quotes,
   appending decoded bytes into scratch. The token's value_off /
   value_len reference the resulting scratch slice. */
static bool
parse_attr_value(AxlXmlReader *r, size_t *out_off, size_t *out_len)
{
    int quote = peek(r);
    if (quote != '"' && quote != '\'') {
        set_error(r, "attribute value must be quoted");
        return false;
    }
    advance(r);
    *out_off = r->scratch_len;
    while (peek(r) != quote) {
        int c = peek(r);
        if (c == -1) {
            set_error(r, "unterminated attribute value");
            return false;
        }
        if (c == '<') {
            set_error(r, "literal '<' not allowed inside attribute value");
            return false;
        }
        if (c == '&') {
            if (!decode_entity(r)) {
                return false;
            }
        } else {
            if (!scratch_append_byte(r, (char)c)) {
                return false;
            }
            advance(r);
        }
    }
    *out_len = r->scratch_len - *out_off;
    if (!scratch_terminate(r)) {
        return false;
    }
    advance(r);  /* eat closing quote */
    return true;
}

/* Skip a `<!-- ... -->` comment. Caller has already advanced past
   `<!--`. */
static bool
skip_comment(AxlXmlReader *r)
{
    while (r->pos < r->len) {
        if (starts_with(r, "-->")) {
            consume(r, "-->");
            return true;
        }
        advance(r);
    }
    set_error(r, "unterminated comment");
    return false;
}

/* Skip a `<?...?>` processing instruction or XML declaration. */
static bool
skip_pi(AxlXmlReader *r)
{
    /* Caller has already advanced past '<?'. */
    while (r->pos < r->len) {
        if (starts_with(r, "?>")) {
            consume(r, "?>");
            return true;
        }
        advance(r);
    }
    set_error(r, "unterminated processing instruction");
    return false;
}

/* Skip a `<!DOCTYPE ...>` declaration. Note: we deliberately do NOT
   process any internal entity definitions inside the DOCTYPE — this
   forecloses the billion-laughs class of attacks. The decl is
   matched at the top level by balancing `[` `]` and the outer `>`. */
static bool
skip_doctype(AxlXmlReader *r)
{
    /* Caller has already advanced past `<!DOCTYPE`. */
    int bracket_depth = 0;
    while (r->pos < r->len) {
        int c = peek(r);
        if (c == '[') {
            bracket_depth++;
        } else if (c == ']') {
            bracket_depth--;
        } else if (c == '>' && bracket_depth == 0) {
            advance(r);
            return true;
        }
        advance(r);
    }
    set_error(r, "unterminated DOCTYPE declaration");
    return false;
}

/* Parse the body of a CDATA section starting AFTER the `<![CDATA[`
   prefix has been consumed. Bytes up to `]]>` go into scratch
   verbatim (no entity decoding). */
static bool
parse_cdata(AxlXmlReader *r, size_t *out_off, size_t *out_len)
{
    *out_off = r->scratch_len;
    while (r->pos < r->len) {
        if (starts_with(r, "]]>")) {
            *out_len = r->scratch_len - *out_off;
            if (!scratch_terminate(r)) {
                return false;
            }
            consume(r, "]]>");
            return true;
        }
        if (!scratch_append_byte(r, r->buf[r->pos])) {
            return false;
        }
        advance(r);
    }
    set_error(r, "unterminated CDATA section");
    return false;
}

// ---------------------------------------------------------------------------
// Namespace binding stack
// ---------------------------------------------------------------------------

/* Append an xmlns binding for the element currently being parsed.
   @p depth is the element-depth at which the binding is declared
   (so pop_ns_bindings_at can drop it when that element closes).
   @p prefix may be NULL to record a default-namespace binding
   (`xmlns="URI"`); otherwise it points into r->buf with the bytes
   AFTER `xmlns:`. URI is copied out of the attribute scratch so it
   survives the next() call's scratch reset. */
static bool
push_ns_binding(AxlXmlReader *r, uint32_t depth,
                const char *prefix, size_t prefix_len,
                const char *uri_decoded, size_t uri_len)
{
    /* Invariant at entry: ns_count == ns_alive_count. reset_token_state
       (run at the top of every non-synthetic next() call) drains the
       alive count down to active before parse_start_element runs; the
       synthetic-END branch never pushes. So we never overwrite a
       still-alive uri pointer below ns_alive_count.

       Special case: uri_decoded == NULL records an "undeclare"
       binding (`xmlns=""` per Namespaces 1.0 errata). It shadows the
       outer default-ns binding without supplying a replacement, so
       resolution returns NULL for elements in this scope. */
    if (r->ns_count >= r->ns_cap) {
        size_t new_cap = (r->ns_cap == 0) ? 4 : r->ns_cap * 2;
        NsBinding *p = axl_realloc(r->ns_bindings, new_cap * sizeof(*p));
        if (p == NULL) {
            set_error(r, "out of memory growing namespace bindings");
            return false;
        }
        r->ns_bindings = p;
        r->ns_cap      = new_cap;
    }
    char *uri_copy = NULL;
    if (uri_decoded != NULL) {
        uri_copy = axl_strdup(uri_decoded);
        if (uri_copy == NULL) {
            set_error(r, "out of memory copying namespace URI");
            return false;
        }
    }
    NsBinding *b = &r->ns_bindings[r->ns_count++];
    b->depth      = depth;
    b->prefix     = prefix;
    b->prefix_len = prefix_len;
    b->uri        = uri_copy;
    b->uri_len    = uri_len;
    if (r->ns_count > r->ns_alive_count) {
        r->ns_alive_count = r->ns_count;
    }
    return true;
}

/* Pop every binding whose depth equals @p depth — logically. The
   URI strings stay heap-owned (between ns_count and ns_alive_count)
   so the caller can still safely read the END token's ns_uri this
   call returned via resolve_ns_for_name. They get freed at the
   start of the next next() call via reset_token_state. */
static void
pop_ns_bindings_at(AxlXmlReader *r, uint32_t depth)
{
    while (r->ns_count > 0 &&
           r->ns_bindings[r->ns_count - 1].depth == depth)
    {
        r->ns_count--;
    }
}

/* Resolve the namespace URI for an element name. Looks for a `:` in
   @p name to extract the prefix (NULL for unprefixed), then walks
   the binding stack top-down — most-recently-declared wins, so
   shadowing works naturally. */
static void
resolve_ns_for_name(AxlXmlReader *r, const char *name, size_t name_len,
                    const char **out_uri, size_t *out_uri_len)
{
    const char *prefix     = NULL;
    size_t      prefix_len = 0;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == ':') {
            prefix     = name;
            prefix_len = i;
            break;
        }
    }
    for (size_t k = r->ns_count; k > 0; k--) {
        const NsBinding *b = &r->ns_bindings[k - 1];
        if (prefix == NULL) {
            if (b->prefix == NULL) {
                /* First-match wins. NULL uri here is an undeclare
                   marker (xmlns="") — resolve to NULL so the element
                   appears unbound, shadowing the outer default. */
                *out_uri     = b->uri;
                *out_uri_len = b->uri ? b->uri_len : 0;
                return;
            }
        } else {
            if (b->prefix != NULL &&
                b->prefix_len == prefix_len &&
                axl_strncmp(b->prefix, prefix, prefix_len) == 0)
            {
                *out_uri     = b->uri;
                *out_uri_len = b->uri ? b->uri_len : 0;
                return;
            }
        }
    }
    *out_uri     = NULL;
    *out_uri_len = 0;
}

// ---------------------------------------------------------------------------
// Top-level: per-token state machine
// ---------------------------------------------------------------------------

/* Reset per-token reader state — scratch buffer, attribute table,
   and any ns bindings popped during the previous token's emit.
   Called at the start of each successful next() so the previous
   token's storage is invalidated. */
static void
reset_token_state(AxlXmlReader *r)
{
    /* Free URIs of bindings that were logically popped during the
       previous token's emit. Their pointers may have been handed to
       the caller in out->ns_uri; their lifetime ends here. */
    while (r->ns_alive_count > r->ns_count) {
        r->ns_alive_count--;
        axl_free(r->ns_bindings[r->ns_alive_count].uri);
        r->ns_bindings[r->ns_alive_count].uri = NULL;
    }
    r->scratch_len = 0;
    r->attr_count  = 0;
    r->attrs_valid = false;
}

/* Grow the attribute table by one slot. */
static bool
ensure_attrs(AxlXmlReader *r)
{
    if (r->attr_count < r->attr_cap) {
        return true;
    }
    size_t new_cap = (r->attr_cap == 0) ? 4 : r->attr_cap * 2;
    AttrSlot *p = axl_realloc(r->attrs, new_cap * sizeof(*p));
    if (p == NULL) {
        set_error(r, "out of memory growing attribute table");
        return false;
    }
    r->attrs    = p;
    r->attr_cap = new_cap;
    return true;
}

/* Parse `<name [attr="value"]* (/>|>)`. Caller has already advanced
   past the leading `<`. Fills *out_token with START_ELEMENT info
   and parses attributes into r->attrs. If the tag self-closes,
   sets r->emit_end_next so the next next() returns END_ELEMENT. */
static bool
parse_start_element(AxlXmlReader *r, AxlXmlToken *out)
{
    const char *name;
    size_t      name_len;
    if (!parse_name(r, &name, &name_len)) {
        return false;
    }
    if (r->depth >= MAX_DEPTH) {
        set_error(r, "element nesting exceeds MAX_DEPTH");
        return false;
    }

    /* Parse attribute list. */
    for (;;) {
        skip_ws(r);
        int c = peek(r);
        if (c == '>' || c == '/' || c == -1) {
            break;
        }
        if (!ensure_attrs(r)) {
            return false;
        }
        const char *aname;
        size_t      anlen;
        if (!parse_name(r, &aname, &anlen)) {
            return false;
        }
        skip_ws(r);
        if (peek(r) != '=') {
            set_error(r, "expected '=' after attribute name");
            return false;
        }
        advance(r);
        skip_ws(r);
        size_t off, len;
        if (!parse_attr_value(r, &off, &len)) {
            return false;
        }
        AttrSlot *slot = &r->attrs[r->attr_count++];
        slot->name      = aname;
        slot->name_len  = anlen;
        slot->value_off = off;
        slot->value_len = len;
    }

    bool self_closing = false;
    if (peek(r) == '/') {
        advance(r);
        if (peek(r) != '>') {
            set_error(r, "expected '>' after '/' in self-closing tag");
            return false;
        }
        self_closing = true;
    } else if (peek(r) != '>') {
        set_error(r, "expected '>' to close start tag");
        return false;
    }
    advance(r);  /* eat '>' */

    uint32_t depth_here = r->depth;

    /* Sweep attributes for xmlns declarations — `xmlns="URI"`
       (default-ns) and `xmlns:prefix="URI"` (prefix binding). Push
       each binding at this element's depth so they pop together
       when the element closes. Done BEFORE resolve so the element's
       own bindings affect its own resolution (a root that declares
       `xmlns="X"` is itself in X). */
    for (size_t i = 0; i < r->attr_count; i++) {
        const AttrSlot *s = &r->attrs[i];
        const char *uri = r->scratch + s->value_off;
        if (s->name_len == 5 &&
            axl_strncmp(s->name, "xmlns", 5) == 0)
        {
            /* `xmlns=""` undeclares the default namespace per
               Namespaces 1.0 errata. Push an "undeclare" marker —
               a binding with NULL uri that shadows any outer
               default-ns binding so unprefixed children in this
               scope resolve to NULL. Without the marker, the parent's
               default would leak through. */
            const char *push_uri  = s->value_len == 0 ? NULL : uri;
            size_t      push_ulen = s->value_len;
            if (!push_ns_binding(r, depth_here, NULL, 0,
                                 push_uri, push_ulen))
            {
                return false;
            }
        } else if (s->name_len > 6 &&
                   axl_strncmp(s->name, "xmlns:", 6) == 0)
        {
            if (!push_ns_binding(r, depth_here,
                                 s->name + 6, s->name_len - 6,
                                 uri, s->value_len))
            {
                return false;
            }
        }
    }

    /* Push onto the balance stack regardless — self-closing
       elements get popped immediately on the synthetic END. */
    r->stack_name[r->depth]     = name;
    r->stack_name_len[r->depth] = name_len;
    r->depth++;

    out->type      = AXL_XML_TOKEN_START_ELEMENT;
    out->name      = name;
    out->name_len  = name_len;
    out->text      = NULL;
    out->text_len  = 0;
    out->is_cdata  = false;
    resolve_ns_for_name(r, name, name_len,
                        &out->ns_uri, &out->ns_uri_len);

    r->attrs_valid    = true;
    r->root_emitted   = true;

    if (self_closing) {
        r->emit_end_next        = true;
        r->pending_end_name     = name;
        r->pending_end_name_len = name_len;
    }
    return true;
}

/* Parse `</name>` and emit END_ELEMENT. Caller has already advanced
   past `</`. Validates against the top of the stack. */
static bool
parse_end_element(AxlXmlReader *r, AxlXmlToken *out)
{
    const char *name;
    size_t      name_len;
    if (!parse_name(r, &name, &name_len)) {
        return false;
    }
    skip_ws(r);
    if (peek(r) != '>') {
        set_error(r, "expected '>' to close end tag");
        return false;
    }
    advance(r);

    if (r->depth == 0) {
        set_error(r, "end tag with no matching start");
        return false;
    }
    uint32_t top = r->depth - 1;
    if (r->stack_name_len[top] != name_len ||
        axl_strncmp(r->stack_name[top], name, name_len) != 0)
    {
        set_error(r, "mismatched end tag");
        return false;
    }

    out->type      = AXL_XML_TOKEN_END_ELEMENT;
    out->name      = name;
    out->name_len  = name_len;
    out->text      = NULL;
    out->text_len  = 0;
    out->is_cdata  = false;
    /* Resolve the END's namespace BEFORE popping — the bindings the
       START introduced (at depth `top`) are still in scope for the
       element closing right now. Pop afterwards so child/sibling
       parsing sees the parent's scope. */
    resolve_ns_for_name(r, name, name_len,
                        &out->ns_uri, &out->ns_uri_len);
    pop_ns_bindings_at(r, top);
    r->depth--;
    if (r->depth == 0) {
        r->root_closed = true;
    }
    return true;
}

/* Parse text content starting at a non-`<` byte. Stops at `<` or
   EOF. Decodes entities into scratch. */
static bool
parse_text(AxlXmlReader *r, AxlXmlToken *out)
{
    size_t off = r->scratch_len;
    while (r->pos < r->len && r->buf[r->pos] != '<') {
        int c = peek(r);
        if (c == '&') {
            if (!decode_entity(r)) {
                return false;
            }
        } else {
            if (!scratch_append_byte(r, (char)c)) {
                return false;
            }
            advance(r);
        }
    }
    size_t len = r->scratch_len - off;
    if (!scratch_terminate(r)) {
        return false;
    }
    out->type       = AXL_XML_TOKEN_TEXT;
    out->name       = NULL;
    out->name_len   = 0;
    out->ns_uri     = NULL;
    out->ns_uri_len = 0;
    out->text       = r->scratch + off;
    out->text_len   = len;
    out->is_cdata   = false;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlXmlReader *
axl_xml_reader_new(const char *buf, size_t len)
{
    if (buf == NULL && len > 0) {
        return NULL;
    }
    AxlXmlReader *r = axl_calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->buf  = buf;
    r->len  = len;
    r->line = 1;
    r->col  = 1;
    return r;
}

void
axl_xml_reader_free(AxlXmlReader *r)
{
    if (r == NULL) {
        return;
    }
    /* Free every URI still owned — both active and dead-but-pending. */
    for (size_t i = 0; i < r->ns_alive_count; i++) {
        axl_free(r->ns_bindings[i].uri);
    }
    axl_free(r->ns_bindings);
    axl_free(r->scratch);
    axl_free(r->attrs);
    axl_free(r);
}

// ---------------------------------------------------------------------------
// Token helpers — local-name extraction + compare for namespace-aware
// consumers that want to match the post-colon portion of a qname.
// Pure functions over a borrowed AxlXmlToken; no reader state touched.
// ---------------------------------------------------------------------------

const char *
axl_xml_token_local_name(const AxlXmlToken *tok, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (tok == NULL || tok->name == NULL ||
        (tok->type != AXL_XML_TOKEN_START_ELEMENT &&
         tok->type != AXL_XML_TOKEN_END_ELEMENT))
    {
        return NULL;
    }
    /* Scan forward for the FIRST `:` — the qname format is
       <prefix>:<local-name>; XML names cannot contain more than one
       colon, so first-match is sufficient and matches the producer
       side's expectations. */
    for (size_t i = 0; i < tok->name_len; i++) {
        if (tok->name[i] == ':') {
            if (out_len != NULL) {
                *out_len = tok->name_len - i - 1;
            }
            return tok->name + i + 1;
        }
    }
    /* Unprefixed — local name == full name. */
    if (out_len != NULL) {
        *out_len = tok->name_len;
    }
    return tok->name;
}

bool
axl_xml_token_local_name_eq(const AxlXmlToken *tok, const char *want)
{
    if (tok == NULL || want == NULL) {
        return false;
    }
    size_t      local_len = 0;
    const char *local     = axl_xml_token_local_name(tok, &local_len);
    if (local == NULL) {
        return false;
    }
    size_t want_len = 0;
    while (want[want_len] != '\0') {
        want_len++;
    }
    if (local_len != want_len) {
        return false;
    }
    return axl_strncmp(local, want, want_len) == 0;
}

bool
axl_xml_reader_error(const AxlXmlReader *r, uint32_t *line, uint32_t *col,
                     const char **msg)
{
    if (r == NULL || !r->error) {
        if (line != NULL) *line = 0;
        if (col  != NULL) *col  = 0;
        if (msg  != NULL) *msg  = NULL;
        return false;
    }
    if (line != NULL) *line = r->error_line;
    if (col  != NULL) *col  = r->error_col;
    if (msg  != NULL) *msg  = r->error_msg;
    return true;
}

const char *
axl_xml_reader_attr(AxlXmlReader *r, const char *name)
{
    if (r == NULL || name == NULL || !r->attrs_valid) {
        return NULL;
    }
    size_t key_len = 0;
    while (name[key_len] != '\0') {
        key_len++;
    }
    for (size_t i = 0; i < r->attr_count; i++) {
        const AttrSlot *s = &r->attrs[i];
        if (s->name_len == key_len &&
            axl_strncmp(s->name, name, key_len) == 0)
        {
            return r->scratch + s->value_off;
        }
    }
    return NULL;
}

bool
axl_xml_reader_next(AxlXmlReader *r, AxlXmlToken *out)
{
    if (r == NULL || out == NULL) {
        return false;
    }
    if (r->error || r->eof_delivered) {
        return false;
    }

    /* Synthetic END pending from a self-closing start tag. */
    if (r->emit_end_next) {
        out->type      = AXL_XML_TOKEN_END_ELEMENT;
        out->name      = r->pending_end_name;
        out->name_len  = r->pending_end_name_len;
        out->text      = NULL;
        out->text_len  = 0;
        out->is_cdata  = false;
        /* Resolve ns BEFORE popping — same shape as parse_end_element. */
        resolve_ns_for_name(r, r->pending_end_name,
                            r->pending_end_name_len,
                            &out->ns_uri, &out->ns_uri_len);
        r->emit_end_next = false;
        if (r->depth > 0) {
            pop_ns_bindings_at(r, r->depth - 1);
            r->depth--;
            if (r->depth == 0) {
                r->root_closed = true;
            }
        }
        r->attrs_valid = false;
        return true;
    }

    reset_token_state(r);

    /* Main per-token loop: skip over comments / PIs / DOCTYPE
       declarations until we hit a real token (or EOF). */
    for (;;) {
        if (r->pos >= r->len) {
            /* EOF. */
            if (r->depth > 0) {
                set_error(r, "unexpected EOF - unclosed element(s)");
                return false;
            }
            if (!r->root_emitted) {
                set_error(r, "no root element");
                return false;
            }
            out->type       = AXL_XML_TOKEN_END_DOCUMENT;
            out->name       = NULL;
            out->name_len   = 0;
            out->ns_uri     = NULL;
            out->ns_uri_len = 0;
            out->text       = NULL;
            out->text_len   = 0;
            out->is_cdata   = false;
            r->eof_delivered = true;
            return true;
        }

        int c = peek(r);

        if (c == '<') {
            /* Discriminate the `<` flavors. */
            if (starts_with(r, "<!--")) {
                consume(r, "<!--");
                if (!skip_comment(r)) {
                    return false;
                }
                continue;
            }
            if (starts_with(r, "<![CDATA[")) {
                consume(r, "<![CDATA[");
                if (r->depth == 0) {
                    set_error(r, "CDATA outside any element");
                    return false;
                }
                size_t off, len;
                if (!parse_cdata(r, &off, &len)) {
                    return false;
                }
                out->type       = AXL_XML_TOKEN_TEXT;
                out->name       = NULL;
                out->name_len   = 0;
                out->ns_uri     = NULL;
                out->ns_uri_len = 0;
                out->text       = r->scratch + off;
                out->text_len   = len;
                out->is_cdata   = true;
                return true;
            }
            if (starts_with(r, "<!DOCTYPE")) {
                consume(r, "<!DOCTYPE");
                if (r->root_emitted) {
                    set_error(r, "DOCTYPE after root element");
                    return false;
                }
                if (!skip_doctype(r)) {
                    return false;
                }
                continue;
            }
            if (starts_with(r, "<?")) {
                consume(r, "<?");
                if (!skip_pi(r)) {
                    return false;
                }
                continue;
            }
            if (starts_with(r, "</")) {
                consume(r, "</");
                return parse_end_element(r, out);
            }
            /* Plain `<` → start element. */
            if (r->root_closed) {
                set_error(r, "content after root element");
                return false;
            }
            advance(r);  /* eat '<' */
            return parse_start_element(r, out);
        }

        /* Non-`<` byte: text. After the root has closed, leading
           whitespace is permitted (trailing newlines) but anything
           non-whitespace is an error per XML well-formedness. */
        if (r->root_closed) {
            if (is_space(c)) {
                advance(r);
                continue;
            }
            set_error(r, "non-whitespace content after root element");
            return false;
        }
        if (r->depth == 0) {
            /* Whitespace before the root is permitted, anything else
               is an error. */
            if (is_space(c)) {
                advance(r);
                continue;
            }
            set_error(r, "non-whitespace content before root element");
            return false;
        }

        return parse_text(r, out);
    }
}
