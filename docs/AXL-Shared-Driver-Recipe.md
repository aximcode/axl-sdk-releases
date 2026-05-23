# Recipe: shared-driver pattern

How to split a UEFI tool into a thin launcher + a resident driver
image that hosts the tool's heavy code. Useful when the same
diagnostic verb is invoked many times in a session and you want to
amortize startup cost (LoadImage, parsing sidecar data, opening
firmware protocols) over the boot rather than paying it on every
shell invocation.

Three small helpers in [`<axl/axl-shared-driver.h>`](../include/axl/axl-shared-driver.h)
wrap the underlying lifecycle:

- `axl_shared_driver_publish(name, iface, &handle)` — driver-side, in
  DriverEntry. Derives the identity GUID from `name` via
  `axl_guid_v5` against the SDK's shared-driver namespace, then
  publishes the consumer's vtable on a fresh UEFI handle.
- `axl_shared_driver_unpublish(name, handle, iface)` — driver-side,
  in the unload callback.
- `axl_shared_driver_locate(name, driver_filename, embed, embed_len,
  &iface)` — launcher-side, in `int main`. Ensures the driver is
  loaded (resident → on-disk → embedded blob), then resolves the
  vtable.

No new AXL type is introduced: the consumer owns its vtable struct
and the AXL_DRIVER / `int main` CRT wiring. These three functions
only hide the GUID-derivation convention and the
`axl_driver_ensure_with_embedded` + `axl_protocol_find_guid`
choreography. The distribution shape is a single `.efi` file: the
driver bytes are linked into the launcher via `.incbin`.

The full pattern lives at
[`sdk/examples/shared-driver-demo/`](../sdk/examples/shared-driver-demo/).

## When to use

Reach for this pattern when:

- The tool ships a launcher + driver pair, and every invocation
  needs the driver loaded.
- Per-invocation cost is dominated by repeated heavy work
  (parsing JSON5 sidecars, opening + closing the same firmware
  protocols, re-walking the PCI bus) — not by the verb dispatch
  itself.
- You want to ship a single binary, not two files the user must
  copy together.

If the driver has periodic work to do *between* invocations, reach
for [`<axl/axl-service.h>`](../include/axl/axl-service.h) instead;
that pattern bundles a loop with the driver. The shared-driver
pattern is purely synchronous-RPC — between invocations the driver
sits in memory and does nothing.

## Code shape

### Shared header

The vtable struct is consumer-owned and must be #included by both
images. Treat it as part of your tool's public contract:

```c
// my-tool-protocol.h
#define MY_TOOL_NAME  "my-tool"   // shared-driver identity

typedef struct {
    int (*verb_a)(int arg);
    int (*verb_b)(const char *name);
} MyToolVtable;
```

### Driver side

The driver publishes the vtable via `axl_shared_driver_publish` from
its DriverEntry, after any per-boot setup work. Multi-source-file
is fine — only the entry-point file needs `AXL_DRIVER`:

```c
// my-tool-dxe.c
#include <axl.h>
#include "my-tool-protocol.h"

static int do_verb_a(int arg) { /* ... */ return arg + 1; }
static int do_verb_b(const char *name) { /* ... */ return 0; }

static MyToolVtable gVtable;
static AxlHandle    gPublishedHandle;

static int my_main(AxlHandle h, AxlSystemTable *st) {
    (void)h; (void)st;
    gVtable.verb_a = do_verb_a;
    gVtable.verb_b = do_verb_b;
    return axl_shared_driver_publish(MY_TOOL_NAME, &gVtable,
                                     &gPublishedHandle);
}

static int my_unload(AxlHandle h) {
    (void)h;
    return axl_shared_driver_unpublish(MY_TOOL_NAME,
                                       gPublishedHandle, &gVtable);
}

AXL_DRIVER(my_main, my_unload)
```

### Launcher side

The launcher calls `axl_shared_driver_locate`, which ensures the
driver is loaded (resident → on-disk → embedded blob) and resolves
the vtable in one call:

```c
// my-tool.c
#include <axl.h>
#include <axl/axl-embed.h>
#include "my-tool-protocol.h"

AXL_EMBED_DECLARE(my_tool_driver);

int main(int argc, char **argv) {
    MyToolVtable *vt = NULL;
    if (axl_shared_driver_locate(MY_TOOL_NAME,
                                 "myToolDxe.efi",
                                 AXL_EMBED_DATA(my_tool_driver),
                                 AXL_EMBED_SIZE(my_tool_driver),
                                 (void **)&vt) != AXL_OK) {
        axl_printf("my-tool: failed to load driver\n");
        return 1;
    }

    /* Parse argv and dispatch into the resident driver. */
    (void)argc; (void)argv;
    return vt->verb_a(7);
}
```

After the first invocation, the driver image stays resident.
Subsequent runs of `my-tool.efi` skip the LoadImage step entirely
— `axl_driver_ensure_with_embedded` short-circuits at step 1 when
`LocateProtocol(gMyToolGuid)` already succeeds.

## Build

Either the CMake helpers (preferred for non-trivial projects) or
`axl-cc` directly.

### CMake

```cmake
find_package(axl REQUIRED)

axl_add_driver(myToolDxe myToolDxe.c)

axl_add_app(myTool myTool.c
    EMBEDS ${myToolDxe_EFI_PATH}=my_tool_driver
)
add_dependencies(myTool myToolDxe)
```

The `${TARGET}_EFI_PATH` variable is set by `axl_add_driver` and
`axl_add_app`; use it to pass the driver's output to a launcher's
`EMBEDS` clause without re-deriving the path. The
`add_dependencies` line is required so the launcher's embed step
sees an up-to-date driver `.efi` on rebuild.

The `EMBEDS` clause takes entries of the form `PATH=NAME` (the
canonical form) or `PATH` (the embed symbol is derived from the
file's basename). Multiple entries are supported. If a path
itself contains `=`, the separator is the *last* `=` — i.e.
`a=b.efi=my_blob` embeds the file `a=b.efi` under symbol
`my_blob`. Paths containing `=` are rare in practice; if you hit
one, use the explicit `PATH=NAME` form to remove ambiguity.

### axl-cc

```bash
# Driver first — produces myToolDxe.efi
axl-cc --type driver myToolDxe.c -o myToolDxe.efi

# Launcher second — embeds the driver, produces myTool.efi
axl-cc --embed myToolDxe.efi=my_tool_driver myTool.c -o myTool.efi
```

## Sharing helpers between launcher and driver

Non-trivial consumers have helper functions both halves use: argv
peek/strip routines, output formatters, error-line builders, common
data parsing. These need to live in a translation unit that's
compiled into *both* binaries.

**Build pattern**: list the shared `.c` files in both targets'
source lists. Each binary compiles + links its own private copy of
the symbols; nothing crosses image boundaries at the symbol level.

```cmake
set(MY_TOOL_SHARED_SOURCES
    my-tool-format.c
    my-tool-argv-helpers.c
)

axl_add_driver(myToolDxe
    myToolDxe.c
    ${MY_TOOL_SHARED_SOURCES}
)

axl_add_app(myTool
    myTool.c
    ${MY_TOOL_SHARED_SOURCES}
    EMBEDS ${myToolDxe_EFI_PATH}=my_tool_driver
)
```

**Cross-TU symbol audit** — required step when splitting a
previously single-binary tool. After deciding what verbs run in
the driver vs. launcher, enumerate every function and global the
driver-side code references:

| Symbol referenced by | Lives in | Resolution |
|---|---|---|
| Driver-side TU only | Driver-side TU | Already in driver source list |
| Launcher-side TU only | Launcher-side TU | Already in launcher source list |
| Both sides | A shared TU | Add to **both** source lists (above) |

Anything in the third row but not in a shared TU is a link-time
bug. **`axl-cc` enforces `ld --no-undefined`** so a missing helper
surfaces as a precise build error:

```
undefined reference to `my_tool_helper'
  referenced from cmd_pci.c:160
```

A shared TU must list every symbol it can transitively pull in via
its own internal calls — if `my-tool-format.c` calls a helper in
`my-tool-strings.c`, both must end up on both source lists.

Hand-rolled `ld -shared` invocations (not going through axl-cc or
the CMake helpers) DON'T enforce this by default — `ld` silently
accepts undefined symbols under `-shared`, linking them to a
zero/garbage address. The call then crashes at runtime with RIP
pointing at random low memory — diagnostically opaque, hard to
correlate to "you forgot a .c file in your source list." Use
axl-cc or the helpers; both pass `--no-undefined`.

The [`sdk/examples/shared-driver-demo/`](../sdk/examples/shared-driver-demo/)
example demonstrates this exact pattern with a small
`shared-driver-demo-format.{c,h}` TU compiled into both images.

## Performance properties

Once resident, a launcher invocation pays:

- One `LocateProtocol` call (step 1 of
  `axl_driver_ensure_with_embedded`) → microseconds.
- One `axl_protocol_find_guid` → microseconds.
- The vtable dispatch + verb body itself.

What it doesn't pay:

- `LoadImage` of a large launcher binary (~hundreds of KB).
- Per-invocation parsing of any data the driver loaded once at
  startup.
- Re-opening firmware protocols (PCI root bridges, SMBIOS table
  access, NIC SimpleNetwork, etc.) that the driver already holds.

For diagnostic scripts that invoke the same tool dozens of times
across a session, this typically reduces aggregate runtime by an
order of magnitude.

## Hazards and contracts

**Shared vtable struct layout.** The launcher and driver must
agree on the protocol GUID *and* on the vtable struct layout. Put
both in a shared header (`my-tool-protocol.h`) included by both
build targets. ABI shifts on the consumer's side will silently
crash the launcher on the first vtable call.

**Held-protocol cleanup.** If the driver's setup opens UEFI
protocols (`OpenProtocol` with a BY_DRIVER attribute), the unload
callback must close them. Otherwise `axl_driver_unload` (or
firmware-side `UnloadImage`) returns `EFI_ACCESS_DENIED`. Use
`axl_protocol_register_guid` and `axl_protocol_unregister_guid` for
the published vtable — those don't have the BY_DRIVER hazard.

**Dangling pointers after unload.** A launcher that calls
`axl_driver_unload` (or sees the driver unloaded out from under it
some other way) holds a stale `vt` pointer. Either keep the driver
resident for the full boot session, or re-locate the protocol on
every entry to the launcher.

**Identity.** The vtable GUID is derived from the `name` string
both halves pass to the helpers (`axl_guid_v5` against the SDK's
shared-driver namespace). Two consumers passing the same name will
collide — pick something tool-specific (e.g. `"my-vendor/my-tool"`)
rather than generic words. The derivation is deterministic so the
driver and launcher always reach the same GUID; a name typo on one
side silently breaks pairing, so keep the constant in a shared
header (the `MY_TOOL_NAME` `#define` above).

## How this composes with other AXL primitives

The shared-driver pattern is "just" two pieces of vanilla AXL code
talking through a UEFI protocol. Everything that works in an
`int main` app or an `AXL_DRIVER` driver continues to work here:

- The launcher can use `<axl/axl-args.h>` for argv parsing, exit
  cleanly, and let the runtime tear down per-process state.
- The driver can hold expensive shared resources
  (`<axl/axl-pci.h>` tree caches, parsed `<axl/axl-sidecar.h>`
  data, opened streams) and serve them across invocations.
- Cross-process timing of launcher invocations works directly via
  [`axl_clock_gettime(AXL_CLOCK_MONOTONIC, ...)`](../include/axl/axl-time.h)
  — the boot-relative epoch makes timestamps from separate
  launcher runs comparable.
- The launcher can pass per-invocation configuration through to
  the driver via the `load_options` parameter of
  `axl_driver_ensure_with_embedded` and the driver-side
  `axl_driver_get_load_options_raw`.

## See also

- [`<axl/axl-shared-driver.h>`](../include/axl/axl-shared-driver.h) —
  the three convenience helpers used above.
- [`<axl/axl-driver.h>`](../include/axl/axl-driver.h) — underlying
  driver lifecycle primitives (`axl_driver_ensure_with_embedded`
  etc.).
- [`<axl/axl-embed.h>`](../include/axl/axl-embed.h) — link-time
  blob embedding (`AXL_EMBED_DECLARE` / `AXL_EMBED_DATA` /
  `AXL_EMBED_SIZE`).
- [`<axl/axl-service.h>`](../include/axl/axl-service.h) — sibling
  pattern for drivers that run a periodic event loop between
  invocations.
- [`sdk/examples/shared-driver-demo/`](../sdk/examples/shared-driver-demo/)
  — runnable pair (driver + launcher + shared header) that maps
  one-to-one onto the recipe above.
- [`sdk/examples/driver.c`](../sdk/examples/driver.c) — canonical
  `AXL_DRIVER` shape (single-image example).
