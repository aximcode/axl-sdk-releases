/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-log.h:
 *
 * Domain-based logging with level filtering, custom handlers,
 * ring buffer storage, and file output.
 *
 * GLib-style API: axl_log_set_level, axl_log_add_handler, etc.
 * Convenience macros (axl_error, axl_info, ...) inject __func__/__LINE__.
 */

#ifndef AXL_LOG_H
#define AXL_LOG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Log levels
// ---------------------------------------------------------------------------

#define AXL_LOG_ERROR    0
#define AXL_LOG_WARNING  1
#define AXL_LOG_INFO     2
#define AXL_LOG_DEBUG    3
#define AXL_LOG_TRACE    4

// ---------------------------------------------------------------------------
// Domain declaration + convenience macros
// ---------------------------------------------------------------------------

/**
 * AXL_LOG_DOMAIN:
 *
 * Declare the log domain for the current source file.
 * Place at the top of each .c file.
 */
#define AXL_LOG_DOMAIN(d)  static const char *_AxlLogDomain __attribute__((unused)) = (d)

/**
 * Convenience macros — inject __func__ and __LINE__.
 * Stay uppercase because they are macros, not functions.
 */
#define axl_error(...)   axl_log_full(AXL_LOG_ERROR,   _AxlLogDomain, \
                                     __func__, __LINE__, __VA_ARGS__)
#define axl_warning(...) axl_log_full(AXL_LOG_WARNING,  _AxlLogDomain, \
                                     __func__, __LINE__, __VA_ARGS__)
#define axl_info(...)    axl_log_full(AXL_LOG_INFO,     _AxlLogDomain, \
                                     __func__, __LINE__, __VA_ARGS__)
#define axl_debug(...)   axl_log_full(AXL_LOG_DEBUG,    _AxlLogDomain, \
                                     __func__, __LINE__, __VA_ARGS__)
#define axl_trace(...)   axl_log_full(AXL_LOG_TRACE,    _AxlLogDomain, \
                                     __func__, __LINE__, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Core API
// ---------------------------------------------------------------------------

/**
 * @brief Log a message with full source location.
 *
 * Prefer the convenience macros (axl_error, axl_info, etc.) which
 * fill in func/line.
 */
void
axl_log_full(
    int         level,   ///< log level (AXL_LOG_ERROR..AXL_LOG_TRACE)
    const char *domain,  ///< module name, or NULL
    const char *func,    ///< __func__ (or NULL)
    int         line,    ///< __LINE__ (or 0)
    const char *fmt,     ///< standard C printf format string
    ...
) __attribute__((format(printf, 5, 6)));

/**
 * @brief Log a message without source location.
 */
void
axl_log(
    int         level,   ///< log level
    const char *domain,  ///< module name, or NULL
    const char *fmt,     ///< standard C printf format string
    ...
) __attribute__((format(printf, 3, 4)));

/**
 * @brief Set the global log level.
 *
 * Messages above this level are suppressed. Default: AXL_LOG_INFO.
 */
void
axl_log_set_level(
    int level  ///< new global level
);

/**
 * @brief Set the log level for a specific domain.
 */
void
axl_log_set_domain_level(
    const char *domain,  ///< domain name
    int         level    ///< level for this domain, or -1 to clear the override
);

/**
 * @brief Apply log-level configuration from the @c AXL_LOG_LEVEL
 *     environment variable.
 *
 * Format (@c value of the env var):
 *
 *     AXL_LOG_LEVEL=debug                     # all domains, debug
 *     AXL_LOG_LEVEL=smbus:debug               # smbus only
 *     AXL_LOG_LEVEL=smbus:debug,net:info      # multi-domain
 *     AXL_LOG_LEVEL=*:warn,smbus:debug        # explicit default
 *     AXL_LOG_LEVEL=all                       # alias for *:debug
 *     AXL_LOG_LEVEL=off                       # alias for *:none
 *
 * Each entry is `<domain>:<level>` separated by commas. Level
 * keywords (case-insensitive): @c off / @c none, @c error,
 * @c warning / @c warn, @c info, @c debug, @c trace. Domain
 * @c * sets the global default. Bare `<level>` (no colon) is a
 * shorthand for `*:<level>`.
 *
 * The function is idempotent and safe to call multiple times.
 * It is invoked automatically:
 *   - on first log emission (`axl_log` / `axl_log_full`)
 *   - on first call to `axl_log_set_level` /
 *     `axl_log_set_domain_level`
 * — so setting @c AXL_LOG_LEVEL in the shell before invoking a
 * tool takes effect with no further configuration.
 *
 * **Precedence**: the env var is the **baseline**; programmatic
 * @c axl_log_set_level / @c axl_log_set_domain_level calls
 * **always win** because the lazy init runs first inside those
 * setters before they apply the explicit level. This matches
 * @c RUST_LOG semantics — env defines the floor, code overrides.
 *
 * Level keywords are case-insensitive (`debug`, `Debug`,
 * `DEBUG` all parse the same). Unrecognized levels and malformed
 * entries are silently ignored — no log churn during init.
 *
 * Per-domain configuration is the larger value-add over a CLI
 * flag — keeps `-d` / `-v` / `--debug` namespace free for
 * tool-specific use. Mirrors the @c RUST_LOG / @c GST_DEBUG /
 * @c G_MESSAGES_DEBUG conventions.
 */
void
axl_log_init_from_env(void);

// ---------------------------------------------------------------------------
// Handler management
// ---------------------------------------------------------------------------

/**
 * @brief Custom log handler callback.
 */
typedef void (*AxlLogHandler)(
    int         level,    ///< log level
    const char *domain,   ///< module name (may be NULL)
    const char *message,  ///< formatted message (no prefix, no newline)
    void       *data      ///< opaque callback data
);

/**
 * @brief Add a global handler.
 *
 * Receives all messages that pass level filtering. The handler table
 * is bounded; once full, additional registrations are rejected.
 *
 * **Re-entrancy.** Handlers must not allocate, send HTTP responses,
 * or do anything that can itself emit a log line — the dispatcher is
 * not re-entrant. A handler that triggers another `axl_warning` will
 * recurse and corrupt the in-flight message.
 *
 * @return AXL_OK on success, AXL_ERR if @p handler is NULL or the table is full.
 */
int
axl_log_add_handler(
    AxlLogHandler handler,  ///< callback
    void         *data      ///< opaque data passed to handler
);

/**
 * @brief Add a handler that only fires for a specific domain and level range.
 *
 * @return AXL_OK on success, AXL_ERR if @p handler is NULL or the table is full.
 */
int
axl_log_add_domain_handler(
    const char   *domain,     ///< domain to filter (NULL matches all)
    int           max_level,  ///< maximum level to deliver
    AxlLogHandler handler,    ///< callback
    void         *data        ///< opaque data
);

/**
 * @brief Remove a previously added handler.
 */
void
axl_log_remove_handler(
    AxlLogHandler handler  ///< handler to remove
);

/**
 * @brief Suppress default console output.
 *
 * Call after adding custom handlers.
 */
void
axl_log_suppress_console(void);

/**
 * @brief Enable or disable console timestamps (on by default).
 */
void
axl_log_set_console_timestamp(
    bool enable  ///< true to show `HH:MM:SS[.uuuuuu]` timestamps (the
                 ///< fractional field is omitted on platforms whose
                 ///< firmware doesn't populate `EFI_TIME.Nanosecond`)
);

/**
 * @brief Enable or disable console color output (on by default).
 *
 * When disabled, log output is plain ASCII with no EFI console
 * attribute changes. Useful for serial consoles and log capture.
 */
void
axl_log_set_console_color(
    bool enable  ///< true for color, false for plain ASCII
);

// ---------------------------------------------------------------------------
// Fatal level
// ---------------------------------------------------------------------------

/**
 * @brief Set the fatal level.
 *
 * Messages at or below this level cause exit. Pass -1 to disable.
 */
void
axl_log_set_fatal_level(
    int level  ///< level at or below which messages cause exit
);

/**
 * @brief Set the image handle needed for fatal exit via gBS->Exit.
 */
void
axl_log_set_fatal_image_handle(
    void *image_handle  ///< application image handle (void* to avoid EFI_HANDLE leak)
);

// ---------------------------------------------------------------------------
// Ring buffer handler
// ---------------------------------------------------------------------------

typedef struct AxlLogRing AxlLogRing;

typedef struct {
    const char  *message;
    int          level;
    const char  *domain;
    /* Microseconds since axl init — strictly monotonic across the
       lifetime of the running app. Use for ordering / time-deltas
       between entries. To recover wallclock, capture
       (mono_us, AxlTime) once at app start and add the offset. */
    uint64_t     timestamp;
} AxlLogEntry;

/**
 * @brief Create a new log ring buffer.
 *
 * @return new ring, or NULL on failure.
 */
AxlLogRing *
axl_log_ring_new(
    size_t max_entries,  ///< ring capacity
    size_t entry_size    ///< max message length per entry (bytes)
);

/**
 * @brief Free a log ring buffer. NULL-safe.
 */
void
axl_log_ring_free(
    AxlLogRing *ring  ///< ring to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlLogRing, axl_log_ring_free)
#endif

/**
 * @brief Attach a ring as a log handler.
 */
void
axl_log_ring_attach(
    AxlLogRing *ring  ///< ring to attach
);

/**
 * @brief Get the number of entries stored in a ring.
 *
 * @return number of entries stored.
 */
size_t
axl_log_ring_count(
    AxlLogRing *ring  ///< ring to query
);

/**
 * @brief Retrieve an entry from a ring by index.
 *
 * @return true if entry returned, false if index out of range.
 */
bool
axl_log_ring_get(
    AxlLogRing  *ring,   ///< ring to query
    size_t       index,  ///< entry index (0 = newest)
    AxlLogEntry *entry   ///< filled with entry data
);

// ---------------------------------------------------------------------------
// File handler
// ---------------------------------------------------------------------------

/**
 * @brief Open a log file and register a handler that buffers output.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_log_file_attach(
    const char *path  ///< UTF-8 file path (e.g. "fs0:/app.log")
);

/**
 * @brief Flush the file handler's buffer to disk.
 */
void
axl_log_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_LOG_H */
