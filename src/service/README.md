# AxlService

Structured-lifecycle wrapper over `AxlLoop`. A service is a
long-running event loop with a `setup` callback that builds
sources/timers/handlers, a `teardown` that releases them, and an
options descriptor that auto-applies values into a consumer struct
via `offsetof`.

This is **not a Unix daemon** — UEFI has no `fork`/`setsid`/`chdir`/
`umask`. AxlService is closer in shape to a systemd unit: a thing
with a defined lifecycle, supervised by an external dispatcher
(the shell's foreground caller, or the firmware's notify-timer in
driver mode).

## Shape

AxlService is a DXE driver. The consumer's `main()` is a launcher
/ supervisor; the actual service body — setup, the long-running
loop, teardown — lives in a separate driver `.efi` image,
typically embedded into the foreground via `.incbin`.

```c
#include <axl.h>

typedef struct { uint16_t port; bool verbose; } MyOpts;
static MyOpts opts;

static const AxlConfigDesc opts_descs[] = {
    { "port",    AXL_CFG_UINT, "8080", "Port",
      offsetof(MyOpts, port), sizeof(uint16_t) },
    { "verbose", AXL_CFG_BOOL, "false", "Verbose",
      offsetof(MyOpts, verbose), sizeof(bool) },
    { 0 }
};

static int my_setup(AxlLoop *loop, void *user) {
    /* build sources against `loop`, e.g. axl_http_server_start */
    return AXL_OK;
}
static int my_teardown(void *user) { /* release */ return AXL_OK; }

static const AxlService my_service = {
    .name           = "my-service",
    .opts_descs     = opts_descs,
    .setup          = my_setup,
    .teardown       = my_teardown,
    .user           = &opts,
    .driver_tick_ms = 50,
};
/* Identity GUID is derived from `.name` via axl_guid_v5 — no
   uuidgen required. Same name in launcher + driver image →
   same derived GUID. */
```

The driver image's only line is the `AXL_SERVICE_DRIVER` macro:

```c
/* my-service-dxe.c, compiled as the embedded driver */
#include <axl.h>
#include "shared.h"   /* same my_service descriptor as the foreground */
AXL_SERVICE_DRIVER(my_service);
```

**Driver-tick mode (raw).** Used by the macro to attach the
service to the firmware notify-timer:

```c
AxlLoop *loop = axl_loop_new();
axl_service_attach_driver(loop, &my_service);  /* tick = my_service.driver_tick_ms */
/* ...firmware drives the loop until... */
axl_service_detach_driver(loop, &my_service);
axl_loop_free(loop);
```

Direct calls are uncommon — `AXL_SERVICE_DRIVER` covers the
DriverEntry/Unload boilerplate.

**Embedded-driver launch (foreground side).** Foreground app
ships the driver image as an embedded blob (via `axl-cc --embed`
or the Makefile's `EMBED_BLOB`), serializes its current options
to LoadOptions, and `axl_service_start_embedded` hands them to
the driver. `AXL_EMBED_*` hides the .incbin symbol-naming
convention:

```c
AXL_EMBED_DECLARE(my_driver);  /* from <axl/axl-embed.h> */

/* AXL_EMBED_SIZE is a runtime pointer subtraction (not a constant
   expression), so build the deploy at runtime rather than as a
   static const initializer. The protocol GUID lives on the inner
   AxlService — both binaries share the same descriptor, so they
   agree on identity by construction. */
AxlServiceDeploy d = {
    .service         = &my_service,
    .driver_blob     = AXL_EMBED_DATA(my_driver),
    .driver_blob_len = AXL_EMBED_SIZE(my_driver),
    .driver_name     = "my-service-dxe.efi",
};

if (axl_service_is_running(&d)) {
    axl_printf("Already running\n");
    return 0;
}
return axl_service_start_embedded(&d);
```

Build:

```bash
axl-cc --embed my-service-dxe.efi=my_driver launch.c -o launch.efi
```

By default the driver image is resolved by name: the four-path search
(`axl_driver_ensure`'s order), then the embedded blob. A launcher that
staged its own driver and knows where it put it can skip all of that
with `.driver_path`:

```c
d.driver_path = "fs0:\\svc\\my-service-dxe.efi";   /* exactly this file */
```

That routes through `axl_driver_ensure_from_path` — no search, no
embedded fallback — so a stale `drivers/<arch>/my-service-dxe.efi`
left by an older install cannot shadow the copy the launcher just
staged. (It really can: a launcher sitting at the volume root has no
usable image directory, so its own sibling is only search candidate
#4 while `drivers/<arch>/` is #2.) `driver_name`, `driver_blob` and
`driver_blob_len` all feed the default resolution only, so with a pin
none of them is read — or required. `service` + `driver_path` is a
complete deploy descriptor. Note `override_name` does NOT solve this: it substitutes
a *name* into the same search, so it cannot separate two files that
share a name.

For the opposite deployment — "ship as one binary, never touch disk" —
set `.embedded_only` instead: it loads the embedded `driver_blob`
directly (via `axl_driver_ensure_embedded_only`) and skips the search
entirely, so the stale-sibling shadow above can't happen from the disk
side. It requires `driver_blob`/`driver_blob_len` (`driver_name` only
names the loaded image) and is mutually exclusive with `driver_path`
(setting both returns `AXL_ERR`):

```c
d.embedded_only = true;   /* use the baked-in blob, no disk search */
```

The symmetric stop verb is `axl_service_stop(&deploy)` — resolves
the running image's handle via the protocol GUID and unloads it.
`AXL_SERVICE_DRIVER` publishes the GUID on the driver image's own
handle (not a sentinel), so stop's `LocateHandleBuffer` returns
the image handle directly. Stop is idempotent — calling it on a
not-running deploy returns `AXL_OK` without doing anything.

```c
return axl_service_stop(&deploy);
```

The driver image's `DriverEntry` is one line — `AXL_SERVICE_DRIVER`
emits the boilerplate (decode `LoadOptions` magic-prefixed UTF-8
query string back into `opts` via `axl_config_from_string`, create
loop, attach):

```c
#include <axl.h>
/* same opts_descs and my_service descriptor as the foreground side.
   Tick period comes from my_service.driver_tick_ms (0 = 50 ms
   default); the macro takes only the descriptor. */
AXL_SERVICE_DRIVER(my_service);
```

## Setup-failure contract

`setup` owns its own unwind on failure (axl_free anything it
allocated, `axl_protocol_unregister` anything it published). The
framework calls `teardown` **only after** a successful setup.
This rule is the same in foreground and driver modes.

## Held-protocol hazard

Any UEFI protocol the setup callback opens with `OpenProtocol`
(including the implicit opens that `axl_http_server_start`,
`axl_tcp_listen`, etc. do via service-binding) MUST be closed
before the matching teardown returns. The firmware's
post-callback refcount check at the end of `gBS->UnloadImage`
will refuse the unload with `EFI_ACCESS_DENIED` otherwise —
making the service un-stoppable for the lifetime of the
process. `axl_service_stop` will return `AXL_ERR` and the SDK
will log:

```
axl_driver_unload: UnloadImage(handle=0x...) returned
EFI_ACCESS_DENIED (0x800000000000000F) — image still holds open
protocol references; if your service opens UEFI protocols in
setup, ensure teardown closes every one (axl_http_server_free,
axl_tcp_close, etc.) before returning
```

The SDK's wrappers (`axl_http_server_free`, `axl_tcp_close`,
etc.) close the protocols they opened. If your setup uses
`OpenProtocol` directly, your teardown owns the matching
`CloseProtocol`.

## Teardown is invoked at two sites — all visible

The teardown callback fires from one of:

  1. `axl_service_attach_driver`'s failure path (rolls back setup)
  2. `AXL_SERVICE_DRIVER`'s unload stub after `axl_service_detach_driver`

Each call goes through `axl_service_teardown`, which logs
`teardown ENTER` / `teardown EXIT rc=N` at debug level so
consumers can confirm without adding their own printfs. Bump
`AXL_LOG_LEVEL=debug` to see the trace.

`axl_service_detach_driver` does NOT call teardown — the caller
follows up with `axl_service_teardown` explicitly. This is the
P1 contract change; previously detach_driver invoked teardown
internally, which made the macro unreadable (no literal
`teardown(...)` call appeared in the unload stub source) and
silently skipped teardown if `axl_loop_detach_driver` returned
ERR.

## Cross-binary ABI tripwire

When the same `static const AxlService` descriptor is initialized
in two binaries (foreground app + embedded driver image), the
option struct's layout MUST match across both. Build both from
the same source tree with identical compile flags (`AXL_MEM_DEBUG`,
arch, and anything reaching `CFLAGS`). The Makefile records a
build-state signature over `CFLAGS`/`CXXFLAGS`/`INCLUDES` and
`CC`/`CXX`, and wipes AXL-internal stale objects when it changes;
it cannot see consumer-side struct shifts.

The wire format prefixes each `LoadOptions` payload with the
8-byte magic `AXLSVC1\0` so a shell-launched UCS-2 LoadOptions
buffer doesn't get misparsed as the AXL UTF-8 format. The
driver-image macro logs and falls back to descriptor defaults
on missing-magic.

## Self-reload (in-place upgrade)

`axl_service_reload(svc, new_path)` (and the memory-image
`axl_service_reload_buffer(svc, image, len)`) hot-swap a running
`AXL_SERVICE_DRIVER` service to a new version with no reboot and —
if your teardown frees its server with `AXL_TEARDOWN_RESET` — no port
downtime. Called from **inside** the service (e.g. an `/upgrade`
handler on the service loop), it:

  1. loads `new_path` as the replacement (validated *before*
     anything is released — a load failure tears down nothing),
  2. runs your teardown (releases the listen ports),
  3. starts the replacement, handing it this image's handle + a
     signal event via LoadOptions,
  4. confirms the replacement actually published the service
     protocol — `StartImage` succeeding is not proof it attached,
     and this image still publishes the GUID itself, so the check
     is "did the publisher *count* go up", not a plain
     `LocateProtocol`,
  5. detaches this loop and signals; the replacement rebinds the
     ports, comes up resident, and — from its own timer tick, once
     this image is off-stack — unloads and reclaims it.

The handoff rides a second LoadOptions magic `AXLSVR1\0`, whose
payload is `[magic][old-image-handle][event][config C-string]` —
the replacement decodes the two handles, arms the event on its own
loop, and reclaims the old image when it fires (never synchronously
from the old image's stack, and never on a delay guess).

Failure semantics: a load failure (bad path / corrupt image) tears
down nothing and the service keeps running; a start failure after a
successful load leaves the service down — teardown has run and the
loop is detached (it no longer serves), but the image stays resident
and still publishes the service GUID (`axl_service_is_running` still
reports true) until it is unloaded or the system resets.

The return code says which of those happened, so a caller does not
have to conservatively roll back and cold-reset a box that never
stopped serving. **`AXL_ERR` means — and only means — the service is
DOWN**; every other non-OK code reports a failure that happened
before anything was released:

| return | meaning | this service |
|---|---|---|
| `AXL_OK` | replacement resident, handoff armed | replaced, serving |
| `AXL_INVALID` | NULL / not-the-running `svc`, NULL path or image | untouched, serving |
| `AXL_NOT_FOUND` | the replacement could not be **loaded** | untouched, serving |
| `AXL_NO_RESOURCES` | options would not serialize, handoff event / LoadOptions could not be installed | untouched, serving |
| `AXL_ERR` | the replacement loaded but failed to **start** | **DOWN — fatal** |

Your teardown must be **idempotent** — the happy path runs it exactly
once (the replacement's unload stub skips a second teardown), but the
start-failure path can re-enter it from a later unload — and must
not free the service loop.

See `sdk/examples/service-demo.c` for the one-line `AXL_SERVICE`
shape and `sdk/examples/service-demo-custom.c` for a hand-written
`main()` mixing the standard verbs with consumer-specific verbs.
`<axl/axl-service.h>` has the full API doxygen.

