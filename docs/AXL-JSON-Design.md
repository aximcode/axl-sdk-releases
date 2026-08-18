# AXL JSON API redesign — one flag space, granular dialect control

**Date:** 2026-07-29
**Status:** **Every phase implemented.** P12 (the pull scanner) and P13
(incremental input / NDJSON) both landed 2026-08-02/03: the whole-document face
is built on the scanner, and the scanner reads from a pull source through a
window it owns.
Extended 2026-07-29 by an API-completeness review against Jansson / JSON-GLib /
yyjson — see "Two engines, four faces" and the revised execution order.
**Supersedes:** `AxlJsonParserFlags` / `AxlJsonWriterFlags` (both removed)

## Problem

AXL's JSON options are two small, unrelated enums:

```c
typedef enum { AXL_JSON_PARSER_DEFAULT = 0,
               AXL_JSON_PARSER_JSON5   = 1u << 0 } AxlJsonParserFlags;

typedef enum { AXL_JSON_WRITER_DEFAULT         = 0,
               AXL_JSON_WRITER_PRETTY          = 1u << 0,
               AXL_JSON_WRITER_TRAILING_COMMAS = 1u << 1 } AxlJsonWriterFlags;
```

Three problems follow from that shape:

1. **JSON5 is all-or-nothing.** A consumer that wants comments in its config
   files must also accept single quotes, hex literals, unquoted keys and
   `\x` escapes. There is no way to say "comments, nothing else."
2. **JSON5 is input-only.** The writer has trailing commas and comments as
   separate ad-hoc flags; nothing ties them to the reader's dialect, so
   "read and write the same dialect" is not expressible.
3. **Formatting is one bit.** `PRETTY` is hardcoded 2-space. There is no
   indent width, no compact separator control, no ASCII-only escaping.

Jansson solved the formatting half of this years ago with `JSON_INDENT(n)`,
`JSON_COMPACT`, `JSON_ENSURE_ASCII` and friends. This design adopts that
shape and extends it to cover dialect and encoding.

## Prior art consulted

| Library | What it does | What we take |
|---|---|---|
| [Jansson](https://jansson.readthedocs.io/en/latest/apiref.html) | `JSON_INDENT(n)`, `JSON_COMPACT`, `JSON_ENSURE_ASCII`, `JSON_SORT_KEYS`, `JSON_ESCAPE_SLASH`, `JSON_EMBED`, `JSON_REJECT_DUPLICATES`, `JSON_DECODE_ANY`; numeric args packed into the flag word | The formatting flag set and the packed-numeric-argument technique |
| [nlohmann/json](https://json.nlohmann.me/api/basic_json/error_handler_t/) | `error_handler_t`: `strict` (throw) / `replace` (U+FFFD) / `ignore` (drop invalid) | The three-mode shape for ill-formed UTF-8 |
| [yyjson](https://github.com/ibireme/yyjson/blob/master/doc/API.md) | `YYJSON_READ_ALLOW_INVALID_UNICODE` / `YYJSON_WRITE_ALLOW_INVALID_UNICODE`, copies invalid bytes verbatim, documents the downstream-safety caveat | The RAW (verbatim) mode and the safety warning |
| [RapidJSON](https://rapidjson.org/md_doc_features.html) | Encoding validation is *opt-in*; `kParseNumbersAsStringsFlag` | Confirmation that lenient-by-default is a defensible choice (we do not take it), and the number-as-string idea |
| [MongoDB Extended JSON](https://github.com/mongodb/specifications/blob/master/source/extended-json/extended-json.md) | canonical (type-preserving `$numberLong`) vs relaxed (readable, lossy) | The relaxed/extended vocabulary. **Deferred** — see below |
| [JSON-GLib](https://gnome.pages.gitlab.gnome.org/json-glib/) | `JsonParser` (DOM) / `JsonReader` (cursor over a DOM) / `JsonBuilder` / `JsonGenerator` / `JsonPath`; `GError` domains; refcounted `JsonNode` tree | The `<noun>_<verb>` naming PATTERN. **Not** the object decomposition, which presumes a refcounted mutable tree |
| [RapidJSON](https://rapidjson.org/md_doc_sax.html) | SAX `Reader` + `Handler` **and** DOM `Document`; streaming `Writer<OutputStream>`; `Document::Accept(writer)` | The four-faces symmetry, and "buffering belongs to the stream, not the serializer" |

## Design

### One flag space

```c
typedef uint64_t AxlJsonFlags;
```

Bits 0–31 are boolean flags; bits 32+ hold packed numeric fields. 64 bits
so packed fields never have to fight boolean flags for room.

A single space, not two, because the dialect flags must provably mean the
same thing in both directions — "read and write JSON5" should be one
constant, not two that can drift. The cost is that a writer-only flag
passed to the parser is a silent no-op; every flag's docstring states
which side honors it.

### Dialect — honored by BOTH reader and writer

```c
AXL_JSON_ALLOW_COMMENTS        (1u << 0)   // // line and /* block */
AXL_JSON_ALLOW_TRAILING_COMMA  (1u << 1)
AXL_JSON_ALLOW_UNQUOTED_KEYS   (1u << 2)   // ASCII IdentifierName only
AXL_JSON_ALLOW_SINGLE_QUOTES   (1u << 3)
AXL_JSON_ALLOW_HEX             (1u << 4)   // 0x1F
AXL_JSON_ALLOW_EXTRA_ESCAPES   (1u << 5)   // \x## \v \0, line continuations
AXL_JSON_ALLOW_PLUS_SIGN       (1u << 6)   // +5
AXL_JSON_ALLOW_LEADING_POINT   (1u << 7)   // .5 and 5.
AXL_JSON_ALLOW_NAN_INF         (1u << 8)   // lexed only — see caveat
```

### UTF-8 handling — both directions

Bit 9 is deliberately left free so another dialect flag can be added
without disturbing the field below it.

This is a two-bit **field**, not two independent flags — as separate bits,
"RAW and STRICT both set" would have no defined meaning:

```c
#define AXL_JSON_UTF8_REPAIR  ((uint64_t)0 << 10)  // DEFAULT: -> U+FFFD
#define AXL_JSON_UTF8_RAW     ((uint64_t)1 << 10)  // copy bytes verbatim
#define AXL_JSON_UTF8_STRICT  ((uint64_t)2 << 10)  // ill-formed => error
#define AXL_JSON_UTF8_MASK    ((uint64_t)3 << 10)  // value 3 reserved
#define AXL_JSON_UTF8_OF(f)   ((f) & AXL_JSON_UTF8_MASK)
```

`REPAIR` stays the default so the writer keeps the guarantee commit
`66ed676e` established: a JSON text is defined over Unicode code points
(RFC 8259 §8.1), so an ill-formed sequence invalidates the whole document.
`RAW` is the escape hatch for byte-exact round-tripping of firmware
strings; like yyjson, its docstring must warn that it hands downstream
code strings that are not valid UTF-8. `STRICT` sets the writer's sticky
error, for a consumer that wants to *detect* upstream corruption rather
than paper over it.

One rule, stated once: **AXL never emits or hands back ill-formed UTF-8
unless you ask for `RAW`.**

### Writer-only

```c
AXL_JSON_COMPACT       (1u << 12)  // drops the space after ':' -- a no-op
                                   // without INDENT; see decision 26
AXL_JSON_ENSURE_ASCII  (1u << 13)  // escape non-ASCII as \uXXXX
AXL_JSON_ESCAPE_SLASH  (1u << 14)  // '/' -> '\/'
AXL_JSON_EMBED         (1u << 15)  // omit outermost braces/brackets
AXL_JSON_SORT_KEYS     (1u << 16)  // write_token only — see caveat
AXL_JSON_HAS_INDENT    (1u << 17)  // set by the INDENT() macro

/* CLAMPS above AXL_JSON_INDENT_MAX; stays a macro so it remains a constant
   expression. axl_json_indent() is the single-evaluation sibling. */
#define AXL_JSON_INDENT(n)                                                  \
    (AXL_JSON_HAS_INDENT |                                                  \
     ((AxlJsonFlags)((uint32_t)(n) > AXL_JSON_INDENT_MAX                    \
                     ? AXL_JSON_INDENT_MAX : (uint32_t)(n)) << 32))
#define AXL_JSON_INDENT_OF(f) ((uint32_t)(((f) >> 32) & 0x3F))
```

The presence bit is load-bearing: it distinguishes "no indent requested"
(compact, no newlines) from `AXL_JSON_INDENT(0)` (newlines, zero indent).
Jansson has the same problem and solves it the same way.

`AXL_JSON_INDENT(2)` replaces `AXL_JSON_WRITER_PRETTY`.

### Container-scoped overrides (P8)

> **SHIPPED, THEN REMOVED 2026-08-04 — see decision 41.** It worked and was
> tested; it had no caller outside its own tests, and keeping it cost
> `AxlJsonWriter` 2 KB of `saved_flags`. Read this section and decision 31 for
> why it cannot be merged into `axl_json_obj_begin(w, flags)` if it ever comes
> back.

Flags are otherwise fixed at `axl_json_writer_init`. P8 adds per-container
overrides that revert automatically when the container closes:

```c
void axl_json_obj_begin_flags(AxlJsonWriter *w, AxlJsonFlags flags);
void axl_json_arr_begin_flags(AxlJsonWriter *w, AxlJsonFlags flags);
```

The motivating case is a pretty document with one subtree emitted
compactly, or one field carrying firmware bytes under `UTF8_RAW` while the
rest of the document stays repaired.

Scoped to the container rather than exposed as `set_flags` or push/pop for
one reason: the writer already maintains a depth stack, so the override
rides it and **unbalanced state is structurally impossible** — there is no
pop to forget, and a mismatched indent width on a closing brace cannot
happen. A raw `set_flags` would make that a matter of caller discipline.

Only per-value flags may be overridden: `INDENT(n)`, `COMPACT`,
`ENSURE_ASCII`, `ESCAPE_SLASH`, and the UTF-8 mode. `EMBED` and
`SORT_KEYS` are structural — `EMBED` is decided at depth 0 and at
`finish`, `SORT_KEYS` applies only to `write_token` — so overriding either
is meaningless and must be rejected with the writer's sticky error rather
than silently ignored.

**The reader deliberately gets no equivalent.** AXL tokenizes the whole
document up front, so the dialect bits are consumed at parse time and a
reader-side mutation API would be a no-op for most of the flag space. The
sole exception is the UTF-8 mode; if a reader-side override is ever wanted it
should arrive as a per-accessor argument, not as mutable reader state.
(SUPERSEDED IN PART by decision 30: the mode turned out to split by VALUE.
`UTF8_STRICT` is consumed at PARSE time, because it asks whether the document
is well-formed and an accessor cannot carry a position. Only `REPAIR` and
`RAW` are lazy.)

### Reader-only

```c
AXL_JSON_REJECT_DUPLICATES  (1u << 18)
/* bit 19 is FREE: AXL_JSON_DECODE_ANY was removed in P3 -- see
   "Decisions taken during implementation" #5. A conformant STRICT already
   accepts a bare-primitive root, so it gated nothing. */

/* Nesting-depth bound, packed at bits 38+ (P3, decision 6). Was a SAFETY
   limit -- recursive descent on a freestanding stack, where the
   alternative to rejecting a 100000-deep document was faulting on it --
   and became a POLICY number in P12e, when both faces stopped recursing.
   No presence bit -- 0 is not a request anyone can mean, so it doubles
   as "absent" and selects the default. */
#define AXL_JSON_DEPTH_DEFAULT  32u    // measured against real documents
#define AXL_JSON_DEPTH_MAX      256u   // clamps, never wraps
#define AXL_JSON_DEPTH(n)       /* clamped, constant-expression macro */
#define AXL_JSON_DEPTH_OF(f)    /* raw field; 0 means DEPTH_DEFAULT */
```

### Reserved

```c
AXL_JSON_EXTENDED  (1u << 20)  // RESERVED — not implemented
```

Listed separately because it is **not** reader-only: emitting typed
wrappers is a writer concern and consuming them is a reader concern, so
when it lands it belongs with the dialect flags above.

### Presets

```c
AXL_JSON_STRICT   0      // RFC 8259
AXL_JSON_JSON5    (ALLOW_COMMENTS | ALLOW_TRAILING_COMMA |
                   ALLOW_UNQUOTED_KEYS | ALLOW_SINGLE_QUOTES |
                   ALLOW_HEX | ALLOW_EXTRA_ESCAPES |
                   ALLOW_PLUS_SIGN | ALLOW_LEADING_POINT |
                   ALLOW_NAN_INF   | ALLOW_EXTRA_WHITESPACE)
AXL_JSON_RELAXED  (AXL_JSON_JSON5 | AXL_JSON_UTF8_RAW)
```

`RELAXED` means "accept whatever you can" on the read side and "emit the
loosest dialect" on the write side. It originally also bundled the reader-only
`DECODE_ANY`; P3 removed that flag, since a bare-primitive root is now accepted
under every flag value including `STRICT`.

`AXL_JSON_RELAXED` is also what the no-flags entry points — `axl_json_parse`
and `axl_json_load_file` — use.

### New accessor

```c
bool
axl_json_get_number_str(
    const AxlJsonReader *r,      ///< reader
    const char          *key,    ///< key to look up
    char                *buf,    ///< receives the literal text
    size_t               size    ///< size of @a buf
);
```

Returns the number token's raw literal text. This is the lossless escape
hatch for anything `axl_json_get_int` / `axl_json_get_uint` must reject:
a value wider than 64 bits, a fractional or exponent literal, or (with
`ALLOW_NAN_INF`) `NaN` / `Infinity`. Mirrors RapidJSON's
`kParseNumbersAsStringsFlag` and serde_json's `arbitrary_precision`.

It exists because commit `5eae8129` made out-of-range integers a hard
`false`. That is the right call — a silently wrapped number is worse than
no number — but it must not make a value *unreachable*.

## Decisions taken during implementation

Recorded here because they deviate from what this document originally
specified, and the document is the contract.

**1. `AXL_JSON_INDENT(n)` CLAMPS, it does not mask (P1).** The spec said mask,
following Jansson. Masking makes an out-of-range width wrap to a *smaller*
one — 64 becomes 0, "no indent at all" — a silent wrong answer for a caller
computing a width at runtime. Clamping is wrong visibly.

It stays a MACRO. A first attempt made it a `static inline` function to get
single-evaluation, and that silently removed constant-expression usability:
file-scope initializers, `case` labels and C++ `constexpr` all stopped
compiling, and nothing in the suite caught it, because C99 permits
non-constant initializers for automatic aggregates. `axl_json_indent()`
exists alongside for a runtime or side-effecting width, and a file-scope
initializer in the unit suite now pins the constant-expression property.

**2. `AXL_JSON_ALLOW_EXTRA_WHITESPACE` claims bit 9 (P2).** Reserved as spare
in P1; spent on a feature the original nine flags had no bit for. `\v` and
`\f` are ES5 whitespace but NOT RFC 8259 whitespace, and a raw TAB is legal
in an ES5 string but not an RFC one. Both were permanently on, so
`AXL_JSON_ALLOW_COMMENTS` did not in fact mean "comments and nothing else" —
the claim the whole granular design rests on.

**3. Indent WIDTH landed in P1, not P5.** A macro ignoring its own argument
would have been a shipped lie, and honoring it is three lines in
`emit_indent`. P5 keeps `COMPACT` / `ESCAPE_SLASH` / `EMBED`.

**4. Three P2 bug fixes that no flag gates**, because they are illegal under
RFC 8259 *and* ES5, so there is no dialect in which they are correct: leading
zeros (`01`), a raw LF/CR inside a string, and an unterminated trailing block
comment — whose error `skip_ws` discarded, silently ACCEPTING a truncated
document.

**5. `AXL_JSON_DECODE_ANY` is REMOVED, not implemented (P3).** The spec listed
it as reader-only "allow a bare-primitive root", bundled into `RELAXED`, and
scheduled it for P7. Running the corpus killed it: **all 8 of the `y_` cases
AXL was failing were bare-primitive roots**, so a conformant `AXL_JSON_STRICT`
has to accept `42` and `"asd"` and there is nothing left for a flag to unlock.
The rule came from RFC 4627 (2006); RFC 8259 (2014) dropped it, and AXL — with
no pre-2014 history — had inherited it from jsmn and from its own lexer
mirroring jsmn "so consumers see consistent behavior across modes."

Bit 19 stays free rather than being reused, so a caller still passing the old
name gets a compile error instead of a silent request for something else.
`RELAXED` loses it, and P7 loses one item.

The writer changed with it. `check_atom_context` refused any atom at depth 0,
justified in a comment by "mirror `axl_json_parse`, which requires the root
token to be an object or array" — the rule P3 deletes. Fixing only the reader
would have left a reader that parses `42` and a writer that cannot emit it,
with a comment citing a rule that no longer existed.

**6. A reader nesting-depth bound, which the spec did not have (P3).** Not a
grammar rule, and the one place AXL is deliberately narrower than "accept every
`i_` case."

It was a SAFETY bound when it landed. The parser was recursive descent, so
nesting depth was stack depth, and AXL is freestanding: no guard page, so an
overflow is a `#GP`, not a diagnostic. Measured ~128 bytes of stack per array
level and ~144 per object level (x86-64, `-Og`), so JSONTestSuite's
`n_structure_100000_opening_arrays` needed ~12.8 MB and faulted rather than
failing. jsmn was iterative and rejected that document, so **without a bound,
P3 would have traded a conformance win for a crash** — verified by removing the
bound, at which point the conformance binary stalled instead of reporting.

**P12e retired the reason, not the bound.** Neither face recurses now: the
scanner tracks one bit per open container and the whole-document builder 8
bytes, so the 100000-array document is rejected by a counter rather than
survived by luck, and the numbers above are history. The bound stays because
how much nesting to accept is a choice worth making deliberately — a caller
raising it is now buying memory proportional to depth, not running closer to a
fault.

- `AXL_JSON_DEPTH_DEFAULT` = **32**. Measured against real documents, not
  picked: DMTF's 109 published Redfish resource mockups nest 5; across 11k
  Redfish schema, interop-profile and example documents the deepest is 13;
  AXL's own `pci-ids.json5` is 7. It is also exactly
  `AXL_JSON_WRITER_MAX_DEPTH`, so anything the reader accepts by default,
  `axl_json_write_token` can re-emit.
- `AXL_JSON_DEPTH(n)` raises or lowers it, clamped to `AXL_JSON_DEPTH_MAX`
  = **256** (~37 KB of stack, the most that can be offered without knowing the
  caller's headroom). Packed at bits 38+, using the same technique as
  `AXL_JSON_INDENT` — but with **no presence bit**: a document has at least one
  level, so `0` is not a request anyone can mean and doubles as "absent".

Consequence for the testing plan below: `i_structure_500_nested_arrays` is
REJECTED. That is legal — `i_` is implementation-defined — but it is an
explicit exception to "accept all `i_`", and it is pinned by name with its
reason rather than absorbed into a tally.

**7. The `i_` decisions are pinned per case, not as a tally (P3).** The spec
said "ACCEPT, and pin the tally." Accept-all turned out to be unachievable and
a tally too weak. Four of the 35 `i_` cases are not UTF-8 JSON documents at all
(three UTF-16 files, one UTF-8 BOM) and RFC 8259 §8.1 requires UTF-8, so they
are refused; a fifth is the nesting case above. A bare tally cannot tell "we
fixed one and broke another" from "nothing moved", so all five are listed by
name in `test/unit/axl-test-json-conformance.c` with the reason at each.

**8. A signed `NaN` / `Infinity` needs TWO flags, and the sign stays in the
token (P4).** The spec listed `ALLOW_NAN_INF` as covering
"`NaN` / `Infinity` / `-Infinity`" without saying what owns the sign.
`-Infinity` needs only `ALLOW_NAN_INF` — RFC 8259 already permits `-` on a
number — but `+Infinity` needs `ALLOW_PLUS_SIGN` as well, because the leading
`+` is that flag's feature and one-feature-one-flag is the rule the whole
design rests on. `-NaN` is accepted for grammar fidelity: ES5, which JSON5
inherits, lets a sign precede either word.

Signed forms are lexed in `parse_number`, not `parse_literal`, because
`parse_value` dispatches on the leading sign — and they are emitted from there
rather than delegated, so the token's `start` points at the SIGN. That is what
makes `axl_json_get_number_str` return `"-Infinity"` and not `"Infinity"`.
Both paths apply the same trailing-character rule, so `NaNa` and `Infinity2`
are errors rather than a literal followed by junk.

**9. `axl_json_get_number_str` REFUSES a short buffer; it does not truncate
(P4).** The spec asked for "one byte too small (false, and pin what happens to
the buffer)" without choosing. It returns false and leaves the buffer
UNTOUCHED, deliberately unlike `axl_json_get_string`, which truncates and still
reports success. A clipped string is merely incomplete; a clipped number is a
DIFFERENT number, and handing back `"1e1"` for `1e10` would be precisely the
silent wrong answer this accessor exists to prevent.

It also refuses non-numbers. `true`, `false` and `null` are primitive tokens
too, and an accessor named `_number_str` returning `"true"` would be a trap.
The check is a positive test on the first byte rather than a blacklist of the
three literals — a blacklist silently reclassifies anything added to the
literal table later, which is exactly what P4 did to it by adding NaN and
Infinity.

**10. No reference counting, ever (design review 2026-07-29).** Not an omission
-- a choice, with the growth path named. See "Ownership and lifetimes". The
condition that keeps it safe: never hand out a value handle whose lifetime is
independent of a document.

**11. `AxlJsonReader` keeps its name; JSON-GLib's PATTERN is adopted, its object
decomposition is not (2026-07-29).** 967 in-tree references, and JSON-GLib's
`JsonReader` means "cursor over a DOM" where ours means "the parsed document" --
reusing the vocabulary would mislead arrivals from both directions. `_new` vs
`_init` follows the convention already measured in the tree (52 vs 19).

**12. Errors carry a CODE and a position, not a message (2026-07-29).** No
`text[]`, no `source[]`. A formatter that takes the document later can quote the
line and point a caret, which beats a canned 160-byte string and costs nothing
today. Never allocated -- allocating to report an allocation failure is why
`GError**` is the wrong model.

**13. Buffering belongs to the sink, not the serializer (2026-07-29).** `AxlStream`
already has `BUF_NONE|LINE|FULL`; a stage buffer in the writer would be a second
layer. Corrected mid-review after Mike asked whether `AxlStream` was already
buffered -- it is, though its default is `BUF_NONE`, unlike stdio.

**14. `key == NULL` does NOT mean "own value" (2026-07-29).** Considered as a way
to collapse the by-key and own-value families and rejected: an accidental NULL
key would silently become "operate on the root" instead of returning false. The
by-key family stays sugar for `get_value` + `value_X`.

**15. Iterators store the reader BY VALUE (design review 2026-07-29;
SHIPPED `3b58c6ee`).** `AxlJsonArrayIter` kept a raw `const AxlJsonReader *`,
which made reusing an element reader silently retarget any iterator built from
it -- a wrong answer with no crash and nothing for ASan to see. Fixed first in
P11, before `AxlJsonObjectIter` could mirror the shape. The regression test was
confirmed RED against the old code and re-confirmed by restoring the pointer
form after the fix. See "One type vocabulary".

**16. Errors are QUERIED from the reader/scanner/writer, not returned through an
out-param (2026-07-29).** The alternative would be a second breaking change to
`axl_json_parse` (51 call sites at the time) or an `_err` twin for every
accessor. It
also matches what `axl_json_writer_error()` already does, so the library has one
mechanism instead of two.

**17. AXL has floating point; it has no `libm` (correction, 2026-07-29).** This
document asserted "no double accessor and no real emission" in constraint 4 for
several revisions. `axl_dtoa()` is public, `%f`/`%e`/`%g` exist, and `double`
appears in 18 public headers. `JSON_REAL_PRECISION` is therefore deferred rather
than n/a, and `axl_json_get_double`'s real cost is the READ side (correctly
rounded decimal-to-double), not the write side.

**18. An unrepresentable code point that will not FIT truncates the string
there; it does not vanish (phase A, 2026-07-30).** The `\uXXXX` fix and the
JSON5 `\0` / `\x00` hardening were written on separate branches. They agreed
that the answer is U+FFFD and disagreed about the bound: the `\u` arm dropped the
code point whole and stopped, while the `\0` / `\x00` arms emitted nothing and
CONTINUED — so a tight buffer made the replacement disappear from the MIDDLE of a
string while its successors survived. `"a\0b"` came back as `"ab"`, reading `\0`
as "nothing" rather than "unrepresentable". That is a quieter version of the very
smuggling primitive the substitution exists to block, and it differs from plain
truncation, which is this accessor's documented convention for a value too long
for its buffer.

Resolved on the `\u` side's behaviour, in ONE shared helper (`append_bytes`)
that every multi-byte append goes through — two arms with private copies of the
same bound is how they came to disagree.

The discriminating test is the one whose document has a character AFTER the
overflowing escape. **None of the 21 tests that came with the `\u` fix catch
vanish-and-continue** — in every one of them the overflowing escape is the last
thing in the string. Verified by sabotage, and it is why the phase adds tests
rather than only merging.

**19. Co-locating two spellings of a constant is not sharing it (phase A,
2026-07-30).** U+FFFD had three private spellings — a byte string plus a length
in `axl-json-build.c`, and the same pair plus a code point in
`axl-json-parse.c`. The first fix moved all three into `axl-json-internal.h`,
which looked like sharing and was not: `parse.c` used only the scalar and
`build.c` only the string, so **no single name was used by both files** and
either could have been changed alone. The review caught it by running the
sabotage separately per name — 17 reader-side failures from one, 8 writer-side
from the other, never both. A claim of 20-across-both had been recorded here on
the strength of changing BOTH at once, which proves nothing about either.

There is now exactly ONE constant, in encoded form, because that is the form
both callers can use directly: the reader appends those bytes rather than
encoding `0xFFFD` itself. `_LEN` derives via `sizeof` instead of duplicating.
Sabotaging the one line now fails 20 assertions spanning the reader
(`json \u:`, `nul union:`, `escape:`) and the writer (`json utf8:`), from a
single edit. The general rule: **a shared constant is one NAME that more than
one caller uses, not two names in one file.**

**20. `\xNN` emitted a raw byte (phase A, 2026-07-30).** Found by the review
pass, and NOT P7's read-side UTF-8 validation work — P7 is about ill-formed
INPUT, while this is AXL breaking input that arrived well-formed.

JSON5 inherits ES5's `HexEscapeSequence`, in which `\xE9` is the code UNIT
U+00E9 and encodes as the two bytes `C3 A9`. AXL emitted a lone `0xE9` — an
ESCAPE producing ill-formed UTF-8, from a decoder that had just grown a helper
whose entire purpose is preventing exactly that. It survived because the only
two `\x` assertions in the tree pin `\x21` and `\x41`, both ASCII, so nothing
ever exercised the high half of the range.

Verified against the JSON5 spec rather than inferred: `\xHH` denotes a
character in U+0000–U+00FF. No in-tree consumer relied on the raw-byte
behaviour (`share/*.json5` carry no `\x` escapes at all), so this is a
behaviour change with no in-tree fallout.

**21. Truncation never leaves half a sequence, and ONE post-hoc trim is what
achieves it (phase A, 2026-07-30).** This took two attempts, and the discarded
one is the instructive part.

The first attempt made each raw RUN atomic on the way in: measure a lead byte
plus the continuation bytes following it, refuse the run whole. It fixed the
common case and **missed the general one**, because two ADJACENT one-byte units
can concatenate into a sequence that no single decode arm ever saw as a unit:

| source | what it is |
|---|---|
| `\<C3>\<A9>` | each byte escaped separately — what a naive byte-oriented escaper emits for U+00E9 |
| `<C3>\<80>` | a raw lead byte, then a separately-escaped continuation |

A cut *between* those two units still split a sequence the untruncated decode
had whole. Both are reachable through `axl_json_parse` under
`AXL_JSON_RELAXED`, which carries `ALLOW_EXTRA_ESCAPES` -- and which that entry
point applied by DEFAULT until decision 41. Under it the lexer accepts `\`
followed by any byte at all. The review pass found this by execution after the first attempt had
already shipped a green suite.

It does **not** matter that such a source is itself ill-formed UTF-8. The test
that matters is whether truncation introduced ill-formedness the FULL decode did
not have — and it did. An earlier version of this reasoning waved these cases
away as "ill-formed source, so pass-through applies"; that is wrong, because
pass-through is about bytes AXL was HANDED, not about a sequence AXL assembled
and then broke.

The fix is `trim_split_tail`: after the loop, **if AXL cut the value short**,
walk back over any trailing incomplete sequence and drop it whole. It looks at
the bytes actually WRITTEN, which is what lets it cover cases no arm can see.

And then the atomic-run measurement is **redundant** — verified by removing it
and finding that 1.15M host-side (input, buffer-size) pairs still hold, and that
every truncation row in the suite still fails when the trim alone is removed. So
it is gone, along with its one genuinely subtle rule (count the continuation
bytes that are THERE, never the count the lead byte declares, or a following `\`
gets swallowed and stops being an escape). **One mechanism, no lookahead.**
Shipping both would have been two mechanisms where one provably suffices, which
is exactly what a reviewer should reject.

What remains load-bearing, each confirmed by its own sabotage: the trim itself;
BOTH halves of its `full || i < src_len` guard; and `append_bytes` setting `full`
to stop the loop, which is what prevents a decoded escape from vanishing while
its successors survive (the trim cannot catch that — nothing ill-formed results).

`\xNN` and `\uXXXX` each decode to one complete code point and go through
`append_scalar`, so they are whole-or-nothing at the write step already.

**22. A property test derived from the implementation is a tautology (phase A,
2026-07-30).** The host harness that found decision 21 needed three attempts,
and the failures are worth more than the harness:

- **"Truncation is a PREFIX of the untruncated decode" is not enough.** With the
  bound broken, `E2 82` is a perfectly good prefix of `E2 82 AC` — so the
  invariant stayed silent on the split-sequence bug and fired only on
  vanish-and-continue.
- **The second attempt instrumented the decoder** to record the offset at which
  it began each unit, then asserted truncation landed on one of them. Sabotaging
  the decoder to emit one-byte units made every offset a boundary, so the
  assertion could not fail — it compared the implementation against itself.
  **Both sabotages passed.** An invariant has to come from a SPEC, never from the
  code under test; this is `test_check(true, ...)` wearing a property test's
  clothes.
- **The version that works** uses UTF-8 character boundaries — an external spec —
  and claims them **unconditionally** on the output.

  It was first written SCOPED to a well-formed source, in order to excuse a
  `C3 5C 80` "false positive". That scoping was itself the error, and it hid
  decision 21's general case for a full round: the reader's pass-through contract
  covers bytes AXL was HANDED, not a sequence AXL assembled and then split, so
  there was nothing to excuse. **Narrowing an invariant until it passes is the
  same mistake as writing one that cannot fail** — it merely looks more
  principled. Widening it back reproduced the defect in one run.

1.15M (input, buffer-size) pairs, valgrind clean, P4 claimed unconditionally and
holding on 221455 of them, and every sabotage caught by the invariant that should
catch it. The harness lives in the session scratchpad, not the tree — the QEMU
suite carries the cases it found.

**Consequence for the guarantee, stated once:** what AXL DECODES is always
well-formed UTF-8 and never an interior NUL; **raw source bytes are passed
through unvalidated.** The README had claimed the unscoped version — "an
accessor never hands back ill-formed UTF-8" — which the public header
contradicted and which a raw UTF-8-encoded lone surrogate disproves in one line.
That is the README-contradicts-implementation trap, caught in review rather than
in the field.

**23. `AxlJsonWriteFn` returns a COUNT, not a bool (P10, 2026-07-30).** The
snippet below originally had `bool`, and the header written from it kept it
through two review passes. It cannot work. Three of this design's own
statements are jointly unsatisfiable with a bool:

- a buffer sink that is full must NOT fail (or the sticky error halts the
  writer at the first fragment over, and `needed` reports that fragment
  instead of the document);
- `axl_json_writer_written()` is "for a buffer sink, how much of the buffer
  was filled";
- `axl_json_writer_finish()` "reports `AXL_JSON_ERR_IO` once if anything was
  dropped".

A sink that never fails on capacity, and whose only channel back is
false-means-broken, cannot tell the writer it dropped anything -- so `written`
could only ever equal `needed` and the truncation report could never fire. The
counters being on the writer is what makes this a contract hole rather than an
implementation detail: `ctx` is opaque, so the writer cannot read
`AxlJsonBufSink.used` behind the sink's back.

Fixed by giving the write function the same shape the read function already
has, for the same stated reason -- **there are three outcomes and a bool holds
two**: `len` took everything, a short count dropped the rest, `-1` is broken.
`write(2)` has had this signature for four decades. The alternative considered
was a third `size_t *accepted` field on `AxlJsonSink` pointing into
caller-owned state; rejected because it keeps two ways to count, adds a field
one built-in uses, and breaks the mirror with `AxlJsonReadFn` that the rest of
this section is built on.

Worth noting WHERE the reviews stopped: both read the sink as a descriptor and
checked it against the copy-by-value contract, which is what the previous
draft got wrong. Neither traced a byte from a full buffer back to
`finish()`. A contract review confirms the parts are consistent with each
other; only following a value end to end shows a promise with no mechanism.

**24. A stream sink maps SHORT to "full", not to "broken" (P10 follow-up,
2026-08-01, `1270a5ee`).** The sink shipped issuing one `axl_write` and
reporting any short count as -1. That was correct against the closed set of
four stream backends AXL had then -- none of them short-transfers -- and
became wrong the moment `main`'s `axl_stream_open_custom` made short
transfers contractually legal for anyone's backend. A socket handing back a
partial write was being called a malfunction, and the document truncated at
the first fragment it did not swallow whole.

Both halves of the fix come from AxlStream's own contract rather than being
invented here: `axl_fwrite` is the layer documented to LOOP until the request
is satisfied ("short transfers are handled above the backend"), and
`axl_ferror()` splits what is left -- set means the backend failed (-1, halt),
clear means it took nothing and retrying under this call could only spin
("full", a short count the writer keeps counting and `finish` reports once).

A stream with NO write slot is neither, and the write path cannot tell:
`axl_write` answers -1 at its own NULL-slot guard without setting the error
flag, so `axl_ferror()` stays false and it is indistinguishable from a backend
that took nothing. `axl_json_sink_init_stream` therefore asks
`axl_stream_can_write()` up front and refuses with `INVALID_ARGUMENT` -- a
caller bug named as one, and the same refusal every other malformed sink gets.

This is also what closed P10's one shipped coverage hole. "No public API
builds a stream that takes some bytes and then stops" was true when written
and had already stopped being true; a custom backend with a one-byte bite is
six lines of test. The bite is ONE byte, not four: four passed against the
UNFIXED sink, because the writer's longest fragment is the 4-byte literal
`true` and nothing straddled.

**25. `axl_json_get_object` must not write `out` before it type-checks (P11,
2026-08-02).** P11 rewrote every by-key `get_X` as `get_value` + `value_X` so
the two families cannot drift. For six of them that is exact. For
`get_object` it was not: `get_value` borrows as soon as the KEY exists,
whatever its type, so composing it straight into `out` overwrote the caller's
reader and only then discovered the value was a string. The family's
untouched-on-false promise exists to license seeding `out` with a default and
narrowing only if the section is present --

```c
AxlJsonReader cfg = root;
axl_json_get_object(&root, "tls", &cfg);   /* narrow only if present */
```

-- and the regression retargeted `cfg` at the `true` primitive instead of
leaving it at the root. Composed through a local now. The general rule, since
this will recur wherever a by-key accessor gains a value_* twin: **an
out-param that is itself a READER is written by the descent, not by the leaf,
so the type check has to happen before the caller's storage is touched.** The
scalar members are safe for the opposite reason -- only the leaf `value_X`
writes their out-param, and only on success.

**26. `AXL_JSON_COMPACT` is a no-op without `INDENT`, and that is correct
(P5, 2026-08-02).** The spec line reads "no space after `:` or `,`", which
presumes the default HAS them. Jansson's does -- it separates with `", "` and
`": "` and needs the flag to stop. AXL's never did: unindented output has
always been `{"a":1,"b":2}`.

So implementing the flag literally would have meant ADDING spaces to the
default first, just so a flag could remove them -- changing the output of every
existing caller and every exact-string test in the suite to buy nothing. The
flag instead strips the one space the INDENT path inserts, which is exactly the
combination Jansson documents it for (`JSON_INDENT | JSON_COMPACT`). Recorded
because "the flag does nothing on its own" reads like a bug until you know AXL's
default was already the compact one.

**27. `AXL_JSON_EMBED` is defined by an identity, not by prose (P5,
2026-08-02).** **Wrapping the embedded output in the delimiter it omitted
reproduces the unembedded output byte for byte.** One predicate, asked at the
moment the delimiter would be written -- `depth == 0`, which on open means "not
yet pushed" and on close means "already popped" -- and nothing else in the
writer changes.

The consequence to know: an indented document's leading and trailing newlines
SURVIVE, because they belong to the members rather than to the braces.
Suppressing them reads tidier and was the first instinct; it breaks the
identity, and with it composition -- a caller splicing the result between their
own braces would get output that no longer matches what the writer produces on
its own. This is the "an implementation that special-cases depth 0 will break"
warning in the P5 test plan, and the identity is what makes it a testable
property instead of a caution. Asserted across five indent settings rather than
one, since a special case would survive any single setting.

**28. `SORT_KEYS` orders by the DECODED key, and duplicates keep source order
(P6, 2026-08-02).** This document fixed the MECHANISM -- `AxlArray` plus
`axl_array_sort` -- and left the semantics open, so they are recorded here.

Ordering is byte-wise over the key's decoded name, shorter-first on a prefix.
Decoded, not the source spelling, because `token_equals` already defines a
key's identity as its decoded NAME: a key found by iteration feeds back into
the by-key accessors, and a sort that disagreed with that would be a second,
contradictory notion of what a key IS. It is also the visibly correct answer
-- `{"\u0062":1,"a":2}` sorted by source bytes stays put, because `\` (0x5C)
precedes `a` (0x61), so the document comes out looking untouched rather than
wrong. Case is not folded, because byte order does not fold it.

Duplicate keys keep the order the document listed them. `axl_array_sort` is
explicitly NOT stable, so the comparator tie-breaks on source token index,
making it a total order in which no two members compare equal. Reproducible
output is the entire point of the flag; leaving repeated keys to introsort's
pivoting would defeat it for exactly the documents where it matters most.

**That tie-break is invisible to any test small enough to read.**
`axl_qsort` runs pure insertion sort at or below `INSERTION_THRESHOLD` (16)
and insertion sort is stable, so a four-member duplicate case stays green with
the tie-break deleted -- a sabotage proved it. The regression test uses twenty
identical keys, past the threshold, where partitioning swaps equal elements
across the pivot.

The flag also makes `axl_json_write_token` ALLOCATE, which nothing else in the
writer does. Scoped so it costs nothing when unused: the unsorted path is
untouched, objects of fewer than two members return early, and only an object
with ESCAPED keys needs a decode block -- plain keys borrow their names from
the document. Allocation failure raises `AXL_JSON_ERR_NO_MEMORY`, and the
member walk that computes the subtree end runs BEFORE any allocation so a
failure can still return the index the caller needs.

**29. `REJECT_DUPLICATES` is a post-lex pass, and compares DECODED names
(P7, 2026-08-02).** A duplicate key is LEGAL JSON -- RFC 8259 §4 calls
repeated names "unpredictable", not invalid -- so this is a caller's stricter
policy rather than a grammar rule, and it runs over the finished token array
instead of inside the lexer. The lexer never has an object's members in hand
at once, and making it check would mean decoding every key on every parse:
work an unflagged parse must not pay for.

Comparison is by decoded name, so `{"\u0061":1,"a":2}` is a duplicate
though no two bytes of the two keys match. Anything else would let the check
be evaded by escaping one of the pair, and would contradict the by-key
accessors, which already find a key by its NAME. The set is per OBJECT, so
siblings may repeat each other and a child may reuse its parent's key.

Position is the SECOND key's first NAME byte -- inside the quotes when there
are any. Not the opening quote: a JSON5 unquoted key has none, and one rule
that holds for every spelling beats two that differ by a byte.

Detection is a SINGLE recursive descent, not a flat sweep over the token
array. The first version swept every token and re-derived each object's extent
independently, which re-visits a token once per enclosing object -- O(n *
depth). Review measured it: adding 250 wrapper levels to a 560 KB document
grew the input 0.5% and the work 4.8x. Recursing into each value as the member
walk advances visits every token exactly once.

Objects of `DUP_LINEAR_MAX` (8) members or fewer compare their keys LINEARLY
and allocate nothing -- an escape-free key borrows its name from the document,
as the writer's key sort already does. The hash table costs a struct plus a
64-bucket array whatever the member count, so building one per object made a
document of many SMALL objects -- the shape that dominates real JSON --
allocate once per object: review measured 960,017 allocations and 128 MB for a
2.2 MB document of two-member objects, against 17 allocations without the
flag. Above the threshold the quadratic term is what the hash set exists to
remove. Same shape and same reasoning as `INSERTION_THRESHOLD` in
`axl-sort.c`.

**The flag must fail CLOSED.** `axl_hash_table_add` returns false on
allocation failure WITHOUT taking ownership of the key, and the first version
discarded that return: an OOM then leaked the key and let the parse SUCCEED --
a strictness check reporting that a document it never finished examining is
clean, which is worse than not having the flag. Every allocation index is now
swept by a test asserting a duplicate document is never accepted.

**The member walk is what makes it correct, and nothing obvious tests that.**
Replacing the subtree skip with a two-token stride passed every case written
for this flag: stepping into a nested value happens to land on plausible
tokens. Two documents catch it and are the reason they exist --
`{"a":{"a":1},"b":2}`, which must PARSE (the stride sees the child's `a` as a
second member of the parent, a false duplicate), and
`{"x":[1,2],"a":3,"a":4}`, which must FAIL (the stride lands on an array
element, is not a string, gives up, and misses a real duplicate).

**30. The UTF-8 mode is consumed at DIFFERENT TIMES by the two sides, and on
read it splits by mode (P7, 2026-08-02).** This document said the mode is
"consumed lazily at `axl_json_get_string` time". That is right for two of the
three values and wrong for the third, so the rule is now per mode.

`UTF8_STRICT` asks whether the DOCUMENT is well-formed, so the reader settles
it at PARSE time: an ill-formed string token fails the parse with
`AXL_JSON_ERR_BAD_UTF8` at the first bad byte. Deciding it in an accessor was
the alternative and it cannot carry a position -- `axl_json_get_string`
returns `bool` over a `const` reader, so a caller could not tell ill-formed
from absent-key or buffer-too-small, and every other error in this library
carries offset, line and column. `REPAIR` and `RAW` only decide which bytes an
accessor hands back, so they stay lazy and never fail a parse.

STRICT checks RAW BYTES, not code points. Every byte of every escape sequence
is ASCII, so `"\ud800"` is well-formed input to this check and reaches the
decoder intact, becoming U+FFFD as under any mode. Making it a code-point
policy instead would have it reject documents whose SYNTAX is valid, which is
a different feature.

Ordered before the duplicate check, because an ill-formed byte makes a key's
decoded name meaningless and there is no sense reporting a duplicate derived
from one.

`REPAIR` and `RAW` landed after `STRICT` and are judged on the DECODED bytes,
which is the subtle half. JSON5 lets any byte be escaped, so one character can
arrive split across escapes and raw bytes -- `\<C3>\<A9>` is U+00E9 as two
separately-escaped bytes and `<C3>\<80>` is a raw lead with an escaped
continuation, both already pinned by `test_json_accessor_utf8_integrity`.
Repairing the SOURCE would see a lone lead in the second and destroy a
character the decoder assembles correctly, so the repair runs as a pass over
the decoded buffer instead -- where a well-formed result costs one scan and no
writes. Growth (one byte becomes three) obeys the decoder's existing
"refused WHOLE" rule rather than inventing a second truncation policy.

Because the mode is consumed lazily it had to be STORED: `AxlJsonReader` gained
`utf8_mode`, and a sub-reader and both iterators inherit it through the one
`ITER_BIND` macro. A view that decoded differently from the reader it came from
would be the same contradiction object iteration exposed once between key
lookup and key iteration.

`utf8_step` moved into `axl-json-internal.h` as a `static inline` because both
sides need it and it is a short leaf on a hot path -- NOT to preserve a link
edge. Review checked: `axl-json-build.o` has referenced `axl_json_decode_string`
and `axl_json_tok_subtree_end` from the reader since P6, and the build is
`-ffunction-sections` + `--gc-sections`, so gc granularity is a section rather
than an object file.

STRICT scans the WHOLE document, not its string tokens. A token walk was the
first shape and it missed JSON5 COMMENT bodies -- `skip_ws` walks those looking
only for the terminator, so arbitrary bytes survive there too, and the WRITER
already repairs comment bodies, so the two sides disagreed about the same
document. RFC 8259 §8.1 defines a JSON text as UTF-8, which is the unit the
question belongs to; everything outside a string or comment is ASCII by the
grammar, so the wider scan rejects nothing the narrow one accepted.

The field's fourth value is RESERVED and is now REFUSED with
`AXL_JSON_ERR_INVALID_ARGUMENT`. It is reachable by accident rather than by
inventing a constant: `AXL_JSON_RELAXED` already names `UTF8_RAW` (1), so the
natural `AXL_JSON_RELAXED | UTF8_STRICT` ORs to 3. Left undefined that silently
means "not STRICT" -- the feature disabled by the very act of asking for it.

**31. A container-scoped override REPLACES the per-value flags, and rides the
depth stack (P8, 2026-08-02).** Two things this document left open.
**The feature was removed on 2026-08-04 — decision 41.** What is recorded here
is why the replace-not-merge semantics made a merged opener impossible, which
is the part that outlived the code.

Replace, not merge. Passing `COMPACT` alone inside an `INDENT(2)` document
gives a fully compact subtree, which is what people reach for this to do;
inheriting the outer indent and only dropping the space after the colon would
be a strange default to have to opt OUT of. Everything outside the per-value
set -- the dialect bits above all -- is inherited untouched.

Anything unscopeable is an ERROR, not an ignored bit. `EMBED` and `SORT_KEYS`
are the two that look like they belong and do not, and this document already
said so; a dialect bit is refused on the same reasoning, since it is fixed for
the whole document, and so is the UTF-8 field's reserved fourth value --
`RAW | STRICT` ORs to it by accident, exactly as `RELAXED | STRICT` does on the
read side. Silently doing nothing is what would let a caller believe it worked.

`EMBED` is read when the depth-0 container OPENS and again when it CLOSES --
not at `axl_json_writer_finish()`, which never consults it. Three places said
otherwise until review checked.

The mechanism is one line in each of `push_container` and `pop_container`:
push records what was in effect OUTSIDE the container, pop restores it. Done on
every open rather than only a scoped one, so closing never has to ask whether
this level overrode anything -- which is what makes an unbalanced override
unrepresentable rather than merely unlikely.

The restore lands AFTER the dedent, which belongs to the closing container and
must use the width its own contents used. Its visible consequence: an
`INDENT(2)` subtree of an `INDENT(8)` document closes two spaces in, not eight.
A first draft of this paragraph also claimed the restore had to precede
`finish_value`; review sabotaged that and every assertion still passed, because
`finish_value` reads no flags. The claim is gone rather than kept as
decoration.

It cost `AxlJsonWriter` an `AxlJsonFlags[32]`. A packed per-depth record would
be a fifth of that and is not worth the encode/decode it would add to a struct
callers put on the stack once.

The writer's UTF-8 mode landed just before this, for this: the P8 test plan
asks for `UTF8_RAW` scoped to one value while a sibling stays repaired, and
that is unaskable while the writer ignores the field. It also fixed a real
asymmetry -- `AXL_JSON_RELAXED` names `UTF8_RAW`, so a document read and
written back under it could not round-trip its own bytes.

**32. The error formatter WINDOWS a long line, and builds its caret from the
source (P15, 2026-08-02).** Two things this document did not settle when it
split the renderer out of P9.

Minified JSON is ONE line, and it is the shape machines emit -- so "quote the
offending line" would either overflow any reasonable buffer or refuse outright
on exactly the documents most likely to fail a parse. A line longer than
`AXL_JSON_ERROR_QUOTE_MAX` is therefore windowed around the column, with `...`
marking each end that was cut and the caret moved to match.

The caret line is built by walking the quoted SOURCE, not by emitting
`column - 1` spaces. A TAB has to be copied through as a TAB or the caret lands
wherever the terminal expanded it to, and the walk has to advance by CHARACTER
because the column does. Those two interact: the cell COUNT is the column
either way, so walking bytes is only visible when a tab FOLLOWS a multi-byte
character -- the byte walk spends two steps inside the character and puts that
tab one cell late. Sabotage found this: the byte walk passed every other
assertion, and the case that catches it is `{"<C3><A9>":<TAB>1 2}`.

The message table is a `switch`, not an array indexed by the enum. A table's
failure mode for a code added later is an empty string -- a diagnostic that
renders blank, which is worse than none because it looks like the formatter
worked. A `switch` is a `-Wswitch` warning instead (verified: the trailing
`return` suppresses `-Wreturn-type`, not `-Wswitch`), and a test pins every
code's EXACT text. The first version of that test asked only that something
non-empty came back, which the fallthrough return satisfies for any code at
all -- so it would have passed for a code with no case, and swapping two
messages did not fail it either. Review caught both.

**The quote is SANITISED.** The document is untrusted and this text is written
to a console, in a library that parses JSON off the network and ships a VT
stack. A raw ESC carries an ANSI sequence out of a JSON body; a raw CR is a
correctness problem as much as an injection one, returning the cursor to column
0 and wrecking the quote and the caret together; an embedded NUL would end the
buffer early, so the returned length would exceed what the caller can read.
TAB survives because the caret line mirrors it; every other C0 byte and DEL
become `?`, one byte out per byte in so the column arithmetic is untouched.

`AXL_JSON_ERR_DIALECT` renders its `missing_flag`. It is the only recoverable
code in the enum, and the whole reason the record carries a fifth field; the
formatter is the tool the flag exists for, so dropping it delivered the half a
caller cannot act on.

`AXL_JSON_ERROR_BUF_MAX` exists because the function REFUSES rather than
truncating: without a stated bound a caller has no way to pick a size that
cannot fail, and the widest render -- a full window of 4-byte characters -- is
well past the 256 the first worked example used. On refusal the buffer is left
EMPTY rather than partial, unlike its two siblings: the caller is already on an
error path and the obvious thing to do with the buffer is print it.

`AXL_JSON_OK` renders as `no error` with NO position, because `0:0` names a
place that does not exist. Too small a buffer is REFUSED rather than truncated,
matching `axl_json_escape_string` and `axl_json_decode_string`: a half-written
diagnostic can point the caret at the wrong column.

**33. `get_double` demands the WHOLE token, and refuses a range error rather
than returning the IEEE result (P14, 2026-08-02).** The two seams this document
flagged, settled.

Whole-token, which is what separates it from `axl_json_get_int`: that one
truncates at the first non-digit, so `1.5` yields 1. There is no sensible
prefix of a float, and JSON5's hex literal makes it concrete -- `0x1F` would
parse as 0 and stop at the `x`, handing back a number the document does not
contain. `get_int` still reads hex, so the refusal costs a caller nothing.

Range errors are refused. `axl_str_to_double` reports overflow as ±infinity and
underflow as ±0.0 -- the correct IEEE answers -- together with `AXL_ERR`, unlike
the integer members of its family. Passing that through would make `double` the
one accessor where `true` does not mean "you got the number in the document".
Parsing into a LOCAL is what keeps the IEEE value out of the caller's variable,
so `1e400` leaves it untouched exactly as an out-of-range integer does.

The `NaN`/`Infinity` seam turned out to be a non-issue: `axl_str_to_double`
matches case-insensitively and tries `infinity` before `inf`, so both JSON5
spellings are consumed whole. Checked rather than assumed -- had it stopped at
`inf`, the whole-token rule would have rejected `Infinity` outright.

**34. The writer pairs an escape introducer with a whole CHARACTER, always
(2026-08-02).** Two defects, one cause, both found by `test/fuzz/json_fuzz`'s
representation oracle rather than by review or by any test.

`wr_str_utf8` splices token bytes in SOURCE form, so a backslash and what it
escapes have to travel together -- otherwise an existing `\\/` becomes `\\\\/`.
That rule was there, but it was gated on `ESCAPE_SLASH` and it counted BYTES.
Both were wrong, and only in combination with JSON5's `ALLOW_EXTRA_ESCAPES`,
where the payload of `\\<anychar>` may be a raw multi-byte character:

- **Without `ESCAPE_SLASH`**, the pair was not recognised at all, so
  `ENSURE_ASCII` emitted the introducer as a literal backslash and then
  re-encoded the payload separately: `\\<char>` became `\\\\uXXXX`, which decodes
  to a backslash followed by the TEXT `uXXXX`. A different value.
- **With `ESCAPE_SLASH`**, the two-byte pairing cut a 4-byte sequence after
  its lead byte. The three orphaned continuation bytes each repaired to
  U+FFFD, and the lead byte went out RAW -- breaking the one guarantee
  `ENSURE_ASCII` makes.

The rule is now unconditional and character-wise. An ASCII payload still
travels verbatim (every RFC 8259 escape, and `\\uXXXX`, are pure ASCII). A
non-ASCII payload goes through the normal unit path, and under `ENSURE_ASCII`
the introducer is DROPPED, because the `\\uXXXX` that replaces it is
self-contained -- keeping it would escape the backslash instead of the
character.

Why no test caught it: the splice test used `\\u00e9`, an escape made entirely
of ASCII, which survives by accident because the writer copies an ASCII run
verbatim. The gap was a payload that is BOTH escaped and non-ASCII, and it
took an oracle -- "the two spellings of a value must decode alike" -- rather
than another exact-string assertion, because the wrong output was still
well-formed JSON that re-parsed happily.

**35. SORT_KEYS orders by the name the key will actually CARRY, which under
UTF8_REPAIR is not the source bytes (2026-08-02).** The flag's docstring
promises order over the key's DECODED name, and `write_object_sorted` decoded
a key only when it contained a backslash. An ill-formed byte is not an escape,
so a key like `"\xFF"` borrowed its raw source bytes as the sort name -- while
being EMITTED with UTF-8 repair applied, as U+FFFD.

Those two disagree, and not subtly: 0xFF sorts AFTER 0xF0 as a raw byte, but
its repaired name EF BF BD sorts BEFORE it. So sorting the writer's own output
produced a DIFFERENT order than the first pass did. Reproducible output is the
entire point of the flag, and it was not reproducible on any document with an
ill-formed key.

The gate is "escaped, OR ILL-FORMED where repair will apply". Repair applies
under UTF8_REPAIR and ALSO under ENSURE_ASCII whatever the mode, since
`\uXXXX` needs a code point and an invalid byte has none -- so
`UTF8_RAW | ENSURE_ASCII` repairs too, and an earlier version of this decision
claiming RAW is always safe to borrow was wrong by one flag. UTF8_RAW ALONE
emits the raw bytes, so borrowing is correct there and a test pins it.
Well-formed non-ASCII keys also still borrow: repair does not touch them, so
there is nothing to materialize.

The name itself comes from `axl_json_decode_key_name()` -- the reader's own
key decoder, exported for this. That is what makes "the writer's name and the
reader's name are the same" true by construction rather than by assertion. The
first cut instead decoded here and ran a private repair walk over the result,
which was a third copy of repair logic AND sized its buffer for a decode that
never grows: JSON5's two-byte `\0` names a three-byte U+FFFD, so SORT_KEYS
began REFUSING documents it had written fine the day before. Caught by review,
not by the fuzzer -- `round_trip` returns early on a writer error by design, so
a new writer error is exactly the shape its oracle cannot see.

Found by `test/fuzz/json_fuzz`'s round-trip oracle, which asserts exactly the
property that broke: serialize, re-read, serialize again, and require the two
to match. Note it is invisible to an exact-string test, because any single
run's output looks perfectly reasonable -- it is only the SECOND pass that
disagrees.

Implementation note worth keeping: `axl_json_utf8_step` documents that its
callers screen `< 0x80` themselves, and handing it an ASCII byte gets that byte
reported INVALID. The first cut of the repair walk did not screen, which turned
every decoded key into U+FFFD -- caught because an existing ordering test went
red, not because anything about the new path looked wrong.

**37. The writer's depth cap is lifted to 256 INLINE, at ~2 KB of struct
(P12f, 2026-08-02).** The reader accepted 256 levels and the writer stopped at
32, so a document between the two parsed cleanly and could not be written back
-- and read-then-re-emit is most of what the writer is for.

This document's own line was "the writer's hard 32-level cap can be lifted the
same way", meaning the array-vs-object bitmap. That is true and it is 32 bytes.
It is not the binding constraint: `saved_flags` holds one flags word per level
for P8's container-scoped overrides, so 256 levels is 2 KB and `AxlJsonWriter`
grows from 344 bytes to ~2160.

Three options were real, and the choice was the user's because it is an ABI
change to a caller-placed struct in a shipped SDK:

- **inline 256** -- 2 KB of struct, allocation-free, symmetric immediately.
- **heap `saved_flags` sized to the resolved depth** -- keeps the struct small
  and follows the precedent this document already set for the reader's builder
  stack, but makes the writer allocate, which falsifies "SORT_KEYS is the only
  part of the writer that allocates".
- **64 levels** -- half the cost, and leaves the asymmetry in place.

**Inline 256 was chosen.** The writer cannot then fail for want of memory while
serializing a deeply nested document, which on firmware is worth more than the
bytes -- and a 2 KB caller-placed struct is well inside what decision 6
accepted at the time (~37 KB of recursive stack for the same 256-level bound,
since retired by P12e).

**40. Network JSON is STRICT in both directions; the liberal dialect is for
files AXL reads locally (2026-08-03, Mike's call).** The flag space made both
dialects equally reachable, and that left the choice to whoever wrote each call
site. The rule:

- **Anything crossing the network is `AXL_JSON_STRICT`** — HTTP request and
  response bodies, WebSocket payloads, Redfish resources — parsed strict and
  emitted as RFC 8259. A peer sending JSON5 is broken or probing.
- **The liberal dialect is for local files**: the JSON5 sidecars in this tree
  (pci.ids, usb.ids, JEDEC vendors), config files, and anything a consumer
  deliberately opts into. Hand-edited, so comments and trailing commas earn
  their place.

`axl_http_request_get_json()` already applied this on the server side and says why.
The gaps were on the READ side of responses, where there is no equivalent
helper and every caller reaches for the liberal `axl_json_parse()`: `rfbrowse`
parsed Redfish error bodies and member listings from a remote BMC that way, and
`netload`'s `--json` self-check parsed its own output that way -- which made
the check weaker than it looked, since a writer regression that started
emitting JSON5 would have been accepted by the reader verifying it.

**The docstring was actively arguing the wrong way**, naming "an API response
it does not control" as a case FOR the liberal form. Not controlling it is the
argument for strict. That sentence had already reached two tools, which is the
argument for writing the rule down rather than leaving it to taste.

**The structural gap is CLOSED**: `axl_http_response_get_json()` mirrors
`axl_http_request_get_json()`, so on a response the strict parse is what a
caller gets rather than what they must remember. That matters more than the
call-site fixes, because all three violations were written by people reaching
for the obvious `axl_json_parse(resp->body, ...)` -- there was no other easy
option. A rule enforced by review gets re-broken; a rule enforced by the shape
of the API does not. `rfbrowse` now uses it rather than spelling the flags out.

The request side's own strictness was ALSO unpinned until now: its only
negative fixture was `{not-json`, which every dialect refuses, so nothing
distinguished strict from liberal. Both helpers now have a JSON5 row, and that
row is the one sabotage kills.

**39. P13 needed no resumable sub-token machine, and the reserved `uint16_t
state` stays inter-token (2026-08-03).** This document said twice that P12's
scope was "explicit container stack **plus** an explicit sub-token state
machine" and that P13 depended on both. Re-read against the event contract this
same document specifies, the second half does not follow: `ev.text`/`ev.len` is
a BORROWED CONTIGUOUS span of a whole token, so the window has to hold the
entire token however the scan is structured. A machine that could suspend
mid-token still could not emit one.

So a token straddling a refill is **re-scanned from its start** instead --
.NET's `Utf8JsonReader` states exactly this rule, expat implements it, and
Jackson accumulates into a `TextBuffer`, which amounts to the same bound. Only
yajl has a true intra-token machine, and it can because it is PUSH-based and
still accumulates. The payoff is that the five leaf scanners are shared between
the two modes rather than forked, which after P12e is the difference between
one grammar and two.

**What the re-run strategy does NOT remove is the SIGNAL.** Both cited prior
arts carry one (`isFinalBlock`, `isFinal`) and the first draft here omitted it,
which a contract review caught before any test existed. Three leaves treat
running out of bytes as a legitimate END of token -- a number, an identifier
and a line comment all end at end of input -- so a window cut after `{"n":12`
emitted the number 12 and then met `3}` as a fresh value. `Lexer::at_eof` is
that signal, and it appears in two places for two different reasons:

- explicit guards on the SUCCESS paths, where a leaf would otherwise return a
  truncated token;
- one rule in `lex_fail`, where a failure raised with the cursor at the window
  end is reclassified as `INCOMPLETE`. That half was missed by the first
  implementation and found by an over-window test: `65.65e` cut by the window
  is an empty exponent, `BAD_NUMBER` is not retryable, and a 7 KB document
  stopped dead at byte 1018.

Both are inert over a contiguous source, where `at_eof` is always true -- which
is what makes "the same bytes produce the same events at any chunking" a
property rather than a hope.

**Four more defects came out of the pre-commit review, and every one of them
was invisible to a contiguous scan.** Recorded because they are the shape of
thing this mode gets wrong, not because the list is interesting:

- A source carrying BOTH a view and a read function is the VIEW. Left
  ambiguous the window path refilled against a buffer the contiguous path
  never allocated -- a NULL dereference, and where it survived it scanned the
  document twice.
- **Latching end-of-input counts as PROGRESS** even when no byte arrives. The
  read loop stops at a full window without asking again, so an input ending
  exactly on a boundary had not latched yet; the next refill latched, added
  nothing, reported "no progress", and the scan gave up holding an
  `INCOMPLETE` one re-run away from `OK`. A 1024-digit number produced no
  events at all.
- `parse_number` needed `parse_literal`'s settle-the-word guard. Without it
  `-Infinity` cut by a refill is a terminal `BAD_NUMBER`, so a valid JSON5
  document failed purely because of where the chunks fell.
- `scan_fail` now takes an INPUT-relative offset, like `scan_emit`. Five sites
  in `scan_value`/`scan_key` already held an absolute one and got the base
  added twice; `scan_line_col` then clamped the over-range position and
  counted the whole window instead of the prefix.

**Settled whitespace is DROPPED, not re-scanned.** `skip_ws` reports where a
refill may safely resume -- past the whitespace it has already consumed, but
anchored at a comment's opening `/`, because resuming inside a comment would
read its body as JSON. Without this the window doubled until a whole run of
whitespace fit, which an adversarial stream can drive at no cost to itself:
200 KB of spaces reached a 256 KB window. A comment still grows it, which is
the carve-out stated above; whitespace no longer does.

**The bound is O(largest single token)**, not O(document): a million-element
array costs nothing per element. A COMMENT counts as a token, because it is
scanned as one unit, so a 10 MB block comment grows the window to 10 MB.
Streaming past it is the one thing the abandoned machine would have bought, and
it was judged not worth forking the grammar for.

**38. The whole-document face is a scan loop, and the recursive parser was
DELETED rather than kept beside it (2026-08-03, P12e).** `axl_json_parse`
now runs `axl_json_scanner_next()` to `EV_EOF` and appends a token per event.
Three things it has to supply that the event stream does not carry, and each is
a place the two faces could have silently diverged:

- **`start`, `end`, `size`.** Leaf bounds come from `ev.text`/`ev.len`, NOT
  `ev.offset` -- for a quoted string those differ, since `[start, end)` brackets
  the inner content while `offset` points at the opening quote. Pointer
  arithmetic off `text` reproduces the string convention, the container
  convention and a JSON5 unquoted key without the builder knowing which it has.
  A container's `start` is its `BEGIN.offset` and its `end` is
  `END.offset + 1`; `size` is counted -- KEYS for an object, ELEMENTS for an
  array, which is why an object counts on `EV_KEY` and an array on the value.
- **A builder stack**, `{ int32_t idx; int32_t count; }` per open level, sized
  to the resolved depth limit. Indices, not `AxlJsonTok` pointers: the token
  array doubles as it grows, so a pointer captured when a container opened
  dangles the moment a child crosses a boundary. A 20-token fixture with a
  container open across the growth pins that; every other fixture in the suite
  stays under 16 tokens and would not.
- **The trailing region.** The scanner reports a document BOUNDARY and judges
  nothing after it, which is what lets it also serve NDJSON, so "was one
  document all there was?" is asked here -- through the same `skip_ws` leaf, so
  a legal trailing comment stays legal and an unterminated one is still that
  leaf's error rather than `TRAILING`. Symmetrically, the scanner's clean
  exhaustion means "end of stream" and this face must turn it into `INCOMPLETE`
  while still CONSULTING it, or a comment before the root stops naming its
  flag.

The old parser is gone -- 440 lines, `parse_value`/`parse_object`/`parse_array`
plus `alloc_tok`, `enter_container` and `lex_finish_error`. Keeping it would
have meant two walks of one grammar reconciled by hand, which is exactly the
arrangement the jsmn split cost us in P3; a 4.1M-case differential (P12d) is
what made deleting it safe rather than hopeful, and `Lexer` shrank to a cursor
plus a dialect.

**36. A single-quoted JSON5 token must be RE-ESCAPED when spliced into a
double-quoted one (2026-08-02).** `axl_json_write_token` re-quotes every string
with `"` and splices the token's bytes verbatim. Inside JSON5 single quotes a
`"` needs no escape, so a document like `{a:'v"w'}` came back out as
`{"a":"v"w"}` -- not JSON in any dialect, and the writer reported no error.
Keys had it too: `{'k"x':1}` produced `{"k"x":1}`.

The mirror case is `\'`, an escape that is legal only inside single quotes.
Spliced into a double-quoted string it is an unknown escape, which strict JSON
rejects -- so converting a JSON5 document to strict JSON, the main reason to
re-emit at all, produced something unreadable.

Both are fixed in `wr_str_utf8`, which is the single point both the value
splice and `key_raw` pass through: an unescaped `"` is escaped, and `\'`
drops the escape it no longer needs. The `"` rewrite cannot touch a
double-quoted source -- the lexer would have ENDED the string at an unescaped
`"`, so the byte cannot appear in such a token. The `\'` rewrite CAN reach one
(`{"a":"v\'w"}` is a valid JSON5 document), and that is fine: dropping the
escape preserves the character either way.

**Scope, stated so it is not mistaken for more.** This makes a re-emitted
STRING valid between double quotes. It does not make JSON5 -> strict JSON
conversion work in general: `\x41`, `\v`, `\0`, line continuations, a raw TAB,
and every JSON5 PRIMITIVE (`NaN`, `Infinity`, `0x1f`, `+1`, `.5`) still splice
through verbatim and a strict reader still rejects them. A real converter
would have to re-encode primitives too, which is a bigger change than this
one and is not attempted here.

Found by `test/fuzz/json_fuzz`'s round-trip oracle -- specifically its first
property, that the writer's output must satisfy the writer's own reader. No
exact-string test would have found it, because nothing in the suite re-emitted
a single-quoted document.

**The stated demand for this phase did not survive a grep, and the doc has been
corrected.** It claimed `tools/rfbrowse.c` reads Redfish today; rfbrowse uses
three JSON accessors and all three are `axl_json_get_string`. The Redfish float
fields this document named appear nowhere in the tree. The honest argument is
an API asymmetry -- every other scalar has a `get_X`/`value_X` pair, and
`double` was the only type whose caller had to fetch `get_number_str` and parse
it by hand, in a library that ships a correctly-rounded decimal parser.

`AxlJsonType` still collapses integral and fractional numbers, but its stated
REASON expired here: it was "a REAL type would name something no accessor could
return". Kept on its own merits instead -- JSON has ONE number type, the split
is about how a literal was spelled, and `get_number_str` already hands back the
spelling for a caller who needs to tell them apart.

**41. The `_flags` twins are FOLDED into their base signatures, and P8's
container-scoped overrides are DELETED (2026-08-04, Mike's call).** When a
function needs a new parameter, add the parameter. The twins existed only
because an earlier session declined to change a shipped signature -- a
constraint Mike does not have, owning every consumer -- and the cost was two
ways to do one thing plus a default that meant "the parameter would not fit".

Net: -6 public functions, 3 renames, no new constants.

- `axl_json_parse(json, len, flags, r)` and
  `axl_json_load_file(path, flags, r, out_buf, out_len)` take the flags word;
  the `_flags` twins are gone. Flags before the out-param, matching
  `axl_json_parse_source` and `axl_json_scanner_init`.
- `axl_json_indent_flags` -> `axl_json_indent`,
  `axl_json_depth_flags` -> `axl_json_depth`,
  `axl_json_type_of` -> `axl_json_get_type`.
- `axl_json_root_array_begin` and `axl_json_extract_string` deleted; their
  jobs are `axl_json_value_array_begin` and `axl_json_parse` +
  `axl_json_get_string`.

**The arity change IS the migration mechanism, and it had to be.**
`AXL_JSON_STRICT` is `0` while the no-flags `axl_json_parse` defaulted to
`AXL_JSON_RELAXED`, so "add a `0`" compiles clean and silently flips 77 sites
from liberal to strict. Every migrated site got `AXL_JSON_RELAXED` spelled out,
preserving behaviour exactly; auditing which of them should actually be strict
is a separate pass, deliberately, so ~250 mechanical edits and 77 semantic
ones are not indistinguishable in one diff.

**P8 is deleted rather than merged**, and the distinction matters. A merged
`axl_json_obj_begin(w, flags)` cannot use `0` for "no override", because
decision 31 made the override REPLACE rather than merge and a mode not named
takes its ZERO value -- so an override silent about UTF-8 puts the subtree in
`UTF8_REPAIR` even inside a `UTF8_RAW` document. Merging therefore needs a
sentinel or a pointer form, on ~250 call sites of the one-argument openers
(118 in this tree, 130 in the largest consumer, measured 2026-08-04), every one
of which would be saying "ignore this". Both shapes were designed
before anyone counted the callers: every call site of the scoped openers in
every tree that builds against this SDK was the definition itself or a unit
test for it. The feature had no consumer.

Deleting it also retires `saved_flags` -- decision 37's 2 KB, chosen inline
precisely so the writer could not fail for want of memory while serializing.
`AxlJsonWriter` goes back to ~344 bytes and `push_container`/`pop_container`
lose the save/restore, because with no override there is nothing that can
differ from the flags already in `w->flags`. The `set_flags` shape decision 31
rejected is still rejected, for the reason it always was.

**42. The strict-vs-relaxed audit, run 2026-08-04, and what it found.**
Decision 41 deferred this deliberately so ~250 mechanical edits and the
semantic ones would not be indistinguishable in one diff. Run afterwards, on
the now-explicit call sites.

**Production code was already compliant, everywhere.** Every parse in `src/`
and `tools/` already named its dialect before the fold: JOSE `STRICT`, both
HTTP body helpers `STRICT`, `rfbrowse` `STRICT`, `netload`'s self-check
`STRICT`, and `axl-sidecar.c` `JSON5`. Zero library call sites changed
behaviour, because the 77 sites relying on the liberal default were tests,
docs and the SDK example (76 in `test/unit/axl-test-data.c`, 1 in
`sdk/examples/json.c`).

**The figure was `~97` until a review re-measured it.** That came from the
handoff doc's grep census, which labelled its own column *"Upper bounds — some
hits are comments or doc snippets"* — and it was carried into this record
unqualified, in a decision whose next sentence says "measured, not assumed."
The honest count is what the migration script actually rewrote: sites with the
old arity, 73 `parse` + 3 `load_file` in the unit suite and 1 in the example.
A grep hit count is not a call-site count, and the difference is exactly the
comments and declarations a decision record should never be quoting. The rule held -- by review.

**So the finding is not a violation, it is the absence of a gate.** This tree
wrote decision 40 and enforced it nowhere; SoftBMC, a CONSUMER, built a source
lint for it after a mutation build showed a JSON5 `PUT /api/users/admin`
demoting the administrator account. `make check-json-dialect` closes that:
every `axl_json_parse` / `load_file` / `parse_source` / `scanner_init` under
`src/` and `tools/` must pass `AXL_JSON_STRICT` unless the line above carries
`/* json-dialect: local-file -- <why> */`. There is exactly one exception in
the tree, the sidecar loader, which is the intended shape -- liberal parsing
concentrated in one helper everything else goes through.

The gate excludes `src/data/axl-json-*.c` by PATH, not by "the flags argument
is a variable". The JSON module forwards its caller's flags word and has no
dialect of its own, but exempting variables generally would be a loophole:
assigning `AXL_JSON_RELAXED` to a local first would dodge the check from
anywhere. Sabotage confirms all three arms fire -- a network read turned
liberal, a dialect laundered through a variable, and a removed justification.

**The test suite's 84 `RELAXED` sites were measured, not assumed, and left
alone.** Flipping every one to `STRICT` fails 47, and all 47 are genuine
JSON5-feature tests: `\xNN` and `\0` escapes, comments, unquoted and
single-quoted keys, plus the tests whose subject IS the `RELAXED` preset. The
other 37 are dialect-agnostic fixtures -- plain RFC 8259 documents where the
spelling changes nothing about what is asserted. Converting them would buy
strict-path coverage this suite already has 274 `STRICT` sites and a whole
JSONTestSuite binary for, at the cost of 37 edits to UTF-8-sensitive
assertions in a 15k-line file (`RELAXED` names `UTF8_RAW`; `STRICT` means
`REPAIR`). Measured, judged not worth it, and recorded here so the next person
does not re-measure it.

**Twelve comments were arguing from the deleted default** -- "STRICT, not the
liberal `axl_json_parse()`" and similar, in `axl-jose.c`, `axl-http-client.c`,
`axl-http-request.c`, `rfbrowse.c`, three unit-test files and this document.
Each named a hazard that no longer exists, which is worse than saying nothing:
a reader checks the claim, finds `axl_json_parse` has no default at all, and
learns to distrust the comment rather than the API. Corrected to argue from
the DECISION, which is what survived. `sdk/examples/json.c` was the same
mistake in the place that teaches hardest -- it parsed a self-authored literal
with `RELAXED` -- and now names `STRICT` and states the rule.

## Four constraints worth stating explicitly

**1. `SORT_KEYS` cannot apply to the streaming writer.** AXL's writer is
streaming: the caller emits keys in its own order and nothing is buffered.
Sorting requires holding a whole object in memory, which would defeat the
design. `SORT_KEYS` is therefore scoped to `axl_json_write_token`, the
parse→write bridge, where the full token tree already exists. Its
docstring must say so; passing it to a streaming write is a documented
no-op. Buffering the streaming writer to support it is explicitly rejected.

**2. `ENSURE_ASCII` requires surrogate-pair handling.** Commit `6ee757cd`
deliberately did not emit `\uXXXX`, precisely because non-BMP code points
need surrogate pairs and there was no gain at the time. This flag makes
that work mandatory: U+10000 and above must be emitted as a
`\uD800`-`\uDC00` style pair. It is the fiddliest piece of the change and
needs its own exact-string tests at the BMP boundary.

**3. `ENSURE_ASCII` and the UTF-8 mode must define their interaction.**
Escaping to `\uXXXX` requires a code point, and `UTF8_RAW` is precisely
the mode in which there may not be one. The rule, matching yyjson's
`ESCAPE_UNICODE` + `ALLOW_INVALID_UNICODE` behavior:

| UTF-8 mode | + `ENSURE_ASCII` |
|---|---|
| `REPAIR` | ill-formed becomes U+FFFD, then escapes as `\ufffd` |
| `RAW` | an ill-formed byte has no code point to escape, so it escapes as `\ufffd` too — `RAW` cannot survive `ENSURE_ASCII`, and the docstring must say so |
| `STRICT` | ill-formed sets the error; nothing is emitted |

The `RAW` + `ENSURE_ASCII` row is the surprising one: the two flags are in
direct conflict and `ENSURE_ASCII` wins. Left undefined, it would be
discovered as a bug.

**4. `ALLOW_NAN_INF` does not mean IEEE support — but the reason is narrower
than this document claimed until 2026-07-29.**

The original wording said AXL "has no double accessor and no real emission."
**That is false, and the design review caught it.** AXL has:

- `axl_dtoa()` — PUBLIC, in `include/axl/axl-format.h`, a Grisu2 shortest-round-trip
  double-to-decimal converter, documented as "the primitive a consumer needs to
  serialize a double without losing precision"
- `%f` / `%e` / `%g` in the format engine (`src/format/axl-format.c`), i.e. real
  emission that already exists
- `AxlMath` (`axl_sqrt`, `axl_sin`, ...) and `double` in 18 public headers,
  `axl-json.h` among them

"No `libm`" is true and deliberate; "no floating point" was never true.

The real reason NaN/Infinity are text-only is simply that **AXL JSON has no
`axl_json_get_double` yet** — not that a double is unrepresentable. So they are
lexed as primitive tokens, retrievable via `axl_json_get_number_str`, and
`axl_json_get_int` / `_get_uint` reject them because there is no integer they
could mean.

Consequences for the deferred list, both corrected below: `JSON_REAL_PRECISION`
is **deferred, not n/a** — it lands with real support, and the write side is
nearly free on top of `axl_dtoa`. The genuinely hard half is the READ side:
correctly-rounded decimal-to-double has no `strtod` equivalent in `axl-str*.c`
and is a well-studied hard problem (Clinger, Eisel-Lemire). That, not the
writer, is what `axl_json_get_double` actually costs — and it is why
`get_number_str` shipping first was the right order: it makes every value
reachable losslessly without anyone having to write a float parser.

`src/data/README.md` listed NaN/Infinity as unsupported; P4 rewrote that section
to describe the new, narrower meaning.

## Deferred: `AXL_JSON_EXTENDED`

Type-preserving wrappers (MongoDB's `$numberLong` / `$binary`) are
deferred; the bit is reserved so the flag space does not have to be
reshuffled later.

Rationale: the read side is already covered by
`axl_json_get_number_str`, and MongoDB's spec has **no unsigned 64-bit
type** — `$numberLong` is int64 — which is exactly the case AXL cares
about (masks, physical addresses). Adopting it would mean either
misusing `$numberLong` or inventing `$numberULong`, and neither is worth
doing before a consumer actually needs it. The real motivation when it
arrives will be JavaScript consumers, which lose precision on bare 64-bit
JSON numbers.

## Compatibility: clean break

`AxlJsonParserFlags`, `AxlJsonWriterFlags`, `AXL_JSON_PARSER_DEFAULT`,
`AXL_JSON_PARSER_JSON5`, `AXL_JSON_WRITER_DEFAULT`,
`AXL_JSON_WRITER_PRETTY` and `AXL_JSON_WRITER_TRAILING_COMMAS` are
**removed**, not aliased. No deprecation period.

Both entry points change type:

```c
void axl_json_writer_init(AxlJsonWriter *w, AxlString *out, AxlJsonFlags flags);
bool axl_json_parse(const char *json, size_t len,
                    AxlJsonFlags flags, AxlJsonReader *r);
```

Migration:

| Old | New |
|---|---|
| `AXL_JSON_PARSER_JSON5` | `AXL_JSON_JSON5` |
| `AXL_JSON_WRITER_PRETTY` | `AXL_JSON_INDENT(2)` |
| `AXL_JSON_WRITER_TRAILING_COMMAS` | `AXL_JSON_ALLOW_TRAILING_COMMA` |
| `AXL_JSON_*_DEFAULT` | `AXL_JSON_STRICT` |

In-tree consumers, verified by grepping the five removed constants
(`tools/lsproto.c` passes a literal `0` and so does not appear):

| File | Uses |
|---|---|
| `test/unit/axl-test-data.c` | 39 |
| `src/data/axl-sidecar.c` | 2 (the single shared sidecar loader) |
| `src/data/axl-json-parse.c` | 2 |
| `src/data/axl-json-build.c` | 2 |
| `sdk/examples/json.c` | 2 |
| `tools/rfbrowse.c`, `tools/netload.c`, `src/net/axl-jose.c` | 1 each |
| `src/data/axl-json5-parse.c` | 1 |
| `experiments/axl-kernel/test/axlk-{hwinfo,bootconfig,reqlog}-server.c` | 1 each |

**The `experiments/` row was missing from the first version of this table**,
because the grep behind it was scoped to `src/ tools/ sdk/ test/ include/`.
Three `.PHONY` make targets stopped compiling and only the review pass caught
it. Scope completeness greps to the REPO, not to the directories you expect.

Out-of-tree: axl-webfs, softbmc, uefi-devkit, axl-utils.

## Two parsers, and what granular flags did to them

**HISTORICAL as of P3** — there is now one parser, `src/data/axl-json-lex.c`,
and `axl_json_parse` has no routing decision left to make. This section
is kept because it is why P2 and P3 have the shapes they do; read it in the
past tense.

Verified 2026-07-29, because it drove P2's shape:

- **Strict parsing ran on vendored jsmn** (`src/data/jsmn.h`). `jsmn_parse`
  was called twice in `axl-json-parse.c` (count pass, then fill pass).
- **JSON5 parsing ran on our own lexer** (then `axl-json5-parse.c`), a
  hand-written recursive-descent parser that emitted `jsmntok_t`-LAYOUT
  tokens so `AxlJsonReader` and every accessor worked unchanged. jsmn was not
  involved in that path beyond the token struct.

While the choice was binary this was invisible. Granular flags broke
that: "comments only" is neither strict nor full JSON5, and jsmn cannot
parse a comment at all. So P2 needed an explicit routing rule:

> **Route on the dialect mask.** If any `AXL_JSON_ALLOW_*` bit is set, the
> document goes to our lexer with those sub-flags gating it. If none is
> set, it goes to jsmn. Non-dialect bits (UTF-8 mode, `REJECT_DUPLICATES`,
> `DECODE_ANY`) never affect routing.

### jsmn is NOT a strictness oracle (measured 2026-07-29)

An earlier draft of this section said our lexer with every `ALLOW_*` bit
clear "must be exactly as strict as jsmn", and made that equivalence P3's
gate. **That is wrong, and following it would have defeated the whole
design.**

jsmn is compiled without `JSMN_STRICT`, so today's "strict" mode is
permissive. Characterized and pinned in `test/unit/axl-test-data.c`:

| input | `AXL_JSON_PARSER_DEFAULT` today |
|---|---|
| `{ a: 1 }` unquoted key | **accepted** |
| `{ "a": 0x10 }` hex | **accepted** |
| `{ 'a': 1 }` single quotes | **accepted** |
| `{ "a": 1, }` trailing comma | **accepted** |
| `// c` comment | rejected |
| `{ "a": 1` unterminated | rejected |

So matching jsmn would mean STAYING permissive — and
`AXL_JSON_ALLOW_UNQUOTED_KEYS` would describe something permanently on,
which makes the P2 rejection matrix meaningless. The oracle is RFC 8259
via JSONTestSuite, not jsmn's behavior.

Two things this does NOT change, which is why no extra phase is needed:

- **P2's matrix is unaffected.** A row with one `ALLOW_*` bit set routes to
  OUR lexer, so its strictness for the other eight features is entirely
  within our control in P2. jsmn is never consulted.
- **P3 fixes strictness by construction.** All-flags-clear is the only case
  still routed to jsmn, and deleting jsmn is what makes `AXL_JSON_STRICT`
  genuinely strict. Tightening jsmn first would be tightening a parser we
  are about to delete.

### Liberal by default: nothing stops parsing

An earlier draft concluded P3 was a behavior change because "documents that
parse today stop parsing". That was a consequence of a default I had not
actually decided, and the decision (2026-07-29) removes it:

> **`axl_json_parse()` parses LIBERALLY.** The no-flags convenience entry
> point uses `AXL_JSON_RELAXED`, so anything that parses today still parses,
> and a great deal that fails today starts working. Strictness is opt-in
> via `AXL_JSON_STRICT`.

The goal is a library that can read just about any JSON, JSON5, or
relaxed-JSON document a consumer is handed — a firmware SDK reads config
files, sidecars, and API responses it does not control, so being liberal in
what it accepts is the useful default. Validation is the specialised case,
and it gets a flag.

So the two reader entry points have distinct jobs, which is what keeps `0`
meaning strict from being surprising:

| call | dialect |
|---|---|
| `axl_json_parse(doc, len, &r)` | liberal — `AXL_JSON_RELAXED` |
| `axl_json_parse_flags(doc, len, flags, &r)` | exactly `flags`; `0` is RFC 8259 |

> **SUPERSEDED 2026-08-04 — see decision 41.** There is one entry point now,
> `axl_json_parse(doc, len, flags, &r)`, and no default: the `_flags` twin was
> folded into the base signature and every call site names its dialect. The
> paragraph above is why the default was liberal while there was one, which is
> also why the migration could not be done by adding a `0`.

**P3 is therefore NOT a behavior change in the restricting direction.** The
nine in-tree `axl_json_parse` callers (JOSE header/payload/JWK ×4, Redfish
responses ×2, an HTTP request body, a netload check, the SDK example) become
strictly more capable, not less. Its commit still needs to state what
CHANGED — a liberal default accepts input that used to be rejected, notably
comments — but nothing regresses.

One tradeoff recorded deliberately rather than discovered later: a liberal
default means `axl-jose.c` accepts more in JWT headers and JWKs, which are
attacker-influenced. That was weighed and accepted; `AXL_JSON_STRICT` is
available per call site if a consumer wants validation there, and adding it
is a one-line change at four sites.

**Deleting jsmn is P3 and is in scope for this effort** — one parser, no vendored dependency,
and every dialect flag honored by a single code path instead of two.

Removal is cheaper than it looks, verified 2026-07-29:

- **The public API does not leak jsmn.** `AxlJsonReader.tokens` is
  `int32_t *`, not `jsmntok_t *`, so this is an internal change with no
  API break. (The header's opening comment does say "parses RFC 8259 JSON
  via jsmn" — that prose goes stale and must be updated in the same
  change.)
- **The surface is small**: ~30 `JSMN_*` references, all inside
  `src/data/*.c` — `JSMN_OBJECT`, `JSMN_ARRAY`, `JSMN_STRING`,
  `JSMN_PRIMITIVE`, `JSMN_UNDEFINED`. They become an AXL-native token
  enum and struct.
- **Our lexer needs nothing structural.** It already grows its token array
  dynamically (`tok_cap` + realloc), so jsmn's two-pass count-then-fill
  model is not something we have to reimplement — it is something we get
  to drop.

The gate is evidence, not confidence. jsmn is battle-tested and our lexer
had only ever been exercised on JSON5, a *superset* — so it had never had
to prove it can REJECT anything. That is exactly the wrong bias for a
strict parser, and it is why the corpus, not a code reading, is what
cleared P3.

**Outcome, measured.** Before P3, `AXL_JSON_STRICT` on the 316 embedded cases:
8 `y_` wrongly rejected (all of them bare-primitive roots) and **99 of 186
`n_` wrongly accepted**. After: 95/95 `y_` accepted, 186/186 `n_` rejected,
and all 35 `i_` matching a per-case pinned decision. The suite went 8573 → 8625
on both arches with no other count moving.

## Two engines, four faces

Added 2026-07-29 after an API-completeness review against Jansson, JSON-GLib and
yyjson. P1-P4 shipped a correct parser with an incomplete API around it; this
section is the target architecture the remaining phases build toward.

### The problem it solves

The reader and writer were exact opposites, and neither was what the other
implied:

- The **reader** tokenizes the WHOLE document up front. No incremental feed, no
  pull interface, no early exit.
- The **writer** is fully incremental -- nothing buffered, no tree -- but its
  sink is hardcoded to an `AxlString`.

So "streaming" was true of the writer and false of the reader, which makes
"read and write the same dialect" only half a guarantee.

Prior art says the asymmetry is not necessary. RapidJSON has SAX **and** DOM on
the read side, a streaming `Writer` **and** `Document::Accept(writer)` on the
write side -- and the important trick is that its DOM serializer is not a second
writer, it is the DOM walking itself into the streaming one.

We already have three quarters of that shape and did not notice:

| existing | is really |
|---|---|
| `axl-json-lex.c` | a scanner, with its output hardwired into a token array |
| `AxlJsonReader` | the whole-document layer over that scanner |
| `AxlJsonWriter` | a streaming emitter, with its sink hardwired to `AxlString` |
| `axl_json_write_token` | the document-into-writer bridge, i.e. RapidJSON's `Accept` |

The asymmetry is two hardcoded endpoints, not a structural fact.

### The shape

**Two engines** -- scan and emit -- and **four faces**, where the
whole-document forms are conveniences over the streaming ones:

| | streaming | whole-document |
|---|---|---|
| **in** | scanner: pull events from a source | `AxlJsonReader` = run the scanner to completion |
| **out** | `AxlJsonWriter` = emit to a sink | `axl_json_write_token` = walk a document into the writer |

Four public faces, two implementations, one grammar. That last point is the
whole argument: two grammars kept in agreement by hand is exactly what the
jsmn-vs-lexer split cost us (P3), and it is not a mistake worth repeating for
the streaming/whole-document split.

`axl_json_parse` is therefore, since P12e: init a scanner, loop `next()`
to EOF, append each event to the token array — plus a small amount of builder
state, because container tokens must be patched after their children are known,
and one question the scanner refuses to answer (see "Where the one-document
boundary lives"). See "The document builder needs its own stack" below; it is
deliberately NOT literally a `while (next())` loop, and reading it that way
understates P12.

### Naming

`axl_json_<noun>_<verb>`, which is JSON-GLib's pattern, applied to NEW surface
only. Two things deliberately not taken from JSON-GLib:

- **Its object decomposition** (Parser / Reader / Builder / Generator / Node /
  Object / Array). That split exists because JSON-GLib has a refcounted mutable
  node tree; ours is a zero-copy read-only token array, and importing the shape
  would drag in a DOM we have deliberately not built.
- **The name `reader` for a cursor.** In JSON-GLib, `JsonParser` owns the tree
  and `JsonReader` navigates one. Our `AxlJsonReader` IS their `JsonParser`, so
  reusing their vocabulary would mislead arrivals from both directions.

`AxlJsonReader` keeps its name: 967 in-tree `axl_json_*` references (100
`axl_json_free`, 51 `parse_flags`, 50 `get_string`), plus softbmc's 14 files,
AGT, axl-webfs, axl-utils and uefi-devkit. `axl_json_get_string` ->
`axl_json_parser_get_string` is worse ergonomically and churns all of it.

Lifecycle follows the convention already in the tree -- measured at 52 `_new(`
versus 19 `_init(` across the public headers, and the split is principled:

- `_new` -> the library owns heap (`axl_string_new`, `axl_cache_new`)
- `_init` -> the caller places it, usually on the stack (`axl_json_writer_init`,
  `axl_line_reader_init`, `axl_hash_table_iter_init`)

Every type added here is caller-placed, so all of them are `_init`.

### Ownership and lifetimes

Three rules everything else must respect.

**1. Single owner, no reference counting.** A document owns its token array and
(optionally) its bytes. Sub-readers and value handles are non-owning views into
it, valid for its lifetime.

Jansson refcounts because its `json_t` nodes are shared and mutable:
`json_object_get` returns a borrowed reference and `json_array_append` shares a
node between parents. Once a node can have two owners you need counting. Ours
cannot. **yyjson makes the same call** -- `yyjson_doc` owns everything,
`yyjson_val*` are borrowed, no refcounts -- so this is the modern design rather
than a shortcut.

There is also a freestanding argument: refcounts shared across CPUs need
ATOMICS, AXL runs `AxlTaskProc` on APs, and the P3 review already caught a
"no atomics needed" comment that was false. Non-atomic refcounts plus MP is a
live bug class.

Declining also deletes an entire API category. Jansson duplicates roughly ten
mutators purely for refcount hygiene -- `object_set`/`set_new`,
`array_append`/`append_new`, `insert`/`insert_new`, `update`/`update_new`,
`iter_set`/`iter_set_new` -- because
`json_object_set(o, "k", json_string("v"))` LEAKS and `set_new` steals instead.
Under arena ownership the question never arises: the value was allocated FROM
the document, so it is already the document's. **yyjson has zero stealing
variants**, which is the proof.

The condition that keeps a mutable DOM reachable: **never hand out a value
handle whose lifetime is independent of a document.** Hold that line and a
mutable DOM can arrive later with arena ownership (`src/mem/axl-arena.c`
already exists and is the `yyjson_mut_doc` pool model) and never need a
refcount. The alternative escape route, single-ownership move semantics, is
also already idiomatic here via `axl_steal_pointer`.

**2. A reader MAY own its bytes.** `owns_json` alongside the existing
`owns_tokens`. Which applies is not a flag -- it follows from the source:

- contiguous source -> borrows the caller's buffer, zero-copy, unchanged
- stream or callback source -> the reader owns its buffer

This incidentally removes a real wart. `axl_json_load_file` currently hands back
TWO objects with an ordering constraint (free the reader BEFORE the buffer);
with reader-owned bytes it is one object and one `axl_json_free`. And it is not
merely ergonomic: a scanner reading from a stream has no caller buffer to point
into, so the whole-document-from-a-stream path REQUIRES it.

**3. A streaming event payload is valid only until the next `next()`.** A
scanner over a stream refills a scratch buffer, so an event's text cannot
outlive the call that produced it; the caller copies what it keeps. Same
contract as RapidJSON's SAX handlers. This is decided here because it is baked
into the event struct's shape.

### Errors

Equivalent to Jansson's `json_error_t`, minus two fields it should not have.

```c
typedef struct {
    AxlJsonErrorCode code;    ///< stable enum; AXL_JSON_OK (0) means no error
    size_t           offset;  ///< byte offset from the start of the input
    uint32_t         line;    ///< 1-based
    uint32_t         column;  ///< 1-based, counted in bytes
} AxlJsonError;
```

Caller-placed out-param, **never allocated**. Allocating in order to report an
allocation failure is why `GError**` is the wrong model here; Jansson's
caller-placed `json_error_t *` is the right one.

Dropped from Jansson's version:

- **`text[160]`.** A message buffer inside the struct forces formatting at
  failure time, in the parser, in every build, and caps quality at 160 bytes.
  Instead store nothing and let a formatter take the document later:
  `axl_json_error_format(&err, json, len, buf, size)` can quote the offending
  line with a caret under the column, which is strictly better -- and costs
  zero bytes today. **That formatter is P15, not P9** -- the two were conflated
  until a contract review pointed out that the phase table had never scoped a
  renderer into P9, and that "the struct is useless without it" is an argument
  for sequencing them together, not for merging them. With a stream or callback source the bytes are gone, so
  passing NULL yields the terse `12:7: unexpected byte` form.
- **`source[80]`.** The caller knows the filename and prefixes it. A borrowed
  `const char *` could dangle; a copy costs 80 stack bytes in a struct meant to
  be cheap.

**How the caller receives it — queried, not an out-param.** The design review
caught that this was never specified, and it is the contract question P9 turns on.

`AxlJsonError` lives IN the reader / scanner / writer and is queried:

```c
const AxlJsonError *axl_json_reader_error (const AxlJsonReader *r);
const AxlJsonError *axl_json_scanner_error(const AxlJsonScanner *s);
const AxlJsonError *axl_json_writer_error_info(const AxlJsonWriter *w);
```

Rejected alternative: a Jansson-style `AxlJsonError *error` out-param on the load
functions. It would mean a SECOND breaking signature change to
`axl_json_parse` immediately after P1's — 51 in-tree call sites — or a
parallel `_err`-suffixed entry point for every reader function, i.e. the API
doubling this redesign exists to avoid.

Querying also mirrors what the writer already does (`axl_json_writer_error()`
inspects the writer's own sticky state), so it is one mechanism rather than two.
`axl_json_writer_error()` keeps its `bool` signature and gains the detail
accessor beside it, so no existing call site changes.

Consequence for the failure path: on a `false` return the reader must be
POPULATED with the error rather than zeroed. It already leaves
`tokens == NULL, owns_tokens == false`, so `axl_json_free` stays a safe no-op on
a failed reader — the error just rides along in it.

```c
AXL_JSON_OK = 0,
AXL_JSON_ERR_INCOMPLETE,      /* input ENDED mid-value -- feed me more */
AXL_JSON_ERR_UNEXPECTED_BYTE, /* input is WRONG -- more bytes will not help */
AXL_JSON_ERR_BAD_ESCAPE,
AXL_JSON_ERR_BAD_NUMBER,
AXL_JSON_ERR_BAD_UTF8,
AXL_JSON_ERR_DEPTH,
AXL_JSON_ERR_TRAILING,        /* a complete value, then more bytes */
AXL_JSON_ERR_DUPLICATE_KEY,
AXL_JSON_ERR_DIALECT,         /* feature present, its ALLOW_* flag is clear */
AXL_JSON_ERR_IO,              /* source read or sink write failed */
AXL_JSON_ERR_NO_MEMORY,
```

**Checked against Jansson's error-reporting reference (2026-07-30), which found
four gaps.** The enum above was written from our own failure sites; comparing it
to a library that has shipped this for years found things that audit could not:

- **No "unset" code.** `json_error_unknown` exists to make a missed
  classification VISIBLE. Ours defaulted to `AXL_JSON_OK`, so a failure site
  that forgot to set a code would report success beside a `false` return —
  indistinguishable from the real thing. Added `AXL_JSON_ERR_UNKNOWN` as the
  entry-time default, which also makes the 31-site reclassification *checkable*:
  no rejected document may report it, and the 188 JSONTestSuite reject-cases
  assert that in bulk.
- **No invalid-argument code**, and worse, the early returns
  (`json == NULL`, `len == 0`, `r == NULL`, and `axl_json_load_file`'s failed
  open) return false WITHOUT TOUCHING the reader. The docstring promised the
  error was valid after any failure; on those three paths a caller would have
  read stack garbage. Added `AXL_JSON_ERR_INVALID_ARGUMENT`, and `ERR_IO` stops
  being "reserved for P10" — `axl_json_load_file` can fail to open a file today
  and reported nothing at all.
- **`column` counted in BYTES.** Jansson counts CHARACTERS and supplies the byte
  offset separately as `position`. Ours had byte offset *and* byte column, which
  is the same information twice and leaves P15's caret visibly misplaced on any
  line with non-ASCII before the error — the line a caret is most needed on.
  Changed to characters; `offset` remains the byte index. Counting lead bytes
  needs no UTF-8 validation, so it stays honest on the ill-formed input the
  reader passes through.
- **ACCESSOR-level failures have no error channel at all.** Jansson has
  `json_error_item_not_found`, `json_error_wrong_type` and
  `json_error_numeric_overflow`; `axl_json_get_int` returning `false` cannot
  distinguish "no such key" from "not a number" from "will not fit in 64 bits",
  and the last is a case `axl_json_get_number_str` exists specifically to
  rescue. **Deliberately NOT in P9** — P9 is parse-time errors, and the accessor
  family is P11's subject. Recorded so P11 does not have to rediscover it.

One place we are ahead: Jansson stores its code in the LAST BYTE of `text[]` and
added `json_error_code()` in 2.11 to dig it out. A typed field costs nothing and
needs no accessor.

Three of these are load-bearing beyond naming a failure:

- **`INCOMPLETE` distinct from `UNEXPECTED_BYTE`** is yyjson's
  `YYJSON_READ_ERROR_MORE`. Without the split, incremental input needs a parser
  rewrite.

  **The split is necessary but NOT sufficient, and the design review was right to
  push on this.** Resuming needs two kinds of saved state. The container bitmap
  in the scanner covers the INTER-token kind (which levels are open). It does
  nothing for the INTRA-token kind: a chunk that ends in the middle of a
  `\uXXXX` escape, a block comment, or a number's exponent needs to remember how
  far into that sub-grammar it got. The recursive parser got that for free from
  the C call stack (`parse_string`'s loop index, `parse_number`'s
  saw_int/saw_frac); a resumable scanner cannot.

  **This concluded that P12's scope had to be "explicit container stack plus an
  explicit sub-token state machine", and that was wrong** -- see decision 39.
  The leaves still run to completion in one call, and P13 shipped without the
  second half, because a straddling token is re-scanned rather than resumed.
  What the analysis got RIGHT is that the split is not sufficient on its own:
  something extra WAS needed, and it turned out to be a one-bit at-end-of-input
  signal rather than a state machine.
- **`TRAILING`** is Jansson's `JSON_DISABLE_EOF_CHECK` in code form, and it is
  what makes NDJSON / concatenated streams possible.
- **`DIALECT`** is our most common RECOVERABLE failure. A tool can say "your
  sidecar uses comments -- pass `AXL_JSON_ALLOW_COMMENTS`" instead of
  "parse error".

Both cost one enumerator now and are expensive to retrofit, because retrofitting
means re-classifying all 31 existing failure sites in `axl-json-lex.c`.

A success-path companion: **`axl_json_reader_consumed()`**, the twin of
`TRAILING`. Parse one value, learn where it stopped, parse the next. Three lines,
and shipping `TRAILING` without it would be odd.

### Source and sink

Two mirrored value types, each a function pointer plus a context.

```c
/* Both signed, and both counts: 0 = EOF / -1 = error on the read side,
   short = dropped / -1 = broken on the write side. See decision 23. */
typedef axl_ssize_t (*AxlJsonReadFn) (void *ctx, void *buf, size_t max);
typedef axl_ssize_t (*AxlJsonWriteFn)(void *ctx, const char *buf, size_t len);

axl_json_source_init_mem(&src, json, len);      /* zero-copy -- the fast path */
axl_json_source_init_stream(&src, stream);
axl_json_source_init_callback(&src, fn, ctx, hint);
axl_json_parse_source(&src, flags, &reader);      /* the entry point */

axl_json_sink_init_string(&snk, out);           /* what the writer does today */
axl_json_sink_init_buffer(&snk, &state, buf, size);  /* Jansson's dumpb */
axl_json_sink_init_stream(&snk, stream);
axl_json_sink_init_callback(&snk, fn, ctx);
```

That is eight Jansson entry points -- `loads`, `loadb`, `loadf`,
`load_callback`, `dumps`, `dumpb`, `dumpf`, `dump_callback` -- falling out of
two vtables and seven initializers, rather than eight functions each needing
their own flags-and-error plumbing.

**Streaming input to the WHOLE-DOCUMENT face saves no memory, and the design
should not be read as implying otherwise.** Tokens are `int32_t` offsets, so the
bytes they index must stay resident and at stable relative positions for the
reader's whole life. A sliding refill window would break the token model
outright. So a stream or callback source feeding `axl_json_parse`-equivalent
means the reader accumulates and RETAINS the entire document: the win is
ergonomic (one object instead of two, no caller pre-read), not spatial. Real
memory savings exist only on the scanner face, which retains nothing.

The reader-owned buffer grows by doubling, seeded by a size hint when the source
can supply one (a file stream usually can).

**The source must not destroy zero-copy.** Tokens are offsets into the caller's
buffer today; routing every read through a callback would force the scanner to
own a copy and make the library slower and allocation-heavy for all 967 call
sites. So `AxlJsonSource` carries an **optional contiguous view**: present means
the scanner reads it directly and copies nothing. Two modes, one type, existing
fast path untouched -- and it is what decides ownership rule 2 above.

**Buffering belongs to the sink, not the serializer.** `AxlStream` already has
`AXL_STREAM_BUF_NONE | LINE | FULL` with `axl_stream_set_buffering()` and a
1024-byte default, so a stage buffer inside the writer would be a second layer
for nothing. Note AXL's default is `BUF_NONE`, unlike stdio, so
`axl_json_sink_init_stream()` documents "set `AXL_STREAM_BUF_FULL` for bulk
output" -- and deliberately does NOT set it, since mutating a caller-owned
stream's mode would break someone writing unbuffered to a console on purpose.
Callback sinks may be invoked with small fragments and their owner buffers if
they care, which is the same promise `json_dump_callback` makes. The
`(buf, len)` signature stands on its own merits.

A sink write failure sets the sticky error to `AXL_JSON_ERR_IO` with the offset
reached, so a truncated write is reportable rather than silent.

### One type vocabulary, and completing the mirror

```c
typedef enum {
    AXL_JSON_TYPE_NONE = 0,   /* absent, or not found */
    AXL_JSON_TYPE_OBJECT, AXL_JSON_TYPE_ARRAY, AXL_JSON_TYPE_STRING,
    AXL_JSON_TYPE_NUMBER, AXL_JSON_TYPE_BOOL,   AXL_JSON_TYPE_NULL,
} AxlJsonType;
```

Used by `axl_json_get_type()`, by the scanner's event kinds, and by any future
DOM. Deliberately does NOT collapse number/bool/null the way the internal
`AXL_JSON_TOK_PRIMITIVE` does -- a caller asking "what is this?" needs them
apart, as Jansson's `json_is_integer`/`real`/`true`/`false`/`null` are.

It needs no token-format change: the first byte is decisive because the lexer
already validated the shape (`{`, `[`, `"`, `t`, `f`, `n`, or
digit/`-`/`+`/`.`/`N`/`I`), which is the trick `primitive_is_number()` already
uses. Defining it once is the point: if the scanner invents its own event enum
first, we own two type vocabularies forever.

**The by-key / own-value mirror is one-sixth complete, and that is a functional
hole.** Only `axl_json_value_string` exists (added because JWT `aud` arrays
forced it). There is no `value_int`, `value_uint`, `value_bool`, `value_object`
or `value_number_str` -- so **`[1, 2, 3]` is unreadable**: `array_next` hands
back a sub-reader per element and nothing can read a bare number out of one.

Worse, the naming has two prefixes for one concept and one of them lies.
`axl_json_root_array_begin` and `axl_json_value_string` are the SAME operation
-- both read `tokens[0]`, the reader's own value -- and `root_` is wrong,
because a sub-reader from `array_next` has no root yet that is exactly where the
function gets called.

```c
/* Descend to any value by key -- the primitive both iterators already produce,
   and the general form of get_object. */
bool axl_json_get_value(const AxlJsonReader *r, const char *key, AxlJsonReader *out);

/* Own-value family, complete and consistently prefixed. */
bool        axl_json_value_string    (const AxlJsonReader *r, char *buf, size_t size);
bool        axl_json_value_int       (const AxlJsonReader *r, int64_t *v);
bool        axl_json_value_uint      (const AxlJsonReader *r, uint64_t *v);
bool        axl_json_value_bool      (const AxlJsonReader *r, bool *v);
bool        axl_json_value_number_str(const AxlJsonReader *r, char *buf, size_t size);
AxlJsonType axl_json_value_type      (const AxlJsonReader *r);
bool        axl_json_value_array_begin (const AxlJsonReader *r, AxlJsonArrayIter *it);
bool        axl_json_value_object_begin(const AxlJsonReader *r, AxlJsonObjectIter *it);
```

`axl_json_root_array_begin` stayed as a deprecated alias through P11 and was
DELETED on 2026-08-04 (decision 41). (It had three call sites when this was
written and six when P11 landed -- the count is not the argument; the name
being wrong on a sub-reader is.)

**Why two families rather than one.** `get_X(r, key, ...)` is sugar for
`get_value` + `value_X`, and the sugar is what the 967 existing call sites use.
Unifying them by letting `key == NULL` mean "own value" was considered and
rejected: an accidental NULL key -- a lookup that returned nothing, a very
common bug -- would silently become "operate on the root" instead of returning
false. Every other decision in this library chose refuse-over-silently-
doing-something-else, and this is the same call.

**Object iteration**, mirroring the array iterator:

```c
bool axl_json_object_begin(const AxlJsonReader *r, const char *key, AxlJsonObjectIter *it);
bool axl_json_object_next(AxlJsonObjectIter *it,
                          char *key_buf, size_t key_size,   /* DECODED */
                          AxlJsonReader *value);
```

The key is **decoded into a caller buffer**, not handed back as a borrowed raw
view, because object keys can carry escapes (`{"\\u0041":1}` is a key named `A`)
and returning raw bytes would reproduce the `\u` decode bug one layer up.
Consistent with `axl_json_get_string`, at the cost of a copy per key.

Kept separate from `AxlJsonArrayIter` rather than unified: arrays yield values,
objects yield key plus value, and a shared `next()` would carry a dead
out-param half the time. Jansson and yyjson both keep them separate.

**But do NOT mirror the existing iterator's shape — it has a latent aliasing
bug, found by the design review.** `AxlJsonArrayIter` stores
`const AxlJsonReader *reader`, a raw pointer to the CALLER's struct, and
`axl_json_array_next` re-dereferences it on every call. So reusing an element
reader while an inner iterator points at it silently retargets that iterator:

```c
axl_json_array_next(&outer, &elem);        /* elem = element 0        */
axl_json_value_array_begin(&elem, &inner); /* inner.reader == &elem   */
axl_json_array_next(&outer, &elem);        /* elem = element 1        */
axl_json_array_next(&inner, &sub);         /* walks element 1's tokens
                                              with element 0's indices */
```

Nothing is freed and nothing faults — ASan sees nothing. It is a silent wrong
answer, which is the failure class this library keeps refusing everywhere else.

Fix: **both iterators store the reader BY VALUE** (`json`, `json_len`, `tokens`,
`token_count` — four fields, no allocation), so a caller reusing an element
reader cannot retarget an iterator built from it. Roughly 24 bytes per iterator
and it makes the aliasing structurally impossible rather than a documented
hazard. Doing it while `AxlJsonObjectIter` is still on paper costs nothing;
shipping the mirror first would double the exposure.

### The scanner

The most speculative piece here, and the honest framing is that **no comparable
library has it.** Jansson, JSON-GLib, yyjson and cJSON are all DOM-only on the
read side -- Jansson's `json_load_callback` streams input BYTES and still
returns a full tree, and JSON-GLib's `JsonReader` is a cursor over an
already-built DOM. Only RapidJSON exposes parse events, and push (SAX) rather
than pull. Pull appears in Jackson, .NET's `Utf8JsonReader`, Go's
`json.Decoder.Token()` and simdjson On Demand.

So it is not Jansson parity. It earns its place on the symmetry principle plus
firmware-specific wins: read one key out of a large sidecar without tokenizing
all of it, O(depth) memory instead of O(tokens), early exit, NDJSON.

**Pull, not push.** The writer is already caller-driven, so a push reader would
be the asymmetric choice -- the parser driving while the writer is driven. Pull
also composes with `INCOMPLETE` for incremental input, and push can be built on
pull in a few lines while the reverse is impossible.

```c
typedef enum {
    AXL_JSON_EV_EOF = 0,     /* document complete */
    AXL_JSON_EV_OBJ_BEGIN, AXL_JSON_EV_OBJ_END,
    AXL_JSON_EV_ARR_BEGIN, AXL_JSON_EV_ARR_END,
    AXL_JSON_EV_KEY,
    AXL_JSON_EV_STRING, AXL_JSON_EV_NUMBER, AXL_JSON_EV_BOOL, AXL_JSON_EV_NULL,
} AxlJsonEventKind;
```

A documented superset of `AxlJsonType` -- a type enum cannot express
`BEGIN`/`END` -- 1:1 with it on the value kinds, with `axl_json_event_type()`
mapping back. Same shape as RapidJSON's SAX handler set.

`ev.text` / `ev.len` are **borrowed raw source bytes with escapes intact**:
zero-copy, valid only until the next `next()`. Which forces a decision the `\u`
bug makes obvious -- **the string decoder becomes public**:

```c
/* Shipped in P11 as int: bytes written, or -1 on truncation -- see below. */
int  axl_json_decode_string(const char *src, size_t len, char *buf, size_t size);
bool axl_json_event_string(const AxlJsonEvent *ev, char *buf, size_t size);
```

`decode_json_string` is `static` today. Handing out raw text with no public
decoder would mass-produce the bug phase A fixes. `axl_json_event_string` is the
event-level twin of `value_string`, extending the mirror to the streaming face.

**No recursion, and no allocation for the container stack.** The scanner needs
one bit per level -- array or object -- so a `uint8_t in_array[32]` bitmap
covers 256 levels in 32 bytes, inline in the caller-placed scanner. Two
consequences:

- The stack-overflow hazard is GONE rather than bounded. `AXL_JSON_DEPTH_MAX`
  becomes a policy number, not a stack budget; the measured 144-bytes-per-level
  figure behind decision 6 stops mattering.
- It extends a pattern already in the tree -- the writer's
  `uint32_t in_array_bits` -- which means the **writer's hard 32-level cap can
  be lifted the same way**, symmetrically, instead of remaining an artifact of
  one integer's width.

```c
axl_json_scanner_init(&s, &src, flags);
while (axl_json_scanner_next(&s, &ev)) { ... }   /* stop whenever */
if (s.err.code != AXL_JSON_OK) { ... }
axl_json_scanner_free(&s);                        /* required; no-op when nothing was owned */
```

`_free` is required even where a contiguous source allocates nothing, so we can
start allocating later without changing the contract.

**Where the one-document boundary lives.** The scanner emits `EV_EOF` when the
ROOT value completes and stops there; it does not consume trailing bytes and
does not decide policy. The whole-document face then checks for trailing
non-whitespace and reports `AXL_JSON_ERR_TRAILING`. An NDJSON consumer simply
calls `axl_json_scanner_next()` again and gets the next document's events. Same
scanner, three behaviours, no flag — and it is why `TRAILING` is a code rather
than a hard error inside the scanner.

**The document builder needs its own stack, which the scanner does not provide.**
Caught in self-review: the scanner needs one BIT per level, but
`AxlJsonReader` needs to patch each container token's `.end` and `.size` after
its children are consumed -- which means the builder must track, per open level,
the token index and a running child count. That is builder state, not scanner
state, so it does not belong in the bitmap. Same shape as the bitmap though:
`AXL_JSON_DEPTH_MAX` entries of `{ int32_t idx; int32_t count; }` is exactly
2 KB. Heap-allocated once per parse, **sized to the RESOLVED depth limit rather
than the maximum** — typically 32 levels, 256 bytes. Shipped that way in P12e.

The sizing is the argument, not the byte count: 2 KB on the stack would have
been fine by this document's own standard (decision 6 accepted ~37 KB of
recursive stack for the same 256-level bound), so claiming it is "too much for
a stack" would contradict that. The real reason is that reserving the worst
case unconditionally on every parse is waste when the resolved limit is almost
always 32. One allocation, alongside the token array the builder is already
making.

**An INDEX per level, not an `AxlJsonTok *`.** The token array doubles as it
grows and frees the old block, so a pointer captured when a container opened
dangles the moment a child crosses a boundary — a use-after-free reachable only
on documents past 16 tokens with a container still open across the growth. See
decision 38.

This is worth stating because it is the one place "the whole-document face is
just a scan loop" is not literally true: the loop carries state the scanner
deliberately does not.

### Reusing AXL's own containers

`axl-array.c` and `axl-hash-table.c` live in `src/data/` alongside the JSON
files, so there is no circular-dependency issue like the Log-to-Data one that
forced `AxlFormat` to be zero-dependency.

**Yes, in four places:**

1. **The token array -> `AxlArray`.** Value-mode with inline elements
   (`axl_array_new(element_size)`), and `axl_array_get` returns a pointer INTO
   the buffer -- which `parse_object` needs, since it patches `.end`/`.size`
   after children are parsed. Replaces the hand-rolled doubling in `alloc_tok`.
   **Blocked on a missing `axl_array_steal()`**: the reader must own a
   contiguous block because sub-readers rebase it by pointer arithmetic, and
   `AxlArray` exposes neither `steal` nor `data`. GLib has `g_array_steal`. So
   the verdict is yes AND add the missing function -- dogfooding surfacing a gap
   in our own container is the argument for dogfooding.
2. **`REJECT_DUPLICATES` -> `AxlHashTable`.** Not a style preference: without a
   seen-key set, duplicate detection is O(n^2) comparisons per object. Hand-
   rolling means shipping a worse ALGORITHM. Only allocates when the flag is set.
3. **`SORT_KEYS` on `write_token` -> `AxlArray` + `axl_array_sort`.** Collecting
   an object's members to sort them is what the array is for, and the sort exists.
4. **A future mutable DOM -> the arena allocator.** `src/mem/axl-arena.c` is
   already the `yyjson_mut_doc` pool model.

**And three deliberate noes:**

5. **The scanner's container stack.** The inline bitmap stays; an `AxlArray`
   would allocate on every scan and destroy the "contiguous source allocates
   nothing" property.
6. **A key-lookup index.** `find_value_token` is linear, so N lookups on an
   M-member object is O(N*M). A hash index fixes that but costs an allocation
   plus hashing every key up front, which LOSES for the common case of two or
   three lookups on a small object. yyjson has identical linear behavior and
   documents it. A natural opt-in flag later; not a corner.
7. **`AxlStrBuf` for the refill buffer.** A plain `char[]` is simpler and
   allocation-free.

### What we still decline, and why it is not a corner

| Jansson / JSON-GLib feature | status |
|---|---|
| reference counting, `json_auto_t` | declined; single-owner + `AXL_AUTOPTR`. Arena is the growth path |
| stealing `_new` mutators | never needed under arena ownership; yyjson has none either |
| mutable DOM (`json_object_set`, `JsonBuilder`) | deferred; arena-per-document when a consumer needs it |
| `json_equal`, `json_copy`, `json_deep_copy` | additive over a token array, no shape constraint |
| `json_pack` / `json_unpack` | additive; genuinely ergonomic for config reads |
| `json_object_update_recursive` (deep merge) | additive; pairs with our config layering |
| JSON Pointer (RFC 6901), `JsonPath` | additive traversal over the token array |
| `json_string_length`, `JSON_ALLOW_NUL` | we substitute U+FFFD for an interior NUL instead -- safer, already decided |
| `JSON_DECODE_INT_AS_REAL`, `JSON_REAL_PRECISION` | **deferred, not n/a** — corrected 2026-07-29. AXL has `axl_dtoa` and `%f`/`%e`/`%g`; these land with real support. See constraint 4 |
| `json_object_seed` (HashDoS) | n/a: we do not hash keys |
| ~~`axl_json_get_double`~~ | **PROMOTED to P14 (2026-07-30).** No longer declined — see below |

Every row is additive or n/a. None requires changing a type, a signature or an
ownership rule -- which is the definition of not being cornered.

### `axl_json_get_double` was declined; the demand arrived (P14)

Deferred on 2026-07-29 with "`get_number_str` is the lossless stand-in until
someone needs it". Promoted 2026-07-30, for two reasons that are worth
separating from each other.

**The demand is prospective, and the first version of this paragraph
overstated it.** It said "`tools/rfbrowse.c` reads Redfish today", which is
true and irrelevant: rfbrowse uses exactly three JSON accessors and all three
are `axl_json_get_string`. It reads no numbers at all. Of the three fields this
paragraph named, only `ReadingCelsius` appears in the tree -- twice, as an
INTEGER, in `test/integration/redfish-mock-server.py`. No in-tree caller reads
any of them as a float, and the claim that one did is the sort of drift the
rest of this document exists to catch. (The first correction of this paragraph
said the fields "appear nowhere in the tree", which was itself unverified;
review caught that too, which is the joke writing itself.)

The real argument is an API asymmetry. Every other scalar has a `get_X` and a
`value_X`; `double` is the only type where a caller must fetch
`get_number_str` and parse the token themselves -- in a library that ships a
correctly-rounded decimal parser. Redfish floats are a plausible next consumer
rather than a present one.

**The severity was overstated, and correcting that is what makes the phase
small.** `canada.json` (~99% float literals, from simdjson-data) parses,
round-trips and reads back losslessly through `axl_json_get_number_str` --
verified, it is one of the 30 documents the corpus runner passes. Nothing is
unreachable or corrupted. This is a TYPE gap, not a data-loss gap, which puts it
in a different class from everything phase A fixed and is why it belongs after
the queue rather than in front of it.

**The hard half does not live in this document.** AXL has no decimal-to-double
parser at all -- no `strtod`, no `axl_str_to_double`, nothing (grepped, not
assumed). That primitive belongs in `axl-str.h` beside `axl_str_to_u64` and the
rest of the `axl_str_to_*` family, not inside the JSON reader, exactly as
`axl_dtoa` lives in `axl-format.h` rather than in the writer. It serves config
files, SPD timings and sensor decoding too. **So P14 depends on
`axl_str_to_double` landing on `main` first**, and it is being handed to a main
session rather than built here.

Given that primitive, the JSON side is genuinely small: one accessor, one
`value_*` mirror entry, and the same refuse-do-not-guess rule the rest of the
reader follows.

```c
bool axl_json_get_double  (const AxlJsonReader *r, const char *key, double *v);
bool axl_json_value_double(const AxlJsonReader *r, double *v);
```

The rule that keeps it consistent with `get_int` / `get_uint`: **refuse rather
than round.** A value the primitive cannot represent exactly returns false, and
`get_number_str` remains the lossless escape hatch -- the same division of
labour commit `5eae8129` established when it made out-of-range integers a hard
`false`. A silently-rounded double is precisely the "accept and corrupt" shape
this library keeps refusing.

Test oracle: `canada.json` through the corpus runner, with jq supplying expected
values over the differential path that already exists. That is the whole reason
the bulk suites were worth wiring up.

## The C++ face (C6, 2026-08-17)

`<axl/axl-json.hpp>` gives each of the four faces above a C++ form. It lives in
`docs/AXL-Cxx-Design.md` §9/§9e as a C++-layer phase; recorded here because it
is what finally exercised the four-face shape end to end, and because it
required four additions to THIS API.

| face | C | C++ |
|---|---|---|
| streaming in | `AxlJsonScanner` | `axl::json_scanner`, a range of events |
| whole-document in | `AxlJsonReader` | `axl::json_document` / `axl::json_value` |
| streaming out | `AxlJsonWriter` | `axl::json_writer`, containers as RAII scopes |
| document into writer | `axl_json_write_token` | `json_writer::splice()` |

**The bridge needed no C change.** A sub-reader is REBASED, so token 0 of
`doc["items"]` is that array — `axl_json_write_token(w, sub, 0)` splices it.
That is the property the "two engines, four faces" section predicted would make
the DOM serializer not a second writer, and it held.

### Four additions, and why the obvious shortcut was wrong

**`axl_json_get_string_len` / `axl_json_value_string_len` /
`axl_json_object_peek_key_len`.** Nothing could size a decoded string:
`get_string` truncates silently and returns true, and object iteration reports
a truncated key only after the pair is consumed. A `std::string` return was
impossible on any of the three.

The shortcut — expose the raw source span, let the caller size `len * 3 / 2 + 1`
and decode with the already-public `axl_json_decode_string()` — is wrong, and
the reason is worth keeping: **that helper takes no UTF-8 mode.** A caller
decoding through it would get a different string from the one the reader hands
back, which is exactly the "one document, two answers" contradiction
`token_equals` and the iterator's decoded keys were both written to remove. So
the queries go through the reader.

The peek exists rather than a length-of-the-pair-just-yielded because the
question a caller has is "how big a buffer do I need", and only a
before-the-fact answer can prevent the truncation rather than report it.

Measuring runs the REAL decoder into a scratch buffer. A count-only pass would
duplicate the escape, surrogate and split-tail logic and could then drift from
the thing it claims to predict — and `repair_decoded_utf8` decides it anyway,
since REPAIR rewrites in place and needs the bytes.

**`axl_json_double` / `axl_json_kv_double`** complete the scalar mirror the
`get_double` entry above opened: the reader has read doubles since P14 and the
writer could not emit one without formatting it by hand and splicing through
`axl_json_raw` — which puts JSON validity on the caller for the one type where
it is genuinely hard.

Emitted `%.17g`, which on AXL's engine is the SHORTEST round-trippable spelling
rather than the 17 digits it looks like: `axl_dtoa` (Grisu2) produces at most 17
shortest digits, so the significant-digit rounding is a no-op and `%g` picks the
shorter of fixed/exponential and trims. `0.1` emits as `0.1`. Verified by
round-trip: every value written reads back bit-identical.

Non-finite values follow the DIALECT rather than the formatter. RFC 8259 has no
NaN and no Infinity, so a strict writer latches its sticky error and emits
nothing; `AXL_JSON_ALLOW_NAN_INF` — the same bit the READER accepts them under —
emits `NaN` / `Infinity` / `-Infinity`. One flag, both directions, which is what
the shared flag space is for.

## Implementation phases

Each phase is independently green (both arches) and independently
committable.

| Phase | Scope | Notes |
|---|---|---|
| **P1** | DONE. Flag space + clean break + consumer updates | Mechanical. `AXL_JSON_JSON5` initially still drove the existing monolithic lexer, so behavior was unchanged. Indent width landed here too (decision 3). |
| **P2** | DONE. Thread the sub-flags through the lexer | The real work. The lexer was monolithic — no per-feature gating. Each sub-flag got its own accept/reject test, plus the N×N rejection matrix. |
| **P3** | DONE. **Removed jsmn.** One parser for every flag value; `src/data/jsmn.h` deleted, `axl-json5-parse.c` renamed `axl-json-lex.c`, token struct now AXL-native (`axl-json-internal.h`) | Gate was RFC 8259 conformance via JSONTestSuite, NOT jsmn equivalence. Also: bare-primitive root (reader + writer), `DECODE_ANY` removed, reader depth bound, liberal `axl_json_parse` / `axl_json_load_file`. Decisions 5–7. |
| **P4** | DONE. `axl_json_get_number_str()` + `ALLOW_NAN_INF` | The accessor makes NaN/Infinity reachable, so they landed together. Decisions 8–9. |
| **P5** | DONE. Writer formatting: `COMPACT`, `ESCAPE_SLASH`, `EMBED` | Exact whole-document string assertions. `INDENT(n)` already landed in P1. `COMPACT` is a no-op without `INDENT` — decision 26. `EMBED` is pinned by an identity rather than by prose: wrapping its output in the omitted delimiter reproduces the unembedded output, asserted across five indent settings. |
| **P6** | DONE. `ENSURE_ASCII` (surrogate pairs) + `SORT_KEYS` on `write_token` | Highest-risk phase; BMP-boundary tests required. `SORT_KEYS` semantics were open and are now decision 28 — decoded-key order, duplicates by source position. It is the only part of the writer that allocates. Its tie-break needed a twenty-member test to be visible at all, because insertion sort is stable below `INSERTION_THRESHOLD`. |
| **P7** | DONE. `REJECT_DUPLICATES` + all three UTF-8 modes on read | `DECODE_ANY` was dropped from this phase — see decision 5. `REJECT_DUPLICATES` runs as a post-lex pass over the finished token array rather than inside the lexer — decision 29. The UTF-8 half split by mode — decision 30. |
| **P8** | DONE, then REMOVED 2026-08-04 (decision 41) — no caller outside its own tests, and `saved_flags` cost 2 KB of a caller-placed struct. Container-scoped flag overrides on the writer | Needed P5's formatting flags, and the writer honoring the UTF-8 field — its own test plan asks for `UTF8_RAW` scoped to one value, which is unaskable while the writer ignores the mode. Decision 31. |
| **P9** | DONE. `AxlJsonError` + code enum, both directions; `axl_json_reader_consumed()` | Re-classifies all 31 failure sites in `axl-json-lex.c`. `INCOMPLETE`/`TRAILING`/`DIALECT` must land here or retrofitting means auditing them twice. |
| **P10** | DONE. `AxlJsonSource` / `AxlJsonSink` -> `loads`/`loadb`/`loadf`/`load_callback`, `dumps`/`dumpb`/`dumpf`/`dump_callback` | Jansson I/O parity. Contiguous sources stayed zero-copy, proved by mutating the caller's buffer behind the reader. `owns_json` landed here. Decision 23 changed `AxlJsonWriteFn` from `bool` to a count. |
| **P11** | DONE. `AxlJsonType` + `axl_json_get_type` / `value_type`; the `value_*` mirror; `axl_json_get_value` — all landed. public `axl_json_decode_string` landed too — P11 is COMPLETE | `[1,2,3]` is readable now. Every by-key `get_X` was rewritten as `get_value` + `value_X`, so the two families cannot drift. Purely additive apart from that refactor. |
| **P12** | DONE. Pull scanner: `AxlJsonScanner`, event enum, explicit container-stack bitmap; `axl_json_parse` reimplemented as a scan loop | Largest rewrite, landed in six steps: P12a-c the scanner over refactored leaves, P12d what a 4.1M-case scanner-vs-parser differential found, P12f the writer's cap to 256, P12e the whole-document face. Recursion is gone from BOTH faces, so the depth bound is a policy number (decision 6) and the writer's 32-level cap could be lifted (decision 37). The recursive-descent parser -- 440 lines -- was deleted, not kept as a second path. |
| **P13** | DONE. Incremental input over a pull `AxlJsonSource`; NDJSON over `TRAILING` + `consumed()` | Only reachable once P9 and P12 existed. The design's premise turned out to be wrong and is recorded as decision 39: no resumable sub-token machine was needed or built. NDJSON already fell out of P12's document boundary; what P13 added is the refill window. |
| **P15** | DONE. `axl_json_error_format()` — render an #AxlJsonError as text, quoting the line and pointing a caret at the column | Split OUT of P9 (2026-07-30) so that phase is the error TYPE and nothing else. The phase table never scoped a formatter into P9; it was included by implication because the "no `text[160]`" decision leans on one existing, and a contract review flagged the drift. Needs P9's struct and nothing more. |
| **P14** | DONE. `axl_json_get_double` + `axl_json_value_double` | **Landed 2026-08-02.** `axl_str_to_double` is in `axl-str.h` and merged into this branch. It is correctly rounded (ties to even), is the exact inverse of `axl_double_to_str`, and takes an `endptr` so a JSON accessor can demand the WHOLE token be consumed. Two contract seams to settle when this lands: its range errors WRITE `out` and still return `AXL_ERR` (unlike the integer family), and it accepts `nan`/`inf` spellings that JSON5's `NaN`/`Infinity` do not match case-for-case. |

### Execution order

Phase NUMBER is not execution order. Bug fixes and API parity come before new
formatting features, so P9-P11 jump ahead of the P5-P8 that were planned first:

| order | phase | why here |
|---|---|---|
| **A** | DONE. the `\uXXXX` decode bug, merged from `main` and reconciled with this branch's `\0` / `\x00` hardening | Silent data corruption in a SHIPPED accessor -- see below. Independent of everything else. Decision 18. |
| **B** | P9 errors | The stated must-have, and P10 needs its error type. |
| **C** | DONE. P10 source/sink | Jansson I/O parity: eight entry points from two vtables. |
| **D** | DONE. P11 type vocabulary + `value_*` mirror + object iteration | `[1,2,3]` was unreadable; the mirror, the type vocabulary and object iteration have all landed. Public `axl_json_decode_string` landed with it, returning -1 on truncation like its inverse `axl_json_escape_string` rather than truncating like `axl_json_get_string` — a standalone caller has no reader to interrogate afterwards. |
| **E** | P5 DONE. P6 DONE. P7-P8 | Writer formatting and reader flags, as originally planned. P6's two halves are independent and landed separately: `ENSURE_ASCII` first, then `SORT_KEYS` — which shares no code with it, being the one flag that cannot touch the streaming writer at all. |
| **F** | DONE. P12, then P13 | Most speculative, largest rewrite, no comparable library has it, nothing in-tree needed it -- deferred until everything else had landed, then done last as planned. Both shipped 2026-08-02/03. |
| **F.5** | P15 error formatter DONE | Small, and it is what makes P9's "store a position, not a message" decision pay off for a caller. Sequenced after the phases that ADD error sites (P10's `ERR_IO`, P7's `ERR_DUPLICATE_KEY`) so the renderer is written once against the finished enum. |
| **G** | P14 real support DONE | A TYPE gap, not a data-loss one — `get_number_str` already reads every float losslessly. No longer blocked: the `main`-side primitive landed and is merged, so this is now schedulable on its merits rather than gated. |

**Phase A is a separate concern from this design.** `decode_json_string` in
`src/data/axl-json-parse.c` has no `case 'u':`, so every `\uXXXX` escape is
mis-decoded -- `"A"` returns `"u0041"`, a surrogate pair returns
`"ud83dude00"`. The lexer VALIDATES the escape, so the document parses and no
error is raised: accept-and-corrupt, the same failure class we deleted jsmn for.
It affects `axl_json_get_string` and `axl_json_value_string` in both dialects.

It is present in released **v3.1.0** and on `main`, not introduced by this
redesign, so it was fixed and pushed on `main` independently (`0352d185`) and
this branch takes it by MERGE, not reimplementation. Full analysis, host-only
reproduction and the twelve-row test table:
`local/docs/bug-json-u-escape-2026-07-29.md`.

The merge is where the two halves of `decode_json_string` became one. `main`
deliberately left the JSON5 `\0` / `\x00` raw-NUL hazard alone — unreachable
there, since its strict path was jsmn — and this branch is the only place both
halves exist, so it is where they had to be made consistent. They agreed on
U+FFFD and disagreed on the bound; **decision 18** records what that resolved to
and why the tests that came with the `\u` fix could not have caught it.

Why no test caught it: both `\u` tests in `axl-test-data.c` exercise
`axl_json_write_token`, which splices string bytes verbatim and is therefore
correct. No test ever asked `get_string` to DECODE one -- the tests passed
because they tested the other path.

A mid-point independent review after P2 (per
`feedback_code_review_before_commit` — large multi-phase change, review at
the first stable green) and the standard pre-commit review on every phase.

## Testing

Per the project's test-first workflow: bucket A (new public API) and
bucket B (output format). **Exact whole-document `axl_strcmp` assertions,
never `axl_strstr`** — a substring match would let most of the regressions
below through silently.

### The load-bearing test: the P2 rejection matrix

Every other test here is routine. This one is not, and it is the reason
P2 is the risky phase.

The obvious test for a granular flag is "flag X accepts feature X." That
test is **worthless on its own**: the lexer today is monolithic, so an
implementation that ignores the flag word entirely and accepts all of
JSON5 passes every single positive case. The test that actually
discriminates is the negative half — **flag X must REJECT features Y≠X**.

So P2 needs an N×N matrix: for each of the 9 sub-flags, parse a document
exercising each of the 9 features, and assert accept on the diagonal and
reject everywhere else. ~81 assertions, table-driven. Plus the two
endpoints: `AXL_JSON_JSON5` accepts all 9, `AXL_JSON_STRICT` rejects all 9.

Confirm RED with a deliberately flag-ignoring lexer before implementing —
if the matrix passes against that, the matrix is wrong.

### Per phase

**P1 — flag space + clean break.** This phase must prove it changed
nothing but names:
- Every migration-table row produces byte-identical output to the old
  constant. `AXL_JSON_INDENT(2)` vs old `PRETTY`, `AXL_JSON_JSON5` vs old
  `PARSER_JSON5`, `ALLOW_TRAILING_COMMA` vs old `WRITER_TRAILING_COMMAS`.
  Capture the old output as an exact literal BEFORE the rename.
- Packing round-trip: `AXL_JSON_INDENT_OF(AXL_JSON_INDENT(n)) == n` for
  every n in 0..63, and the presence bit is set for all of them.
- No collision: `AXL_JSON_INDENT(63)` OR'd with every boolean flag —
  each flag still reads back, and the indent still reads back as 63.

**P4 — `get_number_str` + `ALLOW_NAN_INF`.**
- Every value P2's bound rejects is retrievable as text: 2^64, 2^70, a
  20-digit decimal, `1.5`, `1e10`.
- The literal is preserved VERBATIM — `"1e10"` stays `"1e10"`, not
  `"10000000000"`; leading zeros and a leading `+` survive as written.
- Buffer exactly large enough, and one byte too small (false, and pin
  what happens to the buffer).
- `NaN` / `Infinity` / `-Infinity`: rejected without the flag; lexed with
  it; `get_int` / `get_uint` reject them either way; `get_number_str`
  returns the exact spelling.

**P5 — writer formatting.** All exact whole-document compares:
- `AXL_JSON_INDENT(0)` vs no indent flag — these MUST differ (newlines
  with zero indent vs fully compact). This is what the presence bit
  exists for; if they match, the bit is broken.
- `INDENT(n)` for n = 0, 1, 2, 8 over a 3-deep nested structure.
- `COMPACT` separators, `ESCAPE_SLASH` on and off.
- `EMBED` alone, and `EMBED | INDENT(2)` — outer braces gone but inner
  indentation still correct. This combination is where an implementation
  that special-cases depth 0 will break.

**P6 — `ENSURE_ASCII` + `SORT_KEYS`.** Highest risk:
- BMP boundary, exact strings: U+FFFF → `\uffff` (single); U+10000 →
  `\ud800\udc00` (pair); U+10FFFF → `\udbff\udfff` (max pair);
  U+1F600 → `\ud83d\ude00`.
- Pure ASCII input is byte-identical with and without the flag.
- The interaction table above: all three UTF-8 modes crossed with
  `ENSURE_ASCII`, each pinned to an exact document.
- `SORT_KEYS` on `axl_json_write_token` — sorted output exact, including
  a nested object (sorting must recurse) and keys differing only in case.
- `SORT_KEYS` on a streaming write is a documented no-op: assert the
  output is emission-ordered, so the no-op is pinned rather than assumed.

**P7 — reader flags.**
- `REJECT_DUPLICATES`: pin CURRENT duplicate-key behavior first (it is
  undocumented today), then assert the flag turns it into a parse error.
- ~~`DECODE_ANY`: bare primitive root rejected without, accepted with.~~
  Dropped: the flag is gone (decision 5), and a bare-primitive root is
  accepted unconditionally. Landed in P3 with its own assertions —
  every spelling accepted, plus the negatives that keep the relaxation
  from becoming "parse a prefix and stop" (`42 43`, `truex`, `42abc`).
- The three UTF-8 modes on READ against one ill-formed input — three
  distinct, exactly-pinned outcomes.

**P8 — container-scoped overrides.**
- A compact array inside an `INDENT(2)` document: exact whole-document
  compare, and the indentation AFTER the scoped container closes must be
  identical to a document that never scoped at all. Reverting correctly is
  the whole feature.
- Nesting: a scoped container inside a scoped container, inner reverting
  to the outer override rather than to the init flags.
- `UTF8_RAW` scoped to one value, with an ill-formed byte, while a sibling
  value in the same object is still repaired — one document, both
  behaviors.
- `EMBED` and `SORT_KEYS` passed to a `_flags` opener set the sticky
  error. Assert the error, not just that the flag had no effect.

**P9 — structured errors.**
- Every one of the 31 failure sites in the lexer re-classified, and the code
  asserted per site rather than "some error was reported".
- Position is asserted as line AND column, with at least one non-ASCII case:
  character-vs-byte columns are caught by exactly one assertion, because
  every other position row is ASCII where the two are identical.
- `missing_flag` asserted for a dialect rejection, including the three sites
  that had to be restructured to know WHICH feature was attempted.

**P10 — source and sink.**
- The contiguous source stays zero-copy, proved BEHAVIOURALLY: mutate the
  caller's buffer after the parse and the reader must see the new byte. A
  field peek would not prove borrowing; this does.
- A buffer sink at EVERY capacity from 0 to the document length, asserting
  the buffer holds exactly the first `min(size, needed)` bytes, nothing lands
  past it, and the error fires exactly when it truncated. One hand-picked
  capacity is not enough -- a sink that drops a straddling fragment whole
  passes any capacity that lands on a fragment boundary.
- The three write outcomes kept apart: `len` took everything, a short count
  is FULL and must not halt, -1 is BROKEN and must halt at the first
  fragment. A count LARGER than the fragment is refused, not clamped.
- Leaks: a failed streamed parse must leave nothing allocated, and the read
  path and the parse path are DIFFERENT cleanups -- a test that fails during
  the read never reaches the second one.

**P11 — the own-value mirror.**
- The headline is `[1,2,3]`: walk it and read 1, 2, 3. That is the gap the
  phase exists to close and it is one assertion.
- A STRING that looks like a number (`"123"`) must refuse every numeric
  accessor. Nothing else exercises the token-type guard, because every other
  case hands `value_int` a PRIMITIVE token.
- `NaN` / `Infinity` / `-Infinity` type as NUMBER beside a lowercase `null`,
  which is what pins the number-before-letters ordering in `value_type`.
- An EMPTY array OPENS true, so false always means "not an array".
- Every by-key accessor's untouched-on-false promise, asserted by seeding the
  out-param and checking it survives a failed lookup -- see decision 25.

### Parser equivalence — the gate for P3

The gate is CONFORMANCE, not agreement with jsmn — see "jsmn is NOT a
strictness oracle" above. Our lexer with every `ALLOW_*` bit clear must
match RFC 8259, which jsmn does not.

**Use [JSONTestSuite](https://github.com/nst/JSONTestSuite) as the
corpus** (MIT, compatible with Apache-2.0). It is the canonical RFC 8259
conformance suite from *Parsing JSON is a Minefield*, and its naming
convention maps directly onto what we need:

| Prefix | Meaning | Our assertion |
|---|---|---|
| `y_` | must be accepted | our lexer accepts, with the expected token tree |
| `n_` | must be rejected | our lexer rejects |
| `i_` | implementation-defined | ACCEPT, and pin the tally |

The `i_` row is the useful subtlety: RFC 8259 leaves these free, so there
is no right answer to assert. Decided 2026-07-29 — **accept them**, keeping
`AXL_JSON_STRICT` to exactly "rejects what RFC 8259 forbids, nothing more",
so strictness never quietly becomes stricter than the standard it names.
Record the tally, because an unpinned choice flips silently on a later
refactor; treat a change to it as a finding rather than noise.

That includes the ones it is tempting to reject: deep nesting, huge numbers
(`axl_json_get_number_str` makes them retrievable losslessly, so there is no
need to refuse them at parse time), and lone surrogates. The last is worth
naming because it is the one with a downstream consequence — an ill-formed
sequence read in is repaired on the way OUT by the writer's U+FFFD
substitution (`66ed676e`), so accepting it at parse time cannot produce an
invalid document.

Note these are NOT run against jsmn for comparison. A `y_`/`n_` case our
lexer gets wrong is a P2 bug to fix; a case where our lexer and jsmn
DISAGREE is expected, and on the four rows in the table above it is the
point.

Vendor the corpus as a generated header (these tests run in QEMU with no
filesystem), and record the accept/reject tally in the commit so a later
regression shows up as a number rather than a vibe. Any `y_`/`n_` case
our lexer gets wrong is a P2 bug to fix before P3 — not a corpus
exception to carve out.

**P3 — jsmn removal.** After the conformance gate passes:
- `src/data/jsmn.h` is deleted, nothing includes it
  (`grep -rn 'include.*jsmn' src/ include/ test/ tools/ sdk/` is empty), and
  the library plus all test binaries BUILD on both arches.

  That build is the real gate, and it is stronger than any grep: with the
  header gone, a single surviving `jsmntok_t`, `JSMN_OBJECT` or `jsmn_parse()`
  is a compile error, not a lint finding. An earlier draft asked instead for
  `grep -r jsmn src/ include/` to return nothing, which is neither achievable
  nor desirable — the NAME survives on purpose in about ten explanatory
  comments (`axl-json-lex.c`, `axl-json-internal.h`, `axl-json-parse.c`,
  `src/data/README.md`, `axl-test-data.c`), because "the vendored jsmn this
  replaced was iterative, so it rejected that document" is a fact a future
  reader needs, and no regex separates that prose from code.
- The unit suite must not move except where this phase deliberately changes
  behavior. It rose 8573 → 8625 on both arches — the new conformance binary
  plus the bare-root, depth and liberal-default assertions — and the only
  pre-existing assertions that changed are the ones this phase inverts on
  purpose (the six `characterize:` rows, `strict_rejected`, the top-level-atom
  writer check, and `axl_json_parse`-is-strict). Any OTHER count moving is a
  regression.
- The full JSONTestSuite corpus runs against the single remaining parser:
  95/95 `y_`, 186/186 `n_`, and all 35 `i_` matching a per-case pinned
  decision.
- Every discriminating mechanism verified by SABOTAGE, since a green suite
  proves nothing about a test that cannot fail. Confirmed: opening every
  dialect gate fails 23 assertions including the whole `characterize:` block;
  removing the depth bound fails 7 and STALLS the binary on the
  100000-bracket document; restoring the object-or-array root rule fails 5.

### Cross-cutting

- **Round-trip per dialect**: for each preset (`STRICT`, `JSON5`,
  `RELAXED`), a document the writer emits under those flags must parse
  back under the same flags. This is the check that the shared flag space
  actually bought symmetry rather than just looking symmetric.
- **Both arches, every phase.** Balanced SKIP counts per
  `feedback_balancer_count`.
- **Ratchet**: `.last-pass-count` moves with each phase; a phase that
  does not raise it has not added coverage.
