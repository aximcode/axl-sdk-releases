Core collection types, string utilities, and message digest checksums.

Headers:

- `<axl/axl-hash-table.h>` — Hash table with string keys (FNV-1a, chained)
- `<axl/axl-array.h>` — Dynamic array (auto-growing, index access)
- `<axl/axl-list.h>` — Doubly-linked list
- `<axl/axl-slist.h>` — Singly-linked list
- `<axl/axl-queue.h>` — FIFO queue
- `<axl/axl-radix-tree.h>` — Radix tree (compact prefix tree, longest-prefix lookup)
- `<axl/axl-ring-buf.h>` — Ring buffer (circular byte buffer, zero-copy, overwrite mode)
- `<axl/axl-digest.h>` — Message digest checksums (MD5, SHA-1, SHA-256)

## Choosing a Collection

| Type | Best for | Lookup | Insert/Remove |
|------|----------|--------|---------------|
| AxlHashTable | Key-value mapping | O(1) by key | O(1) amortized |
| AxlArray | Indexed, sorted data | O(1) by index | O(1) append |
| AxlList | Frequent insert/remove | O(n) by value | O(1) at position |
| AxlSList | Simple linked sequences | O(n) | O(1) prepend |
| AxlQueue | FIFO/LIFO patterns | O(1) head/tail | O(1) push/pop |
| AxlRadixTree | Prefix-match routing | O(k) by key | O(k) insert |
| AxlRingBuf | Streaming I/O, pipes | O(1) push/pop | O(1) |

## AxlHashTable

GLib-style hash table with FNV-1a hashing, chained collision resolution,
and automatic resize at 75% load factor. Supports generic key types via
user-provided hash/equal callbacks.

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

## AxlJson — JSON

JSON parser (JSMN-based) and builder (fixed buffer). Parse JSON strings into
a token tree, query values by path, and build JSON documents incrementally.

Header: `<axl/axl-json.h>`

### Overview

AXL provides two JSON APIs:

- **Parser** — parse a JSON string, extract values by key or iterate arrays
- **Builder** — construct JSON in a caller-provided buffer with no dynamic allocation

### Parsing JSON

```c
const char *json = "{\"name\":\"AXL\",\"version\":1,\"debug\":true}";
AxlJsonCtx ctx;

if (axl_json_parse(json, axl_strlen(json), &ctx)) {
    const char *name;
    size_t name_len;
    int64_t version;
    bool debug;

    axl_json_get_string(&ctx, "name", &name, &name_len);
    axl_json_get_int(&ctx, "version", &version);
    axl_json_get_bool(&ctx, "debug", &debug);

    axl_printf("name=%.*s version=%lld debug=%s\n",
               (int)name_len, name, version,
               debug ? "true" : "false");

    axl_json_free(&ctx);
}
```

**Note**: String values returned by `axl_json_get_string` point into
the original JSON buffer (zero-copy). Do not free the original buffer
while using extracted strings.

### Building JSON

The builder writes into a caller-provided buffer with no heap allocation.
Check `overflow` after building to detect truncation.

```c
char buf[256];
AxlJsonBuilder j;

axl_json_init(&j, buf, sizeof(buf));
axl_json_object_start(&j);
axl_json_add_string(&j, "name", "AXL");
axl_json_add_uint(&j, "version", 1);
axl_json_add_bool(&j, "debug", true);
axl_json_object_end(&j);
axl_json_finish(&j);

if (!j.overflow) {
    axl_printf("%s\n", buf);
    // {"name":"AXL","version":1,"debug":true}
}
```

### Iterating Arrays

```c
// Parse: {"items":["a","b","c"]}
AxlJsonIter iter;
if (axl_json_array_begin(&ctx, "items", &iter)) {
    AxlJsonElement elem;
    while (axl_json_array_next(&iter, &elem)) {
        axl_printf("  %.*s\n", (int)elem.len, elem.value);
    }
}
```

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
