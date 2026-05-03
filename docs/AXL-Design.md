# AXL — AximCode Library for UEFI

A C library that makes UEFI application development look and feel
like writing Linux C code. Applications use snake_case `axl_`
functions with standard C types. No external dependencies.

**Name:** AXL = AximCode Library. Pronounced "axle."

## Audience

AXL targets developers who already write C on Linux — kernel,
systemd unit, GLib daemon, libcurl client — and are reaching into
firmware territory. EDK2 is the official environment for UEFI work
but its conventions (PascalCase, EFI_ prefixes, CHAR16/UTF-16
everywhere, the protocol/service-binding model, gnu-efi's threadbare
runtime) are unfamiliar to that audience and not a great fit for
the API shape they're used to.

AXL exposes the API shape they're used to instead. The mapping to
GLib, the canonical Linux C library for that style, is intentional
and direct:

| Linux C / GLib                  | AXL                                |
|--------------------------------|------------------------------------|
| `GMainLoop` / `g_main_loop_run` | `AxlLoop` / `axl_loop_run`         |
| `GHashTable`                    | `AxlHashTable`                     |
| `GSList` / `GList` / `GQueue`   | `AxlSList` / `AxlList` / `AxlQueue`|
| `GString`                       | `AxlString`                        |
| `GBytes`                        | `AxlStream` (buffer)                         |
| `g_signal_connect`              | `axl_pubsub_subscribe`             |
| `g_io_channel_*`                | `AxlSocket`, `AxlStream`               |
| `g_malloc` / `g_free` / `g_new0`| `axl_malloc` / `axl_free` / `axl_new`|
| `gint64` / `gsize` / `gboolean` | `int64_t` / `size_t` / `bool`      |
| `GError` out-param              | int return + `axl_error` log       |

If you've written a GLib daemon, you already understand the AXL
programming model — what changes is that you're targeting a UEFI
PE/COFF binary instead of an ELF executable, and AXL handles the
firmware-side glue (the CRT0 entry stub, the AXL runtime that
wraps `main` with default loop / atexit / Ctrl-C / leak sweep,
event integration, protocol lookup) so you don't write `gBS->`,
`gST->`, `EFI_*` types, or wide strings in your application code.
The full lifecycle — boot → init → main → cleanup → exit — is
documented in [`AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md).

**Where the UEFI types come from.** AXL has no source-tree
dependency on EDK2. The `EFI_*` structs, status codes, protocol
GUIDs, and service tables are auto-generated from the published
UEFI 2.x and PI 1.x specifications by `scripts/generate-uefi-headers.py`
driven by `scripts/uefi-manifest.json5`. The generator walks the
spec HTML, extracts each declared type by name and kind, and emits
a header layout-compatible with EDK2's. The output lives under
`include/uefi/generated/` and is regenerated on demand; the small
hand-written `include/uefi/axl-uefi-extra.h` covers anything the
specs don't (Shell protocols, a few PI-spec table revisions). Spec
drift is a manifest update + regeneration step rather than a
vendor merge — the same Python script handles UEFI 2.11 today and
will handle UEFI 2.12 by re-running against newer spec HTML. See
Phase N6 in `docs/ROADMAP.md` for the full extraction story.

**Note:** This document was written during the initial design and
migration from UdkLib. All phases described below (R, S1-S5, M1-M6,
C1-C3) are complete. The design rationale remains relevant. See
`docs/ROADMAP.md` for current status of all phases.

## Vision

```c
#include <axl.h>

int axl_main(int argc, char **argv)
{
    axl_printf("Hello from %s\n", argv[0]);

    AxlStream *f = axl_fopen("fs0:/data.txt", "r");
    char *line = axl_readline(f);
    axl_printf("first line: %s\n", line);
    axl_free(line);
    axl_fclose(f);

    return 0;
}
```

No `gBS`, no `Print()`, no `AllocatePool()`, no `CHAR16`, no
PascalCase, no wide strings. AXL is UTF-8 everywhere, like GLib.

## API Style: GLib for UEFI

AXL is a GLib-style library. All new public API uses the same
naming pattern as GLib: types are `AxlPascalCase` (like
`GHashTable`), functions are `axl_snake_case` (like
`g_hash_table_new`). This is the only API that applications use.

```c
#include <axl.h>

AxlHashTable *h = axl_hash_table_new_str();
axl_hash_table_insert(h, "key", value);
char *v = axl_hash_table_lookup(h, "key");
axl_hash_table_free(h);
```

All new code — library modules, application code, tests — must
use this style. No new PascalCase EDK2-style functions.

## What AXL Provides

### API Overview

| Category | Target API (axl.h) | GLib equivalent |
|----------|--------------------|-----------------|
| Hash table | `axl_hash_table_new`, `axl_hash_table_insert`, `axl_hash_table_lookup` | `g_hash_table_new` |
| Dynamic array | `axl_array_new`, `axl_array_append`, `axl_array_get` | `g_array_new` |
| Strings | `axl_strdup`, `axl_strsplit`, `axl_strjoin` | `g_strdup`, `g_strsplit` |
| JSON | `axl_json_parse`, `axl_json_build` | json-glib |
| Event loop | `axl_loop_new`, `axl_loop_run`, `axl_loop_quit` | `g_main_loop_new` |
| Event (signal/wait latch) | `axl_event_new`, `axl_event_signal`, `axl_event_wait_timeout` | Linux `struct completion`, C++ `std::latch` |
| Wait helpers | `axl_wait_for_flag`, `axl_wait_for_word`, `axl_wait_ms`, `axl_wait_for_with_tick` | POSIX condvar family |
| Cancellation token | `axl_cancellable_new`, `axl_cancellable_cancel`, `axl_cancellable_is_cancelled` | `GCancellable` |
| Config + CLI | `axl_config_new`, `axl_config_parse_args`, `axl_config_get_*` | `g_option_context_new`, `GKeyFile` |
| Logging | `axl_log`, `axl_debug`, `axl_warning` | `g_log`, `g_debug` |
| File I/O | `axl_file_read_all`, `axl_file_write_all` | `g_file_get_contents` |
| Task pool | `axl_task_pool_new`, `axl_task_submit` | `g_thread_pool_new` |
| TCP sockets | `axl_tcp_connect`, `axl_tcp_listen` | `g_socket_new` |
| HTTP server | `axl_http_server_new`, `axl_http_server_route` | libsoup |
| HTTP client | `axl_http_get`, `axl_http_post` | libsoup |
| URL parsing | `axl_url_parse` | `g_uri_parse` |

These modules are fully working internally using EDK2-style names
(`AxlHashNew`, `AxlArrayAppend`, etc.). Phase S5 wraps them with
the `axl_` public API shown above.

### New (this document)

| Category | POSIX-style (axl.h) | Purpose |
|----------|---------------------|---------|
| Memory | `axl_malloc`, `axl_free`, `axl_realloc` | Allocation with leak tracking |
| String builder | `axl_string_*` | Mutable auto-growing strings |
| I/O streams | `axl_fopen`, `axl_fclose`, `axl_fread`, `axl_fwrite` | Stream abstraction |
| Printf | `axl_printf`, `axl_fprintf`, `axl_asprintf` | Console and stream output |
| Conversion | `axl_utf8_to_ucs2`, `axl_base64_encode` | Encoding utilities |

For the synchronization-primitive taxonomy (AxlEvent, AxlCancellable,
AxlWait, AxlLoop sources, AxlDefer, AxlPubsub, AxlTask) and a
decision guide for which primitive to reach for, see
[`AXL-Concurrency.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md).

## Phases

| Phase | Module | Scope | Depends On |
|-------|--------|-------|------------|
| R | Rename | UdkLib -> AXL (all symbols, files, packages) | None |
| S1 | axl_mem | `axl_malloc/free/realloc`, leak tracking | Rename |
| S2 | axl_string | Mutable strings, format, convert, base64 | S1 |
| S3 | axl_io | Streams, printf, readline | S1, S2 |
| S4 | AXL_APP | Entry point macro, `int main(argc, argv)` | S1-S3 |
| M1 | Migrate AxlLog | GLib-style logging API | S1-S3 |
| M2 | Migrate AxlData | Hash, Array, String, JSON | M1 |
| M3 | Migrate AxlUtil | File, Path, Args, HexDump, Time | M1, M2 |
| M4 | Migrate AxlLoop | Event loop, timers | M1 |
| M5 | Migrate AxlTask | Worker pool, arena | M1, M4 |
| M6 | Migrate AxlNet | TCP, HTTP, URL | M1-M5 |

---

## Phase R: Rename UdkLib to AXL

Global rename across all repositories. Mechanical, no logic changes.

### Symbol renames

| Old | New |
|-----|-----|
| `UDK_` prefix (types) | `AXL_` |
| `Udk` prefix (functions) | `Axl` |
| `UdkLib` (package) | `AxlLib` |
| `UdkLib.h` | `AxlLib.h` |
| `UdkLib.dec` | `AxlLib.dec` |
| `UdkNet.h` | `AxlNet.h` |
| `UdkSmbios.h` | `AxlSmbios.h` |
| `UDK_LOG_DOMAIN` | `AXL_LOG_DOMAIN` |
| `UdkLogLib`, `UdkDataLib`, etc. | `AxlLogLib`, `AxlDataLib`, etc. |

### Directory renames

| Old | New |
|-----|-----|
| `UdkLib/` | `AxlLib/` |
| `UdkLib/Log/` | `AxlLib/Log/` |
| `UdkLib/Data/` | `AxlLib/Data/` |
| `UdkLib/Util/` | `AxlLib/Util/` |
| `UdkLib/Loop/` | `AxlLib/Loop/` |
| `UdkLib/Task/` | `AxlLib/Task/` |
| `UdkLib/Net/` | `AxlLib/Net/` |

### Affected repositories

- `uefi-devkit` — AxlLib source, DevKitPkg consumers, docs
- `axl-webfs` — HttpFsPkg.dsc references, axl-webfs-dxe consumer
- `softbmc` — SoftBmcPkg.dsc references, all consumers
- `uefi-ipmitool` — IpmiToolPkg.dsc references

### Approach

1. Rename directory `UdkLib/` -> `AxlLib/`
2. Sed all files: `UDK_` -> `AXL_`, `Udk` -> `Axl`, `UdkLib` -> `AxlLib`
3. Rename header files
4. Update all .dsc/.inf/.dec files
5. Build + test all projects
6. Update external repos (axl-webfs, softbmc, ipmitool)

---

## Phase S1: axl_mem

POSIX-style allocation wrappers with dmalloc-inspired debug features.
Backend code uses `gBS->AllocatePool`/`gBS->FreePool` internally
until Phase S5 migration.

### API (axl.h)

```c
void *axl_malloc(size_t size);
void *axl_calloc(size_t count, size_t size);
void *axl_realloc(void *ptr, size_t size);
void  axl_free(void *ptr);
char *axl_strdup(const char *s);
void *axl_memdup(const void *src, size_t size);

#define axl_new(Type)              ((Type *)axl_calloc(1, sizeof(Type)))
#define axl_new_array(Type, Count) ((Type *)axl_calloc((Count), sizeof(Type)))
```

### Debug features (dmalloc-inspired, DEBUG builds only)

All debug features compile in via `AXL_MEM_DEBUG` (set automatically
in DEBUG builds). RELEASE builds have only the size header for realloc.

| Feature | Pattern | Purpose |
|---------|---------|---------|
| Fence-post guards | `0xC0C0AB1B` head/mid, `0xFACADE69` tail | Detect buffer underflow/overflow |
| Alloc fill | `0xDA` | Expose use-before-init |
| Free fill | `0xDF` | Expose use-after-free |
| File/line tracking | `__FILE__`, `__LINE__` via macros | Pinpoint leak and corruption sources |
| Leak report | `axl_mem_dump_leaks()` | Log all unfreed with file:line |
| Fence validation | `axl_mem_check(ptr)` | Validate a pointer's fence-posts on demand |
| Stats | `axl_mem_get_stats()` | Live count/bytes + lifetime totals |

The public API functions are macros that inject `__FILE__`/`__LINE__`
and call `_impl` functions internally.

### The size tracking trick

POSIX `realloc(ptr, new_size)` doesn't take old size. EDK2's
`ReallocatePool(old_size, new_size, ptr)` requires it. Solution:

```
[hidden header: UINTN alloc_size][user data...]
                                  ^-- returned pointer
```

`axl_malloc(n)` allocates `n + header_size`, stores `n` in the
header, returns pointer past it. `axl_realloc` reads the header
to get old size, allocates new, copies, frees old. `axl_free`
backs up to the header before calling `FreePool`.

### Memory layout (DEBUG)

```
[UINTN: fence_head 0xC0C0AB1B]
[UINTN: alloc_size            ]
[CHAR8*: file pointer         ]
[UINTN: line number           ]
[UINTN: fence_mid  0xC0C0AB1B]
[user data ... padded to UINTN]
[UINTN: fence_tail 0xFACADE69]
```

### Memory layout (RELEASE)

```
[UINTN: alloc_size            ]
[user data ......................]
```

### No intermediate wrappers

There is no `AxlAlloc`/`AxlFree` layer. Backend code
uses `AllocatePool`/`AllocateZeroPool`/`FreePool` directly. Phase S5
migrates internals straight to `axl_malloc`/`axl_free`. UEFI protocol
buffers that are passed to/from UEFI APIs stay as raw `AllocatePool`
since UEFI may free them.

---

## Phase S2: axl_string

Mutable auto-growing string buffers. Works with `char *` (UTF-8),
consistent with the rest of the AXL API.

### POSIX-style API (axl.h)

```c
typedef struct AxlString AxlStrBuf;

AxlString *axl_string_new(void);
AxlString *axl_string_new_size(size_t reserve);

void        axl_string_append(AxlString *b, const char *s);
void        axl_string_append_n(AxlString *b, const char *data, size_t len);
void        axl_string_append_printf(AxlString *b, const char *fmt, ...);
void        axl_string_putc(AxlString *b, char c);

const char *axl_string_str(AxlString *b);
size_t      axl_string_len(AxlString *b);
char       *axl_string_steal(AxlString *b);  // caller owns string

void        axl_string_clear(AxlString *b);  // reset, keep alloc
void        axl_string_free(AxlString *b);

// Convenience: format into a new string (like asprintf)
char *axl_asprintf(const char *fmt, ...);
```

### Conversion utilities

```c
// UTF-8 <-> UCS-2 conversion (internal use, exposed for UEFI protocol interop)
// Newly allocated, caller frees.
unsigned short *axl_utf8_to_ucs2(const char *s);
char           *axl_ucs2_to_utf8(const unsigned short *s);

// Base64
char *axl_base64_encode(const void *data, size_t size);
int   axl_base64_decode(const char *b64, void **data, size_t *size);

// Number parsing (handles 0x prefix for hex)
uint64_t axl_strtou64(const char *s);
```

---

## Phase S3: axl_io

Stream-based I/O. Every target (console, file, buffer) is an
`axl_stream` with read/write/printf/close.

### POSIX-style API (axl.h)

```c
typedef struct AxlStream AxlStream;

// Standard streams (initialized at startup)
extern AxlStream *axl_stdout;
extern AxlStream *axl_stderr;
extern AxlStream *axl_stdin;

// File streams
AxlStream *axl_fopen(const char *path, const char *mode);  // "r", "w", "a"
void        axl_fclose(AxlStream *s);

// Buffer streams (in-memory, auto-growing)
AxlStream *axl_bufopen(void);
const void *axl_bufdata(AxlStream *s, size_t *size);
void       *axl_bufsteal(AxlStream *s, size_t *size);

// Read/Write
size_t axl_fread(void *buf, size_t size, size_t count, AxlStream *s);
size_t axl_fwrite(const void *buf, size_t size, size_t count, AxlStream *s);
char  *axl_readline(AxlStream *s);  // caller frees, NULL at EOF

// Printf family
int axl_printf(const char *fmt, ...);            // -> stdout
int axl_eprintf(const char *fmt, ...);           // -> stderr
int axl_fprintf(AxlStream *s, const char *fmt, ...);  // -> any stream
```

### Format specifiers

AXL has its own printf engine (`axl_vformat`) using standard C
`va_arg`. Format specifiers follow standard C printf conventions:

| Specifier | Type |
|-----------|------|
| `%s` | `char *` (UTF-8 string) |
| `%d`, `%i` | `int` (signed decimal) |
| `%u` | `unsigned int` |
| `%x`, `%X` | `unsigned int` (hex) |
| `%ld`, `%lu`, `%lx` | `long` / `unsigned long` |
| `%lld`, `%llu`, `%llx` | `long long` / `unsigned long long` |
| `%zu` | `size_t` |
| `%c` | `char` |
| `%p` | `void *` (pointer) |
| `%%` | literal `%` |

Width (`%10d`), zero-padding (`%08x`), left-align (`%-20s`),
precision (`%.5s`), and `*` width/precision from args are supported.

Use `__attribute__((format(printf, N, M)))` for compile-time checking.

### Stream internals

```c
typedef size_t (*AxlWriteFn)(void *ctx, const void *data, size_t size);
typedef size_t (*AxlReadFn)(void *ctx, void *buf, size_t size);
typedef void   (*AxlCloseFn)(void *ctx);

struct AxlStream {
    void        *ctx;
    AxlWriteFn  write;
    AxlReadFn   read;
    AxlCloseFn  close;
};
```

Extensible: serial port, network socket, UEFI variable backends
can be added by providing the vtable functions.

---

## Phase S4: axl.h and axl_main

The top-level header and entry point that ties everything together.

### axl.h

Single include that pulls in the full POSIX-style API:

```c
#ifndef AXL_H_
#define AXL_H_

#include <stddef.h>       // size_t
#include <stdint.h>       // uint64_t, etc.
#include <stdbool.h>      // bool

// Memory
void *axl_malloc(size_t size);
void *axl_calloc(size_t count, size_t size);
void *axl_realloc(void *ptr, size_t size);
void  axl_free(void *ptr);
char *axl_strdup(const char *s);
void *axl_memdup(const void *src, size_t size);
#define axl_new(Type)              ((Type *)axl_calloc(1, sizeof(Type)))
#define axl_new_array(Type, Count) ((Type *)axl_calloc((Count), sizeof(Type)))

// I/O
typedef struct AxlStream AxlStream;
extern AxlStream *axl_stdout;
extern AxlStream *axl_stderr;
extern AxlStream *axl_stdin;
int   axl_printf(const char *fmt, ...);
int   axl_fprintf(AxlStream *s, const char *fmt, ...);
char *axl_asprintf(const char *fmt, ...);
// ... etc.

// Data structures — GLib-style hash table (see GHashTable docs)
typedef struct AxlHashTable AxlHashTable;
AxlHashTable *axl_hash_table_new(void);                         // string keys, copied
AxlHashTable *axl_hash_table_new_full(AxlHashFunc, AxlEqualFunc,
                                      AxlDestroyNotify key_free,
                                      AxlDestroyNotify val_free); // generic keys
int       axl_hash_table_insert(AxlHashTable *h, const void *key, void *val);
int       axl_hash_table_replace(AxlHashTable *h, const void *key, void *val);
void     *axl_hash_table_lookup(AxlHashTable *h, const void *key);
bool      axl_hash_table_contains(AxlHashTable *h, const void *key);
bool      axl_hash_table_remove(AxlHashTable *h, const void *key);
bool      axl_hash_table_steal(AxlHashTable *h, const void *key);
size_t    axl_hash_table_foreach_remove(AxlHashTable *h, ...);
// Iterator: AxlHashTableIter with iter_init, iter_next, iter_remove

#endif
```

### axl_main entry point

```c
// App code today:
#include <axl.h>

int main(int argc, char **argv)
{
    axl_printf("Hello %s\n", argv[1]);
    return 0;
}
```

The CRT0 entry stub at `src/crt0/axl-crt0-native.c` implements
`_AxlEntry(ImageHandle, SystemTable)` once for the whole SDK. It
sets the firmware-table globals, calls `_axl_init` (which enters
the runtime), parses argv via the runtime's `_axl_get_args`, calls
your `main(argc, argv)`, then calls `_axl_cleanup` and returns
`EFI_SUCCESS` (rc=0) or `EFI_ABORTED`. Apps never see
`EFI_HANDLE`, `EFI_SYSTEM_TABLE`, or `EFIAPI`. The historical
`AXL_APP(main_func)` macro that inlined this stub still exists
for backwards compatibility; new code just declares `int main(int,
char **)` and links against the CRT0 object that `axl-cc`
includes by default. Full lifecycle: [`AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md).

---

## Phase S5: Migration

Rename all UdkLib internals to AXL and rewrite apps to use `axl.h`.

### Internal rename

All UdkLib source files: `UDK_` -> `AXL_`, `Udk` -> `Axl`. The
EDK2-style API (`Library/AxlLib.h`) is the internal layer. The
POSIX-style API (`axl.h`) wraps it.

### App migration example

Before (current):
```c
#include "SysInfo.h"  // pulls in Uefi.h, UefiLib.h, ShellLib.h, ...

BOOLEAN gVerbose = FALSE;
STATIC CONST UDK_OPT_DEF mOpts[] = { ... };

EFI_STATUS EFIAPI SysInfoMain(EFI_HANDLE Img, EFI_SYSTEM_TABLE *ST)
{
    EFI_SHELL_PARAMETERS_PROTOCOL *P;
    gBS->OpenProtocol(Img, &gEfiShellParametersProtocolGuid, ...);
    UDK_ARGS *Args;
    UdkArgsParse(P->Argc, P->Argv, mOpts, &Args);
    if (UdkArgsFlag(Args, 'v')) gVerbose = TRUE;
    Print(L"CPU: %s\n", CpuName);
    UdkArgsFree(Args);
    return EFI_SUCCESS;
}
```

After:
```c
#include <axl.h>

static bool verbose = false;
static const AxlConfigDesc descs[] = {
    { "verbose", AXL_CFG_BOOL, "false", 'v', "Verbose output", 0, 0 },
    { "help",    AXL_CFG_BOOL, "false", 'h', "Show help",      0, 0 },
    { 0 }
};

int sysinfo_main(int argc, char **argv)
{
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    if (cfg == NULL || axl_config_parse_args(cfg, argc, argv) != 0) {
        axl_config_usage(cfg, "SysInfo", "[section]");
        return 1;
    }
    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "SysInfo", "[section]");
        return 0;
    }
    verbose = axl_config_get_bool(cfg, "verbose");
    axl_printf("CPU: %s\n", cpu_name);
    return 0;
}

AXL_APP(sysinfo_main)
```

---

## Migration Phases (M1–M6)

Migrate each internal EDK2-style module to GLib-style. For each
module the work is:

1. Create `axl/<module>.h` header with GLib-style declarations
2. Rename functions: `AxlFoo` → `axl_foo`
3. Rename types: keep `AxlPascalCase` (already correct)
4. Switch allocations: `AllocatePool` → `axl_malloc`, `FreePool` → `axl_free`
5. Switch logging: `axl_error`/`Print` → `axl_printerr`
6. Switch strings: `CHAR8 *` → `char *`, `UINTN` → `size_t`
7. Rename source files: `AxlFoo.c` → `axl-foo.c`
8. Update tests to use new API names
9. Remove old declarations from `Library/AxlLib.h`
10. Update consumer projects (uefi-devkit, axl-webfs, uefi-ipmitool, softbmc)

### Backwards compatibility during migration

Each phase provides a compatibility header
`Library/AxlLib.h` with `#define` macros mapping old names to new:

```c
/* Temporary compat — remove after all consumers are updated */
#define AxlHashNew()          axl_hash_table_new_str()
#define AxlHashFree(h)        axl_hash_table_free(h)
#define AxlHashSet(h, k, v)   axl_hash_table_insert((h), (k), (v))
```

This lets consumer projects migrate incrementally rather than
requiring a flag-day update across all repos. Compat macros are
removed once all consumers have switched.

### Consumer projects to update

| Project | Repo | Modules used | Notes |
|---------|------|-------------|-------|
| uefi-devkit | aximcode/uefi-devkit | Log, Data, Util, Loop, Task, Net | Heaviest consumer — all modules |
| axl-webfs | aximcode/axl-webfs | Log, Data, Loop, Net | axl-webfs-dxe uses Hash + HttpClient; axl-webfs uses Loop + HttpServer |
| uefi-ipmitool | aximcode/uefi-ipmitool | (none yet) | Does not currently use AxlLib |
| softbmc | aximcode/softbmc | Log, Data, Util, Loop, Net | Not yet migrated |

For each migration phase, the "Consumer project changes" subsection
lists exactly what each project needs. All projects need
`AxlFormatLib` added to their DSC (done in M1).

### M1: Migrate AxlLog ✓

Logging is the foundation — every other module depends on it.

Old API (AxlLib.h):
```c
AxlLogFull(level, domain, func, line, fmt, ...);
AxlLogSetLevel(level);
AxlLogAddHandler(handler, data);
```

New API (axl/axl-log.h):
```c
axl_log_set_level(level);
axl_log_add_handler(handler, data);
/* Convenience macros unchanged: axl_error, axl_info, etc.
   — these are macros, not functions, and stay uppercase
   because they inject __func__/__LINE__ */
```

Implementation notes:
- AxlFormatLib extracted as zero-dependency library (breaks Log↔Data cycle)
- Ring buffer and file handler keep `AllocatePool`/`FreePool` (breaks Log↔Mem cycle)
- File handler keeps ShellLib (breaks Log↔IO cycle)
- Format engine switched from PrintLib (`%a`) to axl_vformat (`%s`)
- `__attribute__((format(printf,...)))` on `axl_log_full` — catches mismatches at compile time
- `AxlLogFileAttach(CHAR16*)` replaced by `axl_log_file_attach(char*)` — no compat macro
  (signature changed, callers must update)

Consumer project changes for M1:
- All projects: add `AxlFormatLib` to `.dsc` `[LibraryClasses]`:
  `AxlFormatLib|src/format/AxlFormat.inf`
- All projects using `axl_error`/`axl_info` with `%a` format: change to `%s`
- All projects using `%u`/`%d`/`%x` with `UINTN`/`INTN`: change to `%llu`/`%lld`/`%llx`
- All projects calling `AxlLogFileAttach(L"...")`: change to `axl_log_file_attach("...")`
- All projects defining `AXL_LOG_HANDLER` callbacks: update signature from
  `VOID EFIAPI (UINTN, CONST CHAR8*, CONST CHAR8*, VOID*)` to
  `void (int, const char*, const char*, void*)`
- axl-webfs: no code changes needed (does not call log macros with format args
  or define handler callbacks). DSC needs `AxlFormatLib` line added.
- uefi-ipmitool: no changes needed (does not use AxlLib)

### M2: Migrate AxlData ✓

Hash table, dynamic array, string utilities, JSON.

Old: `AxlHashNew`, `AxlArrayAppend`, `AxlStrDup`, `AxlJsonParse`
New: `axl_hash_table_new`, `axl_array_append`, `axl_strdup` (already exists), `axl_json_parse`

Consumer project changes for M2:
- axl-webfs (axl-webfs-dxe): uses `AxlHashNew`, `AxlHashSet`, `AxlHashFree`,
  `AXL_HASH_TABLE`. Compat macros cover function renames. Type rename
  `AXL_HASH_TABLE` → `AxlHashTable` needs a compat `#define`.
- uefi-devkit: uses hash, array, string, JSON extensively. Compat macros
  cover most usage; manual migration of type names may be needed.

### M3: Migrate AxlUtil

File I/O, path utils, args, hex dump, time.

Old: `AxlFileReadAll`, `AxlPathBasename`, `AxlArgsParse`
New: `axl_file_read_all` (→ merge with `axl_file_get_contents`), `axl_path_get_basename`, `axl_config_parse_args`

Consumer project changes for M3:
- AxlFile paths switch from `CHAR16*` to `char*` (UTF-8). All consumers
  using `AxlFileReadAll`/`AxlFileWriteAll` with wide paths must convert.
  No compat macro possible (signature change).
- AxlArgs originally switched from `CHAR16**` argv to `char**` argv.
  `axl_args_*` was later folded into `axl_config_*` — the standalone
  argument parser is gone. Apps using `AXL_APP()` get `char** argv`
  unchanged; callers that used `axl_args_parse` now declare an
  `AxlConfigDesc` table and call `axl_config_parse_args`.
- uefi-devkit: originally the heaviest user of AxlArgs and AxlFile.
  Needs to switch its tool main()s to `AxlConfig` descriptors.

### M4: Migrate AxlLoop

Event loop, timers, idle dispatch.

Old: `AxlLoopNew`, `AxlLoopRun`, `AxlLoopAddTimer`
New: `axl_loop_new`, `axl_loop_run`, `axl_loop_add_timer`

Consumer project changes for M4:
- axl-webfs (axl-webfs serve command): uses AxlLoop for HTTP server event loop.
  Compat macros cover function renames. Callback types (`AXL_CALLBACK`,
  `AXL_KEY_CALLBACK`) will be renamed — compat `#define` needed.
- uefi-devkit: several tools use AxlLoop. Same compat approach.

#### Event Loop Architecture

AxlLoop is a GLib-style event loop with multiple source types:

| Source | API | Behavior |
|--------|-----|----------|
| Timer | `axl_loop_add_timer` | Repeating at fixed interval |
| Timeout | `axl_loop_add_timeout` | One-shot, auto-removed |
| Keypress | `axl_loop_add_key_press` | Console input |
| Idle | `axl_loop_add_idle` | Fires every iteration |
| Protocol | `axl_loop_add_protocol_notify` | UEFI protocol install |
| **Event** | **`axl_loop_add_event`** | **Raw EFI_EVENT — async notification** |

The raw event source (`AXL_SOURCE_EVENT`) accepts any EFI_EVENT handle
and fires a callback when signaled. The caller owns the event — the
loop watches but does not close it. This enables async integration:

- TCP completion tokens signal when connect/send/recv complete
- Custom protocol events from UEFI drivers
- Any firmware event (USB, storage, network state)

**Dispatch model:** The loop builds an event array from all active
non-idle sources and calls WaitForEvent (blocking) or CheckEvent
(non-blocking). A 10ms poll timer provides Ctrl-C detection. Idle
callbacks fire every iteration before the wait.

**Async TCP (shipped April 2026).** The async TCP API uses
`axl_loop_add_event` to register completion token events directly,
eliminating polling. Each entry point takes an optional
`AxlCancellable *` so callers can abort the operation (see
`src/util/README.md` for the cancellation model):

```
axl_tcp_connect_async(host, port, loop, cancel, on_connect, data);
axl_tcp_recv_async   (sock,  buf, size, loop, cancel, on_data, data);
axl_tcp_accept_async (listener,    loop, cancel, on_accept, data);
```

User callbacks return `bool`. For recv and accept, `true` keeps the
op armed (loop re-issues the UEFI op) and `false` tears it down;
connect and send callbacks ignore the return. Returning `false`
permits closing the passed socket inside the callback — the library
does not access it after a false return. Contract: if you return
`true`, the socket must remain valid.

The blocking API (`axl_tcp_connect`, etc.) is a thin sync wrapper
over the async + a per-call cancellable for its timeout.

### M5: Migrate AxlTask

AP worker pool, arena allocator.

Old: `AxlTaskPoolInit`, `AxlTaskSubmit`, `AxlArenaCreate`
New: `axl_task_pool_init`, `axl_task_submit`, `axl_arena_create`

Consumer project changes for M5:
- uefi-devkit: SysInfo uses task pool for parallel CPU/memory probing.
  Compat macros cover function renames. Callback types
  (`AXL_TASK_PROC`, `AXL_TASK_COMPLETE`) will be renamed.
- axl-webfs, uefi-ipmitool: do not use AxlTask. No changes needed.

### M6: Migrate AxlNet

TCP, HTTP server/client, URL parser.

Old: `AxlTcpConnect`, `AxlHttpServerNew`, `AxlUrlParse`
New: `axl_tcp_connect`, `axl_http_server_new`, `axl_url_parse`

Consumer project changes for M6:
- axl-webfs (axl-webfs-dxe): uses `AxlHttpClientNew`, `AxlHttpGet`,
  `AxlHttpRequest`, `AxlHttpClientResponseFree`, `AxlHttpClientFree`.
  Compat macros cover function renames. Type renames
  (`AXL_HTTP_CLIENT` → `AxlHttpClient`, `AXL_HTTP_CLIENT_RESPONSE` →
  `AxlHttpResponse`) need compat `#define`.
- axl-webfs (axl-webfs serve): uses `AxlHttpServer*` extensively. Same approach.
- uefi-devkit (Fetch): uses HTTP client. Compat macros cover it.
- uefi-ipmitool: does not use AxlNet. No changes needed.

### C1: Style Guide Compliance

Post-migration cleanup. The M-phases focused on API renames and type
migration; this phase enforces the coding style consistently across
all source files.

Scope:
- **Spaces before parens:** remove `func (arg)` → `func(arg)` everywhere
- **Doc comments:** convert `@param` blocks to `///<` inline style
  per the updated coding style guide
- **EDK2 qualifiers:** remove remaining `IN`, `OUT`, `OPTIONAL`,
  `STATIC` (use `const`, pointer semantics, `static`)
- **EDK2 types in migrated code:** `TRUE`→`true`, `FALSE`→`false`,
  `CONST`→`const`, `BOOLEAN`→`bool`, `VOID`→`void` in function
  signatures (internal UEFI code may keep them)
- **Allocator consistency:** replace remaining `AllocatePool`/`FreePool`
  with `axl_malloc`/`axl_free` where safe (not in AP-callable code
  or UEFI protocol buffers)
- **Include cleanup:** remove unnecessary EDK2 includes from files
  that no longer use UEFI APIs directly

Files: all `axl-*.c` and `axl-*.h` under `src/` and `include/`.
Consumer projects: axl-webfs, uefi-devkit (if affected).

### C2: Dogfooding

Switch internal implementations from EDK2 primitives to AXL APIs.
The migration phases renamed the public API but left internals using
raw UEFI calls. This phase makes AXL eat its own dog food.

Scope:
- **Memory:** replace remaining `AllocatePool`/`AllocateZeroPool`/
  `FreePool` with `axl_malloc`/`axl_calloc`/`axl_free` in all
  migrated modules (Net, Data, Util). Exception: AP-callable code
  and buffers passed to/from UEFI protocols.
- **String ops:** replace `AsciiStrLen`/`AsciiStrCmp`/`AsciiStrnCmp`/
  `AsciiSPrint`/`CopyMem` with local helpers or AXL equivalents
  (`axl_strlcpy`, `axl_vformat`, etc.).
- **Console output:** replace `Print(L"...")` with `axl_print()`
  in all migrated modules. `axl-json-print.c` is the main target.
- **Logging:** ensure every module has `AXL_LOG_DOMAIN` and uses
  `axl_error`/`axl_warning`/`axl_info`/`axl_debug` for diagnostics
  instead of silent failures or `Print`. Audit modules that
  currently have no logging (e.g. axl-string.c, axl-url.c).
- **Include cleanup:** remove `<Library/UefiLib.h>`,
  `<Library/PrintLib.h>`, `<Library/MemoryAllocationLib.h>` from
  files that no longer need them after switching to AXL APIs.

### C3: Test Modernization

Migrate test applications from EDK2 entry points to `AXL_APP` and
standardize the test framework.

Scope:
- **AXL_APP entry points:** convert all 9 test files from
  `EFI_STATUS EFIAPI AxlTestFooMain(EFI_HANDLE, EFI_SYSTEM_TABLE*)`
  to `int test_foo_main(int argc, char **argv)` + `AXL_APP(...)`.
  Update .inf files: `ENTRY_POINT = _AxlEntry`.
- **Test output:** replace `Print(L"PASS: %s\n", Name)` with
  `axl_printf("PASS: %s\n", name)` — UTF-8 test names instead of
  wide strings.
- **Test helpers:** extract shared Pass/Fail/Check into a common
  header (`axl-test.h`) instead of duplicating in every test file.
- **Test .inf cleanup:** standardize `[LibraryClasses]` across all
  test .inf files (add AxlAppLib, remove UefiApplicationEntryPoint
  where possible).
- **Consumer test coverage:** add build verification for axl-webfs and
  uefi-devkit as part of the test-axl.sh runner.

---

## Platform Access Modules

A family of modules providing UEFI apps with typed access to
firmware-level platform inventory and control, so consumers don't
each reimplement the transport layer. First entry (AxlSmbios) has
been in the library since the initial migration; AxlIpmi shipped
in April 2026; AxlAcpi / AxlPci / AxlSpd shipped in April-May
2026; AxlUsb shipped in May 2026 (Phases A-F: enumeration, class
decode, string descriptors, vendor/device-name JSON5 sidecar,
`lsusb.efi` tool, and faithful hub-port tree walker).

### Design conventions

All platform-access modules follow a shared shape:

- **Opaque session handle** wrapping per-transport state
  (`AxlSmbiosSession` is trivial since SMBIOS is a memory scan;
  `AxlIpmiSession` holds the selected transport and vtable).
- **Auto-detect at construction.** `axl_ipmi_session_new()` walks
  SMBIOS Type 38 + LocateProtocol; `axl_pci_session_new()` (future)
  walks ACPI MCFG. The consumer says "give me access to X" without
  naming the interface.
- **Raw command entry point** for edge cases + typed wrappers for
  the common path. `axl_ipmi_raw()` + `axl_ipmi_get_device_id()`;
  future `axl_pci_read_config()` + `axl_pci_walk()`.
- **Transport vtable** in the internal header so new transports
  plug in without touching dispatch (e.g., `axl-ipmi-edkii.c`,
  `axl-ipmi-dell.c`).
- **Backend hooks** for low-level platform I/O. Phase B1 added
  `axl_backend_io_read8/write8` (x86 port I/O). SMBus started as a
  backend hook pair alongside these but graduated into its own
  `AxlSmbus` Platform Access Module once a second consumer (AxlSpd)
  came into view — anonymous backend pairs don't scale past one
  caller. Future modules extend the backend when a primitive is
  genuinely single-caller (e.g., ECAM for AxlPci); otherwise they
  stand up their own module.
- **Public API uses standard C types only**; no UEFI types leak
  across the module boundary. Headers belong in the `<axl.h>`
  umbrella.
- **Dogfood tool in `tools/`**: `sysinfo` exercises AxlSmbios,
  `ipmi` exercises AxlIpmi, `lspci` exercises AxlPci, `lsusb`
  exercises AxlUsb, `memspd` exercises AxlSpd. Each tool is a
  thin renderer over its module's public surface — if a consumer
  ever needs a different output shape, they can build their own
  on the same primitives.

### Roster

| Module | Header | Status | Consumer tool |
|--------|--------|--------|---------------|
| AxlSmbios | `axl/axl-smbios.h` | DONE | `tools/sysinfo` |
| AxlIpmi   | `axl/axl-ipmi.h`   | DONE (April 2026) | `tools/ipmi` |
| AxlSmbus  | `axl/axl-smbus.h`  | DONE (April 2026) | (consumed by AxlIpmi SSIF + AxlSpd) |
| AxlAcpi   | `axl/axl-acpi.h`   | DONE (April 2026) — table discovery + header parsing, NO AML | (consumed by AxlPci MCFG lookup) |
| AxlPci    | `axl/axl-pci.h`    | DONE (April 2026) — ECAM config space + ids/class JSON5 sidecars | `tools/lspci` |
| AxlSpd    | `axl/axl-spd.h`    | DONE (April 2026) — DDR4/5 SPD readers + JEDEC ids JSON5 sidecar | `tools/memspd` |
| AxlUsb    | `axl/axl-usb.h`    | DONE (May 2026) — enumeration, class decode, string descriptors, hub-port tree, ids JSON5 sidecar | `tools/lsusb` |

AxlAcpi scope specifically excludes AML interpretation
(ACPICA-sized); it only reads fixed-layout tables.

The four modules with JSON5 vendor/device-name sidecars
(AxlPciIds, AxlPciClassDb, AxlSpdIds, AxlUsbIds) all build on a
common `<axl/axl-sidecar.h>` scaffold (`axl_sidecar_open_file` /
`_open_buffer` / `_check_schema` plus an internal singleton-with-
atexit + foreach trampoline). Adding a new sidecar consumer is
~50 lines of typed-walk code over the shared loader rather than a
fresh hash-table-and-lifecycle reinvention.

### Module layout

```
include/axl/axl-<family>.h   public API (standard C types)
src/<family>/axl-<family>.c              core dispatcher + auto-detect
src/<family>/axl-<family>-internal.h     transport vtable, session layout
src/<family>/axl-<family>-<transport>.c  one file per transport
src/<family>/axl-<family>-cmd.c          typed command wrappers
src/<family>/axl-<family>-format.c       enum-to-string tables
src/<family>/README.md                   module doc (pulled into Sphinx)
test/unit/axl-test-<family>.c            unit tests via callback shim
test/integration/test-<family>.sh        manual hardware runbook
test/fuzz/<family>_fuzz.c                response-parser fuzz target
tools/<consumer>.c                       dogfood tool
```

AxlIpmi is the reference implementation of this layout; future
Platform Access modules mirror it.

---

## Coding Style

See [AXL-Coding-Style.md](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Coding-Style.md) for the full style
guide. The short version: GLib naming, 4-space indent, standard C
types in public API, no space before parens.

## Design Principles

1. **C is the language.** `char *` not `CHAR8`. `size_t` not `UINTN`.
   `void *` not `VOID *`. `bool` not `BOOLEAN`. snake_case, not
   PascalCase.

2. **`%s` means char\*.** AXL's printf follows C convention, not EDK2.
   `%S` (uppercase) for wide strings.

3. **axl.h is self-contained.** One include, no EDK2 headers leak
   through. Apps that need UEFI protocols include `<Library/AxlLib.h>`
   explicitly for those specific APIs.

4. **NULL-safe.** `axl_free(NULL)`, `axl_fclose(NULL)`, etc. are
   no-ops.

5. **Allocation failures return NULL.** No abort. Caller checks.

6. **UTF-8 everywhere.** All strings in the `axl.h` API are `char *`
   (UTF-8). No `CHAR16`, no `char16_t`, no wide strings. AXL converts
   to UCS-2 internally when calling UEFI APIs (file paths, console).
   Conversion functions (`axl_utf8_to_ucs2`, `axl_ucs2_to_utf8`) are
   exposed for apps that need direct UEFI protocol interop.

7. **No EDK2 dependency.** AXL has no source-tree dependency on
   EDK2 — `EFI_*` types are auto-generated from the published UEFI
   and PI specifications (see "Where the UEFI types come from"
   above), and the CRT0 entry stub bridges UEFI's `_AxlEntry`
   directly to `int main(argc, argv)` without any EDK2 build
   plumbing. The CRT0 stub calls `_axl_init` to enter the
   runtime, which wires up streams, memory, the default loop,
   atexit, the signal notify, and the tier-1 registry. The app
   just sees `int main(int argc, char **argv)`.

8. **Eat our own dog food.** AXL's own internals use the AXL API.
   The library allocates with `axl_malloc`, reports errors with
   `axl_printerr`, builds strings with `axl_string`. If the API
   isn't good enough for our own code, it isn't good enough to ship.

   Exception: very early initialization (before `axl_stream_init`) may
   use EDK2 primitives directly since streams aren't available yet.

9. **No legacy EDK2-style API.** There is no separate PascalCase
   layer. All functions follow GLib naming (`axl_snake_case`).
   UEFI types are confined to `_impl` declarations and internal
   implementation files.

## Project

### Repository

**Name:** `aximcode/axl-sdk` (public repo)

AXL is a standalone library. Consumer projects (uefi-devkit, axl-webfs,
ipmitool) depend on it via `axl-cc` or CMake integration.

### Versioning

Semantic versioning (semver). `axl.h` is the stable public API —
once 1.0 ships, breaking changes require a major version bump.

### License

BSD-2-Clause-Patent.

See `CLAUDE.md` for the current repository layout.

## Dependencies

AXL has no external dependencies. The native backend provides its
own UEFI type definitions (auto-generated from spec HTML), CRT0,
and GCC-based build toolchain. See `AXL-SDK-Design.md` for the
SDK architecture.
