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
or the build's `axl_embed_blob`), serializes its current options
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
`AXL_LOG_LEVEL=debug` to see the trace. A FAILED teardown is reported
once, by the site that called it, as one warning saying what it leaves
behind; a failure at either site leaves the service half-stopped (see
below).

`axl_service_detach_driver` does NOT call teardown — the caller
follows up with `axl_service_teardown` explicitly. This is the
P1 contract change; previously detach_driver invoked teardown
internally, which made the macro unreadable (no literal
`teardown(...)` call appeared in the unload stub source) and
silently skipped teardown if `axl_loop_detach_driver` returned
ERR.

## The driver prints its own leak verdict at unload

A successful unload ends the way an application's exit does: after
teardown, the global destructors and the `axl_atexit` callbacks,
the stub sweeps the tier-1 registry and, in a DEBUG build, prints
the teardown memory verdict (`no leaks detected`, or an
`=== AxlMem leak report: ...` block listing each live allocation's
`file:line`). The image's default loop, if anything called
`axl_loop_default()`, is freed first, as an application's is. That
verdict is the DRIVER image's own, so a leak in
`setup` that `teardown` forgets is reported against the service,
not lost behind its launcher's clean exit. The QEMU harness's leak
gate reads it like any other. A failing unload prints none: the
image stays resident and is still allocating.

## A failed teardown leaves the service whole

When `teardown` returns anything but `AXL_OK`, the unload stub returns
`EFI_ABORTED` at once and changes nothing else, except to tag the image
STOPPING: the image stays resident, the service protocol stays published
(so `axl_service_stop` finds it, and `status` reports it stopping -- see
"Service state" below), and its loop, its options and the stub's own
state are kept. The next unload -- `axl_service_stop`,
the Shell's `unload`, a reload -- runs `teardown` AGAIN, and the image
unloads once a teardown succeeds. Before this, a failed teardown dropped
the service anyway, so the second unload skipped teardown and the
firmware freed the image under whatever the first one failed to release.

The other site ends the same way. When setup succeeded but the loop's
tick could not be attached, the framework rolls setup back with
`teardown`; if that fails, `axl_service_attach_driver` returns
`AXL_BUSY` and the driver's entry point still returns SUCCESS, leaving
the same half-stopped service. An error from the entry point would have
the firmware free the image WITHOUT calling its Unload, under whatever
the teardown failed to release.

So **a teardown that can fail must resume**: release only what it
still holds, clearing each pointer as it frees it, and return non-OK
only while something it must release is still held (a console device
whose `axl_console_device_uninstall` returned `AXL_ERR`, for one). And
non-OK is not a way to report a soft error: a teardown that has
released everything returns `AXL_OK`, or the next unload runs it again
over freed state -- and one that always fails can never be unloaded.

A setup can end in the same state on purpose: one that took something
calling into this image, failed, and cannot give it back returns
`AXL_SERVICE_SETUP_HELD`, and the next unload's teardown finishes the
unwinding.

The half-stopped service's loop is **not** ticked again. A failed
teardown may already have freed state that the loop's sources point at
-- relying, as a successful one may, on the loop being freed after it --
so the loop stays detached until the image unloads. Firmware still
reaches anything teardown has not released (a protocol it installed,
for one); the loop's timers and sources do not run.

## Service state: running, degraded, stopping

A half-stopped service is published -- `stop` and reload must find it -- so
"is the GUID published" cannot tell it from a serving one. The driver tags
its own image handle with two more name-derived GUIDs, and
`axl_service_state()` reads the answer from any image:

| state | set by | `status` | `start` |
|---|---|---|---|
| `AXL_SERVICE_STOPPED` | nothing published | `stopped`, exit 1 | starts it |
| `AXL_SERVICE_RUNNING` | published, no tag | `running`, exit 0 | `already running` |
| `AXL_SERVICE_DEGRADED` | the service: `axl_service_set_degraded(true, why)` | `degraded (<why>)`, exit 0 | `already running (degraded: <why>)` |
| `AXL_SERVICE_STOPPING` | the framework: a failed teardown, or `AXL_SERVICE_SETUP_HELD` | `stopping (teardown failed; 'stop' retries; a reset clears it)`, exit 5 | refuses with that line, touches no staged image |

The worst state wins when several images publish one service. STOPPING is
the framework's lifecycle answer and only an unload clears it; DEGRADED is
the service's own health report, set and cleared by the service (from setup,
or from its loop at `TPL_CALLBACK`), with a copied reason of up to 80 bytes
that `axl_service_state_reason()` reads from any image. Both tags go with the
image when it unloads, so a reload's replacement starts healthy. Exit 5
(`AXL_SERVICE_EXIT_STOPPING`) is outside the LSB init-script range (0-4) on
purpose.

`axl_service_start_embedded` and `axl_service_start_staged` return
`AXL_BUSY` for a stopping service -- including an image the call itself just
started that came up stopping -- and leave it resident. A reload over one is
`AXL_DENIED`, as for any image that would not stop; `axl_service_state` says
which kind it is.

**`stop --reset`** is the last resort when a teardown keeps failing: it
stops as `stop` does -- which retries the teardown -- and **only if the
service is then STOPPING** it flushes the log, warns and resets the machine.
A stop that fails for any other reason (a foreign image with no Unload
handler publishing the name, a protocol another agent holds, an allocation
failure) is reported with a line saying the reset does not apply: a reset
could not be relied on to help, as a foreign image comes back at boot. There
is no forced unload: freeing an image something still calls into is a
use-after-free.

`AXL_SERVICE_SETUP_HELD` is `0x48454C44` ('HELD'), so a setup that returns
`true` or a small positive number by mistake is an ordinary failure, and it
needs a teardown: HELD from a service with none is refused with a warning
and treated as an ordinary failure. The tags are derived in a namespace of
their own, so no service name can publish another service's tag.

## Cross-binary ABI tripwire

When the same `static const AxlService` descriptor is initialized
in two binaries (foreground app + embedded driver image), the
option struct's layout MUST match across both. Build both from
the same source tree with identical compile flags (`AXL_MEM_DEBUG`,
arch, and anything reaching `CFLAGS`). The build recompiles an
AXL object whose compile command changed, and refuses to reuse a
tree configured with another toolchain (`cmake/AxlToolchain.cmake`);
it cannot see consumer-side struct shifts.

The wire format prefixes each `LoadOptions` payload with the
8-byte magic `AXLSVC1\0` so a shell-launched UCS-2 LoadOptions
buffer doesn't get misparsed as the AXL UTF-8 format. The
driver-image macro logs and falls back to descriptor defaults
on missing-magic.

The payload lives only for the driver's entry point. The macro
decodes it into the service's own config there, and once
`StartImage` returns the launcher clears the driver's
`LoadOptions` and frees its copy. That is what keeps a detached
start leak-free: the launcher exits while the driver stays
resident, and the eventual `stop` runs in another image that
never held the copy.

## Reload (in-place upgrade)

`axl_service_reload(svc, new_path)` (and the memory-image
`axl_service_reload_buffer(svc, image, len)`) replace a running
`AXL_SERVICE_DRIVER` service with a new version, with no reboot. They
are called by an **application** -- a Shell command, or the service's
launcher -- never by the service itself:

- UEFI allows `LoadImage()` and `StartImage()` only below
  `TPL_CALLBACK` (UEFI 2.11 Table 7.3), and a resident service's own
  code runs in event callbacks at `TPL_CALLBACK`. Called there, they
  return `AXL_UNSUPPORTED` and log the rule.
- The service's only `TPL_APPLICATION` context is an application's call
  into it, and an image cannot unload itself with that call still on its
  stack, so a call from inside the service's own image is refused the
  same way. The caller must not be running under any call from the
  service's image at all (an application the service started, say): that
  case is not detected.

A service that decides it should be upgraded (an `/upgrade` request, a
downloaded image) records that, and an application -- run from
`startup.nsh`, a management script, or by hand -- performs it. The
application, at `TPL_APPLICATION`:

  1. loads `new_path` as the replacement (validated *before* anything
     is stopped -- a load failure leaves the service serving),
  2. stops the running service, as `axl_service_stop` does: its loop is
     detached, its teardown runs (releasing the listen ports -- free an
     HTTP server with `AXL_TEARDOWN_RESET` so they are free on return),
     and its image is unloaded, synchronously,
  3. starts the replacement with the options `svc` carries (serialized
     as `axl_service_start_embedded` serializes them), which rebinds the
     ports and comes up resident -- **the running service's current
     options are not inherited**: set them on the descriptor you pass,
     or the replacement comes up with its defaults,
  4. confirms the replacement actually published the service protocol
     -- `StartImage` succeeding is not proof it attached.

`sdk/examples/reload-svc-dxe.c` (the service) and
`sdk/examples/reload-svc.c` (the application) are the worked pair.

**Downtime.** Clients are disconnected, and the port is unbound from the
old teardown until the replacement's setup binds it -- the old image's
unload plus the replacement's whole entry, network bring-up included. The
replacement is loaded first, so a bad image costs none.

The return code says what happened, so a caller does not have to
conservatively roll back and cold-reset a box that never stopped
serving. **`AXL_ERR` means -- and only means -- the service is DOWN**;
every other non-OK code reports a failure that happened before anything
was stopped:

| return | meaning | the service |
|---|---|---|
| `AXL_OK` | replacement resident and published; the old image unloaded | replaced, serving |
| `AXL_INVALID` | NULL `svc`, NULL path or image; or no image publishes the service | untouched |
| `AXL_UNSUPPORTED` | called at `TPL_CALLBACK` or above, or from inside the service's own image | untouched, serving |
| `AXL_NOT_FOUND` | the replacement could not be **loaded** | untouched, serving |
| `AXL_NO_RESOURCES` | options would not serialize, or LoadOptions could not be installed | untouched, serving |
| `AXL_DENIED` | the running image would not stop and still publishes the service (no Unload handler, or its unload refused); the replacement was not started | untouched, serving -- unless its teardown failed (see below) |
| `AXL_ERR` | the running image was stopped (it no longer publishes the service) but did not unload cleanly, or the replacement failed to **start** or publish | **DOWN -- fatal** |

`AXL_DENIED` is judged by the service GUID still being published. With
several images publishing it, one may already have stopped while another
refused; and a foreign image that tears down and then refuses its unload
still publishes, so it reads as serving although it is not. The same holds
for an `AXL_SERVICE_DRIVER` image whose teardown failed: it is resident and
published but half-stopped, its loop no longer running (see "A failed
teardown leaves the service whole"), and the next stop or reload retries
its teardown.

**After `AXL_ERR`** the old image has been unloaded: nothing is
resident, `axl_service_is_running` is false, and the call cannot bring it
back. Keep the old image (its path or bytes) until the call returns, and
on `AXL_ERR` start it again with `axl_service_start_embedded` -- or reset.

Your teardown must be **idempotent** -- a failed one is called again --
and must not free the service loop.

## Staged upgrade (unattended)

A resident service with no application to ask -- a headless box, an
`/upgrade` request, a downloaded image -- cannot reload itself (see
above). It stages the new image and resets; its launcher applies it at the
next boot:

```c
/* In the service, from a request handler or a timer (TPL_CALLBACK is fine:
   this writes a file and loads nothing). */
int rc = axl_service_stage(&deploy, NULL, image, image_len);
if (rc == AXL_OK && axl_service_is_staged(&deploy)) {
    axl_reset(AXL_RESET_COLD);      /* when it suits you */
}
/* AXL_OK with nothing staged: the image is the one running -- no reset. */
```

```
# startup.nsh -- every boot
fs0:\my-svc.efi start --detach
```

The deploy must pin `driver_path` (the file a staged image goes beside).
The `start` verb of `axl_service_main` then applies a staged image
(`start --no-staged` resolves only what the last boot left), and the
service reads the outcome with `axl_service_last_upgrade`.
`axl_service_unstage`, `axl_service_is_staged`,
`axl_service_upgrade_in_flight` and `axl_service_clear_quarantine`
complete the set.
`sdk/examples/upgrade-svc.c` is the worked example.

With `P` the pinned image:

- **Stage** validates the bytes (a PE32+ driver for this machine, or an
  AxlPack of one, with its CRCs), refuses a quarantined image or one the
  optional `verify` hook refuses, and writes `P.new` crash-safely
  (`axl_file_write_atomic`). An image byte-identical to `P` unstages
  instead. It is not a security gate: authenticate the upload first, and
  use `verify` for a pinned hash, signer or downgrade policy.
- **The trial** starts the candidate straight from `P.new`, under `P`'s
  device path, with the UEFI watchdog armed. It must publish the service
  and then stay up for the grace period (10 s by default).
- **The promotion** copies `P` to `P.bak` and then replaces `P` with
  `P.new` -- after checking, before each step, that `P.new` is still the
  candidate that was trialled. If it was replaced or removed meanwhile,
  nothing is promoted: the candidate serves this boot, unpromoted, and
  the next start trials whatever `P.new` holds.
- **While a trial or promotion is in flight** -- the grace period, when
  the candidate already serves and its own handlers may take an upload --
  `axl_service_stage` and `axl_service_unstage` answer `AXL_BUSY`, and
  `axl_service_last_upgrade` answers `AXL_BUSY` too (the stored verdict is
  the previous attempt's). `axl_service_upgrade_in_flight` says so without
  trying a write; answer "try again shortly". The outcome is recorded
  before the in-flight record is cleared, so a reader never sees the old
  verdict afterwards.
- **A failed trial** quarantines the candidate, renames it to `P.failed`
  and starts `P`, which was never touched. So does a candidate that has
  killed `tries` boots (2 by default) -- a hang the watchdog reset.

`AxlServiceDeploy::staged_opts` sets `tries`, `watchdog_sec` and
`commit_grace_sec`.

**Crash safety.** An AxlAttempt breadcrumb in NV records each phase
before the change it makes (`trial:N`, `promote:bak`, `promote:swap`,
with the candidate's key), and the next start decides from the record
AND the files present -- the table in `axl_service_start_staged`'s
documentation. No step runs without its record: if NV refuses one, the
trial or promotion stops there and the next start resumes it. The
known-good image is never the object of the rename or write in flight,
and a missing `P` is restored from `P.bak` before `P` is started. A
`~AXLhhhh.TMP` a lost rename left is removed when it is byte-identical to
one of the images, and otherwise kept and logged.

**What it does not cover.**
- A CPU exception in the candidate: the firmware's handlers halt with
  interrupts masked, so the UEFI watchdog never fires. Only a hardware or
  BMC watchdog resets that.
- A failure after the grace period: by then the candidate has been
  promoted.
- Anti-rollback: power-cycling the box during a trial makes the previous
  image win.
- FAT metadata damage that is not a rename.

A service that reads `ROLLED_BACK` must not stage the same upgrade again
without new input; the quarantine is a backstop against a reboot loop,
not the policy.

See `sdk/examples/service-demo.c` for the one-line `AXL_SERVICE`
shape and `sdk/examples/service-demo-custom.c` for a hand-written
`main()` mixing the standard verbs with consumer-specific verbs.
`<axl/axl-service.h>` has the full API doxygen.

