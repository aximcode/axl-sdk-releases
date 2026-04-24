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

The default level is INFO -- messages at DEBUG and TRACE are suppressed
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
void my_handler(int level, const char *domain,
                const char *message, void *data) {
    // Write to a network socket, store in a buffer, etc.
}

axl_log_add_handler(my_handler, my_context);
```

## Ring Buffer

Capture the last N messages in memory for crash reports or diagnostics:

```c
AxlLogRing *ring = axl_log_ring_new(100);  // keep last 100 messages
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
axl_log_file_open("fs0:/app.log");
// ... all log messages are now also written to the file ...
axl_log_file_close();
```
