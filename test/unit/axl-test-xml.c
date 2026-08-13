/** @file axl-test-xml.c
    Test application for AxlXml — writer (X1) and reader (X2).
**/

#include "axl-test.h"

#include <axl/axl-xml.h>
#include <axl/axl-json.h>   /* the alignment is asserted against it */

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* Compare AxlString contents to a NUL-terminated expected literal.
   Returns true on byte-exact match. */
static bool
str_equals(const AxlString *s, const char *expected)
{
    const char *got = axl_string_str(s);
    if (got == NULL || expected == NULL) {
        return false;
    }
    return axl_strcmp(got, expected) == 0;
}

// ---------------------------------------------------------------------------
// Writer Tests
// ---------------------------------------------------------------------------

static void
test_writer_empty(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "empty: no error");
    test_check(axl_string_len(s) == 0, "empty: zero output");
    axl_string_free(s);
}

static void
test_writer_self_closing(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "foo");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "self-closing: no error");
    test_check(str_equals(s, "<foo/>"), "self-closing: <foo/>");
    axl_string_free(s);
}

static void
test_writer_text(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "foo");
    axl_xml_writer_text(&w, "hello");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "text: no error");
    test_check(str_equals(s, "<foo>hello</foo>"), "text: <foo>hello</foo>");
    axl_string_free(s);
}

static void
test_writer_attribute(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "foo");
    axl_xml_writer_attribute(&w, "bar", "baz");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "attr: no error");
    test_check(str_equals(s, "<foo bar=\"baz\"/>"), "attr: <foo bar=\"baz\"/>");
    axl_string_free(s);
}

static void
test_writer_multi_attributes(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "foo");
    axl_xml_writer_attribute(&w, "a", "1");
    axl_xml_writer_attribute(&w, "b", "2");
    axl_xml_writer_attribute(&w, "c", "3");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "multi-attr: no error");
    test_check(str_equals(s, "<foo a=\"1\" b=\"2\" c=\"3\"/>"),
               "multi-attr: order preserved");
    axl_string_free(s);
}

static void
test_writer_nested(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_start_element(&w, "b");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "nested: no error");
    test_check(str_equals(s, "<a><b/></a>"), "nested: <a><b/></a>");
    axl_string_free(s);
}

static void
test_writer_pretty(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_INDENT(2));
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_start_element(&w, "b");
    axl_xml_writer_text(&w, "x");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    /* Pretty: top-level <a> on its own line, <b> indented 2 sp;
       <b>x</b> kept on one line because text is leaf content. */
    test_check(!axl_xml_writer_error(&w), "pretty: no error");
    test_check(str_equals(s, "<a>\n  <b>x</b>\n</a>\n"),
               "pretty: indented + newlines");
    axl_string_free(s);
}

static void
test_writer_text_escape(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "x");
    axl_xml_writer_text(&w, "a<b>c&d");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "text-escape: no error");
    test_check(str_equals(s, "<x>a&lt;b&gt;c&amp;d</x>"),
               "text-escape: <>& escaped");
    axl_string_free(s);
}

static void
test_writer_text_quotes_pass_through(void)
{
    /* XML spec only mandates escaping `<` and `&` in body text;
       `>` is conventional, but `"` and `'` are body-legal and we
       intentionally pass them through. Lock that in — used by
       WebDAV PROPFIND's displayname emit on filenames containing
       quotes. */
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "x");
    axl_xml_writer_text(&w, "a\"b'c");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "text-quotes: no error");
    test_check(str_equals(s, "<x>a\"b'c</x>"),
               "text-quotes: \" and ' pass through unescaped");
    axl_string_free(s);
}

static void
test_writer_attr_escape(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "x");
    axl_xml_writer_attribute(&w, "k", "a&b<c\"d");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "attr-escape: no error");
    test_check(str_equals(s, "<x k=\"a&amp;b&lt;c&quot;d\"/>"),
               "attr-escape: &<\" escaped");
    axl_string_free(s);
}

static void
test_writer_prologue(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_prologue(&w);
    axl_xml_writer_start_element(&w, "root");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "prologue: no error");
    test_check(str_equals(s,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><root/>"),
        "prologue: declaration emitted");
    axl_string_free(s);
}

static void
test_writer_doctype(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_doctype(&w, "DellDiag", "DellDiag.dtd");
    axl_xml_writer_start_element(&w, "DellDiag");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "doctype: no error");
    test_check(str_equals(s,
        "<!DOCTYPE DellDiag SYSTEM \"DellDiag.dtd\"><DellDiag/>"),
        "doctype: with DTD URI");
    axl_string_free(s);
}

static void
test_writer_doctype_bare(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_doctype(&w, "root", NULL);
    axl_xml_writer_start_element(&w, "root");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "doctype-bare: no error");
    test_check(str_equals(s, "<!DOCTYPE root><root/>"),
               "doctype-bare: no DTD URI");
    axl_string_free(s);
}

static void
test_writer_textn(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "x");
    /* Pass a bigger buffer; only the first 5 chars matter. */
    axl_xml_writer_textn(&w, "abcdefghij", 5);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "textn: no error");
    test_check(str_equals(s, "<x>abcde</x>"),
               "textn: honors explicit length");
    axl_string_free(s);
}

static void
test_writer_mixed_content(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "p");
    axl_xml_writer_text(&w, "hello ");
    axl_xml_writer_start_element(&w, "b");
    axl_xml_writer_text(&w, "world");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_text(&w, "!");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "mixed: no error");
    test_check(str_equals(s, "<p>hello <b>world</b>!</p>"),
               "mixed: text + child + text");
    axl_string_free(s);
}

static void
test_writer_close_without_start(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(axl_xml_writer_error(&w),
               "close-without-start: sets sticky error");
    axl_string_free(s);
}

static void
test_writer_attribute_outside_start_tag(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_text(&w, "x");          /* closes start tag */
    axl_xml_writer_attribute(&w, "k", "v"); /* INVALID — too late */
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(axl_xml_writer_error(&w),
               "attr-after-content: sets sticky error");
    axl_string_free(s);
}

static void
test_writer_prologue_late(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_prologue(&w);  /* INVALID — must precede elements */
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(axl_xml_writer_error(&w),
               "prologue-after-element: sets sticky error");
    axl_string_free(s);
}

static void
test_writer_doctype_late(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_doctype(&w, "x", NULL);  /* INVALID — must precede elements */
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(axl_xml_writer_error(&w),
               "doctype-after-element: sets sticky error");
    axl_string_free(s);
}

static void
test_writer_unclosed_at_finish(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_finish(&w);  /* never closed <a> */
    test_check(axl_xml_writer_error(&w),
               "unclosed-at-finish: sets sticky error");
    axl_string_free(s);
}

static void
test_writer_pretty_deep(void)
{
    /* 3-level pretty nesting locks the indent escalation. */
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_INDENT(2));
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_start_element(&w, "b");
    axl_xml_writer_start_element(&w, "c");
    axl_xml_writer_text(&w, "x");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "pretty-deep: no error");
    test_check(str_equals(s,
        "<a>\n  <b>\n    <c>x</c>\n  </b>\n</a>\n"),
        "pretty-deep: 3 levels, 2-space step");
    axl_string_free(s);
}

static void
test_writer_text_empty(void)
{
    /* text("") is a documented no-op; element still self-closes. */
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "x");
    axl_xml_writer_text(&w, "");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "text-empty: no error");
    test_check(str_equals(s, "<x/>"), "text-empty: still self-closes");
    axl_string_free(s);
}

static void
test_writer_attr_empty_name(void)
{
    /* Empty attribute name is rejected with sticky error. */
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "x");
    axl_xml_writer_attribute(&w, "", "value");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(axl_xml_writer_error(&w),
               "attr-empty-name: sets sticky error");
    axl_string_free(s);
}

static void
test_writer_prologue_plus_doctype(void)
{
    /* Prologue then doctype, in either order before any element. */
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_prologue(&w);
    axl_xml_writer_doctype(&w, "root", "root.dtd");
    axl_xml_writer_start_element(&w, "root");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "prologue+doctype: no error");
    test_check(str_equals(s,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE root SYSTEM \"root.dtd\"><root/>"),
        "prologue+doctype: both emitted in order");
    axl_string_free(s);
}

static void
test_writer_max_depth(void)
{
    /* Push exactly AXL_XML_WRITER_MAX_DEPTH elements (allowed), then
       one more (must fail). Then close one — the failed extra push
       should leave us at the cap and the sticky error set. */
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    for (int i = 0; i < AXL_XML_WRITER_MAX_DEPTH; i++) {
        axl_xml_writer_start_element(&w, "d");
    }
    test_check(!axl_xml_writer_error(&w),
               "max-depth: at-cap OK");
    axl_xml_writer_start_element(&w, "overflow");
    test_check(axl_xml_writer_error(&w),
               "max-depth: over-cap sets sticky error");
    /* axl_xml_writer_finish is the ONLY thing that releases the strdup'd
       element-name stack, error path included — walking away from a writer
       with 64 names on it leaked all 64 (caught by the teardown leak gate,
       test/integration/common-test.sh). Nothing about the sticky error
       excuses skipping it. */
    axl_xml_writer_finish(&w);
    axl_string_free(s);
}

static void
test_writer_propfind_shape(void)
{
    /* Round-trips the exact WebDAV PROPFIND envelope we hand-roll
       today, demonstrating the writer would be a drop-in for
       emit_entry. */
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "D:multistatus");
    axl_xml_writer_attribute(&w, "xmlns:D", "DAV:");
    axl_xml_writer_start_element(&w, "D:response");
    axl_xml_writer_start_element(&w, "D:href");
    axl_xml_writer_text(&w, "/dav/foo");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w), "propfind: no error");
    test_check(str_equals(s,
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>/dav/foo</D:href></D:response>"
        "</D:multistatus>"),
        "propfind: WebDAV envelope round-trips");
    axl_string_free(s);
}

// ---------------------------------------------------------------------------
// Reader Tests
// ---------------------------------------------------------------------------

/* Convenience: open reader on a NUL-terminated literal. */
static AxlXmlReader *
reader_from(const char *xml)
{
    size_t n = 0;
    while (xml[n] != '\0') {
        n++;
    }
    return axl_xml_reader_new(xml, n);
}

/* token_eq: returns true if the token's name slice equals the
   NUL-terminated literal. */
static bool
name_eq(const AxlXmlToken *tok, const char *expected)
{
    if (tok->name == NULL || expected == NULL) {
        return false;
    }
    size_t i;
    for (i = 0; i < tok->name_len; i++) {
        if (expected[i] == '\0' || tok->name[i] != expected[i]) {
            return false;
        }
    }
    return expected[i] == '\0';
}

/* text_eq: returns true if the token's text slice equals the
   NUL-terminated literal. */
static bool
text_eq(const AxlXmlToken *tok, const char *expected)
{
    if (tok->text == NULL || expected == NULL) {
        return false;
    }
    size_t i;
    for (i = 0; i < tok->text_len; i++) {
        if (expected[i] == '\0' || tok->text[i] != expected[i]) {
            return false;
        }
    }
    return expected[i] == '\0';
}

static void
test_reader_self_closing(void)
{
    AxlXmlReader *r = reader_from("<foo/>");
    AxlXmlToken   t;
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "foo"),
               "reader-self: START foo");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "foo"),
               "reader-self: END foo");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_DOCUMENT,
               "reader-self: END_DOCUMENT");
    test_check(!axl_xml_reader_next(r, &t),
               "reader-self: post-EOF false");
    test_check(!axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-self: clean EOF no error");
    axl_xml_reader_free(r);
}

static void
test_reader_open_close(void)
{
    AxlXmlReader *r = reader_from("<foo></foo>");
    AxlXmlToken   t;
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "foo"),
               "reader-open-close: START foo");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "foo"),
               "reader-open-close: END foo");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_DOCUMENT,
               "reader-open-close: END_DOCUMENT");
    axl_xml_reader_free(r);
}

static void
test_reader_text(void)
{
    AxlXmlReader *r = reader_from("<foo>hello</foo>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT &&
               text_eq(&t, "hello") && !t.is_cdata,
               "reader-text: TEXT hello");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT,
               "reader-text: END");
    axl_xml_reader_free(r);
}

static void
test_reader_attribute(void)
{
    AxlXmlReader *r = reader_from("<foo k=\"v\"/>");
    AxlXmlToken   t;
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT,
               "reader-attr: START");
    const char *v = axl_xml_reader_attr(r, "k");
    test_check(v != NULL && axl_strcmp(v, "v") == 0,
               "reader-attr: k=v");
    test_check(axl_xml_reader_attr(r, "missing") == NULL,
               "reader-attr: missing returns NULL");
    axl_xml_reader_free(r);
}

static void
test_reader_multi_attributes(void)
{
    AxlXmlReader *r = reader_from("<foo a=\"1\" b=\"2\" c=\"3\"/>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);
    const char *a = axl_xml_reader_attr(r, "a");
    const char *b = axl_xml_reader_attr(r, "b");
    const char *c = axl_xml_reader_attr(r, "c");
    test_check(a != NULL && axl_strcmp(a, "1") == 0,
               "reader-multi-attr: a");
    test_check(b != NULL && axl_strcmp(b, "2") == 0,
               "reader-multi-attr: b");
    test_check(c != NULL && axl_strcmp(c, "3") == 0,
               "reader-multi-attr: c");
    axl_xml_reader_free(r);
}

static void
test_reader_nested(void)
{
    AxlXmlReader *r = reader_from("<a><b/></a>");
    AxlXmlToken   t;
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "a"), "reader-nested: START a");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "b"), "reader-nested: START b");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "b"), "reader-nested: END b");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "a"), "reader-nested: END a");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_DOCUMENT,
               "reader-nested: END_DOCUMENT");
    axl_xml_reader_free(r);
}

static void
test_reader_mixed_content(void)
{
    /* Pattern: text, child element, text. */
    AxlXmlReader *r = reader_from("<p>x<b>y</b>z</p>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START p */
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT && text_eq(&t, "x"),
               "reader-mixed: TEXT x");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "b"), "reader-mixed: START b");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT && text_eq(&t, "y"),
               "reader-mixed: TEXT y");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "b"), "reader-mixed: END b");
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT && text_eq(&t, "z"),
               "reader-mixed: TEXT z");
    axl_xml_reader_free(r);
}

static void
test_reader_named_entities(void)
{
    AxlXmlReader *r = reader_from("<x>a&amp;b&lt;c&gt;d&quot;e&apos;f</x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT &&
               text_eq(&t, "a&b<c>d\"e'f"),
               "reader-entities: 5 named decoded");
    axl_xml_reader_free(r);
}

static void
test_reader_numeric_entities(void)
{
    AxlXmlReader *r = reader_from("<x>&#65;&#x42;</x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT &&
               text_eq(&t, "AB"),
               "reader-numeric-entities: &#65;&#x42; → AB");
    axl_xml_reader_free(r);
}

/* The 2-, 3- and 4-byte arms of the character-reference encoder. Only the
   1-byte arm (&#65;) had coverage, so a botched encode above U+007F would have
   gone unnoticed by the whole binary. Written as byte arrays rather than
   escaped literals so the assertion pins the exact wire bytes. */
static void
test_reader_multibyte_entities(void)
{
    static const char expect[] = {
        (char)0xC3, (char)0xA9,                                     /* U+00E9  */
        (char)0xE2, (char)0x82, (char)0xAC,                         /* U+20AC  */
        (char)0xF0, (char)0x9F, (char)0x98, (char)0x80,             /* U+1F600 */
        '\0'
    };
    AxlXmlReader *r = reader_from("<x>&#xE9;&#x20AC;&#x1F600;</x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT &&
               t.text_len == sizeof(expect) - 1 &&
               text_eq(&t, expect),
               "reader-multibyte-entities: 2/3/4-byte references encode exactly");
    axl_xml_reader_free(r);
}

static void
test_reader_attr_entity(void)
{
    AxlXmlReader *r = reader_from("<x k=\"a&amp;b&lt;c\"/>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);
    const char *v = axl_xml_reader_attr(r, "k");
    test_check(v != NULL && axl_strcmp(v, "a&b<c") == 0,
               "reader-attr-entity: decoded");
    axl_xml_reader_free(r);
}

static void
test_reader_comment_skip(void)
{
    AxlXmlReader *r = reader_from("<!-- ignore me --><x/>");
    AxlXmlToken   t;
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "x"),
               "reader-comment: skipped, START x");
    axl_xml_reader_free(r);
}

static void
test_reader_pi_skip(void)
{
    AxlXmlReader *r = reader_from(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><x/>");
    AxlXmlToken   t;
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "x"),
               "reader-pi: XML decl skipped");
    axl_xml_reader_free(r);
}

static void
test_reader_doctype_skip(void)
{
    AxlXmlReader *r = reader_from(
        "<!DOCTYPE foo SYSTEM \"foo.dtd\"><x/>");
    AxlXmlToken   t;
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "x"),
               "reader-doctype: skipped");
    axl_xml_reader_free(r);
}

static void
test_reader_cdata(void)
{
    AxlXmlReader *r = reader_from("<x><![CDATA[a<b&c]]></x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT &&
               text_eq(&t, "a<b&c") && t.is_cdata,
               "reader-cdata: passthrough with is_cdata");
    axl_xml_reader_free(r);
}

static void
test_reader_tag_mismatch(void)
{
    AxlXmlReader *r = reader_from("<a></b>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START a */
    test_check(!axl_xml_reader_next(r, &t),
               "reader-mismatch: next returns false");
    test_check(axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-mismatch: error flagged");
    axl_xml_reader_free(r);
}

static void
test_reader_unclosed(void)
{
    AxlXmlReader *r = reader_from("<a>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START a */
    test_check(!axl_xml_reader_next(r, &t),
               "reader-unclosed: next returns false at EOF");
    test_check(axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-unclosed: error flagged");
    axl_xml_reader_free(r);
}

static void
test_reader_unknown_entity(void)
{
    AxlXmlReader *r = reader_from("<x>&bogus;</x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(!axl_xml_reader_next(r, &t),
               "reader-unknown-entity: TEXT fails");
    test_check(axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-unknown-entity: error flagged");
    axl_xml_reader_free(r);
}

static void
test_reader_multiple_roots(void)
{
    AxlXmlReader *r = reader_from("<a/><b/>");
    AxlXmlToken   t;
    /* First root <a/> emits cleanly. */
    axl_xml_reader_next(r, &t);  /* START a */
    axl_xml_reader_next(r, &t);  /* END a */
    /* Second root must be rejected. */
    test_check(!axl_xml_reader_next(r, &t),
               "reader-multi-root: refused");
    test_check(axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-multi-root: error flagged");
    axl_xml_reader_free(r);
}

static void
test_reader_error_position(void)
{
    AxlXmlReader *r = reader_from("<a>\n<b></c>");
    AxlXmlToken   t;
    while (axl_xml_reader_next(r, &t)) {
        /* drain until error */
    }
    uint32_t    line = 0, col = 0;
    const char *msg  = NULL;
    test_check(axl_xml_reader_error(r, &line, &col, &msg),
               "reader-pos: error flagged");
    test_check(line == 2,
               "reader-pos: line 2 (1-based)");
    test_check(msg != NULL && msg[0] != '\0',
               "reader-pos: message non-empty");
    axl_xml_reader_free(r);
}

static void
test_reader_forbidden_nul(void)
{
    /* &#0; must be rejected (XML 1.0 §4.1; would corrupt C-string
       views of the decoded text). */
    AxlXmlReader *r = reader_from("<x>&#0;</x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(!axl_xml_reader_next(r, &t),
               "reader-nul-entity: next fails");
    test_check(axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-nul-entity: error flagged");
    axl_xml_reader_free(r);
}

static void
test_reader_forbidden_surrogate(void)
{
    /* &#xD800; must be rejected (UTF-16 surrogate; would produce
       invalid UTF-8 in the scratch buffer). */
    AxlXmlReader *r = reader_from("<x>&#xD800;</x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    test_check(!axl_xml_reader_next(r, &t),
               "reader-surrogate-entity: next fails");
    test_check(axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-surrogate-entity: error flagged");
    axl_xml_reader_free(r);
}

static void
test_reader_empty_buffer(void)
{
    /* Empty buffer → "no root element" error on first next(). */
    AxlXmlReader *r = axl_xml_reader_new("", 0);
    AxlXmlToken   t;
    test_check(!axl_xml_reader_next(r, &t),
               "reader-empty: next fails");
    test_check(axl_xml_reader_error(r, NULL, NULL, NULL),
               "reader-empty: error flagged");
    axl_xml_reader_free(r);
}

static void
test_reader_local_name_unprefixed(void)
{
    AxlXmlReader *r = reader_from("<foo/>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START foo */
    size_t llen = 0;
    const char *loc = axl_xml_token_local_name(&t, &llen);
    test_check(llen == 3 && loc != NULL && loc[0] == 'f' && loc[1] == 'o' && loc[2] == 'o',
               "local_name: unprefixed returns full name");
    test_check(axl_xml_token_local_name_eq(&t, "foo"),
               "local_name_eq: unprefixed match");
    test_check(!axl_xml_token_local_name_eq(&t, "bar"),
               "local_name_eq: unprefixed mismatch");
    axl_xml_reader_free(r);
}

static void
test_reader_local_name_prefixed(void)
{
    AxlXmlReader *r = reader_from("<D:response/>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START D:response */
    size_t llen = 0;
    const char *loc = axl_xml_token_local_name(&t, &llen);
    test_check(llen == 8,
               "local_name: prefixed length is post-colon (response = 8)");
    test_check(loc != NULL && loc[0] == 'r' && axl_strncmp(loc, "response", 8) == 0,
               "local_name: prefixed pointer skips past colon");
    test_check(axl_xml_token_local_name_eq(&t, "response"),
               "local_name_eq: prefixed match");
    test_check(!axl_xml_token_local_name_eq(&t, "D:response"),
               "local_name_eq: full qname not matched");
    axl_xml_reader_free(r);
}

static void
test_reader_local_name_end_tag(void)
{
    /* Local-name helpers must work on END_ELEMENT tokens too —
       the WebDAV client matches both opens and closes. */
    AxlXmlReader *r = reader_from("<D:href>x</D:href>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START D:href */
    axl_xml_reader_next(r, &t);  /* TEXT x */
    axl_xml_reader_next(r, &t);  /* END D:href */
    test_check(t.type == AXL_XML_TOKEN_END_ELEMENT,
               "local_name: END token ready");
    test_check(axl_xml_token_local_name_eq(&t, "href"),
               "local_name_eq: END token matches local part");
    axl_xml_reader_free(r);
}

static void
test_reader_local_name_text_token(void)
{
    /* TEXT / END_DOCUMENT have no name — _eq must return false. */
    AxlXmlReader *r = reader_from("<x>hello</x>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    axl_xml_reader_next(r, &t);  /* TEXT */
    test_check(t.type == AXL_XML_TOKEN_TEXT,
               "local_name: TEXT token ready");
    test_check(!axl_xml_token_local_name_eq(&t, "anything"),
               "local_name_eq: TEXT never matches");
    axl_xml_reader_free(r);
}

/* ns_uri match: token's ns_uri slice equals NUL-terminated literal. */
static bool
ns_eq(const AxlXmlToken *tok, const char *want)
{
    if (tok->ns_uri == NULL || want == NULL) {
        return false;
    }
    size_t i;
    for (i = 0; i < tok->ns_uri_len; i++) {
        if (want[i] == '\0' || tok->ns_uri[i] != want[i]) {
            return false;
        }
    }
    return want[i] == '\0';
}

static void
test_reader_ns_prefix_binding(void)
{
    AxlXmlReader *r = reader_from("<x:foo xmlns:x=\"http://example.com/\"/>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);
    test_check(t.type == AXL_XML_TOKEN_START_ELEMENT &&
               ns_eq(&t, "http://example.com/"),
               "ns: prefix-bound element resolves to declared URI");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_default_binding(void)
{
    AxlXmlReader *r = reader_from("<foo xmlns=\"DAV:\"/>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);
    test_check(t.type == AXL_XML_TOKEN_START_ELEMENT &&
               ns_eq(&t, "DAV:"),
               "ns: default-binding element resolves to declared URI");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_inheritance(void)
{
    /* Child inherits parent's default ns binding. */
    AxlXmlReader *r = reader_from("<a xmlns=\"A:\"><b/></a>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START a */
    test_check(ns_eq(&t, "A:"), "ns: parent has its declared ns");
    axl_xml_reader_next(r, &t);  /* START b */
    test_check(t.type == AXL_XML_TOKEN_START_ELEMENT &&
               ns_eq(&t, "A:"),
               "ns: child inherits parent's default ns");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_shadowing(void)
{
    /* Inner element re-declares default ns → shadows parent. */
    AxlXmlReader *r = reader_from(
        "<a xmlns=\"A:\"><b xmlns=\"B:\"/></a>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START a */
    test_check(ns_eq(&t, "A:"), "ns: outer is A:");
    axl_xml_reader_next(r, &t);  /* START b */
    test_check(ns_eq(&t, "B:"), "ns: inner shadows with B:");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_pop_on_close(void)
{
    /* After the inner element closes, a sibling sees the outer's ns
       again — bindings declared by the inner are popped. */
    AxlXmlReader *r = reader_from(
        "<a xmlns=\"A:\"><b xmlns=\"B:\"/><c/></a>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START a */
    axl_xml_reader_next(r, &t);  /* START b */
    axl_xml_reader_next(r, &t);  /* END b */
    axl_xml_reader_next(r, &t);  /* START c */
    test_check(t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "c") && ns_eq(&t, "A:"),
               "ns: sibling re-resolves to outer's ns after inner closes");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_unbound(void)
{
    /* No xmlns in scope → ns_uri is NULL. */
    AxlXmlReader *r = reader_from("<foo/>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);
    test_check(t.type == AXL_XML_TOKEN_START_ELEMENT &&
               t.ns_uri == NULL && t.ns_uri_len == 0,
               "ns: unbound element has NULL ns_uri");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_end_token(void)
{
    /* END token gets the same resolved ns as its START. */
    AxlXmlReader *r = reader_from(
        "<D:href xmlns:D=\"DAV:\">x</D:href>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START */
    axl_xml_reader_next(r, &t);  /* TEXT */
    axl_xml_reader_next(r, &t);  /* END */
    test_check(t.type == AXL_XML_TOKEN_END_ELEMENT && ns_eq(&t, "DAV:"),
               "ns: END token carries the same resolved URI");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_undeclare_default(void)
{
    /* Namespaces 1.0 errata: `xmlns=""` undeclares the default ns.
       The inner element must resolve to NULL ns_uri, NOT to an
       empty-string URI (which would mislead consumers into thinking
       there's a "real" namespace with no URI). */
    AxlXmlReader *r = reader_from(
        "<a xmlns=\"A:\"><b xmlns=\"\"/></a>");
    AxlXmlToken   t;
    axl_xml_reader_next(r, &t);  /* START a */
    test_check(ns_eq(&t, "A:"), "ns-undeclare: outer is A:");
    axl_xml_reader_next(r, &t);  /* START b */
    test_check(t.type == AXL_XML_TOKEN_START_ELEMENT &&
               t.ns_uri == NULL && t.ns_uri_len == 0,
               "ns-undeclare: inner with xmlns=\"\" is NULL");
    axl_xml_reader_free(r);
}

static void
test_reader_ns_propfind_envelope(void)
{
    /* Full WebDAV PROPFIND envelope: D: prefix bound at the root,
       every nested element resolves to "DAV:" — locks down the
       motivating use case. */
    const char *xml =
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>/x</D:href></D:response>"
        "</D:multistatus>";
    AxlXmlReader *r = reader_from(xml);
    AxlXmlToken   t;
    while (axl_xml_reader_next(r, &t)) {
        if (t.type == AXL_XML_TOKEN_START_ELEMENT ||
            t.type == AXL_XML_TOKEN_END_ELEMENT)
        {
            test_check(ns_eq(&t, "DAV:"),
                       "ns: propfind envelope element in DAV: ns");
        }
    }
    axl_xml_reader_free(r);
}

static void
test_reader_propfind_roundtrip(void)
{
    /* Round-trip the WebDAV PROPFIND envelope we tested in the
       writer suite. Demonstrates the reader handles the full
       D:multistatus / D:response / D:href shape and qname
       namespace prefixes. */
    const char *xml =
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>/dav/foo</D:href></D:response>"
        "</D:multistatus>";
    AxlXmlReader *r = reader_from(xml);
    AxlXmlToken   t;

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "D:multistatus"),
               "propfind-rt: START D:multistatus");
    test_check(axl_strcmp(axl_xml_reader_attr(r, "xmlns:D"), "DAV:") == 0,
               "propfind-rt: xmlns:D=DAV:");

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "D:response"),
               "propfind-rt: START D:response");

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_START_ELEMENT &&
               name_eq(&t, "D:href"),
               "propfind-rt: START D:href");

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_TEXT &&
               text_eq(&t, "/dav/foo"),
               "propfind-rt: href text");

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "D:href"),
               "propfind-rt: END D:href");

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "D:response"),
               "propfind-rt: END D:response");

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_ELEMENT &&
               name_eq(&t, "D:multistatus"),
               "propfind-rt: END D:multistatus");

    test_check(axl_xml_reader_next(r, &t) &&
               t.type == AXL_XML_TOKEN_END_DOCUMENT,
               "propfind-rt: END_DOCUMENT");

    axl_xml_reader_free(r);
}

/* --- AxlXmlFlags: the alignment contract ------------------------------- */

/* The indent WIDTH is a parameter now; it was hardcoded at 2. Asserted as
   whole documents, not substrings: the newlines and leading spaces ARE the
   thing under test, and a substring match steps over exactly them. */
static void
test_writer_indent_width(void)
{
    struct { uint32_t n; const char *want; } row[] = {
        { 0, "<a>\n<b>x</b>\n</a>\n"     },
        { 1, "<a>\n <b>x</b>\n</a>\n"    },
        { 2, "<a>\n  <b>x</b>\n</a>\n"   },
        { 4, "<a>\n    <b>x</b>\n</a>\n" },
    };
    for (size_t i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
        AxlString    *s = axl_string_new(NULL);
        AxlXmlWriter  w;
        axl_xml_writer_init(&w, s, axl_xml_indent(row[i].n));
        axl_xml_writer_start_element(&w, "a");
        axl_xml_writer_start_element(&w, "b");
        axl_xml_writer_text(&w, "x");
        axl_xml_writer_end_element(&w);
        axl_xml_writer_end_element(&w);
        axl_xml_writer_finish(&w);
        test_check(!axl_xml_writer_error(&w) && str_equals(s, row[i].want),
                   "xmlflags: the indent WIDTH is honoured, not hardcoded at 2");
        axl_string_free(s);
    }
}

/* AXL_XML_DEFAULT (zero) is compact, and INDENT(0) is NOT the same thing.
   That difference is the whole reason HAS_INDENT is a separate bit. */
static void
test_writer_default_is_compact(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;
    axl_xml_writer_init(&w, s, AXL_XML_DEFAULT);
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_start_element(&w, "b");
    axl_xml_writer_text(&w, "x");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w) && str_equals(s, "<a><b>x</b></a>"),
               "xmlflags: AXL_XML_DEFAULT is compact - no newline, no indent");
    axl_string_free(s);

    s = axl_string_new(NULL);
    axl_xml_writer_init(&w, s, AXL_XML_INDENT(0));
    axl_xml_writer_start_element(&w, "a");
    axl_xml_writer_start_element(&w, "b");
    axl_xml_writer_text(&w, "x");
    axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
    axl_xml_writer_finish(&w);
    test_check(!axl_xml_writer_error(&w)
               && str_equals(s, "<a>\n<b>x</b>\n</a>\n"),
               "xmlflags: INDENT(0) means newlines with ZERO indent, which is "
               "why HAS_INDENT is a separate bit");
    axl_string_free(s);
}

/* The point of the realignment: the bit that used to mean "XML pretty" is
   JSON's ALLOW_COMMENTS, and XML must now REFUSE it rather than pretty-print. */
static void
test_writer_refuses_foreign_flags(void)
{
    AxlString    *s = axl_string_new(NULL);
    AxlXmlWriter  w;

    axl_xml_writer_init(&w, s, (AxlXmlFlags)1 << 0);  /* AXL_JSON_ALLOW_COMMENTS */
    test_check(axl_xml_writer_error(&w),
               "xmlflags: a bit XML does not define is REFUSED, not ignored - "
               "bit 0 is JSON's ALLOW_COMMENTS and used to mean XML pretty");
    axl_string_free(s);

    s = axl_string_new(NULL);
    axl_xml_writer_init(&w, s, AXL_XML_INDENT(2) | ((AxlXmlFlags)1 << 12));
    test_check(axl_xml_writer_error(&w),
               "xmlflags: a foreign bit ALONGSIDE a valid one is refused too, "
               "so a partial request cannot half-apply");
    axl_string_free(s);
}

/* The alignment asserted as an identity rather than described in a comment:
   if these drift, handing one writer the other's indent stops being correct
   and becomes a silent trap again. */
static void
test_flags_agree_with_json(void)
{
    test_check(AXL_XML_INDENT(2) == AXL_JSON_INDENT(2)
               && AXL_XML_INDENT(7) == AXL_JSON_INDENT(7),
               "xmlflags: AXL_XML_INDENT is bit-for-bit AXL_JSON_INDENT, which "
               "is what makes cross-use correct instead of silent");
    test_check(AXL_XML_HAS_INDENT == AXL_JSON_HAS_INDENT
               && AXL_XML_INDENT_MASK == AXL_JSON_INDENT_MASK
               && AXL_XML_INDENT_MAX == AXL_JSON_INDENT_MAX,
               "xmlflags: and so are the presence bit, the width mask and the "
               "maximum");
    test_check((AXL_XML_KNOWN_MASK & 0x3FFu) == 0,
               "xmlflags: XML defines no bit in JSON's dialect range, so a "
               "dialect flag can never mean something here");
    test_check(AXL_XML_INDENT_OF(AXL_XML_INDENT(64)) == AXL_XML_INDENT_MAX
               && AXL_XML_INDENT_OF(axl_xml_indent(1000)) == AXL_XML_INDENT_MAX,
               "xmlflags: an over-large width CLAMPS; masking would wrap it to "
               "a SMALLER indent");
    uint32_t    side = 3;
    AxlXmlFlags once = axl_xml_indent(side++);
    test_check(side == 4 && once == AXL_XML_INDENT(3),
               "xmlflags: axl_xml_indent evaluates n exactly once");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
test_xml_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlXml");

    /* --- Writer --- */
    test_writer_empty();
    test_writer_self_closing();
    test_writer_text();
    test_writer_attribute();
    test_writer_multi_attributes();
    test_writer_nested();
    test_writer_pretty();
    test_writer_indent_width();
    test_writer_default_is_compact();
    test_writer_refuses_foreign_flags();
    test_flags_agree_with_json();
    test_writer_text_escape();
    test_writer_text_quotes_pass_through();
    test_writer_attr_escape();
    test_writer_prologue();
    test_writer_doctype();
    test_writer_doctype_bare();
    test_writer_textn();
    test_writer_mixed_content();
    test_writer_close_without_start();
    test_writer_attribute_outside_start_tag();
    test_writer_prologue_late();
    test_writer_doctype_late();
    test_writer_unclosed_at_finish();
    test_writer_pretty_deep();
    test_writer_text_empty();
    test_writer_attr_empty_name();
    test_writer_prologue_plus_doctype();
    test_writer_max_depth();
    test_writer_propfind_shape();

    /* --- Reader --- */
    test_reader_self_closing();
    test_reader_open_close();
    test_reader_text();
    test_reader_attribute();
    test_reader_multi_attributes();
    test_reader_nested();
    test_reader_mixed_content();
    test_reader_named_entities();
    test_reader_numeric_entities();
    test_reader_multibyte_entities();
    test_reader_attr_entity();
    test_reader_comment_skip();
    test_reader_pi_skip();
    test_reader_doctype_skip();
    test_reader_cdata();
    test_reader_tag_mismatch();
    test_reader_unclosed();
    test_reader_unknown_entity();
    test_reader_multiple_roots();
    test_reader_error_position();
    test_reader_forbidden_nul();
    test_reader_forbidden_surrogate();
    test_reader_empty_buffer();
    test_reader_local_name_unprefixed();
    test_reader_local_name_prefixed();
    test_reader_local_name_end_tag();
    test_reader_local_name_text_token();
    test_reader_ns_prefix_binding();
    test_reader_ns_default_binding();
    test_reader_ns_inheritance();
    test_reader_ns_shadowing();
    test_reader_ns_pop_on_close();
    test_reader_ns_unbound();
    test_reader_ns_end_token();
    test_reader_ns_undeclare_default();
    test_reader_ns_propfind_envelope();
    test_reader_propfind_roundtrip();

    return test_print_results();
}

AXL_APP(test_xml_main)
