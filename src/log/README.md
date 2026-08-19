Domain-based logging with level filtering, custom handlers, ring buffer
storage, and file output. GLib-style API with convenience macros
(`axl_error`, `axl_info`, etc.) that inject `__func__`/`__LINE__`.

Header: `<axl/axl-log.h>`

## Overview

AXL's logging system lets each source file declare a **log domain**
(a short string like `"net"` or `"http"`). Messages are filtered by
level (ERROR through TRACE) globally and per-domain.

## Basic Usage

```c
#include <axl.h>

AXL_LOG_DOMAIN("mymodule");  // declare at file scope

void my_function(void) {
    axl_info("starting up");           // [INFO]  mymodule: starting up
    axl_debug("value=%d", 42);         // [DEBUG] mymodule: value=42
    axl_error("failed: %s", reason);   // [ERROR] mymodule: failed: ...
}
```

## Log Levels

From most to least severe:

| Level | Value | When to use |
|-------|-------|-------------|
| ERROR | 0 | Unrecoverable failures |
| WARNING | 1 | Recoverable problems |
| INFO | 2 | Significant state changes (default visible) |
| DEBUG | 3 | Detailed diagnostic info |
| TRACE | 4 | Very verbose, per-packet/per-call |

The default level is INFO — messages at DEBUG and TRACE are suppressed
unless explicitly enabled.

## Level Filtering

```c
// Show DEBUG messages globally
axl_log_set_level(AXL_LOG_DEBUG);

// Suppress everything below ERROR for the "net" domain
axl_log_set_domain_level("net", AXL_LOG_ERROR);

// Clear per-domain override (reverts to global level)
axl_log_clear_domain_level("net");
```

## Custom Handlers

Route log messages to a custom function:

```c
void my_handler(int level, const char *domain, const char *message,
                const AxlRealtime *stamp, void *data) {
    // Write to a network socket, store in a buffer, etc.
}

axl_log_add_handler(my_handler, my_context);
```

`stamp` is the instant the DISPATCHER recorded for the record, read once
and handed to every sink, so two sinks never disagree about when one
record happened. Render it with `axl_time_format_at()`, or read the
fields directly for a machine-readable form. It is NULL only when the
clock could not be read.

Do not call `axl_time_realtime()` inside a handler to get "the" time:
that is a second reading of a different instant, and on a handler reached
from an event notify it re-enters the RTC the dispatcher already
serialized.

## Ring Buffer

Capture the last N messages in memory for crash reports or diagnostics:

```c
AxlLogRing *ring = axl_log_ring_new(100, 256);  // last 100 msgs, 256 B each
axl_log_ring_attach(ring);

// ... application runs ...

// Retrieve captured messages (newest first)
for (size_t i = 0; i < axl_log_ring_count(ring); i++) {
    axl_printf("  %s\n", axl_log_ring_get(ring, i));
}
axl_log_ring_free(ring);
```

## File Logging

Write log messages to a file on the UEFI filesystem:

```c
axl_log_file_attach("fs0:/app.log");
// ... all log messages are now also written to the file ...
axl_log_flush();         // optional — buffered output is drained periodically
axl_log_file_detach();   // pair with attach for explicit teardown
```

`axl_log_file_attach` opens the file, registers a buffered handler,
and starts forwarding log messages to disk. Calling it again with a
new path transparently detaches the previous one — `_detach` is only
needed when you want to stop logging without re-opening.

`axl_log_file_detach` is the symmetric teardown — flushes the
buffer, removes the internal handler, closes the file. NULL-safe on
not-attached state. The typical AxlService pattern is:
attach in driver `setup`, detach in driver `teardown`, before
firmware UnloadImage tears the per-image static state down.

## Serial Logging

Write the same lines to a UART — the channel that survives a headless
box, a wedged HTTP server, or a console the firmware has redirected
elsewhere:

```c
AxlSerial *port = NULL;                          /* consumer owns the port */
if (axl_serial_open(axl_serial_next(NULL), &port) != AXL_OK) {
    return;                                      /* AXL_BUSY = someone else has it */
}
axl_log_serial_attach(port, AXL_LOG_INFO);       /* cap what reaches the wire */
// ... log messages now also go out the UART, CRLF-terminated ...
axl_log_serial_detach();                         /* does NOT close the port */
axl_serial_close(port);
```

Lines use the **same format as the file sink's**, with CRLF endings: both
build them with the same internal formatter, so a transcript read off a
terminal lines up with one read out of a log file. (The only difference
beyond the ending is where a maximal line truncates — the longer terminator
leaves one byte less for the message.)

The port is **caller-owned** — attach neither opens nor closes it, and it
must outlive the attachment. That keeps port selection, line settings and
lifetime with the consumer, and lets a consumer that already has the port
open (a SOL bridge, say) share it deliberately.

`max_level` is not a nicety: writes are synchronous and 115200 baud is
about 11 KB/s, so an uncapped `trace` stream throttles the caller's event
loop behind the UART. The file sink can afford to skip this; a serial sink
cannot.

The handler runs from the log dispatcher, which is **not re-entrant** and
may be at **raised TPL**. It allocates nothing, logs nothing, and performs
a single write per line — dropping the remainder of a short write rather
than retrying, because a log line must never stall the caller. Losing the
tail of one line beats wedging the box that was logging it.

Not retrying removes the spin, but **not the block**. The write is
synchronous against the port's own timeout, and firmware frequently leaves
that 0 — which several implementations read as "wait indefinitely". A port
that never drains then stalls the caller's loop on every line. **Bound it
before attaching:**

```c
AxlSerialMode m;
if (axl_serial_get_mode(axl_serial_handle(port), &m) == AXL_OK) {
    m.timeout = 250000;                            /* microseconds */
    axl_serial_set_mode(port, &m);                 /* ... modify, write back */
}
axl_log_serial_attach(port, AXL_LOG_INFO);
```

Read-modify-write, not a fresh struct: `axl_serial_set_mode` takes the whole
`AxlSerialMode` and treats a zero field as "device default", so a partly
filled one silently re-rates the port. For scale, a full 640-byte line takes
about 55 ms at 115200 8N1.

## The engine is opt-in at link time

`axl_error` and friends expand to `axl_log_full`, which lives in
`axl-log-emit.o` and forwards to a **weak** `_axl_log_vdispatch` defined in
`axl-log.o`. Nothing pulls the engine implicitly: a link that wants logging
asks for it with `-u _axl_log_vdispatch`.

That indirection is not decoration. Every emitter call site in the library is
an ordinary strong reference — 682 of them across 140 files, and 27 of the 51
archive members in a *do-nothing* image carry one. While the emitters and the
engine shared an object, every image AXL had ever produced linked the level
filter, the domain table, the handler table, the console renderer and the
wallclock chain behind its timestamps, whether or not it logged. It cost 6,144
bytes on x64 and 5,632 on aa64 (5,232 of real `.text`+`.rodata` on x64, rounded
up by the PE's 4 KB section alignment), and it is why `--minimal-runtime`
measured no saving against the full runtime for as long as the flag existed.

**Every ordinary build links the engine and behaves exactly as before** — the
in-tree link macros carry `$(LOG_ENGINE_PULL)` and `axl-cc` passes it by
default. The single exception is `axl-cc --minimal-runtime`, whose contract is
to link nothing it was not asked for:

```sh
axl-cc --minimal-runtime=log    app.c -o app.efi   # emit; costs ~6 KB
axl-cc --minimal-runtime=nolog  app.c -o app.efi   # no-op; keep the bytes
```

An app whose own objects reference an emitter must name one or the other;
`axl-cc` refuses the link otherwise, so an image can never go quiet without
someone having chosen it. Records are discarded *before* formatting, so a
suppressed call costs a load and a branch.

`nolog` declines to ask for the engine; it does not *forbid* one. An app that
calls `axl_log_set_level` or `axl_log_add_handler` pulls `axl-log.o` for those,
which defines the seam, and its `axl_error` calls emit again. That is the right
answer rather than a leak — an app reaching for the log API wants logging — but
it does mean `nolog` is not a silence guarantee, and the bytes come back.

Two consequences worth knowing:

- A `--minimal-runtime` image linked against an `AXL_MEM_DEBUG` build of
  `libaxl.a` prints no leak report unless it asked for the engine — the report
  is emitted with `axl_warning`. `axl-cc` builds are unaffected: `install.sh`
  stages a RELEASE library where the report does not exist.
- The library's *own* diagnostics are not what triggers the `axl-cc` check.
  Silencing those is precisely what `--minimal-runtime` is for.

`test-log-link-granularity.sh` holds the linkage half and
`test-minimal-log-qemu.sh` the runtime half — the second matters because the
weak target is address zero in an engine-less image, so a missing NULL check is
a fault rather than a quiet regression.
