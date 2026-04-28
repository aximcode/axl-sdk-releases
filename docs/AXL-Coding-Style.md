# AXL Coding Style

AXL follows the C conventions familiar to Linux systems
developers — the GLib / kernel-style snake_case + PascalCase
mix — explicitly *not* EDK2's PascalCase-everywhere house style.
The choice is for the audience: a developer who arrives from
glibc / GLib / systemd / libcurl reads this code without a
translation step. All new code uses this style — no exceptions,
no EDK2-style PascalCase functions in public API.

Public API never returns or accepts an `EFI_*` type. UEFI types
exist inside `src/` and `include/uefi/` but never leak across
the `include/axl/` boundary.

## Naming

| Element | Convention | Example | GLib equivalent |
|---------|-----------|---------|-----------------|
| Functions | `axl_snake_case` | `axl_hash_table_new` | `g_hash_table_new` |
| Types | `AxlPascalCase` (no suffix) | `AxlHashTable` | `GHashTable` |
| Macros/constants | `AXL_SCREAMING_CASE` | `AXL_CFG_BOOL` | `G_DEFINE_TYPE` |
| Enum values | `AXL_SCREAMING_CASE` | `AXL_IO_READ` | `G_IO_STATUS_NORMAL` |
| Struct members | `snake_case` | `stream->ctx` | `string->str` |
| Local variables | `snake_case` | `char *line` | |
| Parameters | `snake_case` | `const char *key` | |
| Static/file scope | `snake_case` | `static bool verbose` | |

## Types — Standard C Only in Public API

Public headers (`axl/*.h`) use standard C types exclusively:

| Use | Not |
|-----|-----|
| `void *` | `VOID *` |
| `char *` | `CHAR8 *` |
| `const` | `CONST` |
| `size_t` | `UINTN` |
| `bool` | `BOOLEAN` |
| `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` | `UINT8`, `UINT16`, `UINT32`, `UINT64` |
| `int` (0 = success) | `EFI_STATUS` |
| `NULL` | — |

Include `<stddef.h>` for `size_t`, `<stdbool.h>` for `bool`,
`<stdint.h>` for fixed-width types. These are freestanding headers
provided by GCC even with `-ffreestanding`.

**No `<Uefi.h>` in public headers.** Public headers (`axl/*.h`) must
not include `<Uefi.h>`. Use freestanding headers only (`<stddef.h>`,
`<stdint.h>`, `<stdbool.h>`, `<stdarg.h>`).

**`_impl` declarations** use UEFI types but are isolated in
`axl-mem-impl.h` (included at the bottom of `axl-mem.h`).
Consumers never call `_impl` functions directly — they use macros.
Implementation files (`.c`) may use UEFI types internally.

**UCS-2 interop:** Functions that bridge to UEFI's UCS-2 strings
(e.g. `axl_utf8_to_ucs2`) use `unsigned short *` in their
signatures — equivalent to UEFI's `CHAR16` without the dependency.

## Strings — UTF-8 Everywhere

All strings in the AXL public API are `char *` (UTF-8). No `CHAR16`,
no `wchar_t`, no wide strings. This matches GLib's convention.

All `axl_str*` functions (without `_w` suffix) operate on UTF-8
strings. Since UTF-8 is a superset of ASCII, these functions work
correctly for comparison, searching, copying, and splitting.
Case-insensitive operations (`axl_strcasecmp`, `axl_strcasestr`)
fold ASCII letters only — they do not handle full Unicode case
mapping (same as GLib's `g_ascii_strcasecmp`).

UCS-2 (`unsigned short *`) functions have a `_w` suffix and are
for UEFI internal use. Consumer code should use UTF-8 and convert
at boundaries with `axl_utf8_to_ucs2` / `axl_ucs2_to_utf8`.

UEFI uses UCS-2 (`CHAR16 *`) for file paths and console output.
AXL converts internally when calling UEFI APIs. The conversion
functions are exposed for code that needs direct UEFI protocol
interop (e.g., drivers using `<uefi/axl-uefi.h>`).

## File Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Source files | `axl-<module>.c` | `axl-mem.c`, `axl-io-buf.c` |
| Public headers | `axl/axl-<module>.h` | `axl/axl-log.h`, `axl/axl-str.h` |
| Internal headers | `axl-<module>-internal.h` | `axl-format-internal.h` |
| Umbrella header | `axl.h` | `#include <axl.h>` |
| Test files | `axl-test-<module>.c` | `axl-test-mem.c` |
| Directories | lowercase | `src/mem/`, `src/backend/` |

Source files and headers use **lowercase hyphenated** names.
Internal headers live next to their implementation (e.g.,
`src/backend/axl-backend.h`), not in `include/`.

## Source File Layout

Every `.c` and `.h` file starts with a two-line SPDX/copyright block,
then the existing descriptive `@file` block, then the rest of the
file content:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
```

The `SPDX-License-Identifier` line must be at the very top so SBOM
scanners and compliance tools find it reliably. Every tracked `.c`
and `.h` under `src/`, `include/`, and `tools/` carries these two
lines. Auto-generated files (`include/uefi/generated/*.h`) inherit
the same header from `scripts/generate-uefi-headers.py`.

Every `.c` file then follows this top-to-bottom order. Keep each
category together — don't scatter `#define`s, `typedef`s, or `static`
variables between function implementations.

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-foo.c
    Brief description of the module.
**/

/* 1. Includes — freestanding first, then backend, then public headers */
#include <stddef.h>
#include <stdbool.h>
#include "../backend/axl-backend.h"
#include <axl/axl-foo.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

/* 2. Log domain (if the file uses axl_info / axl_error / etc.) */
AXL_LOG_DOMAIN("foo");

/* 3. Macros */
#define FOO_DEFAULT_CAP  16

/* 4. Types (internal structs, enums, the opaque struct definition) */
typedef struct { ... } FooNode;
struct AxlFoo { ... };

/* 5. File-scope variables (avoid when possible) */
static size_t global_count;

/* 6. Forward declarations of static functions (only when needed —
      most files define helpers before their callers instead) */
static void helper_a(AxlFoo *f);

/* 7. Function implementations — group by section with banner comments:
      // -----------------------------------------------------------------
      // Section name
      // ----------------------------------------------------------------- */
```

**Variables inside functions**: prefer declarations at the top of the
enclosing scope. Mid-block declarations are acceptable when a variable
is only used within a specific branch and hoisting it would hurt
readability. For-loop iterator declarations (`for (size_t i = 0; ...)`)
are always fine.

## Formatting

| Rule | Convention | Example |
|------|-----------|---------|
| Indent | 4 spaces, no tabs | |
| Line length | 80-120 characters | |
| Braces (control flow) | K&R, opening on same line | `if (x) {` |
| Braces (functions) | Opening brace on its own line | see example below |
| Braces (single stmt) | **Always use braces** | `if (x) { return; }` |
| Return type | Separate line from function name | `int\naxl_foo(...)` |
| Pointer style | Star with variable | `char *ptr` |
| Parens | No space before parens (calls, declarations, macros) | `axl_free(ptr)`, `void axl_free(void *ptr)` |
| Comments | `//` and `/* */` both OK | |
| Header guards | `#ifndef AXL_HASH_H` | |
| NULL checks | Explicit: `if (ptr == NULL)` | Not `if (!ptr)` |
| Bool checks | Implicit: `if (found)` | Not `if (found == true)` |
| Result checks | Explicit: `if (rc != AXL_OK)` | Not `if (!rc)` or `if (rc)` |
| Strings | `char *` (UTF-8 everywhere) | Not `CHAR16` |
| Empty lines | Allowed, no trailing whitespace | |

## Return Value Conventions

Two patterns, used consistently:

| Pattern | Return type | Success | Failure | Example |
|---------|------------|---------|---------|---------|
| **Operations** | `int` | `AXL_OK` (0) | `AXL_ERR` (-1) | `axl_file_get_contents` |
| **Predicates** | `bool` | `true` | `false` | `axl_dir_read`, `axl_net_is_available` |

**Operations** (set, parse, open, write, etc.) return `int`. Check with
`!= AXL_OK`, never with `!` or truthiness:

```c
if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
    // error
}
```

**Predicates** (is_X, has_X, dir_read iterator) return `bool`. Check
with truthiness:

```c
while (axl_dir_read(dir, &entry)) {
    // process entry
}
```

Functions returning pointers follow the pointer convention: `NULL` on
error, non-NULL on success.

Critical `int`-returning functions are marked `AXL_WARN_UNUSED` in the
header to catch unchecked returns at compile time.

## Event Loop Callback Convention

Callback return values control the **source**, not the loop:

```c
return AXL_SOURCE_CONTINUE;  // keep this source active
return AXL_SOURCE_REMOVE;    // remove this source (loop continues)
```

To quit the loop, call `axl_loop_quit(loop)` explicitly — typically
passed via the callback's `data` parameter:

```c
static bool
on_timer(void *data)
{
    AxlLoop *loop = (AxlLoop *)data;
    if (done) {
        axl_loop_quit(loop);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

axl_loop_add_timer(loop, 1000, on_timer, loop);
```

This matches GLib's `G_SOURCE_CONTINUE` / `G_SOURCE_REMOVE` convention.

## Example

```c
int
axl_do_something(AxlHashTable *table, const char *key)
{
    if (key == NULL) {
        return -1;
    }

    char *val = axl_hash_table_lookup(table, key);
    if (val == NULL) {
        axl_eprintf("key not found: %s\n", key);
        return -1;
    }

    /* Process value */
    axl_printf("found: %s = %s\n", key, val);
    return 0;
}
```

## Printf and Variadic Functions

AXL has its own lightweight printf engine (`axl_vformat`) that uses
standard C `va_arg`. It does NOT use EDK2's PrintLib, so variadic
functions need no special calling convention annotation.

Format specifiers follow standard C printf — `%s` for `char *`,
`%d` for `int`, `%lu` for `unsigned long`, `%zu` for `size_t`, etc.
Use `__attribute__((format(printf, N, M)))` for compile-time checking:

```c
void
axl_string_append_printf(AxlString *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

char *
axl_asprintf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
```

## Documentation (Doxygen)

Inline parameter docs using Doxygen trailing comments (`///<`).
Parameters are column-aligned in the signature with docs beside them.
The block comment above has the function description and return value.

```c
/**
 * @brief Create a new hash table with string keys.
 *
 * @return new AxlHashTable, or NULL on allocation failure.
 *     Free with axl_hash_table_free().
 */
AxlHashTable *
axl_hash_table_new(void);

/**
 * @brief Insert or replace a key-value pair.
 */
void
axl_hash_table_insert(
    AxlHashTable *h,    ///< hash table
    const char *key,  ///< string key (copied internally)
    void       *val   ///< value pointer (not copied, not freed on removal)
);

/**
 * @brief Look up a key.
 *
 * @return value pointer, or NULL if not found.
 */
void *
axl_hash_table_lookup(
    AxlHashTable *h,    ///< hash table
    const char *key   ///< key to look up
);
```

For simple functions with 0-1 parameters, keep the signature on one line:

```c
/**
 * @brief Free a hash table. Keys freed; values NOT freed.
 */
void
axl_hash_table_free(AxlHashTable *h);  ///< hash table (NULL-safe)
```

## Dogfooding

AXL's own internals use the AXL API:
- Allocate with `axl_malloc`/`axl_calloc`/`axl_free`
- Report errors with `axl_printerr`
- Build strings with `axl_string`
- Use `axl_strlcpy`/`axl_strlcat` for bounded copies

Exception: code that runs before `axl_io_init()` (e.g. very early
allocator errors) may use EDK2 primitives as a fallback.
