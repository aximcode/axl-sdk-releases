/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-xml-writer.c
    AxlXmlWriter — streaming XML writer over an AxlString backing
    store. Tracks open-element depth so closes can't outrun starts;
    auto-escapes text and attribute values; supports optional 2-space
    pretty-print.

    State model:
      - in_start_tag: between `<foo` and the next text/child/close
        (attribute calls valid only in this window).
      - had_text_bits / had_child_bits: per-depth flags telling the
        close path whether to emit `/>` (no content) or `</foo>` (any
        content) and whether to break a line before the close tag
        (pretty mode, only when the element nested child elements).
      - The depth=0 frame slot tracks the "document" element which
        the caller is currently inside; depth==0 at finish means all
        opened elements were closed.
**/

#include <axl/axl-xml.h>
#include <axl/axl-string.h>
#include <axl/axl-mem.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/* Mark the writer as failed and short-circuit subsequent calls. */
static inline void
fail(AxlXmlWriter *w)
{
    w->error = true;
}

/* Append a NUL-terminated string. Sticky on failure. */
static void
append(AxlXmlWriter *w, const char *s)
{
    if (w->error) {
        return;
    }
    if (axl_string_append(w->out, s) != AXL_OK) {
        fail(w);
    }
}

/* Append a single character. Sticky on failure. */
static void
append_c(AxlXmlWriter *w, char c)
{
    if (w->error) {
        return;
    }
    if (axl_string_append_c(w->out, c) != AXL_OK) {
        fail(w);
    }
}

/* Append length-counted bytes. Sticky on failure. */
static void
append_n(AxlXmlWriter *w, const char *s, size_t n)
{
    if (w->error || s == NULL || n == 0) {
        return;
    }
    if (axl_string_append_len(w->out, s, n) != AXL_OK) {
        fail(w);
    }
}

/* Append text content with the three-char body escape: `<` `>` `&`.
   The other XML quote chars (`"` and `'`) are body-legal. */
static void
append_escaped_text(AxlXmlWriter *w, const char *s, size_t n)
{
    if (w->error) {
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        switch (s[i]) {
        case '<': rep = "&lt;";  break;
        case '>': rep = "&gt;";  break;
        case '&': rep = "&amp;"; break;
        default: break;
        }
        if (rep != NULL) {
            if (i > start) {
                append_n(w, s + start, i - start);
            }
            append(w, rep);
            start = i + 1;
        }
    }
    if (n > start) {
        append_n(w, s + start, n - start);
    }
}

/* Append an attribute value with the attr-context escape: `<` `&`
   plus `"` (since attributes are double-quoted). `>` is body-legal in
   attribute values per the XML spec, but most XML libraries escape
   it anyway as defense-in-depth; we follow the minimal-set choice
   to match stout::Fragments. */
static void
append_escaped_attr(AxlXmlWriter *w, const char *s)
{
    if (w->error || s == NULL) {
        return;
    }
    const char *p = s;
    while (*p != '\0') {
        const char *rep = NULL;
        switch (*p) {
        case '<': rep = "&lt;";   break;
        case '&': rep = "&amp;";  break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep != NULL) {
            append(w, rep);
        } else {
            append_c(w, *p);
        }
        p++;
    }
}

/* True when the caller asked for indentation at all.
   The PRESENCE bit, not the width: AXL_XML_INDENT(0) is a real request for
   newlines with a zero-width indent, and testing the width alone would read it
   as "compact" and drop the newlines. That distinction is the whole reason
   AXL_XML_HAS_INDENT exists as a separate bit. */
static bool
is_pretty(const AxlXmlWriter *w)
{
    return (w->flags & AXL_XML_HAS_INDENT) != 0;
}

/* Emit pretty-mode line break + indent at @p depth. No-op when not pretty. */
static void
pretty_break(AxlXmlWriter *w, uint32_t depth)
{
    if (!is_pretty(w) || w->error) {
        return;
    }
    append_c(w, '\n');
    const uint32_t width = AXL_XML_INDENT_OF(w->flags);
    for (uint32_t i = 0; i < depth; i++) {
        for (uint32_t j = 0; j < width; j++) {
            append_c(w, ' ');
        }
    }
}

/* If a start tag is open (`<foo` with attributes pending), commit it
   by appending `>`. Caller-side guard for the text/child/close paths
   that need the start-tag closed before they can emit. */
static void
close_open_start_tag_with_gt(AxlXmlWriter *w)
{
    if (w->in_start_tag) {
        append_c(w, '>');
        w->in_start_tag = false;
    }
}

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

void
axl_xml_writer_init(AxlXmlWriter *w, AxlString *out, AxlXmlFlags flags)
{
    if (w == NULL) {
        return;
    }
    w->out                  = out;
    w->flags                = flags;
    w->depth                = 0;
    w->in_start_tag         = false;
    w->prologue_emitted     = false;
    w->doctype_emitted      = false;
    w->any_element_emitted  = false;
    /* A bit outside the XML vocabulary is REFUSED rather than ignored.
       Ignoring it is exactly what let AXL_XML_WRITER_PRETTY and
       AXL_JSON_ALLOW_COMMENTS be the same bit for as long as they were: a flag
       from the wrong module arrived, meant something, and nobody found out.
       Latched into the sticky error, so the first write is a no-op and
       axl_xml_writer_error() reports it -- the same way every other misuse in
       this writer surfaces. */
    w->error                = (out == NULL)
                              || (flags & ~(AxlXmlFlags)AXL_XML_KNOWN_MASK) != 0;
    w->had_text_bits        = 0;
    w->had_child_bits       = 0;
    for (uint32_t i = 0; i < AXL_XML_WRITER_MAX_DEPTH; i++) {
        w->stack[i] = NULL;
    }
}

size_t
axl_xml_writer_finish(AxlXmlWriter *w)
{
    if (w == NULL) {
        return 0;
    }
    if (w->depth != 0) {
        /* Some elements were left open at finish. */
        fail(w);
    }
    /* In case fail-path left stack entries strdup'd (e.g. structural
       error caught mid-document), free them. Empty in the green path. */
    for (uint32_t i = 0; i < AXL_XML_WRITER_MAX_DEPTH; i++) {
        axl_free(w->stack[i]);
        w->stack[i] = NULL;
    }
    return (w->out != NULL) ? axl_string_len(w->out) : 0;
}

bool
axl_xml_writer_error(const AxlXmlWriter *w)
{
    return w == NULL ? true : w->error;
}

// ---------------------------------------------------------------------------
// Public API: prologue + doctype
// ---------------------------------------------------------------------------

void
axl_xml_writer_prologue(AxlXmlWriter *w)
{
    if (w == NULL || w->error) {
        return;
    }
    if (w->prologue_emitted || w->any_element_emitted) {
        fail(w);
        return;
    }
    append(w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    w->prologue_emitted = true;
}

void
axl_xml_writer_doctype(AxlXmlWriter *w, const char *root,
                       const char *dtd_uri)
{
    if (w == NULL || w->error) {
        return;
    }
    if (w->doctype_emitted || w->any_element_emitted || root == NULL) {
        fail(w);
        return;
    }
    append(w, "<!DOCTYPE ");
    append(w, root);
    if (dtd_uri != NULL) {
        append(w, " SYSTEM \"");
        /* Caller is responsible for URI legality; we don't escape
           the URI body — only `"` would terminate the literal early
           and that's a wire-format bug, not a user-data hazard. */
        append(w, dtd_uri);
        append_c(w, '"');
    }
    append_c(w, '>');
    w->doctype_emitted = true;
}

// ---------------------------------------------------------------------------
// Public API: element emit
// ---------------------------------------------------------------------------

void
axl_xml_writer_start_element(AxlXmlWriter *w, const char *qname)
{
    if (w == NULL || w->error || qname == NULL) {
        if (w != NULL) {
            fail(w);
        }
        return;
    }
    if (w->depth >= AXL_XML_WRITER_MAX_DEPTH) {
        fail(w);
        return;
    }

    /* If an outer start tag is open, commit it as `<...>` and flag
       the parent as having a child (drives pretty-print newline +
       /> vs </name> decision). */
    if (w->in_start_tag) {
        close_open_start_tag_with_gt(w);
        if (w->depth > 0) {
            w->had_child_bits |= (uint64_t)1 << (w->depth - 1);
        }
    } else if (w->depth > 0) {
        /* Already have closed parent start tag — this is a sibling
           or grandchild; still flag parent as having children. */
        w->had_child_bits |= (uint64_t)1 << (w->depth - 1);
    }

    /* Pretty break BEFORE the opening `<` for nested elements. The
       root element gets no leading break (it sits at column 0). */
    if (w->any_element_emitted) {
        pretty_break(w, w->depth);
    }

    /* Save name in stack so close_element can format the close tag
       (and so we can free on error-path finish). */
    char *name_copy = axl_strdup(qname);
    if (name_copy == NULL) {
        fail(w);
        return;
    }
    w->stack[w->depth] = name_copy;

    append_c(w, '<');
    append(w, qname);

    w->in_start_tag         = true;
    w->any_element_emitted  = true;
    w->had_text_bits       &= ~((uint64_t)1 << w->depth);
    w->had_child_bits      &= ~((uint64_t)1 << w->depth);
    w->depth++;
}

void
axl_xml_writer_attribute(AxlXmlWriter *w, const char *name,
                         const char *value)
{
    if (w == NULL || w->error) {
        return;
    }
    if (!w->in_start_tag || name == NULL || value == NULL ||
        name[0] == '\0')
    {
        fail(w);
        return;
    }
    append_c(w, ' ');
    append(w, name);
    append(w, "=\"");
    append_escaped_attr(w, value);
    append_c(w, '"');
}

void
axl_xml_writer_textn(AxlXmlWriter *w, const char *text, size_t n)
{
    if (w == NULL || w->error || text == NULL || n == 0) {
        return;
    }
    if (w->depth == 0) {
        /* Text outside any element is not legal XML. */
        fail(w);
        return;
    }
    close_open_start_tag_with_gt(w);
    append_escaped_text(w, text, n);
    w->had_text_bits |= (uint64_t)1 << (w->depth - 1);
}

void
axl_xml_writer_text(AxlXmlWriter *w, const char *text)
{
    if (w == NULL || w->error || text == NULL) {
        return;
    }
    /* Match textn: empty input is a true no-op — the element still
       self-closes if no other content arrives. Committing the
       start tag first would force `<x></x>` for `text("")`, which
       contradicts the documented "no-op" contract. */
    size_t len = 0;
    while (text[len] != '\0') {
        len++;
    }
    if (len == 0) {
        return;
    }
    if (w->depth == 0) {
        fail(w);
        return;
    }
    close_open_start_tag_with_gt(w);
    append_escaped_text(w, text, len);
    w->had_text_bits |= (uint64_t)1 << (w->depth - 1);
}

void
axl_xml_writer_end_element(AxlXmlWriter *w)
{
    if (w == NULL || w->error) {
        return;
    }
    if (w->depth == 0) {
        /* Close-without-start. */
        fail(w);
        return;
    }
    uint32_t top       = w->depth - 1;
    uint64_t topbit    = (uint64_t)1 << top;
    bool     had_text  = (w->had_text_bits  & topbit) != 0;
    bool     had_child = (w->had_child_bits & topbit) != 0;

    if (w->in_start_tag && !had_text && !had_child) {
        /* No body content — self-close. */
        append(w, "/>");
        w->in_start_tag = false;
    } else {
        close_open_start_tag_with_gt(w);
        /* Pretty-print: line break + indent before close tag iff
           the element nested children. Pure-text elements stay on
           one line. */
        if (had_child) {
            pretty_break(w, top);
        }
        append(w, "</");
        append(w, w->stack[top]);
        append_c(w, '>');
    }

    axl_free(w->stack[top]);
    w->stack[top] = NULL;
    w->depth = top;

    /* Trailing newline after the root element closes (pretty mode). */
    if (w->depth == 0 && is_pretty(w)) {
        append_c(w, '\n');
    }
}
