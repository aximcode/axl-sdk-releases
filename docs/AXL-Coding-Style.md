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
| Macros/constants | `AXL_<MODULE>_SCREAMING_CASE` | `AXL_MATH_PI`, `AXL_GFX_BLACK`, `AXL_CFG_BOOL` | `G_DEFINE_TYPE` |
| Enum values | `AXL_SCREAMING_CASE` | `AXL_IO_READ` | `G_IO_STATUS_NORMAL` |
| Struct members | `snake_case` | `stream->ctx` | `string->str` |
| Local variables | `snake_case` | `char *line` | |
| Parameters | `snake_case` | `const char *key` | |
| Static/file scope | `snake_case` | `static bool verbose` | |
| Static const tables | `snake_case` (no `k`-prefix) | `static const AxlArgsNode verbs[]` | |

### Module-prefix all public macros

Public macros and named constants take a module prefix between
`AXL_` and the rest of the name — `AXL_MATH_PI`, `AXL_GFX_BLACK`,
`AXL_INPUT_BUTTON_LEFT`, `AXL_FONT_MONOSPACE`.  This applies to
all module-scoped macros in public headers, including math
constants (`AXL_MATH_PI`, `AXL_MATH_TWO_PI`, etc.), color palette
entries (`AXL_GFX_RED`), bitfield masks (`AXL_INPUT_MOD_SHIFT`),
and tunables.

The only exception is project-wide infrastructure that doesn't
belong to any module — `AXL_OK` / `AXL_ERR` (umbrella status),
`AXL_APP` (entry-point macro).  These live in `<axl/axl-macros.h>`
and don't need a module prefix because no module owns them.

Module-prefixed names make grep + IDE-jump and "which header
defines this?" trivial, and prevent the kind of bare-`AXL_PI`
collision an unrelated module would have introduced if it ever
needed a `PI`-named constant.

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
| Source files | `axl-<module>.c` | `axl-mem.c`, `axl-stream-buf.c` |
| Public headers | `axl/axl-<module>.h` | `axl/axl-log.h`, `axl/axl-str.h` |
| Internal headers | `axl-<module>-internal.h` | `axl-format-internal.h` |
| Umbrella header | `axl.h` | `#include <axl.h>` |
| Test files | `axl-test-<module>.c` | `axl-test-mem.c` |
| Vendor adapters | `axl-<module>-<vendor>.c` | `axl-ipmi-dell.c`, `axl-ipmi-edkii.c` |
| Directories | lowercase | `src/mem/`, `src/backend/` |

Source files and headers use **lowercase hyphenated** names.
Internal headers live next to their implementation (e.g.,
`src/backend/axl-backend.h`), not in `include/`.

## Vendor and Vendor-Specific Code

AXL is hardware-vendor-agnostic at its **public API surface** — the
function names in `include/axl/*.h` and the types they expose must
not mention vendors (no `axl_dell_*`, no `AxlIntelFoo`). Consumer
code calls `axl_ipmi_session_new()` and never knows whether the
underlying transport is generic KCS or a Dell proprietary protocol.

Vendor names ARE allowed (and expected) in two narrow places:

1. **Vendor adapter files** — `src/<module>/axl-<module>-<vendor>.c`.
   Use when a real-world vendor protocol needs an adapter under the
   neutral public API. Examples in the tree: `axl-ipmi-dell.c`
   (adapts Dell's `EFI_IPMI_TRANSPORT`), `axl-ipmi-edkii.c` (adapts
   EDKII's `IPMI_PROTOCOL`). Same shape as the Linux kernel's
   `drivers/platform/x86/dell_*.c`. The naming reflects what the
   file contains; renaming to a neutral name would obscure
   maintenance and break grep-ability against vendor docs.

2. **Public enum values and probe-struct fields that name real
   vendor things** — e.g., `AXL_IPMI_TRANSPORT_DELL` (identifies
   which transport the auto-detect picked) and
   `AxlIpmiProbe.dell_ipmi_transport` (a diagnostic snapshot field
   that's *explicitly* about real-world vendor presence). These
   stay because they describe vendor-specific external state;
   renaming them would defeat their purpose.

What stays out of vendor naming: function entry points consumers
call directly. `axl_ipmi_session_new` (not `axl_ipmi_dell_session_new`),
`axl_pci_get_vid_did` (not `axl_pci_intel_get_vid_did`). The
auto-detect picks the vendor adapter; the consumer never names one.

### Spec-decoder strings are spec-canonical

When a function decodes a *published-spec* value to a string —
SMBIOS / ACPI / PCI / USB / IPMI / JEDEC / etc. — the string
**matches the spec verbatim**. Vendor-flavored renderings
(rewordings, OEM extensions, alternative interpretations) live
in consumer code on top of the typed reader.

Concrete: `axl_smbios_slot_usage_str(0x05)` returns `"Unavailable"`
(SMBIOS 3.7 Table 12), not Dell's `"CPU NOT INSTALLED"` rendering
that some consumers prefer for socket-associated 0x05 slots. The
typed reader (`AxlSmbiosSystemSlot.current_usage`) surfaces the
raw byte, so a consumer that wants vendor-flavored output writes:

```c
const char *display = (slot.current_usage == 0x05 && is_cpu_socket(slot))
                    ? "CPU NOT INSTALLED"
                    : axl_smbios_slot_usage_str(slot.current_usage);
```

…in their own code. The SDK stays vendor-neutral; the consumer
adds vendor flavor where it actually belongs.

This rule applies to every spec-decoder string in the public
surface. Comments may reference vendor-specific renderings as
*examples* (so consumers know the typed-reader translation
recipe), but never bake them into the returned string.

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

**Split, don't hoist into a giant top-of-file**: if a section under
a banner comment accumulates its own typedefs, macros, and
file-scope statics, that's a signal to break it out into a sibling
file rather than declare them inline in the body or hoist them
into a 100-line top-of-file block. A typedef defined 1500 lines
from its only caller is harder to find than the same typedef
declared inline in the section that uses it; the right answer is
usually a smaller file (`axl-pci.c` → `axl-pci-cap.c`,
`axl-acpi.c` → `axl-acpi-fadt.c` / `-mcfg.c` / `-madt.c`,
`axl-str.c` → `axl-str-bmh.c` / `-base64.c` / `-scan.c`). Use
an internal header (`src/<module>/axl-<module>-internal.h`) to
share helpers between the split files. Public API surface stays
unchanged — only the .c file decomposes.

**Function-local helper macros are an exception**: if a macro
references caller-scope variables (e.g. `va_list` in a
SCAN_STORE_* helper, or local accumulator state in an
ESC_APPEND_* string-builder), it has to live next to its single
caller — moving it to the top of the file would either break
expansion or require an awkward parameter-passing refactor.
Section-local macros that simply happen to be near their first
use, but don't reference local state, should still be hoisted to
the macros section.

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

Pick the narrowest type that carries the actual information. Four
patterns in order of how much the return tells the caller:

| Pattern | Return type | Success | Failure | Example |
|---------|------------|---------|---------|---------|
| **Predicates** | `bool` | `true` | `false` | `axl_dir_read`, `axl_net_is_available` |
| **Pointer producers** | `T *` | non-NULL | `NULL` | `axl_event_new`, `axl_dir_open` |
| **Operations (single failure)** | `int` | `AXL_OK` (0) | `AXL_ERR` (-1) | `axl_file_get_contents`, `axl_file_delete` |
| **Operations (multi-outcome)** | `AxlStatus` or `Axl<Module>Status` | `AXL_OK` (0) | one of the documented codes | `axl_event_wait_timeout`, `axl_sidecar_open_file` |

### Choosing the right shape

Walk the table top-down and stop at the first row that fits the
information you actually return:

1. **Bool** if it's a yes/no question and there's nothing useful to
   say beyond "yes" or "no." `axl_dir_read(dir, &entry)` either
   produced an entry or didn't.
2. **Pointer** if you allocate or look up an object. NULL is the
   universal "couldn't" channel; no need to layer a status code on
   top.
3. **`int` returning `AXL_OK`/`AXL_ERR`** if there's an operation
   that can fail but you don't have multiple failure modes worth
   distinguishing. Most file ops fall here.
4. **Typed `AxlStatus` or `Axl<Module>Status`** if there are 3+
   distinguishable outcomes that consumers will branch on. Promote
   from `int` only when the third code earns its keep.

When in doubt, start narrower. Adding an outcome later is easy;
removing one consumers came to depend on is breaking.

### Multi-outcome status — `AxlStatus` (project-wide)

`<axl/axl-macros.h>` defines:

```c
typedef enum {
    AXL_OK        =  0,  // operation succeeded
    AXL_ERR       = -1,  // operation failed (generic)
    AXL_CANCELLED = -2,  // AxlCancellable signalled or Ctrl-C
    AXL_TIMEOUT   = -3,  // deadline elapsed before completion
} AxlStatus;
```

Numeric values are part of the contract — comparing against the
named constants and against the literal integers both work. New
codes only ever extend the negative range; existing values never
change.

Use `AxlStatus` (rather than a per-module enum) when the four
existing outcomes already cover what your function returns. The
wait/event family (`axl_event_wait_timeout`, the `axl_wait_*`
helpers, the per-protocol `_axl_{udp,tcp,dns,ip4}_wait` Tier 4
helpers) all return `AxlStatus`.

### Per-module status — `Axl<Module>Status`

Define a module-local enum when your function has distinguishable
outcomes that don't map onto `AxlStatus`'s OK/ERR/CANCELLED/TIMEOUT
shape — typically because the outcomes are domain-specific.
`AxlSidecar` is the canonical example:

```c
typedef enum {
    AXL_SIDECAR_OK           =  0,
    AXL_SIDECAR_FILE_MISSING = -1,  // would have been a generic AXL_ERR
    AXL_SIDECAR_PARSE_ERROR  = -2,  // ditto, but distinct
} AxlSidecarStatus;
```

Naming: `Axl<Module>Status` (PascalCase), member prefix
`AXL_<MODULE>_<NAME>` (screaming snake-case). Member values follow
the `AxlStatus` convention — `_OK = 0`, additional outcomes in the
negative range, numeric values stable.

### Check style

For `int` and `AxlStatus`, compare against the named constant. Never
use truthiness on a status return — it works by accident today and
breaks the moment a new code appears.

```c
// good
if (axl_event_wait_timeout(e, NULL, 1000) != AXL_OK) { return -1; }
if (rc == AXL_CANCELLED) { ... }
if (rc == AXL_TIMEOUT)   { ... }

// bad — implicit "anything non-zero is failure" stops working when
// callers want to treat AXL_TIMEOUT differently from AXL_ERR.
if (axl_event_wait_timeout(e, NULL, 1000)) { ... }
if (!axl_event_wait_timeout(e, NULL, 1000)) { ... }
```

Predicates check with truthiness:

```c
while (axl_dir_read(dir, &entry)) { ... }
```

### POSIX-exit-code outliers

A handful of public functions return `int` but **deliberately keep
POSIX exit-code semantics** (`0` success, `1` general error, `2`
misuse), because their value flows directly into a process exit
code via `return ...` from `main()`. The canonical case is
`axl_args_run` — promoting it to `AxlStatus` would make a parse
error exit the process with code 254/255 instead of 1, which is
wrong for shell scripting.

Such functions are explicitly documented as POSIX-exit-shaped in
their docstrings. Don't promote them to `AxlStatus`.

### `AXL_WARN_UNUSED`

Critical `int` / `AxlStatus`-returning functions are marked
`AXL_WARN_UNUSED` (C23 `[[nodiscard]]`) in the header to catch
unchecked returns at compile time. Apply it to any function whose
caller really should look at the return value — every wait/event
function, every operation that can leak resources on the failure
path, every `axl_*_open` that needs paired `_close`.

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

## CLI Patterns

UEFI applications come in two shapes:

1. **Single-purpose tool** — one job, configured via flags.
   Canonical example: `mkrd` (`mkrd <label> [-s size] | -l | -d <label>`).
   Use `axl_config_*` to declare a flag descriptor table, parse
   `argv` once in `main`, dispatch the operation. No subcommand
   layer needed.

2. **Multi-command tool** — multiple distinct operations under a
   common executable, often grouped into categories (e.g.
   `tool bios`, `tool sysid`, `tool crb`). Canonical use case:
   a downstream-consumer hardware-diagnostic CLI port.

   Use `<axl/axl-subcommand.h>` for dispatch, plus `axl_config_*`
   inside each subcommand for its own flag parsing:

   ```c
   static int
   do_bios(int argc, char **argv)
   {
       /* argv[0] is "bios"; flags via axl_config_* on argv */
       /* ... */
       return 0;
   }

   static const AxlSubcommand commands[] = {
       { "bios",  do_bios,  "[test|pci|irq|slot|emb]",
         "mytool bios test  — run BIOS POST self-test\n" },
       { "sysid", do_sysid, "[hexValue]", NULL },
   };

   int
   main(int argc, char **argv)
   {
       return axl_subcommand_dispatch(commands,
           sizeof(commands) / sizeof(commands[0]),
           argc, argv, "mytool");
   }
   ```

   The dispatcher handles `<prog>`, `<prog> help`, `<prog> help <cmd>`,
   `-h`/`--help`, "did you mean" typo suggestions, and shifts `argv`
   so each subcommand sees its own name as `argv[0]`.

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

Exception: code that runs before `axl_stream_init()` (e.g. very early
allocator errors) may use EDK2 primitives as a fallback.
