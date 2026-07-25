# Recipe: shared-driver pattern

How to split a UEFI tool into a thin launcher + a resident driver
image that hosts the tool's heavy code. Useful when the same
diagnostic verb is invoked many times in a session and you want to
amortize startup cost (LoadImage, parsing sidecar data, opening
firmware protocols) over the boot rather than paying it on every
shell invocation.

The turnkey path is two macros in [`<axl.h>`](../include/axl.h):
`AXL_SHARED_DRIVER` on the driver side, `AXL_SHARED_DRIVER_LAUNCHER`
(or the disk-only `AXL_SHARED_DRIVER_LAUNCHER_THIN`) on the launcher
side. A driver collapses to three plain functions; a launcher
collapses to one line. Both macros are built on
[`<axl/axl-shared-driver.h>`](../include/axl/axl-shared-driver.h)'s
lower-level primitives — GUID derivation, publish/locate, the stdio
bridge, exit-status reflection — which most consumers never call
directly.

This recipe leads with the turnkey shape (**Code shape** below). A
consumer that needs to roll its own resolution — a REPL, a warm
self-locate fast path that caches the vtable pointer, a `--reload`
developer flag, or a custom multi-entry-point vtable instead of the
standard single-`run` one — drops to the primitives directly; see
**Advanced: rolling your own resolution** near the end.

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

The only thing that must agree between the two images is the
identity string — the GUID both halves derive from it
(`axl_guid_v5` against the SDK's shared-driver namespace) is how the
launcher finds the driver's published vtable. Put it in a small
shared header so a typo on one side can't silently desync the pair:

```c
// my-tool-shared.h
#define MY_TOOL_NAME  "my-vendor/my-tool"   // shared-driver identity
```

Unlike the manual pattern (see Advanced below), the turnkey path
needs no consumer-owned vtable struct — the cross-image contract is
the SDK's fixed `AxlSharedDriverVtable`, a single
`int run(int argc, char **argv)` entry. If your two images also
share other code (argv helpers, formatters), that's the "Sharing
helpers" section further down; it's unrelated to this identity
string.

### Driver side

Define three `static` functions — `init`, `run`, `unload` — FIRST,
then invoke `AXL_SHARED_DRIVER` LAST:

```c
// my-tool-dxe.c
#include <axl.h>
#include "my-tool-shared.h"

static int my_init(void);
static int my_run(int argc, char **argv);
static int my_unload(void);

static int my_init(void) {
    /* One-time per-boot setup: sidecar loads, protocol opens,
     * caches. Non-zero return aborts the driver load. */
    return 0;
}

static int my_run(int argc, char **argv) {
    /* This IS int main: argv[0] is the program name, verb/args
     * start at argv[1] — exactly the launcher's own argv, verbatim. */
    (void)argc; (void)argv;
    return 0;
}

static int my_unload(void) {
    /* Teardown: close held protocols, free caches. */
    return 0;
}

AXL_SHARED_DRIVER(MY_TOOL_NAME, my_init, my_run, my_unload)
```

`AXL_SHARED_DRIVER` emits the driver image's entire
`DriverEntry`/unload wiring: it calls `my_init` once, publishes the
SDK's standard `AxlSharedDriverVtable{ .run = my_run }` under
`MY_TOOL_NAME`, and on unload unpublishes before calling
`my_unload`. No vtable definition, no
`axl_shared_driver_publish`/`_unpublish` call, no `AXL_DRIVER` — the
three functions above are the whole driver.

**Static-first, macro-last — no exceptions.** The macro
forward-declares all three functions with external linkage. A
`static` *definition* written after that forward declaration fails
to compile ("static declaration follows non-static declaration").
Declaring (and defining) them `static` before the macro call, as
above, sidesteps the ordering hazard entirely.

**One DriverEntry-emitting macro per translation unit.**
`AXL_SHARED_DRIVER`, `AXL_DRIVER`, and `AXL_SERVICE_DRIVER` each emit
a `DriverEntry` symbol; a driver image uses exactly one.

### Launcher side

The entire launcher `int main` is one macro invocation:

```c
// my-tool.c
#include <axl.h>
#include "my-tool-shared.h"

AXL_SHARED_DRIVER_LAUNCHER(MY_TOOL_NAME, "myToolDxe.efi", my_tool_driver)
```

`my_tool_driver` is an `AXL_EMBED` symbol — the macro declares it
internally, you don't need a separate `AXL_EMBED_DECLARE`. The build
step that produces the embedded bytes is covered in Build below. At
runtime the macro resolves the driver (resident → on-disk
`myToolDxe.efi` → the embedded blob, first hit wins), installs the
stdio bridge, calls `my_run(argc, argv)` with the launcher's argv
**verbatim**, and reflects `my_run`'s return value — and any status
it armed via `axl_set_exit_status` — as the launcher's own exit
status.

For a disk-only launcher (no embedded fallback — smallest
per-command transfer, at the cost of requiring `myToolDxe.efi` to
already be reachable on a volume), drop the embed argument:

```c
// my-tool.c
#include <axl.h>
#include "my-tool-shared.h"

AXL_SHARED_DRIVER_LAUNCHER_THIN(MY_TOOL_NAME, "myToolDxe.efi")
```

### Version-pinned launcher (sibling-only)

When the launcher must pair with the **exact** driver co-staged beside
it — never a stale copy found elsewhere on the volume set — use the
sibling-only variant. The two halves share a cross-image vtable ABI, so
loading a wrong-version driver is a silent-corruption hazard, not a
graceful failure.

```c
// my-tool.c
#include <axl.h>
#include "my-tool-shared.h"

AXL_SHARED_DRIVER_LAUNCHER_SIBLING(MY_TOOL_NAME, "myToolDxe.efi")
```

This resolves a resident driver, else cold-loads `myToolDxe.efi` from
the **launcher's own directory only** and **hard-fails** (`AXL_NOT_FOUND`)
if it isn't staged beside the launcher — no `/drivers`, no volume-root,
no cross-volume search. Call `axl_shared_driver_locate_sibling(name,
driver_filename, &vt)` directly if you need the resolved vtable without
the turnkey `int main` (e.g. a launcher that adds its own `--reload`/
unload hatches). Pinning governs the cold path only: once a driver of
that identity is resident, the warm short-circuit returns it regardless
of version — the first cold load pins for the boot.

### What the SDK owns

Both macros are built from `<axl/axl-shared-driver.h>` primitives
composed for you:

- **Resolve.** `axl_shared_driver_run` (which
  `AXL_SHARED_DRIVER_LAUNCHER`/`_THIN` call) tries, in order: an
  already-resident driver (`LocateProtocol` short-circuit), then
  on-disk `driver_filename` (the co-located **sibling** beside the
  launcher first, then `/drivers/<arch>/`, volume root, and other
  volumes), then the embedded blob (skipped for `_THIN`). First hit
  wins; nothing after it runs. On-disk failure returns `AXL_NOT_FOUND`.
  For strict directory-pinning (sibling only, hard-fail), use the
  sibling-only variant above.
- **Stdio bridge.** Installed automatically before `my_run` is
  called, so the resident driver's `axl_stdin`/`axl_stderr` reflect
  *this* launcher invocation's console and redirects — a resident
  driver image otherwise has no shell parameters of its own to read.
  Uninstalled at launcher exit (`axl_atexit`), so it never outlives
  one invocation.
- **Exit-status reflection.** A driver verb calling
  `axl_set_exit_status(N)` (`<axl/axl-signal.h>`) — the same call any
  `int main` app makes — has `N` cross the bridge and become the
  launcher's own exit status, so `%lasterror%` after
  `my-tool.efi verb` reflects what the *driver* decided, not just
  `my_run`'s plain C return value.

#### Per-stream behavior

| Stream | In the driver verb | Redirection honored |
|---|---|---|
| stdin, raw | `axl_stdin` — raw bytes (UCS-2 on the default `\|` pipe) | `<file`, `\|a` (ASCII pipe) |
| stdin, text | `axl_stdin_text()` — decodes UCS-2 (and BOM'd UTF-16/UTF-8) to UTF-8; create fresh per dispatch, close with `axl_fclose` | `<file`, default `\|` pipe, interactive |
| stdout, text | `axl_print`/`axl_printf` → `gST->ConOut` | `>file` |
| stdout, raw | `axl_stdout_raw` → shell StdOut handle, binary | `>file` |
| stderr, text | `axl_printerr`; diagnostics (`axl_log`/`axl_warning`) → `gST->StdErr` | `2>file`, **not** `>file` |
| stderr, raw | `axl_stderr_raw` → shell StdErr handle, binary | `2>file`, **not** `>file` |

Two caveats:

1. **Shell `MAX_BIT` truncation.** The UEFI reference Shell
   (`ShellPkg`'s `RunCommand`) strips bit 63 (`MAX_BIT`) from an
   *error-class* `.efi` exit status before exposing it as
   `%lasterror%` (`Status & ~MAX_BIT`). Small-int / success-class
   statuses survive unchanged; an `ENCODE_ERROR(n)` status does not.
   The full 64-bit value is still available to a programmatic
   `EFI_STATUS` reader (e.g. `gBS->StartImage`'s return), so prefer
   that when bit 63 matters to the caller.
2. **Redirected output is shell-encoded, not UTF-8.** `>` produces
   UCS-2 with a BOM; `>a` produces ASCII. Neither is UTF-8. A driver
   verb that needs to write a UTF-8 file should use `axl_fopen`
   directly rather than rely on shell redirection.

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

A consumer whose launcher keeps custom code — a `--reload` flag, a
REPL, or any of the Advanced patterns below — lists a shared TU like
this in both `axl_add_driver` and `axl_add_app`'s source lists, same
as sketched above. The turnkey
[`sdk/examples/shared-driver-demo/`](../sdk/examples/shared-driver-demo/)
example doesn't need this: its launcher's entire `int main` is the
`AXL_SHARED_DRIVER_LAUNCHER` macro, so there's no launcher-side call
site left to share a helper from — its `shared-driver-demo-format.c`
lives in the driver's source list only. See the comment at the top of
that example's `shared-driver-demo-format.h` for the full explanation.

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

For `AXL_SHARED_DRIVER_LAUNCHER_THIN` (no embed), drop `--embed` and
the driver `.efi` ships as a second file the launcher locates on
disk instead.

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
order of magnitude. This holds whether the resolve step runs via
the turnkey macros or the Advanced primitives directly — both go
through the same `axl_driver_ensure_with_embedded` short-circuit.

## Hazards and contracts

**Shared vtable struct layout.** Only a concern if you've opted into
the Advanced custom-vtable pattern (the turnkey path's vtable is the
SDK's fixed `AxlSharedDriverVtable`, so there's no consumer-owned
layout to drift). If you do roll a custom vtable, the launcher and
driver must agree on its layout; put it in a shared header and
rebuild both images together when it changes. ABI shifts on the
consumer's side will silently crash the launcher on the first vtable
call.

**Held-protocol cleanup.** If the driver's setup opens UEFI
protocols (`OpenProtocol` with a BY_DRIVER attribute), the unload
function (`my_unload` above) must close them. Otherwise
`axl_driver_unload` (or firmware-side `UnloadImage`) returns
`EFI_ACCESS_DENIED`. Use `axl_protocol_install` and
`axl_protocol_uninstall` for the published vtable — those don't
have the BY_DRIVER hazard.

**Dangling pointers after unload.** Only a concern for the Advanced
patterns that cache a resolved vtable pointer across multiple
dispatches within one process (a warm self-locate fast path, a
REPL). A turnkey launcher resolves and dispatches once per process
and then exits, so it never observes a stale pointer. A caching
consumer that calls `axl_driver_unload` (or otherwise sees the
driver unloaded from under it) holds a stale `vt` pointer after
that point — either keep the driver resident for the full boot
session, or re-locate the protocol on every entry.

**Identity.** The vtable GUID is derived from the `name` string both
halves pass — `AXL_SHARED_DRIVER`'s and
`AXL_SHARED_DRIVER_LAUNCHER`'s first argument on the turnkey path, or
every direct call to the primitives on the Advanced path
(`axl_guid_v5` against the SDK's shared-driver namespace). Two
consumers passing the same name will collide — pick something
tool-specific (e.g. `"my-vendor/my-tool"`) rather than generic
words. The derivation is deterministic so the driver and launcher
always reach the same GUID; a name typo on one side silently breaks
pairing, so keep the constant in a shared header (the `MY_TOOL_NAME`
`#define` above).

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
  `axl_shared_driver_locate_with_load_options` /
  `_with_image_info` and the driver-side
  `axl_driver_get_load_options_raw`.

## Advanced: rolling your own resolution

Everything above is the default. Drop to the primitives directly
only when you need something the turnkey macros don't give you:

- A **warm self-locate fast path** that resolves the vtable once and
  caches the pointer across multiple dispatches in one process
  (rather than re-resolving on every launcher invocation, which the
  turnkey macros always do — it's cheap, but not free).
- A **REPL-style launcher** that dispatches into the driver
  repeatedly within one process lifetime.
- A **`--reload` developer flag** that forces a fresh driver image on
  the next invocation, bypassing the resident short-circuit — there's
  no turnkey hook for this; it requires calling `axl_shared_driver_unload`
  yourself.
- A **custom, multi-entry-point vtable** instead of the standard
  single-`run` `AxlSharedDriverVtable` — e.g. a tool whose driver-side
  API is naturally several distinct functions rather than one verb
  dispatcher.

The [`sdk/examples/shared-driver-demo/`](../sdk/examples/shared-driver-demo/)
example demonstrates the turnkey macros above (**Code shape**), not
the pattern below — reach for the custom-vtable pattern only when you
actually need one of the four bullets above. The turnkey macros' own
runnable, both-arches-tested proof is the `sd-ergo` fixture in
`test/integration/`.

### Custom vtable pattern

The vtable struct is consumer-owned and must be `#include`d by both
images. Treat it as part of your tool's public contract:

```c
// my-tool-protocol.h
#define MY_TOOL_NAME  "my-tool"   // shared-driver identity

typedef struct {
    int (*verb_a)(int arg);
    int (*verb_b)(const char *name);
} MyToolVtable;
```

**Driver side** — publish the vtable via `axl_shared_driver_publish`
from `DriverEntry`, after any per-boot setup work. Multi-source-file
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
                                       &gVtable, gPublishedHandle);
}

AXL_DRIVER(my_main, my_unload)
```

**Launcher side** — call `axl_shared_driver_locate`, which ensures
the driver is loaded (resident → on-disk → embedded blob) and
resolves the vtable in one call:

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

Note this custom-vtable launcher does **not** get the stdio bridge
or exit-status reflection for free — `axl_shared_driver_locate`
installs the bridge (see below), but applying an armed exit status
and dispatching through the standard vtable are separate steps you
opt into explicitly, covered next.

### The per-dispatch bracket: `axl_shared_driver_dispatch`

If your driver publishes the *standard* `AxlSharedDriverVtable`
(single `run(argc, argv)` entry) but you still want to control
resolution yourself — a cached warm pointer, a REPL loop, a
`--reload` chain — `axl_shared_driver_dispatch(vt, argc, argv)` is
the bracket to call once you have a resolved `vt`:

```c
AxlSharedDriverVtable *vt = my_cached_or_resolved_vt();   /* however you found it */
int rc = axl_shared_driver_dispatch(vt, argc, argv);
```

It installs the stdio bridge, calls `vt->run(argc, argv)` forwarding
`argc`/`argv` unchanged, then applies any exit status the driver
armed — the same three steps `axl_shared_driver_run` (and therefore
`AXL_SHARED_DRIVER_LAUNCHER`) performs after its own resolve step.
Use it whenever you resolve the vtable through a path other than
`axl_shared_driver_locate*` but still want the bridge + exit-status
behavior "for free" per dispatch.

### Reload / teardown

Drop the resident driver from outside its own image — useful for a
launcher's `--reload` developer flag (pick up a freshly-built
driver `.efi` without a firmware reboot) or for crash-recovery
scenarios where you want to discard a driver that's in a bad
state. This composes with either driver flavor above; the example
below assumes a driver built with `AXL_SHARED_DRIVER` (standard
vtable, from Code shape), driven by a launcher that wants manual
control over the resident instance instead of the turnkey
`AXL_SHARED_DRIVER_LAUNCHER`:

```c
if (consumer_wants_reload) {
    /* Returns AXL_OK if driver wasn't resident (post-condition
     * "not loaded" already holds). On success the next locate
     * call falls through LocateProtocol's short-circuit and
     * does a fresh LoadImage. */
    axl_shared_driver_unload(MY_TOOL_NAME);
}
AxlSharedDriverVtable *vt = NULL;
axl_shared_driver_locate(MY_TOOL_NAME, "myToolDxe.efi",
                         AXL_EMBED_DATA(my_tool_driver),
                         AXL_EMBED_SIZE(my_tool_driver),
                         (void **)&vt);
return axl_shared_driver_dispatch(vt, argc, argv);
```

Resolution: `axl_shared_driver_unload` derives the protocol GUID
from `name`, calls `LocateHandleBuffer(ByProtocol, ...)` to find
the driver's image handle (publish installs on the driver's
`gImageHandle`, so the protocol-bearing handle IS the
loaded-image handle), then `axl_driver_unload` → `gBS->UnloadImage`,
which fires the driver's registered unload callback (which calls
`axl_shared_driver_unpublish` to remove the protocol install).
The driver's pages get freed; the next launcher invocation pays
the full LoadImage cost again.

**Must not be called from inside the driver image itself.**
`gBS->UnloadImage` on a self-executing image is undefined behavior
(the image's pages get freed mid-stack-frame). The driver-side
teardown path is `axl_shared_driver_unpublish` from the driver's
unload callback; the launcher-side teardown is
`axl_shared_driver_unload`. They are not interchangeable.

### Bridging stdio when you resolve the driver yourself

Every `axl_shared_driver_locate*` variant (and, transitively,
`axl_shared_driver_run`/`axl_shared_driver_dispatch`) installs the
stdio bridge automatically. If your launcher resolves the resident
driver **some other way** — the warm fast-path
(`axl_shared_driver_guid` + `axl_protocol_find_guid`),
`axl_driver_load_sibling`, a hand-rolled embedded-blob fallback — the
bridge is **never installed** on that path. The resident driver's
`axl_stdin` stays EOF and `echo args | my-tool.efi verb` reads
nothing.

For that case, call the public escape hatch from the launcher, once,
before dispatching into the driver:

```c
/* Launcher resolved the driver itself (no axl_shared_driver_locate). */
MyToolVtable *vt = my_custom_resolve();

/* Install the bridge so the resident driver sees THIS launcher's
   piped / redirected / interactive StdIn. */
axl_shared_driver_install_stdio_bridge();

return vt->do_run(argc, argv);   /* driver reads via axl_stdin_text() */
```

It re-publishes on every call (so it's safe — and correct — to call
on each launcher invocation), is auto-uninstalled at launcher exit,
and is a no-op when the launcher has no shell handles of its own.
Launchers that use `axl_shared_driver_locate*`, `axl_shared_driver_run`,
or `axl_shared_driver_dispatch` must **not** call it — they already
get the bridge for free.

### Applying exit status when you dispatch yourself

`axl_shared_driver_dispatch` (and therefore
`axl_shared_driver_run`/`AXL_SHARED_DRIVER_LAUNCHER`) already applies
a driver's armed exit status automatically. If you call a resolved
vtable's `run` directly instead of going through
`axl_shared_driver_dispatch` — e.g. a REPL that dispatches many times
per process and wants explicit control over when the reflected
status lands — call `axl_shared_driver_apply_exit_status()`
yourself, immediately after the dispatch call returns:

```c
int rc = vt->run(argc, argv);
axl_shared_driver_apply_exit_status();   /* no-op if the driver armed nothing */
return rc;
```

It does NOT clear a previously-applied launcher exit status — it
only drains the bridge's pending cell into `axl_set_exit_status`
when one is pending. A REPL-style launcher that wants strict
per-dispatch semantics (this round's status only, not a stale one
left over from an earlier round) should clear its own armed status
between dispatches, or rely on the `AXL_ERR` return here to know
nothing was applied this round.

**EDK2 caveat:** the reflection itself carries the full `uint64_t`
verbatim across the bridge, but see the shell `MAX_BIT` truncation
caveat under "What the SDK owns" above — it applies here too.

## See also

- [`<axl.h>`](../include/axl.h) — `AXL_SHARED_DRIVER`,
  `AXL_SHARED_DRIVER_LAUNCHER`, `AXL_SHARED_DRIVER_LAUNCHER_THIN`
  (the turnkey macros used above).
- [`<axl/axl-shared-driver.h>`](../include/axl/axl-shared-driver.h) —
  `AxlSharedDriverVtable` and the underlying primitives
  (`axl_shared_driver_publish`/`_unpublish`/`_locate`/`_unload`/
  `_dispatch`/`_run`, the stdio-bridge and exit-status functions)
  used directly by the Advanced patterns.
- [`<axl/axl-driver.h>`](../include/axl/axl-driver.h) — underlying
  driver lifecycle primitives (`axl_driver_ensure_with_embedded`
  etc.).
- [`<axl/axl-embed.h>`](../include/axl/axl-embed.h) — link-time
  blob embedding (`AXL_EMBED_DECLARE` / `AXL_EMBED_DATA` /
  `AXL_EMBED_SIZE`); `AXL_SHARED_DRIVER_LAUNCHER` calls
  `AXL_EMBED_DECLARE` internally, so a turnkey launcher doesn't
  write it directly.
- [`<axl/axl-service.h>`](../include/axl/axl-service.h) — sibling
  pattern for drivers that run a periodic event loop between
  invocations.
- [`test/integration/sd-ergo-driver.c`](../test/integration/sd-ergo-driver.c) /
  [`sd-ergo-launcher.c`](../test/integration/sd-ergo-launcher.c) —
  the turnkey macros' own runnable, both-arches-tested proof
  (stdin/stdout/exit-status round trip).
- [`sdk/examples/shared-driver-demo/`](../sdk/examples/shared-driver-demo/)
  — a thoroughly-commented teaching pair (driver + launcher + shared
  identity header) built entirely from `AXL_SHARED_DRIVER` /
  `AXL_SHARED_DRIVER_LAUNCHER`, exercising stdin/stdout/stderr and
  exit-status from plain app-style verb code.
- [`sdk/examples/driver.c`](../sdk/examples/driver.c) — canonical
  `AXL_DRIVER` shape (single-image example).
