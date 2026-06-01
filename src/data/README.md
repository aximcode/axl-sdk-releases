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
- `<axl/axl-digest.h>` — Message digest checksums (MD5, SHA-1, SHA-256)
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

Dynamic array that stores elements by value (not pointers). Auto-grows
on append. Supports indexed access, sorting, and removal. Matches
GLib's GArray.

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

```c
AxlQueue q = AXL_QUEUE_INIT;    // stack-allocated
axl_queue_push_tail(&q, "first");
axl_queue_push_tail(&q, "second");
axl_queue_push_tail(&q, "first");  // duplicate

axl_queue_remove(&q, "first");     // removes first match
axl_queue_remove_all(&q, "first"); // removes all matches

AxlList *node = axl_queue_find(&q, "second");
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
`axl_ntree_unlink` detaches a subtree (it becomes its own root). Counts
and queries: `n_children`, `nth_child`, `depth` (root = 1), `max_height`,
`n_nodes(flags)`, `is_ancestor`, `get_root`. Traversal supports
pre/post/in/level order, an ALL/LEAVES/NON_LEAVES filter, a depth limit,
and early stop (the callback returns `true`). Data is borrowed; the tree
owns only its node objects. Single-threaded, no locking.

For a **pull-style** walk without a callback, `AxlNTreeIter` is a
stack-allocated pre-order cursor (uses the parent/sibling links — no
internal stack, any depth):

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
%x %X %s (with required width) %[set] %% %n`, length modifiers
`hh h l ll z j`, and `*` assignment suppression.

## AxlString — String Builder

Mutable auto-growing string builder, like GLib's `GString`. All strings
are UTF-8. Supports append, prepend, insert, printf-style formatting, and
truncation.

Header: `<axl/axl-string.h>`

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
allocated buffer released.

### Error Handling

All mutation functions (`append`, `printf`, etc.) return `int`:
0 on success, -1 if the internal realloc fails. This matches the
convention used by `axl_array_append`, `axl_hash_table_insert`, etc.

## AxlJson — JSON / JSON5

JSON reader (jsmn-based) and writer. Parse JSON strings into a token
tree, query values by key, and build JSON documents incrementally over
an `AxlString`. A separate colored UEFI-console pretty-printer is
provided for debug output.

The reader also accepts the [JSON5](https://json5.org) grammar
superset — comments, trailing commas, single-quoted strings, unquoted
keys, hex numbers — via an opt-in flag (see the **JSON5 Support**
section below). The writer can emit trailing commas and JSON5
comments via additional opt-in flags.

Header: `<axl/axl-json.h>`

### Overview

AXL provides three independent JSON APIs:

- **Reader** (`AxlJsonReader`) — parse a JSON string, extract values
  by key, iterate arrays.
- **Writer** (`AxlJsonWriter`) — build JSON into an auto-growing
  `AxlString`. Orthogonal calls (containers, keys, atoms) with a state
  machine that handles comma placement and string escaping. Optional
  pretty-print mode with 2-space indent.
- **Console printer** (`axl_json_console_print`) — colored,
  attribute-based pretty output to the UEFI console. Distinct from the
  writer's pretty-print flag (which produces buffer output, no colors).

### Reading JSON

```c
const char *json = "{\"name\":\"AXL\",\"version\":1,\"debug\":true}";
AxlJsonReader r;

if (!axl_json_parse(json, axl_strlen(json), &r)) {
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
`false` if the key is missing, the value has the wrong type, or (for
strings) the caller buffer is too small. String values are copied into
the caller buffer — no zero-copy lifetime concerns. Always call
`axl_json_free` on the reader; the token array is heap-allocated.

### Writing JSON

The writer builds into a caller-owned `AxlString` and grows on demand.
Comma placement, string escaping, and (optional) indentation are
handled internally. Containers, keys, and atoms are independent calls
— a single state machine knows whether the current container is an
object or an array.

```c
AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
AxlJsonWriter w;

axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
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

### Pretty Printing

Pass `AXL_JSON_WRITER_PRETTY` at init for 2-space-indent output with
newlines at every container and member boundary:

```c
axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);
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
        axl_json_get_string(&elem, NULL, value, sizeof(value));
        axl_printf("  %s\n", value);
    }
}
```

For root-level arrays (`[{...}, {...}]`) use `axl_json_root_array_begin`
instead. Element readers borrow the parent's token array — do not call
`axl_json_free` on them.

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
Like array elements, the sub-reader borrows the parent's token array:
do not call `axl_json_free` on it, and it stays valid only while the
parent reader lives.

### Round-Trip Transforms

`axl_json_write_token` splices an already-parsed token into the
writer's output. The bridge writes string and key bytes verbatim from
the source — jsmn keeps escape sequences in source form, so this
preserves `\uXXXX`, escaped quotes, etc. without re-escaping. Useful
for parse → mutate → re-emit flows:

```c
AxlJsonReader r;
axl_json_parse(input, input_len, &r);

AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
AxlJsonWriter w;
axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);

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

- **AxlString OOM** — auto-grow failed.
- **Structural misuse** the writer can detect:
  - emit a bare-primitive root (only objects and arrays are valid
    JSON root values; matches `axl_json_parse`'s contract)
  - emit a value outside any container after the root has closed
  - emit a key inside an array
  - emit a value when a key was expected in object context
  - `axl_json_obj_end` on an open array (or vice versa)
  - close when nothing is open
  - mismatched begin/end counts at `axl_json_writer_finish`

What the writer does **not** catch:

- Duplicate keys in the same object — JSON technically allows them; the
  writer doesn't track emitted keys.
- Non-UTF-8 bytes in `axl_json_str` — passed through escaping unchanged.
- The contents of `axl_json_raw(&w, fragment)` — the caller asserts the
  fragment is valid JSON; the writer splices it as-is.

### JSON5 Support

[JSON5](https://json5.org) is a strict superset of JSON aimed at
human-edited config files. AXL accepts the JSON5 grammar on the reader
side via an opt-in flag, and emits JSON5-flavored extras (trailing
commas, comments) on the writer side via additional flags. Strict
callers see no behavior change.

**Reader — opt in with `AXL_JSON_PARSER_JSON5`:**

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
if (!axl_json_parse_flags(cfg, axl_strlen(cfg),
                          AXL_JSON_PARSER_JSON5, &r)) {
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

if (axl_json_load_file_flags("jedec.json5", AXL_JSON_PARSER_JSON5,
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
- Extended string escapes: `\'`, `\v`, `\0`, `\x##`, line continuations

Strict callers (`axl_json_parse`, no flags) still go through the
existing jsmn-based path and reject JSON5 input. The JSON5 path is
strictly opt-in.

**Writer — emit JSON5 extras with `AXL_JSON_WRITER_TRAILING_COMMAS`
and `axl_json_comment`:**

```c
AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
AxlJsonWriter w;
axl_json_writer_init(&w, out,
    AXL_JSON_WRITER_PRETTY | AXL_JSON_WRITER_TRAILING_COMMAS);

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

AXL's JSON5 support is scoped to the features that matter for
firmware-edited sidecar configs (the in-tree consumer is
`tools/memspd.c` reading `share/jedec.json5`). The following parts
of the [json5.org](https://json5.org) spec are intentionally
**not** supported:

- **`Infinity`, `-Infinity`, `NaN`** — AXL is freestanding UEFI
  with no `libm`. There's no `axl_json_get_double` accessor, so
  IEEE 754 special values would be lex-only and unretrievable.
  `axl_json_get_int` on a fractional or scientific-notation token
  truncates at the first non-digit character (pre-existing
  strict-mode behavior, unchanged): `{x: 1.5}` returns `1`.

- **Unicode `IdentifierName` for unquoted keys** — only the ASCII
  subset (`[A-Za-z_$][A-Za-z0-9_$]*`) is recognized. Keys with
  Unicode letters, combining marks, ZWNJ, or ZWJ require
  quoting (single or double quotes both work).

- **Unicode whitespace** (U+00A0 NBSP, U+FEFF BOM, U+2028 LS,
  U+2029 PS, and other `Space_Separator` characters) — only the
  ASCII whitespace set plus `\v` and `\f` is treated as
  insignificant. Documents using Unicode separators as whitespace
  will fail to parse.

These gaps are deliberate — adding lex support for tokens that
can't be retrieved (floats) or for grammar that no firmware tool
actually authors (Unicode identifier keys) would expand the
attack surface and surprise consumers without unlocking new use
cases. Open an issue if a real consumer needs any of these and
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

axl_xml_writer_init(&w, out, AXL_XML_WRITER_DEFAULT);
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

Pass `AXL_XML_WRITER_PRETTY` at init for 2-space indent + newlines
between child elements; text-only elements stay on one line.

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

## AxlRadixTree — Radix Tree

Compact prefix tree (radix tree) with string keys. Supports exact
lookup, longest-prefix lookup, insert with automatic edge splitting,
remove with node collapse, and depth-first iteration. Lookup is O(k)
where k is the key length, independent of the number of entries.

Header: `<axl/axl-radix-tree.h>`

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
