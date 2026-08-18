Core collection types, string utilities, and message digest checksums.

Headers:

- `<axl/axl-hash-table.h>` — Hash table with string keys (FNV-1a, chained)
- `<axl/axl-array.h>` — Dynamic array (auto-growing, index access)
- `<axl/axl-list.h>` — Doubly-linked list
- `<axl/axl-slist.h>` — Singly-linked list
- `<axl/axl-queue.h>` — FIFO queue
- `<axl/axl-radix-tree.h>` — Radix tree (compact prefix tree, longest-prefix lookup)
- `<axl/axl-ntree.h>` — N-ary tree (GLib GNode-style hierarchy, public node fields)
- `<axl/axl-tree.h>` — Balanced sorted map (GLib GTree, AVL; ordered iteration + range queries)
- `<axl/axl-ring-buf.h>` — Ring buffer (circular byte buffer, zero-copy, overwrite mode)
- `<axl/axl-digest.h>` — Message digest checksums (MD5, SHA-1, SHA-256) + PBKDF2-HMAC-SHA256 (RFC 8018) + rolling CRC-32 / Adler-32
- `<axl/axl-compress.h>` — DEFLATE / gzip / zlib / LZMA-alone compression (one-shot + AxlStream filters; LZMA encode+decode backed by the vendored LZMA SDK)
- `<axl/axl-sidecar.h>` — Common JSON5 sidecar loader (used by
  AxlPciIds / AxlPciClassDb / AxlSpdIds / AxlUsbIds; see its
  dedicated module page for the open / schema-check / singleton-
  with-atexit pattern)

## Choosing a Collection

| Type | Best for | Lookup | Insert/Remove |
|------|----------|--------|---------------|
| AxlHashTable | Key-value mapping | O(1) by key | O(1) amortized |
| AxlArray | Indexed, sorted data | O(1) by index | O(1) append |
| AxlList | Frequent insert/remove | O(n) by value | O(1) at position |
| AxlSList | Simple linked sequences | O(n) | O(1) prepend |
| AxlQueue | FIFO/LIFO patterns | O(1) head/tail | O(1) push/pop |
| AxlRadixTree | Prefix-match routing | O(k) by key | O(k) insert |
| AxlNTree | Parent/child hierarchy | O(depth) navigate | O(1) child insert |
| AxlTree | Sorted map + range queries | O(log n) by key | O(log n) insert |
| AxlRingBuf | Streaming I/O, pipes | O(1) push/pop | O(1) |

## AxlHashTable

GLib-style hash table with FNV-1a hashing, chained collision resolution,
and automatic resize at 75% load factor. Supports generic key types via
user-provided hash/equal callbacks.

### Ownership

All three constructors allocate the `AxlHashTable` struct. They differ
in how the table treats the **content** (key/value pointers passed to
insert/replace):

| Constructor | Keys | Values |
|---|---|---|
| `axl_hash_table_new_str()` | **Copied** (strdup'd) — table owns + frees | Borrowed |
| `axl_hash_table_new(hash, equal)` | Borrowed | Borrowed |
| `axl_hash_table_new_full(hash, equal, key_destroy, value_destroy)` | **Owned** if `key_destroy` non-NULL (no copy, transferred); borrowed otherwise | **Owned** if `value_destroy` non-NULL; borrowed otherwise |

"Owned" means the table calls the destroy callback when the entry is
removed or the table is freed. "Borrowed" means the caller is
responsible for the lifetime; the table never copies and never frees.

`new_str` is the "make this go away easily" choice for literal/borrowed
string keys. `new_full` is for caller-allocated keys/values where the
caller wants to transfer ownership at insert time. `new` is for
borrowed-on-both-sides scenarios (e.g. integer keys cast to `void *`,
values that outlive the table).

### Convenience Constructor (string keys)

`axl_hash_table_new_str()` creates a table with string keys that are copied
internally. Values are opaque pointers (not freed).

```c
AXL_AUTOPTR(AxlHashTable) h = axl_hash_table_new_str();

axl_hash_table_insert(h, "name", "AXL");
axl_hash_table_insert(h, "version", "0.1.0");

const char *name = axl_hash_table_lookup(h, "name");   // "AXL"
size_t count = axl_hash_table_size(h);               // 2

axl_hash_table_remove(h, "version");
axl_hash_table_foreach(h, my_callback, user_data);
```

### Full Constructor (generic keys, owned entries)

`axl_hash_table_new_full(hash, equal, key_free, value_free)` creates a
table with custom hash/equal functions and optional destructors. Keys
are NOT copied — the table takes ownership.

```c
// Owned string keys and values
AxlHashTable *h = axl_hash_table_new_full(
    NULL, NULL,             // NULL = axl_str_hash/axl_str_equal
    axl_free_impl,          // key destructor
    axl_free_impl);         // value destructor

axl_hash_table_replace(h, axl_strdup("key"), axl_strdup("value"));
axl_hash_table_replace(h, axl_strdup("key"), axl_strdup("new"));  // old key+value freed
axl_hash_table_free(h);                                        // remaining entries freed
```

### Pointer Keys

Use `axl_direct_hash`/`axl_direct_equal` for pointer or integer keys:

```c
AxlHashTable *h = axl_hash_table_new_full(
    axl_direct_hash, axl_direct_equal, NULL, NULL);

axl_hash_table_insert(h, (void *)42, my_data);
void *data = axl_hash_table_lookup(h, (void *)42);
```

### contains / steal / foreach_remove

```c
// contains: distinguishes NULL-valued key from absent key
axl_hash_table_contains(h, "key");   // true even if value is NULL

// steal: remove without calling destructors (caller takes ownership)
axl_hash_table_steal(h, "key");

// foreach_remove: bulk conditional removal
size_t n = axl_hash_table_foreach_remove(h, predicate, data);
```

### Iterator

Safe removal during iteration:

```c
AxlHashTableIter iter;
void *key, *value;

axl_hash_table_iter_init(&iter, h);
while (axl_hash_table_iter_next(&iter, &key, &value)) {
    if (should_remove(key, value)) {
        axl_hash_table_iter_remove(&iter);   // calls destructors
    }
}
```

## AxlArray

Dynamic array with value and pointer modes. Auto-grows on append.
Supports indexed access, sorting, removal, capacity reservation,
buffer stealing and element destructors. Merges GLib's GArray and
GPtrArray into one type; see the header for the deliberate divergences.

```c
AXL_AUTOPTR(AxlArray) a = axl_array_new(sizeof(int));

int values[] = {50, 20, 40, 10, 30};
for (int i = 0; i < 5; i++) {
    axl_array_append(a, &values[i]);
}

axl_array_sort(a, int_compare);
axl_array_remove_index(a, 0);        // remove first element
axl_array_remove_index_fast(a, 1);   // O(1) swap-with-last removal
axl_array_set_size(a, 10);           // grow (zero-initialized)
```

Pointer mode stores `void *` instead of copies — `axl_array_append_ptr`,
`axl_array_get_ptr`, and friends.

**Read the whole array at once** with `axl_array_data()` and
`axl_array_element_size()`. `axl_array_get()` costs an out-of-line call and a
bounds check per element, which is invisible on one lookup and dominant on a
traversal — measured over a C++ view, indexed reads ran 4.2x and a sort 19.4x
slower than the same loop over a base pointer:

```c
size_t     n    = axl_array_len(a);
const int *base = axl_array_data(a);
for (size_t i = 0; i < n; i++) { total += base[i]; }
```

The struct stays opaque and there is still no typed indexing macro: this is a
`void *` you cast explicitly against `axl_array_element_size()`, not
`g_array_index`'s silent pun. The pointer is **invalidated** by anything that
can move the buffer — `append`, `insert`, `prepend`, `set_size`, `steal` — so
treat it as a borrow that lives until the next mutation. A NULL return is
always paired with a length of 0, so `(pointer, length)` is safe to iterate
with no separate NULL test.

From C++, `axl::array_span<T>()` (`<axl/axl-array.hpp>`) wraps exactly this
pair and checks the stride for you.

**Reserve capacity** when the size is roughly known, so the append loop
pays no grow-and-copy:

```c
AxlArray *a = axl_array_sized_new(sizeof(int), 1000);   // g_array_sized_new
```

**Steal the buffer** to hand ownership of the elements to a caller. The
array is left valid and EMPTY (`g_array_steal` semantics, not
`g_array_free(arr, FALSE)`), so it stays usable and still needs freeing.
`out_len` is an ELEMENT count:

```c
size_t n;
int *block = axl_array_steal(a, &n);   // a is now empty but reusable
...
axl_free(block);
axl_array_free(a);
```

**Element destructors.** Without one, `axl_array_free` releases only the
buffer, so anything reached through a stored pointer leaks. There are two
setters because this one type covers both of GLib's array types and cannot
infer which convention you mean:

```c
axl_array_set_clear_func(a, my_clear);        // value mode: gets &element
axl_array_set_ptr_free_func(a, axl_free_impl); // ptr mode: gets the pointer
```

Either hook runs on every path that discards an element — `free`, `clear`,
`remove_index`, `remove_index_fast`, `remove_range`, and a *shrinking*
`set_size` — but never on `axl_array_steal`, which transfers ownership
rather than discarding it. Pass `axl_free_impl`, not `axl_free`: the latter
is a macro and has no address to take.

## AxlList

GLib-style doubly-linked list. Each node has `data`, `next`, and
`prev` pointers. Functions return the new head (which may change after
prepend, remove, or sort). Matches GLib's GList.

```c
AxlList *list = NULL;
list = axl_list_append(list, "first");
list = axl_list_append(list, "second");
list = axl_list_prepend(list, "zeroth");

// Insert relative to a node
AxlList *node = axl_list_find(list, "first");
list = axl_list_insert_before(list, node, "half");

// Remove all matching, deep copy, context-aware sort
list = axl_list_remove_all(list, "first");
AxlList *copy = axl_list_copy_deep(list, my_copy_func, NULL);

axl_list_free(list);
```

## AxlSList

Singly-linked list — lighter than AxlList (no `prev` pointer).
Use when you only traverse forward. Matches GLib's GSList. Same
operations as AxlList: insert_before, remove_all, remove_link,
sort_with_data, copy_deep.

## AxlQueue

FIFO queue with push/pop at both ends and peek. Can also be used as
a stack (push/pop from the same end). Matches GLib's GQueue. Supports
find, remove, and stack-allocated initialization.

A heap queue (`axl_queue_new`) is torn down with `axl_queue_free`; a
stack-allocated or embedded queue (`AXL_QUEUE_INIT` / `axl_queue_init`)
is torn down with `axl_queue_deinit` (or `axl_queue_deinit_full` to also
free element data). Calling `axl_queue_free` on a non-heap queue frees the
struct pointer and corrupts the stack — use `deinit` for those.

```c
AxlQueue q = AXL_QUEUE_INIT;    // stack-allocated
axl_queue_push_tail(&q, "first");
axl_queue_push_tail(&q, "second");
axl_queue_push_tail(&q, "first");  // duplicate

axl_queue_remove(&q, "first");     // removes first match
axl_queue_remove_all(&q, "first"); // removes all matches

AxlList *node = axl_queue_find(&q, "second");

axl_queue_deinit(&q);              // stack queue teardown (NOT axl_queue_free)
```

## AxlNTree

Generic **n-ary tree** (GLib `GNode` equivalent) for parent→children
hierarchies — UI/device/file trees, a DOM, ACPI/SMBIOS structure. The
**node is the subtree handle** (no separate container) and its fields are
public, so traversal is a plain pointer walk:

```c
#include <axl.h>

AxlNTree *root = axl_ntree_new("/");
AxlNTree *etc  = axl_ntree_append_data(root, "etc");
AxlNTree *bin  = axl_ntree_append_data(root, "bin");
axl_ntree_append_data(etc, "hosts");

for (AxlNTree *c = root->children; c != NULL; c = c->next)
    axl_printf("%s\n", (const char *)c->data);          // etc, bin

axl_ntree_traverse(root, AXL_NTREE_PRE_ORDER, AXL_NTREE_ALL, 0,
                   visit_fn, ctx);                       // walk the tree

axl_ntree_free(root);                                    // node + subtree
// axl_ntree_free_full(root, free) also frees each node's data
```

Insertion (`append_child`/`prepend_child`/`insert_before`/`insert_after`)
attaches an existing root node; `append_data` is the new+append shortcut.
`axl_ntree_unlink` detaches a subtree (it becomes its own root).
`move_after`/`move_before` reposition an *already-attached* node (the
`insert_*` twins, with a cycle guard) — place-above/place-below a sibling,
reparent, or (with a NULL sibling) move to first/last; this is how a
scene-graph raise/lower is built. Counts and queries: `n_children`,
`nth_child`, `depth` (root = 1), `max_height`, `n_nodes(flags)`,
`is_ancestor`, `get_root`. Traversal supports pre/post/in/level order, an
ALL/LEAVES/NON_LEAVES filter, a depth limit, and early stop (the callback
returns `true`). Data is borrowed; the tree owns only its node objects.
Single-threaded, no locking.

For a **pull-style** walk without a callback, `AxlNTreeIter` is a
stack-allocated pre-order cursor (uses the parent/sibling links — no
internal stack, any depth); `axl_ntree_iter_init_reverse` walks the same
nodes in reverse pre-order (topmost-first for a paint-order tree — the
hit-test idiom):

```c
AxlNTreeIter it;
axl_ntree_iter_init(&it, root, AXL_NTREE_ALL);
for (AxlNTree *n; (n = axl_ntree_iter_next(&it)) != NULL; )
    use(n->data);
```

This is the **structural hierarchy** container — distinct from
`AxlRadixTree` (string-prefix lookup) and `AxlTree` (balanced sorted map).

## AxlTree

Balanced **sorted map** (GLib `GTree` equivalent, AVL-backed): ordered
key→value storage with O(log n) insert/lookup/remove and — the reason to
reach for it over `AxlHashTable` — **in-order iteration and range /
nearest-key queries**. Opaque container; keys ordered by an
`AxlCompareDataFunc`.

```c
#include <axl.h>

static int cmp_int(const void *a, const void *b, void *user) {
    (void)user; intptr_t x = (intptr_t)a, y = (intptr_t)b;
    return (x > y) - (x < y);
}

AxlTree *t = axl_tree_new(cmp_int, NULL);
axl_tree_insert(t, (void *)30, "thirty");
axl_tree_insert(t, (void *)10, "ten");
axl_tree_insert(t, (void *)20, "twenty");

axl_tree_lookup(t, (void *)20);            // "twenty"
axl_tree_lower_bound(t, (void *)15);       // value at key 20 (first >= 15)
axl_tree_upper_bound(t, (void *)20);       // value at key 30 (first > 20)
axl_tree_foreach(t, visit_fn, ctx);        // ascending key order

axl_tree_free(t);
// axl_tree_new_full(cmp, user, key_destroy, value_destroy) owns entries
```

`axl_tree_insert` keeps the existing key and replaces the value on a
collision; `axl_tree_replace` swaps both (GTree semantics — destructors
run on the dropped key/value). `nnodes` and `height` are O(1).

For a **pull-style** walk without a callback, `AxlTreeIter` is a
stack-allocated ascending-order cursor:

```c
AxlTreeIter it;
void *k, *v;
axl_tree_iter_init(&it, t);
while (axl_tree_iter_next(&it, &k, &v))
    use(k, v);          // ascending key order
```

Pick this for **ordered** keys / range scans; `AxlHashTable` for
unordered O(1) maps, `AxlRadixTree` for string longest-prefix lookup.

## AxlStr — String Utilities

String utilities operating on UTF-8 `char *` strings. Includes length,
copy, compare, search, split, join, and case-insensitive operations (ASCII
fold only). UCS-2 helpers at the bottom are for UEFI internal use.

All allocated results are freed with `axl_free()`.

Header: `<axl/axl-str.h>`

### Overview

AXL uses UTF-8 (`char *`) throughout its public API. UEFI firmware
uses UCS-2 (`unsigned short *`) internally, but AXL handles the
conversion transparently — you never need to deal with UCS-2 unless
making direct UEFI protocol calls.

### UTF-8 vs UCS-2

- **Use UTF-8** (`char *`) for all application code. All `axl_str*`
  functions operate on UTF-8.
- **UCS-2** functions (`axl_wcslen`, `axl_wcscmp`, `axl_str_to_w`,
  `axl_str_from_w`) exist only for UEFI interop. Consumer code should
  not need them.

### Per-codepoint iteration

`axl_utf8_decode` walks a UTF-8 string one Unicode codepoint at a
time. It is the UTF-8-first walker for new code that needs to inspect
characters (e.g. text rendering, syntax highlighting). The existing
`axl_utf8_to_ucs2` / `axl_utf8_to_ucs2_buf` helpers remain for UEFI
protocol interop where a CHAR16 buffer is required.

```c
const char *p = "Hello, 世界!";
uint32_t    cp;
size_t      n;
while ((n = axl_utf8_decode(p, &cp)) > 0) {
    use(cp);   // U+0048, U+0065, ..., U+4E16, U+754C, U+0021
    p += n;
}
```

Well-formed 1/2/3/4-byte sequences decode to U+0000..U+10FFFF.
Malformed leads, truncated continuations, overlong encodings, surrogate
codepoints, and out-of-range values all return 1 with
`*out_codepoint = 0xFFFD` (REPLACEMENT CHARACTER) — the caller advances
by 1 byte to resynchronize. End of string (NULL or `\0`) returns 0.

### Per-codepoint encoding

`axl_utf8_encode` is the reverse: one codepoint in, 1-4 UTF-8 bytes out,
bounded by the room the caller actually has. It writes no NUL and
reserves no room for one, so it composes into a larger buffer the way
`axl_utf8_decode` reads out of one.

```c
char   buf[AXL_UTF8_MAX_LEN];
size_t n = axl_utf8_encode(0x4E2D, buf, sizeof(buf));   // n == 3
```

Two rules make it safe to point at a wire format:

- **Unencodable codepoints are refused, not approximated.** A UTF-16
  surrogate (U+D800..U+DFFF) or anything above U+10FFFF writes nothing
  and returns 0. Encoding them anyway yields CESU-8 / WTF-8, which
  `axl_utf8_decode` — and every conforming decoder — hands back as
  U+FFFD, so emitting it just moves the corruption downstream. A caller
  that wants lenient behaviour substitutes U+FFFD itself.
- **A sequence that does not fit is refused whole.** Never a partial
  sequence, which would be ill-formed UTF-8 that the caller cannot
  distinguish from a complete one afterwards.

Both refusals return 0, which therefore always means "nothing was
written". Passing `dst == NULL` measures instead of writing (returning
the 1-4 bytes required, or 0 if the codepoint is unencodable), so a
caller that needs to tell the two refusals apart measures first — see
the worked loop in the `axl_utf8_encode` docstring, which re-measures
after substituting because the replacement may not fit either.

Note `axl_utf8_encode` reports a successful 1-byte encode for U+0000 —
it is a valid scalar, and the function has no way to signal "valid but
probably not what you want". Code assembling a NUL-terminated C string
must therefore reject or substitute U+0000 *before* calling, because an
interior NUL truncates the value for every `axl_strcmp` reader: an
`"admin\0extra"` value compares equal to `"admin"`.

The JSON string decoder does exactly that, and does it for **all three**
spellings of an escaped NUL: `\u0000`, JSON5's `\0`, and JSON5's
`\x00` each decode to U+FFFD rather than a NUL byte. The two JSON5
arms used to write the raw byte — strict JSON never reaches them (its
lexer whitelists only `" / \\ b f r n t u`), but JSON5 sidecars are
user-replaceable via `--ids-file`, so "unreachable from strict JSON"
was never a safety argument. All three now route through one appender,
so they also cannot diverge on the buffer bound: the 3-byte
replacement is refused whole when it does not fit, exactly as a decoded
`\uXXXX` is.

Only the ZERO byte is substituted in the `\xNN` arm; every other
`\xNN` still writes its byte verbatim.

Everything in the tree that turns a codepoint into UTF-8 routes through
this one function: the JSON string decoder, the XML character-reference
decoder (`&#N;` / `&#xH;`), `axl-vterm`'s glyph re-encoder, and both
console paths (`axl-console-emit`'s UCS-2 → UTF-8 producer side and
`axl-console-term`'s grid cell). The three that can be handed a value
with no UTF-8 spelling substitute U+FFFD; XML instead raises a located
parse error, because a character reference is *authored* text and
quietly replacing it would hide a document bug rather than a wire
glitch.

One site deliberately does **not** use it: `AxlStream`'s UCS-2 wire
transcode (`axl_stream_set_encoding`) keeps its own encoder. Its input
is a UCS-2 code *unit*, not a Unicode scalar, and an unpaired surrogate
is representable there — refusing it would turn a round-trippable code
unit into a dropped one, and that round-trip is a documented promise.
See `src/stream/README.md`.

### Case-Insensitive Operations

`axl_strcasecmp`, `axl_strcasestr`, and `axl_strncasecmp` fold
**ASCII letters only** (A-Z -> a-z). They do not handle full Unicode
case mapping. This is sufficient for UEFI identifiers, HTTP headers,
and file extensions.

### Common Patterns

```c
#include <axl.h>

// Split a string
char **parts = axl_strsplit("a,b,c", ',');
for (int i = 0; parts[i] != NULL; i++) {
    axl_printf("  %s\n", parts[i]);
}
axl_strfreev(parts);  // frees the array AND each string

// Join strings
const char *items[] = {"one", "two", "three", NULL};
char *joined = axl_strjoin(", ", items);
axl_printf("%s\n", joined);  // "one, two, three"
axl_free(joined);

// Search
if (axl_str_has_prefix(path, "fs0:")) { ... }
if (axl_str_has_suffix(name, ".efi")) { ... }
const char *found = axl_strcasestr(header, "content-type");
```

### Memory Ownership

Functions that return `char *` allocate new memory. The caller
must free with `axl_free()`:

- `axl_strdup`, `axl_strndup`
- `axl_strsplit` (free with `axl_strfreev`)
- `axl_strjoin`, `axl_strstrip`
- `axl_str_to_w`, `axl_str_from_w`

Functions that return `const char *` or `char *` pointing into
the input string do NOT allocate:

- `axl_strstr_len`, `axl_strrstr` (return pointer into haystack)
- `axl_strchr` (return pointer into string)

## Number Conversion

Both directions, for both floats and integers, all in `<axl/axl-str.h>`.

| direction | float | integer |
|---|---|---|
| string -> number | `axl_str_to_double`, `axl_str_to_float` | `axl_str_to_u64` / `_s64` (+ 32/16/8 variants) |
| number -> string | `axl_double_to_str`, `axl_float_to_str` | `axl_u64_to_str`, `axl_s64_to_str` |

```c
#include <axl.h>

double d;
if (axl_str_to_double("3.14159", &d, NULL) == AXL_OK) {
    char buf[AXL_DOUBLE_STR_MAX];
    axl_double_to_str(d, buf, sizeof(buf));   // "3.14159"
}

char hex[AXL_U64_STR_MAX];
axl_u64_to_str(255, 16, hex, sizeof(hex));    // "ff", no "0x" prefix
```

### The round-trip guarantee

`axl_double_to_str` writes decimal text that parses back to the
bit-identical double, and `axl_str_to_double` is the parser it is defined
against. So `parse(print(x)) == x` for every finite double, with no
trailing zeros — `100.0` renders `"100"`, not `"100.000"`.

The **round trip is always exact; the length is very nearly always minimal
but not guaranteed to be.** The digits come from Grisu2, which returns the
shortest string *it* finds, and for a fraction of a percent of doubles a
shorter one exists — `1e23` renders `"9.999999999999999e+22"` where
`"1e+23"` would have round-tripped just as well. Closing that last gap
needs Grisu3 or Ryu, i.e. a bignum fallback, and nothing here is worth
that.

`axl_float_to_str` is not "promote and print": it finds the shortest text
that round-trips through the *same float*, which is usually much shorter
(`0.1f` needs 1 significant digit as a float and 17 as a double). Read it
back with `axl_str_to_float`. It also has **no** minimality gap — it
searches upward from one significant digit and stops at the first length
that round-trips, so the first hit is the smallest by construction (a
~1e6-value sweep found no shorter rendering in any case).

The parser is **correctly rounded** — the result is the double nearest the
decimal value, ties to even, for inputs of any length. A fast path handles
the common short cases with exact hardware arithmetic; anything it cannot
prove exact falls back to exact decimal arithmetic rather than guessing.
The integer pair shares its `base` parameter with `axl_str_to_u64`, so it
round-trips at any radix 2..36.

### Range errors write a value AND return AXL_ERR

This is the one place the conversions diverge from the rest of the SDK's
`AXL_OK` / `AXL_ERR` convention, and it is deliberate — it mirrors C's
`strtod`, so a caller that only wants the saturated value can ignore the
status:

```c
double d;
int rc = axl_str_to_double("1e400", &d, NULL);
// rc == AXL_ERR, and d == +infinity (the value is still written)
```

Underflow writes a signed zero the same way, so `"-1e-400"` gives `-0.0`.
A **syntax** error is different: it leaves `*out` untouched and sets
`*endptr` back to the start, so you can tell "not a number here" from
"a number too big to represent."

`axl_str_to_float` reports a range error the double parse could not see:
`1e39` is a perfectly finite double that becomes `+inf` as a float.

The two families place `endptr` differently on a **range** error, and the
difference is load-bearing if you drive a tokenizer off it. The float pair
leaves it **past the digits** — the number was consumed, only
unrepresentable, which is what C99 `strtod` does on `ERANGE`. The integer
pair **rewinds it to `nptr`** on overflow. Both are released and neither is
changing.

### Passing no endptr means strict

Every parser in the family — both floats and all eight integers — treats a
`NULL` `endptr` as "the whole string, or nothing":

```c
double d;
axl_str_to_double("36.6C", &d, NULL);   // AXL_ERR, d untouched
axl_str_to_double("36.6",  &d, NULL);   // AXL_OK,  d == 36.6

const char *end;
axl_str_to_double("36.6C", &d, &end);   // AXL_OK, d == 36.6, end -> "C"
```

Leading whitespace is still skipped; **trailing** whitespace is trailing
content and fails, so `" 5 "` is an error. Passing `endptr` is how you opt
into partial parsing. Strict outranks the range-error rule above: `"1e400"`
writes `+inf` and returns `AXL_ERR`, but `"1e400xyz"` writes nothing at all.

### `nan` has no sign here

`"nan"`, `"inf"` and `"infinity"` all parse, case-insensitively. A sign on
`inf` is applied — `"-inf"` gives negative infinity — but a sign on `nan` is
consumed and **discarded**: `"-nan"` gives a *positive* NaN where glibc
gives a negative one. Nothing in AXL contradicts that, because AXL has no
signed-NaN surface: `axl_double_to_str`, `axl_float_to_str` and `%f` all
render every NaN as `"nan"` with no sign, so the parser never has to read
back text it did not write.

### Buffer-too-small: check the return, not the buffer

All four renderers follow `axl_snprintf`'s convention — as much as fits is
written, the buffer is always NUL-terminated, and the return is the length
the **whole** rendering would have had. So:

```c
if (axl_double_to_str(v, buf, sizeof(buf)) >= sizeof(buf)) {
    /* truncated */
}
```

The test matters more than usual here, because a truncated number is a
different, entirely plausible number that the buffer alone cannot be told
apart from a correct one: `axl_double_to_str(1e-300, buf, 5)` writes
`"1e-3"`, which reparses cleanly as `0.001`. Pass a buffer of
`AXL_DOUBLE_STR_MAX` / `AXL_U64_STR_MAX` / `AXL_S64_STR_MAX` and truncation
is impossible. Note `AXL_S64_STR_MAX` is one byte larger than the unsigned
one: `INT64_MIN` in base 2 is a sign plus 64 digits.

### Where the accuracy contract lives

These conversions are exact; `AxlMath` is not. `axl_sqrt`, `axl_sin` and
friends target UI-coordinate accuracy, not `libm` bit-exactness, and say so
in `src/math/README.md`. Do not read a guarantee from one into the other —
`axl_str_to_double` being correctly rounded says nothing about what
`axl_sin` returns. The predicates `axl_isnan` / `axl_isinf` /
`axl_isfinite` and the `AXL_MATH_INF` / `AXL_MATH_NAN` constants live in
`<axl/axl-math.h>` and are shared by both.

## AxlStrReader — Cursor-Based String Parser

Symmetric counterpart to `AxlString` (the builder). A reader BORROWS a
`const char *` (no allocation, no ownership) and tracks a cursor with
a sticky-error flag. Operations short-circuit when `ok` is false, so
chains compose naturally without per-call error checking:

```c
AxlStrReader r;
uint64_t     v;
axl_str_reader_init(&r, "N[03A8]");
axl_str_reader_consume_char(&r, 'N');
axl_str_reader_consume_char(&r, '[');
axl_str_reader_take_u64(&r, 16, &v);
axl_str_reader_consume_char(&r, ']');
if (!r.ok || !axl_str_reader_eof(&r)) { /* parse failed */ }
```

Header: `<axl/axl-str.h>` (alongside the legacy string utilities and
`axl_sscanf`).

Primitives: `init` / `init_n`, `eof`, `peek`, `remaining`, `skip_ws`,
`consume_char`, `consume_str`, `take_until`, `take_while`, `take_u64`,
`take_ident`. No allocation; `*out` slices point directly into the
input buffer.

For one-shot fixed-pattern parses (IP addresses, MAC addresses,
ASCII numerics with separators), `axl_sscanf` is built on top of the
same primitives and reads cleaner:

```c
unsigned a, b, c, d;
int consumed;
if (axl_sscanf(str, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed) != 4
    || str[consumed] != '\0') {
    return -1;   /* malformed */
}
```

Supports the C99 conversions consumers actually need: `%c %d %i %u %o
%x %X %s (with required width) %[set] %f %e %g %E %G %% %n`, length
modifiers `hh h l ll z j`, and `*` assignment suppression.

The float conversions route at `axl_str_to_double` / `axl_str_to_float`,
so they are correctly rounded and accept `nan` / `inf` / `infinity`.
Mind the C99 pointer types, which are the reverse of `printf`'s: plain
`%f` takes a `float *` and `%lf` takes a `double *` — handing a
`float *` to `%lf` writes 8 bytes into a 4-byte object. An out-of-range
value such as `1e400` is a successful conversion storing the saturated
IEEE result, the way C99 stores `HUGE_VAL`. An explicit field width is
honoured: `%3lf` on `"3.14159"` reads `3.1` and leaves `4159` for the
next conversion, with leading whitespace skipped before the width
applies. A width above 256 returns `-1` rather than being clamped —
that bound is a property of the format string, so it is checked up
front; without a width nothing is staged and a mantissa of any length
parses in full.

Every conversion that takes a width caps it at `SIZE_MAX`, not just the
float ones. Digits that would exceed `SIZE_MAX` return `-1`: the wrap is
downward (2^64+1 would become a width of 1), so honouring it would mean
converting a field the caller never asked for and reporting success.

## AxlString — String Builder

Mutable auto-growing string builder, like GLib's `GString`. All strings
are UTF-8. Supports append, prepend, insert, printf-style formatting,
truncation, and — for callers that need to size or edit the buffer
directly — `reserve` / `capacity` / `shrink_to_fit` / `resize` and a
mutable `axl_string_data()`.

Header: `<axl/axl-string.h>`

C++ callers usually want `axl::string` (`<axl/axl-string.hpp>`) instead:
an RAII wrapper over this builder with `std::string`'s interface, usable
in a freestanding translation unit where `<string>` is unavailable. See
`docs/sphinx/modules/cxx.rst`.

### Overview

Use `AxlString` when you need to build a string incrementally (in a
loop, with formatting, from multiple sources). For simple one-shot
formatting, use `axl_asprintf` instead.

```c
AXL_AUTOPTR(AxlString) s = axl_string_new("Hello");
axl_string_append(s, " ");
axl_string_append_printf(s, "world #%d", 42);
axl_string_append_c(s, '!');

axl_printf("%s\n", axl_string_str(s));  // "Hello world #42!"
axl_printf("length: %zu\n", axl_string_len(s));
```

### Stealing the Buffer

Transfer ownership of the internal buffer to avoid a copy:

```c
AXL_AUTOPTR(AxlString) b = axl_string_new(NULL);
axl_string_append_printf(b, "key=%s", value);

char *result = axl_string_steal(b);  // b is now empty
// caller owns 'result', must free with axl_free()
```

The builder can be reused after stealing — it starts empty with its
allocated buffer released. (That was always the documented contract, but
until recently the first append after a steal spun forever: `steal` left
the capacity at 0 and `grow` sized the replacement by doubling it.)

### Error Handling

All mutation functions (`append`, `printf`, etc.) return `int`:
0 on success, -1 if the internal realloc fails. This matches the
convention used by `axl_array_append`, `axl_hash_table_insert`, etc.

## AxlJson — JSON / JSON5

JSON reader and writer. Parse JSON strings into a token tree, query
values by key, and build JSON documents incrementally over an
`AxlString`. A separate colored UEFI-console pretty-printer is provided
for debug output.

One parser serves every dialect. With no flags set (`AXL_JSON_STRICT`)
it is RFC 8259, verified against the
[JSONTestSuite](https://github.com/nst/JSONTestSuite) conformance
corpus; each `AXL_JSON_ALLOW_*` bit opens exactly one
[JSON5](https://json5.org) extension on top of it — comments, trailing
commas, single-quoted strings, unquoted keys, hex numbers (see the
**JSON5 Support** section below).

Reader and writer draw those bits from ONE flag space, so a dialect cannot
mean two different things in the two directions. On the writer, though,
only `AXL_JSON_ALLOW_TRAILING_COMMA` currently changes the output — the
remaining dialect bits are reader-side today. The writer never emits an
unquoted key or a single-quoted string even when those bits are set, and
`axl_json_comment()` emits regardless of `AXL_JSON_ALLOW_COMMENTS`.

The vendored jsmn that used to serve the strict path is gone. It was
compiled without `JSMN_STRICT`, so the branch that was supposed to mean
"strict" was the permissive one: measured against the corpus it wrongly
accepted 99 of 186 must-reject documents.

Header: `<axl/axl-json.h>`

### Overview

AXL provides three independent JSON APIs:

- **Reader** (`AxlJsonReader`) — parse a JSON string, extract values
  by key, iterate arrays.
- **Writer** (`AxlJsonWriter`) — build JSON into an auto-growing
  `AxlString`, a fixed buffer, a stream, or a callback. Orthogonal calls
  (containers, keys, atoms) with a state machine that handles comma
  placement and string escaping. Optional pretty-print mode with 2-space
  indent.
- **Console printer** (`axl_json_console_print`) — colored,
  attribute-based pretty output to the UEFI console. Distinct from the
  writer's pretty-print flag (which produces buffer output, no colors).

### Reading JSON

```c
const char *json = "{\"name\":\"AXL\",\"version\":1,\"debug\":true}";
AxlJsonReader r;

if (!axl_json_parse(json, axl_strlen(json), AXL_JSON_RELAXED, &r)) {
    axl_printerr("invalid JSON\n");
    return -1;
}

char    name[64];
int64_t version;
bool    debug;

if (!axl_json_get_string(&r, "name",    name, sizeof(name)) ||
    !axl_json_get_int   (&r, "version", &version) ||
    !axl_json_get_bool  (&r, "debug",   &debug)) {
    axl_printerr("missing or wrong-type field\n");
    axl_json_free(&r);
    return -1;
}

axl_printf("name=%s version=%lld debug=%s\n",
           name, (long long)version, debug ? "true" : "false");

axl_json_free(&r);
```

`axl_json_parse` returns `false` on invalid syntax, unbalanced braces,
or token-array allocation failure. Each `axl_json_get_*` returns
`false` if the key is missing or the value has the wrong type. A string
value too long for the caller's buffer is **truncated, and the call still
returns `true`** — the exception is `axl_json_get_number_str`, which refuses,
because a clipped string is merely incomplete while a clipped number is a
different number. String values are copied into
the caller buffer — no zero-copy lifetime concerns. Always call
`axl_json_free` on the reader; the token array is heap-allocated.

#### Two accessor families, and asking what a value IS

Accessors come in two shapes. `axl_json_get_*(r, key, ...)` reads a value **by
key** out of an object. `axl_json_value_*(r, ...)` reads the reader's **own**
value, which is what an array element or a bare-value document actually is —
`axl_json_value_string` / `_int` / `_uint` / `_bool` / `_number_str` /
`_array_begin` / `_type`. Each `get_X` is `axl_json_get_value` followed by the
matching `value_X`, so the two families cannot drift apart.

That is what makes `[1, 2, 3]` readable: `axl_json_array_next` hands back a
sub-reader per element, and `axl_json_value_int` reads a bare number out of
one.

When the type is not known in advance, ask:

```c
switch (axl_json_get_type(&r, "field")) {         // or _value_type(&elem)
case AXL_JSON_TYPE_NUMBER: ... break;
case AXL_JSON_TYPE_STRING: ... break;
case AXL_JSON_TYPE_NULL:   ... break;             // present, and null
case AXL_JSON_TYPE_NONE:   ... break;             // absent — or a FAILED parse
default: break;
}
```

`AXL_JSON_TYPE_NONE` covers five situations, including a parse that failed and
whose return value the caller ignored — that reports `NONE` for every key and
means the opposite of an absent one, so check `axl_json_reader_error` when it
matters. And `NUMBER` does not promise integrality: `axl_json_value_int`
truncates `1.5` to `1` and returns true, so read the literal with
`axl_json_value_number_str` when the distinction counts.

#### Escaping and unescaping outside the reader

`axl_json_escape_string` and `axl_json_decode_string` are the two directions of
the same utility, usable without a reader or writer at all — for JSON text that
arrived from somewhere else.

```c
char enc[128];
int  e = axl_json_escape_string("he said \"hi\"", enc, sizeof(enc));
/* enc is "he said \"hi\"" -- WITH the surrounding quotes */

char dec[128];
int  d = axl_json_decode_string(enc + 1, (size_t)e - 2, dec, sizeof(dec));
/* skip the opening quote and drop two: the encoder brackets, the decoder
   takes the inner content a JSON string token brackets */
```

The decoder resolves the JSON5 superset (`\0`, `\xNN`, `\'`, `\v`, line
continuations) as well as the RFC 8259 set — the lexer is what refuses a JSON5
escape in a strict document, so by the time bytes reach a decoder the dialect
question is already settled.

**It REFUSES truncation** (returns -1), unlike `axl_json_get_string`, which
truncates and still succeeds. A caller here has no reader to interrogate
afterwards, and a prefix cannot be recognised as short from its own contents.

Size the output at `len * 3 / 2 + 1`. Decoding mostly shrinks, but it does not
never grow: `\0` is two source bytes and decodes to the three of U+FFFD, so
two bytes of source need four of output. Every other escape — `\xNN`,
`\uXXXX`, the surrogate pairs — consumes at least as many bytes as it
produces, which makes 3-to-2 the worst case over the whole set.

#### What a string accessor hands back

`axl_json_get_string` and `axl_json_value_string` resolve escapes into the
caller's buffer. The guarantee is scoped precisely, because the reader is not
the writer's mirror: **whatever AXL DECODES, it decodes to well-formed UTF-8,
and it never writes an interior NUL. Raw source bytes are passed through
unvalidated.**

- `\uXXXX` decodes to UTF-8, 1–4 bytes. A surrogate PAIR combines into one
  code point above the BMP, so U+1F600 comes back as its 4 bytes and not as
  two 3-byte sequences.
- A lone or bare `\u` surrogate becomes U+FFFD. The parser deliberately accepts
  these (JSONTestSuite classifies them `i_`), so the accessor is what has to
  refuse them.
- All three spellings of a NUL — the four-zero `\u` escape, and JSON5's `\0`
  and `\x00` — become U+FFFD.
  The buffer is NUL-terminated, so an interior NUL would truncate the
  value for every `axl_strcmp` caller and make `"admin\0extra"` compare equal
  to `"admin"`. That is a string-smuggling primitive, and it is reachable from
  attacker-influenced input (JWT headers and claims, JWKs, HTTP request
  bodies).
- JSON5's `\xNN` is ES5's `HexEscapeSequence`, i.e. the code UNIT U+00NN — so
  `\xe9` decodes to the two bytes of U+00E9, not to a lone `0xE9`.
- A value too long for the buffer truncates, as it always has — but **never in
  the middle of a UTF-8 sequence**, and never by making a character vanish while
  the characters after it survive. That holds for a decoded escape and for a raw
  multi-byte sequence alike, and it holds however the sequence was spelled: as
  raw bytes, as an escaped lead byte with unescaped continuations, or with every
  byte escaped separately. If the last thing that fit was half a sequence, the
  half is dropped.

**What an ill-formed RAW byte does depends on the UTF-8 mode.** A lone `0x80`,
or a UTF-8-encoded lone surrogate, is handed back exactly as found under
`AXL_JSON_UTF8_RAW`; under `AXL_JSON_UTF8_REPAIR` — the default — it becomes
U+FFFD, which makes the guarantee above unconditional rather than escape-only;
and under `AXL_JSON_UTF8_STRICT` the document does not parse at all. Note
`axl_json_parse` names `RAW`, so the no-flags entry point still passes bytes
through. The WRITER honors the same field at emission: RAW writes the byte out
verbatim, STRICT sets its sticky error, REPAIR substitutes. `ENSURE_ASCII`
beats RAW, because escaping to `\uXXXX` needs a code point an ill-formed byte
does not have.

The document still PARSES in every case above: this is about what the accessor
may hand back, not about the grammar.

### Writing JSON

The writer builds into a caller-owned `AxlString` and grows on demand.
Comma placement, string escaping, and (optional) indentation are
handled internally. Containers, keys, and atoms are independent calls
— a single state machine knows whether the current container is an
object or an array.

```c
AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
AxlJsonWriter w;

axl_json_writer_init(&w, out, AXL_JSON_STRICT);
axl_json_obj_begin(&w);
    axl_json_kv_str (&w, "name",    "AXL");
    axl_json_kv_uint(&w, "version", 1);
    axl_json_kv_bool(&w, "debug",   true);
    axl_json_key(&w, "tags");
    axl_json_arr_begin(&w);
        axl_json_str(&w, "uefi");
        axl_json_str(&w, "embedded");
    axl_json_arr_end(&w);
axl_json_obj_end(&w);
axl_json_writer_finish(&w);

if (!axl_json_writer_error(&w)) {
    axl_printf("%s\n", axl_string_str(out));
    // {"name":"AXL","version":1,"debug":true,"tags":["uefi","embedded"]}
}
```

For convenience, `axl_json_kv_*` collapses a key + atomic value into
one call (the dominant shape). Use `axl_json_key` followed by an atom
or container when the value is a nested object/array.

### Writer formatting flags

| flag | effect |
|---|---|
| `AXL_JSON_INDENT(n)` | newlines plus `n` spaces per level. `INDENT(0)` is newlines with no indent — NOT the same as passing no indent flag, which is fully compact. The presence bit is what separates them. |
| `AXL_JSON_COMPACT` | drops the space after `:`. Only meaningful WITH `INDENT`, because AXL's unindented output is already compact. |
| `AXL_JSON_ESCAPE_SLASH` | writes `/` as `\/`, in keys and values alike. Opt-in: RFC 8259 permits both and requires neither. The use is embedding in a `<script>` block, where `</` would close the element early. |
| `AXL_JSON_ENSURE_ASCII` | escapes every non-ASCII code point as `\uXXXX`, so the output is pure 7-bit ASCII. Non-BMP becomes a SURROGATE PAIR — U+1F600 is `\ud83d\ude00`, not one escape. Ill-formed input escapes as `\ufffd`. Applies to values, keys, and `axl_json_write_token` splices. |
| `AXL_JSON_EMBED` | omits the OUTERMOST `{}` or `[]`. Nested containers keep theirs, and only a CONTAINER root is affected — a bare-primitive root, an `axl_json_raw` root, and a document with no container are all unchanged. |
| `AXL_JSON_SORT_KEYS` | orders object members by key. `axl_json_write_token` ONLY — the streaming writer buffers nothing, so there is nothing to sort and the flag is a documented no-op there. |

`AXL_JSON_SORT_KEYS` orders **byte-wise over the key's DECODED name**,
shorter-first when one key is a prefix of another — code-point order for
well-formed UTF-8, and the same notion of a key's identity that a by-key
lookup uses. So `{"\u0062":1,"a":2}` sorts as `a` then `b`, because that
escaped key names `b`; ordering the source spelling would sort it by the
backslash (0x5C, which precedes `a`) and leave the document in its original
order, looking untouched rather than wrong. Case is not folded,
because byte order does not fold it — `Z` (0x5A) precedes `a` (0x61).

Sorting **recurses** into nested objects, wherever they sit, including inside
an array; array elements keep their order, which is data rather than key
order. A key is re-emitted in its original source spelling — the flag decides
the ORDER of members and rewrites none of them. Duplicate keys are all kept in
the order the document listed them.

Unlike the rest of the writer this allocates, proportional to the widest
object being sorted; an object whose keys carry no escapes needs no decode
buffer and borrows its names from the document. Allocation failure sets
`AXL_JSON_ERR_NO_MEMORY` on the writer.

`AXL_JSON_EMBED` is defined by an identity rather than by prose: **wrapping
its output in the delimiter it omitted reproduces the unembedded output byte
for byte.** So an indented document's leading and trailing newlines survive —
they belong to the members, not to the braces. Suppressing them would look
tidier and would break composition, which is the point of pinning it as an
identity: the caller splicing the result between their own braces gets exactly
what the writer produces on its own.

```c
axl_json_writer_init(&w, out, AXL_JSON_INDENT(2) | AXL_JSON_COMPACT);
// {
//   "a":1,
//   "o":{
//     "p":"x/y"
//   }
// }
```

### Reader flags — duplicate rejection and UTF-8

`AXL_JSON_UTF8_STRICT` on the reader is settled at **parse time**: a document
whose raw bytes are not well-formed UTF-8 fails the parse with
`AXL_JSON_ERR_BAD_UTF8`, positioned at the first byte of the first ill-formed
**sequence**, with line and column. An accessor returning `false` could not
carry that, and a caller could not tell ill-formed from absent-key or
buffer-too-small.

It scans the **whole document**, not its string tokens — RFC 8259 §8.1 defines
a JSON text as UTF-8, and a JSON5 comment body is the other place arbitrary
bytes survive lexing. Everything outside a string or comment is ASCII by the
grammar, so the wider scan rejects nothing a token walk would have accepted.

It checks **raw bytes only**. A lone surrogate written as an escape
(`"\ud800"`) is well-formed JSON syntax, is not rejected, and still decodes to
U+FFFD. Keys are checked as readily as values.

The field's fourth value is **reserved and refused**
(`AXL_JSON_ERR_INVALID_ARGUMENT`). It is reachable by accident:
`AXL_JSON_RELAXED` already names `UTF8_RAW`, so `AXL_JSON_RELAXED |
AXL_JSON_UTF8_STRICT` ORs to it. Spell the dialect out —
`AXL_JSON_JSON5 | AXL_JSON_UTF8_STRICT` — for a JSON5 parse that validates
its encoding.

`AXL_JSON_UTF8_REPAIR` and `AXL_JSON_UTF8_RAW` decide only which bytes an
accessor hands back, so they are consumed lazily at `axl_json_get_string` time
and never fail a parse. RAW hands an ill-formed document byte back exactly as
found; REPAIR — the **default** — turns it into U+FFFD, which makes the
reader's "whatever AXL decodes, it decodes to well-formed UTF-8" guarantee
unconditional instead of escape-only.

The mode is judged on the **decoded** bytes, not the source ones, and that is
load-bearing. JSON5 lets any byte be escaped, so one character can arrive
split across escapes and raw bytes — `\<C3>\<A9>` is U+00E9 as two
separately-escaped bytes, and `<C3>\<80>` is a raw lead with an escaped
continuation. Judging the source would see a lone lead in the second and
destroy a character the decoder assembles correctly.

Because the mode is consumed lazily, the reader stores it, and a sub-reader
and both iterators inherit it. `axl_json_parse` resolves to
`AXL_JSON_RELAXED`, which names `UTF8_RAW`, so the no-flags entry point passes
bytes through; a parse that names no mode at all gets REPAIR.


| flag | effect |
|---|---|
| `AXL_JSON_REJECT_DUPLICATES` | a repeated key in any one object fails the parse with `AXL_JSON_ERR_DUPLICATE_KEY`. |
| `AXL_JSON_UTF8_STRICT` | a document that is not well-formed UTF-8 fails the parse with `AXL_JSON_ERR_BAD_UTF8`. |

Without it a duplicate is **accepted** — RFC 8259 §4 calls repeated names
"unpredictable" rather than invalid. What AXL does with one is worth knowing,
because it is what the flag opts out of: a by-key accessor returns the **first**
occurrence, and `axl_json_object_next` yields every one of them separately.
("First" holds for any key a by-key lookup can match at all — an *escaped*
key whose decoded name reaches 256 bytes is skipped by that lookup, so a
later duplicate answers instead. This check has no such ceiling.)

"The same key" means the same **decoded** name, so `{"\u0061":1,"a":2}` is a
duplicate even though no two bytes of the two keys match — the same definition
a by-key lookup already uses, and the one that cannot be evaded by escaping
half of the pair. The check runs in every object at any depth, and
independently per object: sibling objects may each carry the same key, and a
nested object may reuse its parent's.

Decoding is **lossy** where the document is unrepresentable, so two keys that
differ can still collide: every spelling of a zero escape and every lone
surrogate becomes U+FFFD, making `{"\u0000a":1,"\ufffda":2}` a
duplicate. That follows the decoder the by-key accessors already use, so the
two agree on what a name is — but it can reject a document whose keys are
distinct as written.

The error is positioned at the **second** key — specifically at its first name
byte, inside the quotes when it has any, since a JSON5 unquoted key has none.

This is the one reader flag that allocates, and only when set. Detection is by
hash set rather than by comparing each key against its predecessors, which
would be O(n²) per object — a worse algorithm on exactly the wide objects that
motivate the check.

### Encodings, BOMs and line endings

The JSON layer is **UTF-8 only**, and deliberately: RFC 8259 §8.1 requires
UTF-8 for interchange, so `axl_json_parse` neither sniffs nor transcodes, and
a BOM-prefixed buffer is refused.

UEFI is the "closed ecosystem" that sentence carves out, though, and UCS-2 is
its native text form. That composes one layer down, in `AxlStream`:

```c
AxlStream *f = axl_fopen(path, "r");
AxlStream *t = axl_text_stream_wrap(f);   // sniffs the BOM, sets the encoding
AxlJsonSource src;
axl_json_source_init_stream(&src, t);
axl_json_parse_source(&src, AXL_JSON_STRICT, &r);
```

`axl_text_stream_wrap` consumes a `FF FE` / `FE FF` / `EF BB BF` BOM and picks
the byte order; `axl_stream_set_encoding` does the same job when you already
know the encoding — but note it does **not** skip a BOM, which then decodes to
`U+FEFF` and fails the parse. Writing works the same way: set the sink stream
to `AXL_ENC_UCS2_LE` and the writer's UTF-8 output lands as UCS-2 on the wire.

**Both line endings read.** RFC 8259 §2 lists space, tab, LF and CR as
whitespace, so LF, CRLF and lone-CR documents all parse to the same values,
and an error's `line` counts lines rather than CR bytes. A JSON5 line comment
terminates at a lone CR as well as at LF.

### Reading a float

`axl_json_get_double()` and `axl_json_value_double()` complete the scalar
family — `double` was the only type whose caller had to fetch
`axl_json_get_number_str()` and parse the token by hand.

The WHOLE token must parse, unlike `axl_json_get_int()`, which truncates at
the first non-digit so `1.5` yields 1. There is no sensible prefix of a float:
JSON5's `0x1F` would otherwise read as 0 and stop at the `x`. Use
`axl_json_get_int()` for hex.

An out-of-range magnitude is a **failure**, not an infinity. The underlying
`axl_str_to_double()` reports overflow as ±infinity and underflow as ±0.0
together with its error; this accessor does not pass those on, so `1e400`
returns false and leaves the caller's value untouched, exactly as an
out-of-range integer does. A caller who wants the IEEE result can read the
token with `axl_json_get_number_str()` and convert it.

`NaN` and `Infinity` read back as themselves when `AXL_JSON_ALLOW_NAN_INF` let
them into the document.

### Rendering an error

`axl_json_error_format()` turns an `AxlJsonError` into text. This is why the
struct stores a position and not a message: formatting at failure time would
run in the parser, in every build, and cap quality at a fixed buffer.

```c
char buf[AXL_JSON_ERROR_BUF_MAX];
if (!axl_json_parse(doc, len, AXL_JSON_RELAXED, &r)) {
    if (axl_json_error_format(axl_json_reader_error(&r), doc, len,
                              buf, sizeof(buf)) > 0) {
        // 3:10: ill-formed UTF-8
        // "b": "caf?"
        //          ^
    }
}
```

Size the buffer at `AXL_JSON_ERROR_BUF_MAX`: because the contract refuses
rather than truncating, that is the only size guaranteed never to return `-1`
— and the widest render is a full window of 4-byte characters, which is well
past 256. On `-1` the buffer is left empty rather than partial, so printing it
unconditionally is safe even if the check above is skipped.

Pass `NULL` for the document to get the terse `3:9: ill-formed UTF-8` alone —
which is also what a reader over a stream or callback source must use, since
the bytes are gone by then. A line too long to quote is **windowed** around the
column with `...` at each cut end; minified JSON is one line, so refusing would
make this useless on the documents machines produce. A TAB is copied into the
caret line as a TAB so the caret survives tab expansion. `AXL_JSON_OK` renders
as `no error`, and a buffer too small is refused rather than truncated.

The quote **substitutes `?` for every other control byte**. The document is
untrusted and this text goes to a console: a raw ESC would carry an ANSI
sequence out of a JSON body, a raw CR would return the cursor to column 0 and
wreck both the quote and the caret, and an embedded NUL would end the buffer
early so the returned length outran what a caller can read. One byte out per
byte in, so the caret still lines up.

`AXL_JSON_ERR_DIALECT` appends the flag that would have accepted the input —
`1:9: feature needs a dialect flag (pass AXL_JSON_ALLOW_COMMENTS)`. It is the
one recoverable code in the enum, and naming the code without the flag gives a
caller the half it cannot act on.

### Sources and sinks

Where bytes come from and where they go are two mirrored vtables — a function
pointer plus a context — rather than a family of entry points each with its own
flags-and-error plumbing.

```c
axl_json_source_init_mem     (&src, json, len);   // zero-copy — the fast path
axl_json_source_init_stream  (&src, stream);
axl_json_source_init_callback(&src, fn, ctx, hint);
axl_json_parse_source(&src, AXL_JSON_STRICT, &r);

axl_json_sink_init_string  (&snk, out);           // what writer_init does
axl_json_sink_init_buffer  (&snk, &state, buf, size);
axl_json_sink_init_stream  (&snk, stream);
axl_json_sink_init_callback(&snk, fn, ctx);
axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
```

Both are copied by value and need not outlive the call; whatever they point AT
must outlive the reader or writer.

**A contiguous source borrows; the others own.** `axl_json_source_init_mem` is
`axl_json_parse` under another name — the tokens index straight into your
buffer, which must therefore outlive the reader. A stream or callback source
accumulates the document into a buffer the READER owns, released by
`axl_json_free`, so a streamed parse is one object to free instead of two.
It saves no memory: tokens are 32-bit offsets, so every byte has to stay
resident at a stable position for the reader's life. The win is ergonomic.

**A full buffer is not a write failure.** The buffer sink stores what fits,
keeps counting, and `axl_json_writer_finish` reports `AXL_JSON_ERR_IO` once if
anything was dropped — so a truncated document is never mistaken for a complete
one, but the writer is not halted at the first byte over. That is what makes
two-pass sizing work:

```c
AxlJsonSink    snk;
AxlJsonBufSink st;
AxlJsonWriter  w;

axl_json_sink_init_buffer(&snk, &st, NULL, 0);   // sizing pass
axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
build(&w);
axl_json_writer_finish(&w);

size_t need = axl_json_writer_needed(&w);        // true size, always
char  *buf  = axl_malloc(need);                  // NOT NUL-terminated
axl_json_sink_init_buffer(&snk, &st, buf, need);
axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
build(&w);
axl_json_writer_finish(&w);
```

A sink that is BROKEN is different and does halt at once: it returns `-1`
instead of a short count. "The buffer is full" is a fact about the buffer;
"the sink is broken" is a fact about the world.

### Pretty Printing

Pass `AXL_JSON_INDENT(n)` at init for n-space-indent output with
newlines at every container and member boundary:

```c
axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));
// ... same writer calls ...
// {
//   "name": "AXL",
//   "version": 1,
//   "tags": [
//     "uefi",
//     "embedded"
//   ]
// }
```

### Iterating Arrays

```c
// Parse: {"items":["a","b","c"]}
AxlJsonArrayIter iter;
if (axl_json_array_begin(&r, "items", &iter)) {
    AxlJsonReader elem;
    while (axl_json_array_next(&iter, &elem)) {
        char value[32];
        axl_json_value_string(&elem, value, sizeof(value));
        axl_printf("  %s\n", value);
    }
}
```

An element is read with the **own-value** family (`axl_json_value_string`,
`_int`, `_uint`, `_bool`, `_number_str`, `_type`), not with the by-key
getters: an element has no key. This example used to pass `NULL` as the key
to `axl_json_get_string`, which returns false on the spot — a `NULL` key is
rejected, deliberately, so that a lookup which returned nothing cannot
silently turn into "operate on the root".

Use `axl_json_value_array_begin` for a root-level or nested array
(`[{...}, {...}]`). It replaced `axl_json_root_array_begin`, whose name was
wrong on the sub-reader it was mostly called on.

Element readers borrow the parent's tokens and document bytes and own
neither, so `axl_json_free` on one is a harmless no-op — but they are valid
only while the parent reader is.

### Iterating Objects

The only way to ask what keys an object *has* — every other accessor needs the
key you are already looking for.

```c
// Parse: {"host":"axl","port":8080}
AxlJsonObjectIter it;
if (axl_json_value_object_begin(&r, &it)) {       // or _object_begin(&r, "cfg", &it)
    char          key[64];
    AxlJsonReader val;
    while (axl_json_object_next(&it, key, sizeof(key), &val)) {
        axl_printf("  %s = %d\n", key, (int)axl_json_value_type(&val));
    }
}
```

Pairs arrive in **document order**, not sorted, and a **duplicate key is
yielded once per occurrence** — RFC 8259 permits duplicates and collapsing
them here would hide the thing an iterating caller may be looking for.

The key is **decoded** into your buffer, not borrowed: `{"\\u0041":1}` is a
key named `A`, so handing back raw bytes would reproduce the `\uXXXX` corruption
one layer up. A key too long is truncated and the pair is still yielded —
ending the walk over one oversized key would lose every later pair. If you
compare keys, size the buffer at least two bytes longer than the longest key
you compare against and a false match is unrepresentable. Pass `NULL`/`0` for
the key buffer to walk values only.

`AxlJsonObjectIter` holds the document by value, like `AxlJsonArrayIter`, so
reusing the value reader cannot retarget the iterator.

### Nested Objects

The flat getters look up keys in the reader's *current* object.
`axl_json_get_object` steps into a named child object and hands back a
sub-reader scoped to it; chain calls to reach deeper paths.

```c
// Parse: {"server":{"host":"localhost","tls":{"port":443}}}
AxlJsonReader server, tls;
int64_t port = 0;
if (axl_json_get_object(&r, "server", &server) &&
    axl_json_get_object(&server, "tls", &tls)) {
    axl_json_get_int(&tls, "port", &port);   // 443
}
```

The sub-reader composes with every accessor — `axl_json_get_string` /
`_int` / `_uint` / `_bool`, `axl_json_array_begin`, and
`axl_json_get_object` itself all operate relative to the nested object.
Like array elements, the sub-reader borrows the parent's tokens and
document bytes and owns neither, so `axl_json_free` on it is a harmless
no-op — but it stays valid only while the parent reader lives.

`axl_json_get_object` leaves `out` **untouched** when it returns false,
which is what licenses seeding it with a default and narrowing only if the
section turns out to be present:

```c
AxlJsonReader cfg = root;                  // default: read from the root
axl_json_get_object(&root, "tls", &cfg);   // narrow only if "tls" is an object
axl_json_get_int(&cfg, "port", &port);
```

### Round-Trip Transforms

`axl_json_write_token` splices an already-parsed token into the
writer's output, changing as little as correctness allows. The lexer
leaves escape sequences in source form, so `\uXXXX` and every other
ASCII escape survive untouched. What does change: ill-formed UTF-8 is
repaired, an unescaped `"` (only reachable from a JSON5 single-quoted
token) is escaped, `\'` loses an escape that means nothing between
double quotes, and `\<non-ASCII>` travels as one unit. Each of those
was a real defect before it was handled — see decisions 34 and 36 in
`docs/AXL-JSON-Design.md`. Useful for parse → mutate → re-emit flows:

```c
AxlJsonReader r;
axl_json_parse(input, input_len, AXL_JSON_RELAXED, &r);

AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
AxlJsonWriter w;
axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));

axl_json_obj_begin(&w);
    axl_json_kv_str(&w, "wrapped_in", "envelope");
    axl_json_key(&w, "original");
    axl_json_write_token(&w, &r, 0);   /* splice the entire input doc */
axl_json_obj_end(&w);
axl_json_writer_finish(&w);

axl_json_free(&r);
```

### Error Handling

The writer uses a sticky error flag. The first failure latches it;
subsequent calls become no-ops; one check after `axl_json_writer_finish`
is sufficient.

Sources of writer error:

- **Output failure** (`AXL_JSON_ERR_IO`) — the sink refused. That covers an
  `AxlString` that could not grow, a stream that would not take the bytes, and
  a callback that returned `-1`. A sink reports failure and not a reason, so
  the writer does not guess at one; `AXL_JSON_ERR_NO_MEMORY` stays read-side,
  where the allocation is AXL's own.
- **A fixed buffer that filled up** — reported ONCE, by
  `axl_json_writer_finish`, rather than at the first byte over. See
  "Sources and sinks".
- **Structural misuse** the writer can detect:
  - emit a SECOND root value (one value is a document; two concatenated
    values are not). A single bare-primitive root is legal — `42` and
    `"text"` are complete JSON texts under RFC 8259 §2, and both the
    reader and the writer treat them as such.
  - emit a key at depth 0, where no object is open
  - emit a value outside any container after the root has closed
  - emit a key inside an array
  - emit a value when a key was expected in object context
  - `axl_json_obj_end` on an open array (or vice versa)
  - close when nothing is open
  - mismatched begin/end counts at `axl_json_writer_finish`

What the writer does **not** catch:

- Duplicate keys in the same object — JSON technically allows them; the
  writer doesn't track emitted keys.
- The contents of `axl_json_raw(&w, fragment)` — the caller asserts the
  fragment is valid JSON; the writer splices it as-is.

#### String encoding

Everything the writer emits is well-formed UTF-8, whatever the caller
passes in — keys and values (NUL-terminated and counted), tokens spliced
by `axl_json_write_token`, and comment bodies. A JSON text is defined
over Unicode code points (RFC 8259 §8.1), so a single ill-formed sequence
invalidates the whole document. Well-formed UTF-8 passes through
byte-for-byte (§7 requires escaping only `"`, `\` and `0x00-0x1F`);
ill-formed bytes become U+FFFD, one per bad byte, matching
`axl_utf8_decode`'s resynchronization contract rather than adding a
validator of its own.

Callers do not need to pre-validate. Strings reaching a writer are
routinely outside the caller's control:

- `axl_smbios_get_string_utf8` returns a direct pointer into firmware
  table memory — vendor tables carry latin-1 in the wild.
- Anything truncated to a byte budget can be cut mid-sequence.
  `axl_log`'s message buffer is one such producer.

`axl_json_strn` / `axl_json_keyn` never read past `n`, so a sequence cut
by the count is repaired rather than silently completed from whatever
bytes follow it in the caller's buffer.

`axl_json_write_token` keeps an ASCII escape verbatim — the parser keeps
escape sequences in source form, so `\uXXXX` survives untouched. Ill-formed
bytes ≥ 0x80 are replaced, and the three JSON5-only shapes above (an
unescaped `"`, `\'`, and `\<non-ASCII>`) are rewritten into forms that
mean the same thing between double quotes.
This matters because the parser validates no encoding unless asked to
(`AXL_JSON_UTF8_STRICT`): without that, a
re-serialized document would carry a source document's bad bytes out.

`axl_json_escape_string` behaves the same way. Note U+FFFD is 3 bytes
where the input byte was 1, so ill-formed input can overflow an output
buffer sized for the raw bytes; that returns `-1` like any other
truncation.

### JSON5 Support

[JSON5](https://json5.org) is a strict superset of JSON aimed at
human-edited config files. AXL accepts the JSON5 grammar on the reader
side and emits JSON5-flavored extras (trailing commas, comments) on the
writer side, both driven by the same flag bits.

**Every reader entry point NAMES its dialect.** `axl_json_parse` and
`axl_json_load_file` take the flags word as a parameter; there is no
default and no no-flags twin. Both used to have one, defaulting to
`AXL_JSON_RELAXED` because `0` would have meant strict — which made the
liberal dialect something you got by not asking. Pass `AXL_JSON_RELAXED`
for the whole JSON5 grammar, `AXL_JSON_STRICT` to validate as RFC 8259.

`AXL_JSON_STRICT` means it. Unquoted keys, hex literals, single quotes and
trailing commas were accepted under it before P3, because strict parsing ran
on a permissively-compiled jsmn; they are refused now that everything runs
on the one lexer.

**Flags are one 64-bit `AxlJsonFlags` space shared by reader and writer**, so
a dialect means the same thing in both directions. Each JSON5 feature has its
own `AXL_JSON_ALLOW_*` bit and `AXL_JSON_JSON5` is the OR of all of them — a
consumer that wants comments in its config files need not also accept
single-quoted strings and hex literals. Presets: `AXL_JSON_STRICT` (0, RFC
8259), `AXL_JSON_JSON5`, `AXL_JSON_RELAXED`.

**Reader — opt in with `AXL_JSON_JSON5`:**

```c
const char *cfg =
    "// jedec.json5 — vendor codes per JEDEC JEP-106\n"
    "{\n"
    "  vendors: [\n"
    "    { code: 0x802C, name: 'Micron'  },\n"
    "    { code: 0x80AD, name: 'Hynix'   },  // trailing comma below\n"
    "    { code: 0x80CE, name: 'Samsung' },\n"
    "  ],\n"
    "}\n";

AxlJsonReader r;
if (!axl_json_parse(cfg, axl_strlen(cfg),
                    AXL_JSON_JSON5, &r)) {
    /* parse error */
}
/* All the standard accessors (axl_json_get_string,
   axl_json_array_begin, axl_json_array_next, ...) work
   unchanged — JSON5 is normalized at parse time. */
axl_json_free(&r);
```

For sidecar files there's a one-shot:

```c
AxlJsonReader  r;
void          *raw;
size_t         raw_len;

if (axl_json_load_file("jedec.json5", AXL_JSON_JSON5,
                       &r, &raw, &raw_len)) {
    /* ... use r ... */
    axl_json_free(&r);
    axl_free(raw);
}
```

JSON5 features the parser accepts:

- Line comments (`//`) and block comments
- Trailing commas in objects and arrays
- Single-quoted strings (`'text'`)
- Unquoted (identifier-name) object keys
- Hex number literals (`0x...`) and `+` / `-` number prefix
- Extended string escapes: `\'`, `\v`, `\0`, `\x##`, line continuations.
  `\0` and `\x00` decode to U+FFFD, not a NUL — see
  **UTF-8 Encoding** above for why an interior NUL is refused.

Every flag value goes through the same parser, so each of those features
is gated individually: `AXL_JSON_ALLOW_COMMENTS` alone permits comments
and nothing else. `test/unit/axl-test-data.c` proves it with an N×N
rejection matrix — search `matrix:` — whose load-bearing half is the
negative one, since a lexer that ignored the flag word entirely would
pass every positive case.

**`AXL_JSON_STRICT` is RFC 8259**, verified against all 316 embedded
JSONTestSuite cases in `test/unit/axl-test-json-conformance.c`: every
`y_` accepted, every `n_` rejected. It rejects what the standard forbids
and nothing more — a bare-primitive root and duplicate object keys are
both accepted, because RFC 8259 permits them.

Two deliberate narrowings, both documented at their assertion:

- **Nesting is bounded** to `AXL_JSON_DEPTH_DEFAULT` (32) levels, raisable
  to `AXL_JSON_DEPTH_MAX` (256) via `AXL_JSON_DEPTH(n)`. This was a stack
  budget: the parser was recursive descent, so nesting depth was stack
  depth, and AXL is freestanding with no guard page — unbounded, the
  corpus's 100000-nested-array document needed ~12.8 MB of stack and
  *faulted* instead of failing. Removing the bound was tried, and the test
  binary stalled; the crash was real, not hypothetical.

  Since P12e neither face recurses. The scanner tracks one bit per open
  container (32 bytes covers all 256 levels); the whole-document face adds
  8 bytes per open container, heap-allocated per parse and sized to the
  *resolved* limit — 256 bytes at the default 32. So the bound is now a
  policy number. It stays because accepting arbitrary nesting is still a
  choice a caller should make deliberately, not because the alternative is
  a fault.
- **A UTF-16 or BOM-prefixed document is refused.** RFC 8259 §8.1
  requires UTF-8 for interchange and AXL does not sniff or transcode.

Until the granular redesign's P3 phase, strict parsing ran on a vendored
jsmn compiled without `JSMN_STRICT` — i.e. the branch that was supposed
to mean "strict" was the permissive one. It refused exactly one of the
eight JSON5 features (a `\x` escape) and tolerated the rest, and it
rejected a comment only *before* the root: `{"a":1 /* c */}` parsed, and
`{/* c */"a":1}` "parsed" into a garbage token tree, so the key was
silently unretrievable — accept **and** misparse. That is what deleting
it fixed. See `docs/AXL-JSON-Design.md`.

**Scanner — pull events instead of building a document.**

`AxlJsonScanner` is the streaming read face. It walks the same grammar
as `axl_json_parse` — the whole-document face *is* this scanner run to
completion — but retains nothing: O(depth) memory rather than
O(tokens), so reading one key out of a large sidecar never materializes
the rest of it.

```c
AxlJsonSource  src;
AxlJsonScanner s;
AxlJsonEvent   ev;

axl_json_source_init_mem(&src, doc, len);
axl_json_scanner_init(&s, &src, AXL_JSON_JSON5);
while (axl_json_scanner_next(&s, &ev) && ev.kind != AXL_JSON_EV_EOF) {
    if (ev.kind == AXL_JSON_EV_KEY && axl_json_event_equals(&ev, "port")) {
        axl_json_scanner_next(&s, &ev);        // the value follows its key
        break;                                 // stop; nothing is owed
    }
    axl_json_scanner_skip(&s);                 // discard this subtree
}
if (axl_json_scanner_error(&s)->code != AXL_JSON_OK) { /* ... */ }
axl_json_scanner_free(&s);                     // required even here
```

Four things that are easy to get wrong from the outside:

- **`ev.text` is borrowed, short-lived and NOT NUL-terminated.** It is
  raw source bytes with escapes intact, valid only until the next
  `next()`. Use `ev.len`, `axl_json_event_string()` to decode, or
  `axl_json_event_equals()` to compare without a buffer.
- **`ev.depth` counts containers OUTSIDE the event**, so a container's
  BEGIN and its matching END report the same number. That is what makes
  `begin.depth == end.depth` the way to match them.
- **`AXL_JSON_EV_EOF` is a document BOUNDARY, not end of input.** Keep
  calling `next()` and you get the next document's events, which is all
  NDJSON needs — no flag. `false` means exhausted *or* failed; ask
  `axl_json_scanner_error()` which.
- **`_free()` is required** even for a contiguous source, which allocates
  nothing today. Demanding it now is what lets a stream mode start owning
  a buffer later without auditing callers.

Trailing bytes are the caller's policy here. `axl_json_parse` is
the caller that wants exactly one document, which is why
`AXL_JSON_ERR_TRAILING` comes from it and never from the scanner.

**Over a PULL source** — an `AxlJsonSource` with a `read` function instead
of a contiguous view — the scanner owns a window it refills, and memory is
**O(largest single token)** rather than O(document):

```c
axl_json_source_init_callback(&src, my_read, my_ctx, 0);
axl_json_scanner_init(&s, &src, AXL_JSON_JSON5);
while (axl_json_scanner_next(&s, &ev)) { ... }   // identical loop
axl_json_scanner_free(&s);                       // now really frees something
```

The event stream does not depend on the chunking: the same bytes give the
same events, and the same error code, offset, line and column, whether they
arrive all at once or one byte at a time. That is asserted by sweeping a
differential across chunk sizes, not merely intended.

A token that straddles a refill is **re-scanned from its start** rather than
resumed mid-way — the rule .NET's `Utf8JsonReader` documents and expat
implements — so the five leaf scanners are shared with the contiguous path
instead of forked into resumable state machines. Consequences worth knowing:

- A single token must fit in the window, which grows to fit it. A comment
  counts as a token, so a 10 MB block comment costs 10 MB.
- `AXL_JSON_ERR_IO` (the read function returned -1) and
  `AXL_JSON_ERR_NO_MEMORY` (the window could not grow) join the codes this
  face can produce. Neither is reachable over a contiguous view.
- `axl_json_scanner_consumed()` counts what the GRAMMAR consumed, which is
  less than what was pulled — the scanner reads ahead, and those bytes are
  inside it. There is no handing the remainder elsewhere; keep scanning with
  the same scanner.
- Pass `NULL`/`0` for the document to `axl_json_error_format()`: `offset` is
  input-relative and the bytes it names have usually scrolled out of the
  window, so any chunk you still hold is a different coordinate space.

**Writer — emit JSON5 extras with `AXL_JSON_ALLOW_TRAILING_COMMA`
and `axl_json_comment`:**

```c
AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
AxlJsonWriter w;
axl_json_writer_init(&w, out,
    AXL_JSON_INDENT(2) | AXL_JSON_ALLOW_TRAILING_COMMA);

axl_json_obj_begin(&w);
    axl_json_comment(&w, "generated — do not edit by hand");
    axl_json_kv_str(&w, "name",    "AXL");
    axl_json_kv_int(&w, "version", 1);
axl_json_obj_end(&w);
axl_json_writer_finish(&w);

/* {
 *   // generated — do not edit by hand
 *   "name": "AXL",
 *   "version": 1,
 * }
 */
```

Comment behavior:

- Pretty mode emits `// text` on its own line at the current indent.
- Compact mode emits an inline `/​* text *​/` block; embedded
  close-comment sequences are split so the comment can't terminate
  early.
- Embedded newlines truncate the comment.
- Comments don't disturb the writer's container state — interleave
  them freely between values, between key+value pairs, or as the
  first/last item in a container.

The writer deliberately does **not** emit unquoted object keys or
single-quoted strings. Those are JSON5 input-side conveniences only;
emitting them adds escape-correctness footguns with no consumer
benefit.

#### JSON5 conformance notes

`Infinity`, `-Infinity`, `NaN` and `-NaN` ARE lexed, behind
`AXL_JSON_ALLOW_NAN_INF` (`+Infinity` and `+NaN` need
`AXL_JSON_ALLOW_PLUS_SIGN` as well — the sign is that flag's feature).

This is deliberately NOT IEEE 754 support, and the distinction is the whole
of it: AXL JSON has no `axl_json_get_double`, so there is no accessor these
could be converted *for*. Not that a `double` is unrepresentable — `axl_dtoa`
is public in `axl-format.h` and `%f`/`%e`/`%g` exist. What is missing is the
READ side, correctly-rounded decimal-to-double, which has no `strtod`
equivalent here. They are primitive TOKENS, reachable
only as text via `axl_json_get_number_str()`. `axl_json_get_int` and
`axl_json_get_uint` refuse them outright — there is no integer they could
mean.

`axl_json_get_number_str` is the general escape hatch for any number those two
must refuse, not just these: a literal wider than 64 bits, a fraction, an
exponent. It hands back the document's own bytes, so `1e10` stays `"1e10"` and
is never normalized to `"10000000000"`. It refuses rather than truncates when
the buffer is too small, unlike `axl_json_get_string` — a clipped string is
incomplete, a clipped number is a different number.

`axl_json_get_int` on a fractional or scientific-notation token still
truncates at the first non-digit (long-standing behavior, unchanged):
`{x: 1.5}` returns `1`. The integral part is bounds-checked, so
`{x: 12345678901234567890.5}` is rejected rather than wrapped — and
`get_number_str` is how you read it losslessly.

The rest of AXL's JSON5 support is scoped to what matters for firmware-edited
sidecar configs (the in-tree consumer is `tools/memspd.c` reading
`share/jedec.json5`). These parts of the [json5.org](https://json5.org) spec
are intentionally **not** supported:

- **Unicode `IdentifierName` for unquoted keys** — only the ASCII
  subset (`[A-Za-z_$][A-Za-z0-9_$]*`) is recognized. Keys with
  Unicode letters, combining marks, ZWNJ, or ZWJ require
  quoting (single or double quotes both work).

- **Unicode whitespace** (U+00A0 NBSP, U+FEFF BOM, U+2028 LS,
  U+2029 PS, and other `Space_Separator` characters) — only the
  ASCII whitespace set plus `\v` and `\f` is treated as
  insignificant. Documents using Unicode separators as whitespace
  will fail to parse.

These gaps are deliberate — adding lex support for grammar that no firmware
tool actually authors (Unicode identifier keys, Unicode whitespace) would
expand the attack surface and surprise consumers without unlocking new use
cases. Note this rationale no longer covers "tokens that can't be retrieved":
`axl_json_get_number_str` retrieves any number losslessly, which is what made
`AXL_JSON_ALLOW_NAN_INF` worth having. Open an issue if a real consumer needs any of these and
they can be revisited.

### Console Output

```c
const char *body = http_response_body;
axl_json_console_print(body, axl_strlen(body));
```

Writes to the UEFI console with cyan keys, green strings, yellow
numbers, magenta booleans. Distinct from the writer's pretty-print
flag, which emits to a buffer without color.

## AxlXml — Streaming XML writer + pull-token reader

Streaming XML writer over an `AxlString` plus a pull-token reader
over a byte buffer. Caller manages namespaces (qnames like
`D:multistatus` are opaque to the writer; namespace declarations
are normal attributes). Out of scope: DTD validation, XSD, RelaxNG,
XPath, XSLT, XML signatures. UTF-8 only.

Header: `<axl/axl-xml.h>`

### Overview

Two independent APIs:

- **Writer** (`AxlXmlWriter`) — value-typed state machine that
  builds XML into a caller-owned `AxlString`. Mirrors
  `AxlJsonWriter`'s shape: init takes flags bitmask, emitters
  return void, errors are sticky and checked at `_finish`.
  Auto-escapes `<` `>` `&` in body text and `<` `&` `"` in
  attribute values. Tag balance and start-tag-open windows are
  enforced; misuse sets the sticky error flag.
- **Reader** (`AxlXmlReader`) — opaque pull-token reader. One
  `AXL_XML_TOKEN_{START_ELEMENT, END_ELEMENT, TEXT, END_DOCUMENT}`
  per `axl_xml_reader_next` call. Attribute lookup via
  `axl_xml_reader_attr` while positioned at a `START_ELEMENT`.
  Entity decoding (5 named + `&#NNN;` / `&#xHH;`) into a
  reader-owned scratch buffer reused per token. CDATA passes
  through with `is_cdata` set. Comments, processing instructions,
  and DOCTYPE declarations are skipped silently.

### Writing XML

```c
AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
AxlXmlWriter w;

axl_xml_writer_init(&w, out, AXL_XML_DEFAULT);
axl_xml_writer_prologue(&w);
axl_xml_writer_start_element(&w, "D:multistatus");
axl_xml_writer_attribute(&w, "xmlns:D", "DAV:");
    axl_xml_writer_start_element(&w, "D:response");
        axl_xml_writer_start_element(&w, "D:href");
        axl_xml_writer_text(&w, "/dav/file.txt");  // auto-escaped
        axl_xml_writer_end_element(&w);
    axl_xml_writer_end_element(&w);
axl_xml_writer_end_element(&w);
axl_xml_writer_finish(&w);

if (!axl_xml_writer_error(&w)) {
    axl_printf("%s\n", axl_string_str(out));
}
```

Pass `AXL_XML_INDENT(n)` at init for an `n`-space indent + newlines between
child elements; text-only elements stay on one line. `AXL_XML_DEFAULT` (zero)
is compact, and `AXL_XML_INDENT(0)` is a different thing again — newlines with
a zero-width indent, which is why the presence bit is separate from the width.

**`AxlXmlFlags` shares its layout with `AxlJsonFlags` deliberately.**
`AXL_XML_INDENT` is bit-for-bit `AXL_JSON_INDENT`, so handing one writer the
other's indent request is simply correct rather than a trap. It used to be a
trap: `AXL_XML_WRITER_PRETTY` was `1 << 0`, the same bit as
`AXL_JSON_ALLOW_COMMENTS`, and because both are plain integers in C the mistake
compiled and silently asked for something else. A distinct typedef would not
have helped — `uint64_t` is `uint64_t` — so the fix is that the same bit means
the same thing, plus `axl_xml_writer_init` refusing any bit XML does not
define (`AXL_XML_KNOWN_MASK`) instead of ignoring it.

### Reading XML

```c
const char *xml = "<root><greet lang=\"en\">hello &amp; world</greet></root>";
AxlXmlReader *r = axl_xml_reader_new(xml, axl_strlen(xml));
AxlXmlToken t;

while (axl_xml_reader_next(r, &t)) {
    switch (t.type) {
    case AXL_XML_TOKEN_START_ELEMENT:
        axl_printf("START: %.*s\n", (int)t.name_len, t.name);
        const char *lang = axl_xml_reader_attr(r, "lang");
        if (lang != NULL) {
            axl_printf("  lang=%s\n", lang);
        }
        break;
    case AXL_XML_TOKEN_TEXT:
        axl_printf("TEXT: %.*s\n", (int)t.text_len, t.text);
        break;
    case AXL_XML_TOKEN_END_ELEMENT:
        axl_printf("END: %.*s\n", (int)t.name_len, t.name);
        break;
    case AXL_XML_TOKEN_END_DOCUMENT:
        break;
    }
}

uint32_t line, col;
const char *msg;
if (axl_xml_reader_error(r, &line, &col, &msg)) {
    axl_printerr("parse error at %u:%u: %s\n", line, col, msg);
}
axl_xml_reader_free(r);
```

Token `name` / `text` pointers reference reader-owned storage and
are invalidated on the next `axl_xml_reader_next` call. Copy out
anything you need to keep.

### Entity decoding + safety

The reader decodes the five named entities (`&amp;` `&lt;` `&gt;`
`&quot;` `&apos;`) plus decimal and hex numeric character
references. UTF-8 encoded for codepoints ≥ 0x80. Per XML 1.0 §4.1:
references to U+0000 and to UTF-16 surrogates (U+D800..U+DFFF) are
rejected as parse errors — both would otherwise corrupt downstream
C-string handling or produce invalid UTF-8.

DOCTYPE declarations are tokenized and skipped, but the reader
balances `[` and `]` brackets across the declaration so any
internal entity definitions are NEVER processed. This closes the
billion-laughs / external-entity attack classes without any
content-side checks.

### Well-formedness

Strict by construction:

- Tag balance enforced (mismatched end tag → error).
- Exactly one root element (content after root → error).
- Non-whitespace content before / after the root → error.
- Tag nesting capped at 64 levels in both writer and reader.

### Status code

X1 (writer) shipped 2026-05-10. X2 (reader) shipped 2026-05-10.
WebDAV `axl_http_server_add_webdav`'s PROPFIND emit migrated to
`AxlXmlWriter` in X3 the same day; class-2 verb bodies
(PROPPATCH / LOCK / UNLOCK) become implementable via the reader
when a consumer asks.

## AxlCache — TTL Cache

TTL cache with LRU eviction. Fixed-size slots, string keys, opaque
fixed-size values. Designed for single-threaded UEFI use.

Header: `<axl/axl-cache.h>`

### Overview

AxlCache is a simple key-value store with time-based expiration and
least-recently-used eviction when full. Values are **copied** into
fixed-size slots (not stored as pointers).

Use cases: DNS resolution caching, HTTP response caching, SMBIOS
lookup caching.

```c
// Cache up to 16 entries, each 4 bytes (e.g., IPv4 addresses)
AXL_AUTOPTR(AxlCache) cache = axl_cache_new(16, sizeof(uint32_t), 60000);
//                                           ^    ^                ^
//                                     slots  value size      TTL (ms)

// Store a value
uint32_t addr = 0xC0A80101;  // 192.168.1.1
axl_cache_put(cache, "gateway", &addr);

// Retrieve
uint32_t result;
if (axl_cache_get(cache, "gateway", &result) == 0) {
    // hit -- result contains the cached value
}

// Invalidate a specific entry
axl_cache_invalidate(cache, "gateway");

// Invalidate all entries
axl_cache_invalidate_all(cache);
```

## AxlPageCache — LRU Page Cache

A fixed-capacity LRU cache of equal-sized pages backed by a
caller-supplied fill function. Unlike AxlCache (TTL, string keys,
copy-in/copy-out values), this is capacity-only, integer-indexed, and
**zero-copy**: a lookup returns a borrowed pointer into the resident
frame. On a miss the least-recently-used frame is evicted and refilled
in place. It knows nothing about files — the fill function decides
where the bytes come from — so it windows any large, randomly-addressed
backing store where only the hot pages should stay resident. It is the
mechanism behind [AxlFileView](../fs/README.md).

```c
// Frame size 4 KiB, 8 resident frames; fill from some backing store.
static int64_t fill(size_t page, void *dst, size_t cap, void *user) {
    return load_page(user, page, dst, cap);   // bytes written, or -1
}

AXL_AUTOPTR(AxlPageCache) pc = axl_page_cache_new(4096, 8, fill, backing);

size_t valid = 0;
const uint8_t *p = axl_page_cache_get(pc, page_index, &valid);
// p points into a resident frame; valid until the next get/clear.

AxlPageCacheStats st;
axl_page_cache_stats(pc, &st);   // hits / misses / evictions / fills
```

**Multi-tenant mode.** `axl_page_cache_new_shared(page_size, max_frames)`
makes a fill-less cache that several owners share: `axl_page_cache_fetch(pc,
owner, page, fill, user, &valid)` keys frames by `(owner, page)` and takes
the fill per call, so one bounded frame budget serves many backing stores
at once (every open file in an editor). `axl_page_cache_drop_owner(pc,
owner)` returns a closing owner's frames to the pool; eviction is global
LRU across all owners. (The single-tenant `axl_page_cache_get` is this same
primitive with the cache as its own owner.) `AxlFileView` /
`AxlPieceTree`'s `*_open_cached` variants are built on it.

## AxlTextBuffer — Editable Text Store

A growable, editable byte buffer with an integral line index, tuned for
an interactive text editor: load a file once, then many small
inserts/deletes near a moving cursor, with O(log n) byte-offset ↔ line
mapping queried every keystroke and once per visible line per frame.

Storage is a **gap buffer** (O(1) amortized edits at the gap). The
**line index** is a sorted array of newline offsets maintained
incrementally on every edit — never a full rescan — so `line_of_offset`
and `line_bounds` are binary searches. The store is byte-oriented:
`'\n'` is the only special byte; UTF-8 / codepoint policy is the
caller's. A gap buffer is not contiguous, so content is read out via
`axl_text_buffer_get` rather than a pointer.

```c
AXL_AUTOPTR(AxlTextBuffer) tb = axl_text_buffer_new(0);
axl_text_buffer_set_bytes(tb, "ab\ncd\nef", 8);   // 3 lines

axl_text_buffer_insert(tb, 2, "X", 1);            // edit near the cursor
size_t line = axl_text_buffer_line_of_offset(tb, 4);

size_t start, end;
axl_text_buffer_line_bounds(tb, line, &start, &end);   // [start,end) excl. '\n'

char out[64];
size_t n = axl_text_buffer_get(tb, start, end - start, out, sizeof(out));

AxlMatch m;                                        // {start, length}
if (axl_text_buffer_find(tb, "cd", 2, 0, AXL_FIND_DEFAULT, &m)) { /* m.start */ }
```

`axl_text_buffer_find` mirrors `axl_piece_tree_find` (case-insensitive /
backward / whole-word; matches that straddle the gap are handled) — both
are thin wrappers over the shared `axl_find_in_source` engine
(`<axl/axl-find.h>`).

For very large / out-of-core files, use **AxlPieceTree** below; the gap
buffer is the memory-resident store.

## AxlRBTree — Intrusive Augmented Red-Black Tree

A generic, **intrusive** red-black tree: the caller embeds an
`AxlRBNode` in its own struct (the tree never allocates nodes) and
descends to the insertion point itself, so the same tree serves ordered
maps, order-statistic trees, and weighted positional trees. An optional
`recompute` callback maintains a cached subtree aggregate (size,
byte/newline sums, …) across every structural change in O(log n).
Distinct from **AxlTree** (a non-intrusive key→value AVL map); this is
intrusive and augmentable. It is the substrate behind **AxlPieceTree**.
Reimplemented under Apache-2.0 from the textbook algorithm — no GPL
source. See `docs/AXL-RBTree-Design.md`.

```c
typedef struct { AxlRBNode node; int key; size_t sub_count; } Ent;
static void recompute(AxlRBNode *n, void *u) {
    Ent *e = AXL_RB_ENTRY(n, Ent, node);
    e->sub_count = 1
        + (n->left  ? AXL_RB_ENTRY(n->left,  Ent, node)->sub_count : 0)
        + (n->right ? AXL_RB_ENTRY(n->right, Ent, node)->sub_count : 0);
}

AxlRBTree t;
axl_rb_tree_init(&t, recompute, NULL);
// caller descends to the slot, then links + rebalances:
AxlRBNode **link = &t.root, *parent = NULL;
while (*link) { parent = *link;
    link = (e->key < AXL_RB_ENTRY(parent, Ent, node)->key)
         ? &parent->left : &parent->right; }
axl_rb_link_node(&e->node, parent, link);
axl_rb_insert(&t, &e->node);     // recompute propagates the subtree sums
```

## AxlPieceTree — Out-of-Core Editable Buffer

An **out-of-core, editable** text buffer for large files. The original
file is never loaded whole — its bytes are read on demand through
[AxlFileView](../fs/README.md) — while edits accumulate in an
append-only add buffer. A balanced tree of pieces (spans into either
source, held in an **AxlRBTree** augmented with subtree byte and newline
sums) presents one logical document with O(log n) offset↔line mapping
and O(log n) edits, so editing a multi-gigabyte file costs memory
proportional to the edits, not the file. This is the structure VS Code
calls a "piece tree." Line semantics match **AxlTextBuffer** exactly
(interchangeable for a renderer). See `docs/AXL-PieceTree-Design.md`.

```c
AXL_AUTOPTR(AxlPieceTree) pt = axl_piece_tree_open("fs0:\\big.log", 0, 0);
axl_piece_tree_insert(pt, 50000, "hello\n", 6);   // O(log n), no big copy

char out[64];
size_t n = axl_piece_tree_get(pt, 50000, 6, out, sizeof(out));
size_t line = axl_piece_tree_line_of_offset(pt, 50000);
axl_piece_tree_save(pt, "fs0:\\big.log");          // crash-safe, streamed

size_t at, sel;                                    // unlimited by default
axl_piece_tree_undo(pt, &at, &sel);                // &at/&sel locate the change
axl_piece_tree_redo(pt, NULL, NULL);               // (out-params optional)
```

**Undo/redo is built in** and unlimited by default
(`axl_piece_tree_set_undo_limit` to cap or disable). `undo`/`redo` report
where the change landed (`affected_offset` + `affected_len` — non-zero to
re-select restored text, zero for a net deletion) so the editor can place
the caret/selection at the edit site. Because the original
and add buffers are immutable/append-only, the bytes needed to reverse an
edit are never discarded — undo records are tiny span deltas, not text
copies. Three undo-grouping tools, three distinct jobs:
`axl_piece_tree_undo_group_begin`/`_end` (nestable, depth-counted) is the
explicit **atomic-transaction** bracket — every edit between them undoes
as one step; use it for imperative multi-edit ops (paste, find-replace-all,
multi-cursor). `axl_piece_tree_apply_edits` does the same for a batch you
already hold as an `AxlEdit[]` (one group, with offset adjustment) — prefer
it when the edits are known up front. `axl_piece_tree_undo_checkpoint` is
the *opposite* model: **accumulate-until-break** keystroke coalescing,
where consecutive edits merge until the editor declares a boundary (a
pause, a cursor jump, a type↔delete switch) for VS Code-like smart
grouping — use it for live typing, not for bracketing a transaction.
*What* to group is always the editor's policy; the buffer supplies the
mechanism.

**Editor-substrate helpers** layer on top for a full editor:
`axl_piece_tree_find` searches a byte substring across the virtual
document (cross-piece) with case-insensitive / backward / whole-word
flags and reports an `AxlMatch` (`{start, length}`). It is a thin
wrapper over the shared search engine `axl_find_in_source`
(`<axl/axl-find.h>`), which runs Boyer–Moore–Horspool over an abstract
`AxlByteReader` — the same engine `axl_text_buffer_find` uses, so a gap
buffer and a piece tree share one matcher (windowing with overlap so a
match straddling a piece boundary or the gap is never missed; a
contiguous source is scanned in place via the reader's `peek`).
`axl_piece_tree_is_modified` is a save-point-aware dirty flag;
`axl_piece_tree_apply_edits` applies a batch of original-coordinate edits
(replace-all, multi-cursor) as one undo group; and the
`axl_piece_tree_line_iter_*` iterator walks every line in one O(n) pass
(`_line_iter_init_at` starts at a given line for a deep viewport).
Caret support: `undo`/`redo` report the affected range, the
`axl_piece_tree_cp_align` / `_cp_next` / `_cp_prev` helpers step UTF-8
codepoint boundaries, and `axl_piece_tree_get_alloc` copies a range out as
a fresh NUL-terminated buffer.
For files that aren't plain UTF-8, `axl_piece_tree_load_encoded` detects
the encoding (UTF-8 ± BOM, UTF-16 LE/BE), decodes to a UTF-8 document
(plain UTF-8 stays out-of-core; others transcode in), and reports the
encoding + BOM so `axl_piece_tree_save_encoded` round-trips them.
`axl_piece_tree_detect_eol` classifies the line endings (LF / CRLF / CR /
MIXED) and `axl_piece_tree_set_eol` makes save normalize every terminator
to a chosen style while streaming (conversion without materializing the
document); `line_bounds` and the line iterator exclude a CRLF's trailing
`\r` so a renderer sees clean content. The document stays `\n`-indexed
internally — only `\n` delimits lines for `line_count` / `line_of_offset`.
`axl_piece_tree_set_read_only` freezes the buffer (insert / delete /
apply_edits return `AXL_ERR`; reads, search, and save still work).
`axl_piece_tree_backing_changed` reports whether the backing file's size
or mtime changed on disk since open, so an out-of-core editor can detect
an external edit (or deletion) and offer a reload. For many files at once,
`axl_piece_tree_open_cached(path, cache)` opens out-of-core sharing a
caller-owned [`AxlPageCache`](#axlpagecache--lru-page-cache) so one bounded
frame budget covers every open document.

**Saving over the open file (rebase).** There is deliberately no in-place
"save over yourself": for an out-of-core document, overwriting the file it
reads from would invalidate the resident `ORIGINAL`-piece offsets (and the
add-buffer-relative undo log) mid-flight. So the consumer composes the
existing primitives and chooses the policy:

```c
// Save over the open file — a rebase: write a sibling temp, drop the
// document, move the temp into place, reopen. Undo history resets; the
// reopened document is a clean, single-original-piece buffer (bounded).
axl_piece_tree_save_encoded(pt, "fs0:\\F.savetmp", enc, bom);  // preserve encoding/EOL
axl_piece_tree_free(pt);
axl_file_move("fs0:\\F.savetmp", "fs0:\\F");
pt = axl_piece_tree_load_encoded("fs0:\\F", 0, 0, &enc, &bom); // clean, not modified
// (then axl_piece_tree_set_eol(pt, axl_piece_tree_detect_eol(pt)) to keep the EOL mode)

// Save-As — keep editing: just save to a new path. The document and its
// undo history are untouched; it is marked clean against the new file.
axl_piece_tree_save_encoded(pt, "fs0:\\F-copy", enc, bom);
```

Use `save_encoded` + `load_encoded` (not the plain `save`/`open`) for the
rebase so the file's encoding and BOM survive it; re-apply `set_eol` after
the reopen if you converted line endings. (Both saves are crash-safe —
they write to `<path>.tmp` and rename — so the sibling-temp name just needs
to differ from the original.) Caret / scroll / selection / read-only state
are the editor's to snapshot and restore around the free→move→reopen; undo
necessarily resets (the add-buffer offsets it referenced are gone).

```c
AxlEncoding enc; bool bom;
AXL_AUTOPTR(AxlPieceTree) doc = axl_piece_tree_load_encoded(
    "fs0:\\notes.txt", 0, 0, &enc, &bom);   // detects + decodes
AxlMatch hit;
if (axl_piece_tree_find(doc, "TODO", 4, 0, AXL_FIND_CASE_INSENSITIVE, &hit)) {
    /* hit.start / hit.length locate the match */
}
axl_piece_tree_save_encoded(doc, "fs0:\\notes.txt", enc, bom);  // same form back
```

## AxlFind — Byte-Substring Search

A Boyer-Moore-Horspool substring search that runs over an abstract
**`AxlByteReader`** — a tiny function table over whatever holds the
bytes. The one engine (`axl_find_in_source`) therefore drives a flat
memory block (the built-in `AxlMemReader`), an **AxlTextBuffer** (gap
buffer), and an **AxlPieceTree** (out-of-core piece table); the
`axl_text_buffer_find` / `axl_piece_tree_find` wrappers just build the
right reader. The reader pulls overlapping windows, so a match
straddling the source's internal boundaries (a piece edge, the gap) is
never missed; a contiguous source that supplies the optional `peek` is
scanned in place with no copy. Forward and backward, case-insensitive,
and whole-word variants. A hit is an `AxlMatch` — `start` + `length`,
with the length carried explicitly so the same result shape fits
variable-length matchers (see AxlRegex).

```c
AxlMemReader r;
axl_mem_reader_init(&r, text, len);
AxlMatch m;
if (axl_find_in_source(&r.reader, "needle", 6, 0, AXL_FIND_DEFAULT, &m))
    use(text + m.start, m.length);
```

## AxlRegex — Regular-Expression Matcher

A **compiled-pattern** regular-expression matcher over the same
`AxlByteReader` seam. Compile once with `axl_regex_new`, then search
many times — the right shape for find-all loops and matching one
pattern across many lines or buffers. The engine is a Thompson NFA /
**Pike VM**, *not* a backtracker, so match time is O(pattern × input)
for **every** pattern: there is no catastrophic ("ReDoS") blow-up.
Backreferences are deliberately unsupported because they are not
regular and would force backtracking.

Supported: literals, `.`, greedy and lazy quantifiers
(`* + ? *? +? ??`), anchors `^ $`, alternation `|`, grouping and
capture `( )`, classes `[...]` / `[^...]` with ranges, and `\d \w \s`
(plus negations). Matching is byte-oriented and leftmost (Perl /
`grep -P` priority, not POSIX leftmost-longest). Compile flags:
`CASELESS`, `MULTILINE`, `DOTALL`. Match flags: `ANCHORED` pins the
match to `from_offset`; `NOTBOL` / `NOTEOL` treat `from_offset` / the
buffer end as mid-stream so `^` / `$` (and the start/end anchors) don't
match there (POSIX `REG_NOTBOL` / `REG_NOTEOL`) — for scanning a larger
source in overlapping windows without anchors firing at every boundary.

```c
AXL_AUTOPTR(AxlRegex) re = axl_regex_new("(\\w+)@(\\w+)", AXL_REGEX_DEFAULT);
AxlMatch g[3];                                   // [0]=whole, [1]/[2]=groups
AxlMemReader r;
axl_mem_reader_init(&r, "contact bob@host now", 20);
if (axl_regex_search_captures(re, &r.reader, 0, AXL_REGEX_MATCH_DEFAULT, g, 3)) {
    /* g[0] = "bob@host", g[1] = "bob", g[2] = "host" */
}
```

Find-all is the same loop literal find uses — re-search from
`m.start + (m.length ? m.length : 1)`. The matcher needs a contiguous
view of the scanned region: it uses the reader's zero-copy `peek` when
available, otherwise materializes the region into a temporary buffer
(O(region) per call — fine for editor-sized regions; for find-all over
a large out-of-core document, read the range out once and search the
buffer). The `axl_text_buffer_find_regex` / `axl_piece_tree_find_regex`
wrappers run a compiled regex over those sources.

### Sizing a decoded string

`axl_json_get_string` TRUNCATES a value too long for the buffer and still
returns true — the right default for a caller filling a fixed field, and the
wrong one for a caller that must not lose bytes. `axl_json_get_string_len` and
`axl_json_value_string_len` report the size that cannot truncate:

```c
size_t n;
if (axl_json_get_string_len(&r, "name", &n)) {
    char *buf = axl_malloc(n + 1);
    axl_json_get_string(&r, "name", buf, n + 1);   /* cannot truncate */
}
```

The answer is the length AFTER escape decoding and after the reader's UTF-8
mode applies, which is why it cannot be computed from the source span: `\uXXXX`
shrinks six bytes to between one and four, a surrogate pair shrinks twelve to
four, and JSON5's `\0` GROWS two to the three of U+FFFD. It costs a decode
pass, which is why the truncating form stays the default.

For object keys the query is a PEEK — `axl_json_object_peek_key_len` reports
the key the next `axl_json_object_next` will yield, without consuming it,
because that call truncates and reports it only after the pair is gone:

```c
size_t klen;
while (axl_json_object_peek_key_len(&it, &klen)) {
    char *key = axl_malloc(klen + 1);
    axl_json_object_next(&it, key, klen + 1, &value);   /* whole, always */
    ...
}
```

### Writing a double

`axl_json_double` and `axl_json_kv_double` complete the scalar mirror — the
reader has had `axl_json_get_double` since P14. Values are emitted in the
SHORTEST round-trippable spelling (`0.1`, not `0.10000000000000001`), and
non-finite values follow the dialect: refused by a strict writer, emitted as
`NaN` / `Infinity` under `AXL_JSON_ALLOW_NAN_INF` — the same bit the reader
accepts them under.

From C++, `<axl/axl-json.hpp>` wraps all of this: `axl::json_document`,
chaining `operator[]`, range-for over arrays and objects, and an
`axl::json_writer` whose containers close themselves.

## AxlRadixTree — Radix Tree

Compact prefix tree (radix tree) with string keys. Supports exact
lookup, longest-prefix lookup, insert with automatic edge splitting,
remove with node collapse, and depth-first iteration. Lookup is O(k)
where k is the key length, independent of the number of entries.

Header: `<axl/axl-radix-tree.h>`

**A NULL value is a value, not an absence.** A key's presence is tracked on
its node rather than inferred from `value != NULL`, so `axl_radix_tree_insert(t,
k, NULL)` is counted once by `axl_radix_tree_size`, visited by
`axl_radix_tree_foreach`, and removable by `axl_radix_tree_remove` like any
other entry; `value_free` is not called for it. Only `axl_radix_tree_lookup`
cannot tell it from an absent key, because its `void *` return has no spare
value to say so — use `axl_radix_tree_foreach` when the difference matters.

Inferring presence from the value was a real defect, fixed 2026-08-17: a
NULL-valued key was counted by `size` and reachable by nothing, so it could
never be removed, and re-inserting it counted it again every time.

### Overview

Use AxlRadixTree when you need longest-prefix matching — finding the
best match for a key that may not be an exact entry. The canonical use
case is URL route matching: given routes `/api/users` and `/api`, a
lookup for `/api/users/42` returns the `/api/users` handler.

```c
AxlRadixTree *tree = axl_radix_tree_new();

axl_radix_tree_insert(tree, "/api/users", handler_users);
axl_radix_tree_insert(tree, "/api",       handler_api);
axl_radix_tree_insert(tree, "/css/",      handler_static);

// Exact lookup
void *h = axl_radix_tree_lookup(tree, "/api/users");  // handler_users

// Longest-prefix lookup (the key feature)
const char *suffix;
h = axl_radix_tree_lookup_prefix(tree, "/api/users/42", &suffix);
// h = handler_users, suffix = "/42"

h = axl_radix_tree_lookup_prefix(tree, "/css/style.css", &suffix);
// h = handler_static, suffix = "style.css"

// Iterate all entries
axl_radix_tree_foreach(tree, print_entry, NULL);

axl_radix_tree_free(tree);
```

### Value Destructor

Use `axl_radix_tree_new_full(value_free)` to auto-free values on
removal or tree destruction:

```c
AxlRadixTree *tree = axl_radix_tree_new_full(axl_free);
axl_radix_tree_insert(tree, "key", axl_strdup("owned value"));
axl_radix_tree_free(tree);  // value freed automatically
```

## AxlRingBuf — Ring Buffer

Byte-oriented circular buffer with power-of-2 sizing and three API
layers. Inspired by Linux kfifo: monotonically increasing indices
with mask-based wrapping, using all buffer slots with no wasted-slot
ambiguity.

Header: `<axl/axl-ring-buf.h>`

### Overview

AxlRingBuf provides three layers, each building on the one below:

- **Layer 1 (Bytes)**: raw byte stream — push, pop, peek, discard,
  zero-copy scatter/gather regions
- **Layer 2 (Messages)**: variable-size length-prefixed frames --
  push_msg, pop_msg, peek_msg, peek_msg_size
- **Layer 3 (Elements)**: fixed-size typed entries — push_elem,
  pop_elem, peek_elem, peek/set_nth_elem

Supports reject-on-full (default) and overwrite-on-full modes.

```c
// Heap-allocated ring buffer
AxlRingBuf *rb = axl_ring_buf_new(1024);

// Layer 1: raw bytes
axl_ring_buf_push(rb, "hello", 5);
char buf[32];
uint32_t n = axl_ring_buf_pop(rb, buf, sizeof(buf));  // n = 5

// Layer 2: variable-size messages
axl_ring_buf_push_msg(rb, "short", 5);
axl_ring_buf_push_msg(rb, "a longer message", 16);
uint32_t actual;
axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual);  // "short", actual=5

axl_ring_buf_free(rb);

// Layer 3: fixed-size elements (use new_fixed constructor)
AxlRingBuf *erb = axl_ring_buf_new_fixed(256, sizeof(int), 0);
int vals[] = {10, 20, 30};
for (int i = 0; i < 3; i++) {
    axl_ring_buf_push_elem(erb, &vals[i]);
}
int out;
axl_ring_buf_pop_elem(erb, &out);  // out = 10 (FIFO)
axl_ring_buf_free(erb);
```

### Overwrite Mode

In overwrite mode, writes always succeed by discarding the oldest data:

```c
AxlRingBuf *rb = axl_ring_buf_new_full(8, AXL_RING_BUF_OVERWRITE);
axl_ring_buf_push(rb, "12345678", 8);  // full
axl_ring_buf_push(rb, "AB", 2);        // discards "12", buffer has "345678AB"
axl_ring_buf_free(rb);
```

### Embedded (Stack/Static) Usage

The struct is exposed, so it can be embedded in other structs or
allocated on the stack with no heap allocation:

```c
uint8_t buf[256];
AxlRingBuf rb;
axl_ring_buf_init(&rb, buf, sizeof(buf), 0, NULL);

axl_ring_buf_push(&rb, "no heap!", 8);
axl_ring_buf_deinit(&rb);
```

### Zero-Copy Access

For high-performance I/O, access the buffer directly without copying:

```c
AxlRingBufRegion regions[2];
uint32_t count = axl_ring_buf_peek_regions(rb, regions);
for (uint32_t i = 0; i < count; i++) {
    process(regions[i].data, regions[i].len);
}
axl_ring_buf_pop_advance(rb, total_processed);
```

### Push Statistics

Cumulative byte counters track every push attempt, including the
ones that don't survive:

```c
uint64_t pushed = axl_ring_buf_pushes_total(rb);  // bytes attempted
uint64_t lost   = axl_ring_buf_pushes_lost(rb);   // bytes invisible to consumer
```

`pushes_lost` covers (a) bytes rejected when reject-mode pushes
exceed available space, (b) bytes dropped from the front of an
oversized input in overwrite mode, and (c) older bytes displaced
by a new overwrite-mode push. Both counters reset on
`axl_ring_buf_clear` and on init.

Element-mode buffers translate trivially: divide by `elem_size` to
get element counts. The kernel POC's reqlog server uses exactly
that pattern to surface "received" and "dropped" totals on its
`/` endpoint.
