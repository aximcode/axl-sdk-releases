/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-xml.h
 *
 * Streaming XML writer + pull-token reader. Caller-managed
 * namespaces: the writer treats qnames like `D:multistatus` as
 * opaque strings; namespace declarations are normal attributes.
 * The reader returns one token at a time — START_ELEMENT,
 * END_ELEMENT, TEXT, END_DOCUMENT — and attribute lookup is via
 * axl_xml_reader_attr while positioned at a START_ELEMENT.
 *
 * Out of scope (intentional): DTD validation, schema validation
 * (XSD / RelaxNG), XPath, XSLT, XML signatures. UTF-8 only.
 *
 * Two independent APIs:
 *   - AxlXmlWriter — build XML into an AxlString
 *   - AxlXmlReader — pull tokens from an XML buffer
 */

#ifndef AXL_XML_H
#define AXL_XML_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// AxlXmlWriter
// ---------------------------------------------------------------------------

/**
 * AxlXmlFlags:
 *
 * One 64-bit flag word for the XML writer, laid out to AGREE WITH
 * #AxlJsonFlags wherever the two express the same idea.
 *
 * @par Why the layout is shared
 *
 * It was `AxlXmlWriterFlags`, a 2-value enum whose `AXL_XML_WRITER_PRETTY`
 * was `1 << 0` — the same bit as `AXL_JSON_ALLOW_COMMENTS`. Both are plain
 * integers in C, so `axl_json_writer_init(&w, out, AXL_XML_WRITER_PRETTY)`
 * COMPILED and silently asked for JSON comments; gcc even suggested the XML
 * name when a removed JSON one was used.
 *
 * A separate typedef does NOT fix that. `typedef uint64_t AxlXmlFlags` and
 * `typedef uint64_t AxlJsonFlags` are the SAME TYPE to a C compiler, so the
 * mistake still compiles. What fixes it is that **the same bit now means the
 * same thing**: #AXL_XML_INDENT and `AXL_JSON_INDENT` are bit-for-bit
 * identical, so handing one writer the other's indent request is not a trap,
 * it is simply correct. Cross-use is either right or diagnosable — never
 * silently a different feature.
 *
 * The other half is that XML defines nothing in JSON's dialect range (bits
 * 0-9), and axl_xml_writer_init() REFUSES any bit it does not define. So a
 * stray `AXL_JSON_ALLOW_COMMENTS` reaching the XML writer sets the sticky
 * error rather than doing something arbitrary.
 *
 * @note The reverse direction is weaker, and honestly so: `axl_json_writer_init`
 *     validates only the UTF-8 field's reserved value, not unknown bits
 *     generally, so an XML-only flag reaching it would be ignored rather than
 *     refused. There are no XML-only flags today — the whole XML vocabulary is
 *     shared — so nothing can currently take that path.
 * @{
 */
typedef uint64_t AxlXmlFlags;

/// Compact output: no indent, no newlines between elements.
#define AXL_XML_DEFAULT      ((AxlXmlFlags)0)

/** Set by #AXL_XML_INDENT; distinguishes "no indent requested" (compact) from
 *  `AXL_XML_INDENT(0)` (newlines, zero indent). Bit 17, matching
 *  `AXL_JSON_HAS_INDENT`. */
#define AXL_XML_HAS_INDENT   ((AxlXmlFlags)1 << 17)

/// Widest indent the packed field can hold. Matches `AXL_JSON_INDENT_MAX`.
#define AXL_XML_INDENT_MAX   63u

/** Pretty-print with @a n spaces per level, and newlines between elements.
 *
 * `AXL_XML_INDENT(2)` is what `AXL_XML_WRITER_PRETTY` used to mean, byte for
 * byte — the width was hardcoded at 2, and making it a parameter is what the
 * shared layout buys for free.
 *
 * CLAMPS to #AXL_XML_INDENT_MAX rather than masking, for the reason
 * `AXL_JSON_INDENT` does: masking wraps an over-large width to a SMALLER one
 * (64 would become 0, i.e. no indent at all), which is a silent wrong answer
 * for a caller that computed the width at runtime. Clamping is wrong in a way
 * you can see.
 *
 * A MACRO, so it stays a constant expression usable in a file-scope
 * initializer and a C++ `constexpr`. That costs evaluating @a n twice — use
 * axl_xml_indent() for a side-effecting or runtime width. */
#define AXL_XML_INDENT(n)                                                   \
    (AXL_XML_HAS_INDENT |                                                   \
     ((AxlXmlFlags)((uint32_t)(n) > AXL_XML_INDENT_MAX                      \
                    ? AXL_XML_INDENT_MAX : (uint32_t)(n)) << 32))

/// The indent WIDTH field as a mask over the flags word (bits 32-37).
#define AXL_XML_INDENT_MASK  ((AxlXmlFlags)0x3F << 32)

/// Extract the indent width (0 when #AXL_XML_HAS_INDENT is clear).
#define AXL_XML_INDENT_OF(f) \
    ((uint32_t)((((AxlXmlFlags)(f)) & AXL_XML_INDENT_MASK) >> 32))

/** Every bit the XML writer defines. Anything outside this is refused by
 *  axl_xml_writer_init() — which is what stops a JSON dialect bit from
 *  arriving here and meaning nothing in particular. */
#define AXL_XML_KNOWN_MASK   (AXL_XML_HAS_INDENT | AXL_XML_INDENT_MASK)

/**
 * @brief Single-evaluation form of #AXL_XML_INDENT, for a runtime width.
 *
 * Same clamping, but evaluates @a n once. Not a constant expression; prefer
 * the macro when you need one. Mirrors axl_json_indent().
 *
 * @return the flags word for an @a n-space indent.
 */
static inline AxlXmlFlags
axl_xml_indent(
    uint32_t  n    ///< spaces per level; clamped to #AXL_XML_INDENT_MAX
)
{
    if (n > AXL_XML_INDENT_MAX) {
        n = AXL_XML_INDENT_MAX;
    }
    return AXL_XML_HAS_INDENT | ((AxlXmlFlags)n << 32);
}
/** @} */

/// Maximum open-tag nesting the writer's balance stack tracks.
/// Exceeding this sets the sticky error flag.
#define AXL_XML_WRITER_MAX_DEPTH 64

/**
 * AxlXmlWriter:
 *
 * Streaming XML writer that builds into a caller-owned AxlString.
 * Element start / attribute / text / end are independent calls;
 * the state machine tracks open-tag depth so closes can't outrun
 * starts. Auto-escapes `&` `<` `>` in text, plus `"` in attribute
 * values. Fields are private — use accessors.
 *
 * Errors are sticky: once any call detects a structural misuse
 * (close-without-start, stack overflow, attribute-after-content,
 * prologue / doctype after first element) or the backing
 * AxlString fails to grow, every subsequent call is a no-op.
 * Check via axl_xml_writer_error at finish.
 */
typedef struct {
    AxlString  *out;                                 ///< backing store (caller-owned)
    AxlXmlFlags flags;                               ///< AxlXmlFlags in effect
    uint32_t    depth;                               ///< current open-tag depth (0..MAX)
    bool        in_start_tag;                        ///< inside `<foo`, attrs/text/close pending
    bool        prologue_emitted;                    ///< prologue may only be emitted once
    bool        doctype_emitted;                     ///< doctype may only be emitted once
    bool        any_element_emitted;                 ///< root element has been started
    bool        error;                               ///< sticky error flag
    uint64_t    had_text_bits;                       ///< bit i: depth-i element has body text
    uint64_t    had_child_bits;                      ///< bit i: depth-i element has a child element
    /// Tag-name stack: stack[i] is the qname of the element opened
    /// at depth i. strdup'd at start, freed at end. Used in pretty
    /// mode (where we need the name on the closing tag emit path)
    /// and the close-without-start guard.
    char       *stack[AXL_XML_WRITER_MAX_DEPTH];
} AxlXmlWriter;

/**
 * @brief Initialize a writer.
 *
 * The writer appends to @p out — it does not clear it. To reuse a
 * string between writes, the caller calls axl_string_clear
 * before init.
 *
 * @p flags outside #AXL_XML_KNOWN_MASK set the sticky error immediately, so a
 * flag from another module's vocabulary is refused rather than ignored. See
 * #AxlXmlFlags for why that matters and what it does not cover.
 */
void
axl_xml_writer_init(
    AxlXmlWriter *w,      ///< writer to initialize
    AxlString    *out,    ///< destination string (caller-owned)
    AxlXmlFlags   flags   ///< #AXL_XML_DEFAULT or #AXL_XML_INDENT(n)
);

/**
 * @brief Finalize the writer.
 *
 * Validates that all opened elements were closed; sets the sticky
 * error flag if not. Frees any heap state the writer still owns
 * (tag stack entries). NULL-safe.
 *
 * @return the length of @p out after this writer's calls. (Equal
 *     to the bytes written iff the AxlString was empty at
 *     axl_xml_writer_init; otherwise includes any pre-existing
 *     content.)
 */
size_t
axl_xml_writer_finish(
    AxlXmlWriter *w  ///< writer (NULL-safe)
);

/**
 * @brief Query the sticky error flag.
 *
 * Set on AxlString OOM, structural misuse (close-without-start,
 * close-of-wrong-tag, stack overflow), or write-after-finish.
 * Once set, all subsequent writer calls become no-ops.
 *
 * @return true if any error occurred since init.
 */
bool
axl_xml_writer_error(
    const AxlXmlWriter *w  ///< writer
);

/**
 * @brief Emit the XML prologue: `<?xml version="1.0" encoding="UTF-8"?>`.
 *
 * Must be the first emit call after init (writer is otherwise
 * unconstrained — prologue is optional). Calling it after any
 * element-emitting call sets the sticky error flag.
 */
void
axl_xml_writer_prologue(
    AxlXmlWriter *w  ///< writer
);

/**
 * @brief Emit a DOCTYPE declaration: `<!DOCTYPE root SYSTEM "dtd_uri">`.
 *
 * @p dtd_uri may be NULL to emit a bare `<!DOCTYPE root>`. Must
 * appear before the first element start; later calls set the
 * sticky error flag. Neither @p root nor @p dtd_uri is escaped —
 * caller is responsible for providing values legal in those
 * positions (no `<` `>` `"` `&`).
 */
void
axl_xml_writer_doctype(
    AxlXmlWriter *w,           ///< writer
    const char   *root,        ///< root element name
    const char   *dtd_uri      ///< DTD system URI (NULL → bare DOCTYPE)
);

/**
 * @brief Open an element: `<qname`.
 *
 * The element stays "open" — `axl_xml_writer_attribute` calls add
 * attributes, then the start tag is auto-closed (`<qname ...>`)
 * on the first child / text / end. Nesting limit:
 * @c AXL_XML_WRITER_MAX_DEPTH.
 *
 * @p qname is treated as opaque (writer does not split namespace
 * prefix from local name) and is NOT escaped — caller is
 * responsible for the name being legal in tag position. Caller
 * also manages namespace declarations via
 * axl_xml_writer_attribute (e.g. attr `xmlns:D` = `"DAV:"`).
 */
void
axl_xml_writer_start_element(
    AxlXmlWriter *w,     ///< writer
    const char   *qname  ///< element qname (NUL-terminated)
);

/**
 * @brief Add an attribute to the currently-open start tag.
 *
 * Only valid between axl_xml_writer_start_element and the
 * next text/child/end call. Calling outside that window — or with
 * an empty @p name — sets the sticky error flag. @p value is
 * auto-escaped for `&` `<` `"`. @p name is NOT escaped; caller
 * provides a legal attribute name.
 */
void
axl_xml_writer_attribute(
    AxlXmlWriter *w,      ///< writer
    const char   *name,   ///< attribute name
    const char   *value   ///< attribute value (escaped)
);

/**
 * @brief Emit body text content for the current element.
 *
 * Auto-escapes `&` `<` `>`. Multiple text calls between a start
 * and end concatenate. NUL-terminated input variant. An empty
 * @p text is a silent no-op (the element may still self-close as
 * `<foo/>` at axl_xml_writer_end_element); to force
 * `<foo></foo>`, omit the empty text call entirely — the
 * difference is purely stylistic since `<foo/>` and `<foo></foo>`
 * are XML-equivalent.
 */
void
axl_xml_writer_text(
    AxlXmlWriter *w,     ///< writer
    const char   *text   ///< NUL-terminated text
);

/// Length-counted variant of axl_xml_writer_text. Allows
/// emitting non-NUL-terminated slices of a larger buffer.
void
axl_xml_writer_textn(
    AxlXmlWriter *w,     ///< writer
    const char   *text,  ///< text bytes
    size_t        n      ///< byte count
);

/**
 * @brief Close the most recently opened element.
 *
 * Emits `</qname>` (or `/>` if the element has no body content).
 * Order is enforced: closing an element when the stack top doesn't
 * match implies a caller-side balance bug — the writer can't
 * detect "wrong tag" because the closer doesn't name it, but
 * close-without-start does set the sticky error flag.
 */
void
axl_xml_writer_end_element(
    AxlXmlWriter *w  ///< writer
);

// ---------------------------------------------------------------------------
// AxlXmlReader
// ---------------------------------------------------------------------------

/// Forward decl — opaque type.
typedef struct AxlXmlReader AxlXmlReader;

/**
 * AxlXmlTokenType:
 *
 * Discriminator for AxlXmlToken. END_DOCUMENT is delivered
 * once after the last element closes; subsequent
 * axl_xml_reader_next calls return false.
 */
typedef enum {
    AXL_XML_TOKEN_START_ELEMENT,   ///< `<qname ...>` (or self-closing `<qname/>`)
    AXL_XML_TOKEN_END_ELEMENT,     ///< `</qname>` (or self-closing pair)
    AXL_XML_TOKEN_TEXT,            ///< body text or CDATA section content
    AXL_XML_TOKEN_END_DOCUMENT,    ///< no more tokens
} AxlXmlTokenType;

/**
 * AxlXmlToken:
 *
 * One pulled token. String pointers reference into reader-owned
 * storage and are valid only until the next
 * axl_xml_reader_next call. Caller must copy out anything it
 * needs to retain past that.
 */
typedef struct {
    AxlXmlTokenType  type;
    const char      *name;       ///< element qname (START / END only); NULL otherwise
    size_t           name_len;
    /// Resolved namespace URI for this element (START / END only).
    /// NULL when no xmlns binding is in scope. The reader maintains
    /// an internal xmlns binding stack: a prefixed qname (e.g.
    /// `D:foo`) resolves against the nearest enclosing `xmlns:D=`
    /// declaration; an unprefixed name resolves against the nearest
    /// enclosing `xmlns=` (default-namespace) declaration. Use
    /// axl_xml_token_local_name to extract the post-colon part.
    ///
    /// Known limitations (lenient v1; tighten when a consumer asks):
    ///   - The reserved `xml:` prefix is NOT implicitly bound to
    ///     `http://www.w3.org/XML/1998/namespace`. Elements like
    ///     `<xml:lang>` without an explicit `xmlns:xml=` declaration
    ///     resolve to NULL ns_uri.
    ///   - `xmlns=""` (the empty-string undeclare-default-ns form
    ///     per Namespaces 1.0 errata) is honored: subsequent
    ///     unprefixed children resolve to NULL ns_uri, not "".
    const char      *ns_uri;
    size_t           ns_uri_len;
    const char      *text;       ///< text bytes (TEXT only); NULL otherwise
    size_t           text_len;
    bool             is_cdata;   ///< TEXT only: true iff token came from `<![CDATA[ ... ]]>`
} AxlXmlToken;

/**
 * @brief Create a reader over @p buf.
 *
 * The reader references @p buf — caller must keep the buffer alive
 * until axl_xml_reader_free. NUL terminator not required.
 * Returns NULL on OOM.
 */
AxlXmlReader *
axl_xml_reader_new(
    const char *buf,   ///< XML bytes
    size_t      len    ///< byte count
);

/**
 * @brief Pull the next token.
 *
 * On success, fills @p out with the next token's data and returns
 * true. On parse error or after END_DOCUMENT was delivered,
 * returns false; call axl_xml_reader_error to distinguish.
 *
 * The reader yields START_ELEMENT, END_ELEMENT, TEXT, and a single
 * END_DOCUMENT after the last close. XML declaration (`<?xml ?>`),
 * processing instructions, comments, and DOCTYPE declarations are
 * skipped silently. CDATA section content arrives as a TEXT token
 * with `is_cdata` set.
 */
bool
axl_xml_reader_next(
    AxlXmlReader *r,    ///< reader
    AxlXmlToken  *out   ///< [out] filled on success
);

/**
 * @brief Look up an attribute on the current START_ELEMENT.
 *
 * Valid between a successful axl_xml_reader_next that
 * returned a START_ELEMENT and the next
 * axl_xml_reader_next call. Returns NULL if the attribute
 * isn't present, the reader is not positioned at a START_ELEMENT,
 * or @p name is NULL. The returned pointer references reader-owned
 * storage and is valid until the next axl_xml_reader_next.
 */
const char *
axl_xml_reader_attr(
    AxlXmlReader *r,     ///< reader
    const char   *name   ///< attribute name (NUL-terminated)
);

/**
 * @brief Retrieve error details after a false return from
 *     axl_xml_reader_next.
 *
 * Each out-param is optional (NULL skips). After clean EOF
 * (@c AXL_XML_TOKEN_END_DOCUMENT was the last delivered token),
 * returns false with no error message. After a parse error,
 * returns true and fills line / column / message.
 *
 * @return true iff a parse error has occurred.
 */
bool
axl_xml_reader_error(
    const AxlXmlReader *r,       ///< reader
    uint32_t           *line,    ///< [out, optional] 1-based line of the error
    uint32_t           *col,     ///< [out, optional] 1-based column of the error
    const char        **msg      ///< [out, optional] static error message
);

/**
 * @brief Free the reader (NULL-safe).
 *
 * Releases heap state. Does NOT free the input buffer.
 */
void
axl_xml_reader_free(
    AxlXmlReader *r  ///< reader (NULL-safe)
);

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------

/**
 * @brief Return the local name (post-colon) of @p tok's qname.
 *
 * For a prefixed name like `D:response`, returns a pointer to
 * `response` and writes 8 into @p out_len. For an unprefixed name
 * like `response`, returns the full name. The returned pointer
 * aliases into @p tok's @c name slice and is valid for the same
 * lifetime (until the next axl_xml_reader_next).
 *
 * Defined only for START_ELEMENT and END_ELEMENT tokens. On TEXT /
 * END_DOCUMENT (or NULL inputs) returns NULL with @c *out_len = 0.
 */
const char *
axl_xml_token_local_name(
    const AxlXmlToken *tok,      ///< token
    size_t            *out_len   ///< [out] local-name byte length (NULL OK)
);

/**
 * @brief Compare @p tok's local name to a NUL-terminated literal.
 *
 * Convenience predicate folding the local-name extract + memcmp.
 * Returns false for TEXT / END_DOCUMENT tokens, NULL inputs, or
 * length / byte mismatches.
 */
bool
axl_xml_token_local_name_eq(
    const AxlXmlToken *tok,
    const char        *want
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_XML_H */
