/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend.h
    Internal backend abstraction for AXL.

    Provides backend-agnostic access to firmware services: memory
    allocation, console output, time, file I/O, and wide-string
    operations. Library .c files include this instead of UEFI
    headers directly.

    This header is internal — application code uses axl.h, not this.
**/

#ifndef AXL_BACKEND_H
#define AXL_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

// ===================================================================
// UEFI types and table pointers
// ===================================================================

#include <uefi/axl-uefi.h>

/* gST, gBS, gRT declared in <uefi/axl-uefi-extra.h> */

#define axl_bs()  gBS
#define axl_st()  gST
#define axl_rt()  gRT

// ===================================================================
// Memory allocation (firmware-level, used by axl-mem.c)
// ===================================================================

/**
 * @brief Allocate memory from the firmware allocator.
 *
 * @return pointer to allocated memory, or NULL on failure.
 */
void *
axl_backend_alloc(
    size_t  size  ///< bytes to allocate
    );

/**
 * @brief Allocate zeroed memory from the firmware allocator.
 *
 * @return pointer to zero-filled memory, or NULL on failure.
 */
void *
axl_backend_alloc_zero(
    size_t  size  ///< bytes to allocate
    );

/**
 * @brief Free firmware-allocated memory. NULL-safe.
 */
void
axl_backend_free(
    void  *ptr  ///< pointer from axl_backend_alloc, or NULL
    );

// ===================================================================
// Console output
// ===================================================================

/**
 * @brief Write a UCS-2 string to the console. NULL-safe.
 */
void
axl_backend_console_write(
    const unsigned short  *str  ///< UCS-2 string to output
    );

/**
 * @brief Set console text attribute (color/style).
 */
void
axl_backend_console_set_attr(
    uint32_t  attr  ///< attribute bitmask
    );

/**
 * @brief Get current console text attribute.
 *
 * @return current attribute bitmask, or 0 if unavailable.
 */
uint32_t
axl_backend_console_get_attr(
    void
    );

// ===================================================================
// Time
// ===================================================================

typedef struct {
    uint16_t  year;
    uint8_t   month;
    uint8_t   day;
    uint8_t   hour;
    uint8_t   minute;
    uint8_t   second;
    uint32_t  nanosecond;
} AxlTime;

/**
 * @brief Get the current date/time from firmware.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_get_time(
    AxlTime  *time  ///< (out) receives current time
    );

/**
 * @brief Read a high-resolution monotonic counter, in microseconds.
 *
 * Uses the architecture's cycle counter (x86 TSC / aarch64
 * CNTPCT_EL0) calibrated once at first call against a brief
 * `gBS->Stall` interval (TSC) or read from the architectural
 * frequency register (CNTFRQ_EL0). Subsequent calls are cheap —
 * a single counter read and a multiplication.
 *
 * The epoch is "first call to this function" — values are
 * monotonically increasing across calls within a single boot but
 * have no defined relationship to wallclock time. Use
 * @ref axl_backend_get_time for wallclock; combine the two for
 * sub-second-precision logging.
 *
 * @return microseconds since the implicit boot epoch. Returns 0
 *         if the architecture has no usable cycle counter.
 */
uint64_t
axl_backend_get_monotonic_us(void);

// ===================================================================
// Low-level platform I/O (for AxlIpmi, future AxlPci/AxlSpd)
// ===================================================================

/**
 * @brief Read an 8-bit value from an x86 I/O port.
 *
 * Platform-specific. On x86 this issues `in`; on AARCH64 the call
 * returns -1 and leaves @a *value untouched (no port I/O on ARM).
 *
 * @return AXL_OK on success, AXL_ERR if port I/O is not available on this arch.
 */
int
axl_backend_io_read8(
    uint16_t   port,   ///< I/O port address
    uint8_t   *value   ///< (out) receives the byte read
    );

/**
 * @brief Write an 8-bit value to an x86 I/O port.
 *
 * Platform-specific. On x86 this issues `out`; on AARCH64 the call
 * returns -1 without side effects.
 *
 * @return AXL_OK on success, AXL_ERR if port I/O is not available on this arch.
 */
int
axl_backend_io_write8(
    uint16_t  port,    ///< I/O port address
    uint8_t   value    ///< byte to write
    );

// SMBus/I2C block transfers used to live here; they graduated into
// the AxlSmbus Platform Access Module. Include <axl/axl-smbus.h>
// for axl_smbus_read_block / axl_smbus_write_block.

// ===================================================================
// File I/O (Shell protocol)
// ===================================================================

/// UEFI file mode flags (from UEFI spec, backend-agnostic)
#define AXL_FILE_MODE_READ    0x0000000000000001ULL
#define AXL_FILE_MODE_WRITE   0x0000000000000002ULL
#define AXL_FILE_MODE_CREATE  0x8000000000000000ULL

typedef void *AxlFileHandle;

/**
 * @brief Open a file by UCS-2 path.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_open(
    const unsigned short  *path,        ///< UCS-2 file path
    uint64_t               mode,        ///< EFI file mode flags
    uint64_t               attributes,  ///< EFI file attributes
    AxlFileHandle         *handle       ///< (out) receives file handle
    );

/**
 * @brief Close a file handle. NULL-safe.
 *
 * @return AXL_OK on success.
 */
int
axl_backend_file_close(
    AxlFileHandle  *handle  ///< file handle (set to NULL on close)
    );

/**
 * @brief Read from a file.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_read(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to read / bytes read
    void          *buf      ///< destination buffer
    );

/**
 * @brief Write to a file.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_write(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to write / bytes written
    const void    *buf      ///< source buffer
    );

/**
 * @brief Get current file position.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_get_position(
    AxlFileHandle  handle,  ///< file handle
    uint64_t      *pos      ///< (out) receives position
    );

/**
 * @brief Set file position.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_set_position(
    AxlFileHandle  handle,  ///< file handle
    uint64_t       pos      ///< byte offset from start
    );

/**
 * @brief Delete a file by UCS-2 path.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_delete(
    const unsigned short  *path  ///< UCS-2 file path
    );

/**
 * @brief Get file size.
 *
 * @return file size in bytes, or -1 on error.
 */
int64_t
axl_backend_file_get_size(
    AxlFileHandle  handle  ///< file handle
    );

/**
 * @brief Check if a path is a directory.
 *
 * @return true if path is a directory.
 */
bool
axl_backend_file_is_dir(
    const unsigned short  *path  ///< UCS-2 path
    );

/**
 * @brief Get file metadata (size, attributes) by path.
 *
 * Opens the file, queries EFI_FILE_INFO, closes.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_stat(
    const unsigned short  *path,       ///< UCS-2 path
    uint64_t              *size,       ///< [out] file size (NULL OK)
    uint64_t              *alloc_size, ///< [out] allocation size (NULL OK)
    bool                  *is_dir,     ///< [out] directory flag (NULL OK)
    bool                  *read_only   ///< [out] read-only flag (NULL OK)
    );

/**
 * @brief Rename a file by path.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_rename(
    const unsigned short  *old_path,  ///< current UCS-2 path
    const unsigned short  *new_path   ///< new UCS-2 filename (not full path)
    );

/**
 * @brief Create a directory by path.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_mkdir(
    const unsigned short  *path  ///< UCS-2 directory path
    );

/**
 * @brief Remove an empty directory by path.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_rmdir(
    const unsigned short  *path  ///< UCS-2 directory path
    );

// ===================================================================
// Shell environment and working directory
// ===================================================================

/**
 * @brief Get a shell environment variable.
 *
 * @return UCS-2 value (pointer to Shell-owned storage), or NULL.
 */
const unsigned short *
axl_backend_shell_getenv(
    const unsigned short  *name  ///< UCS-2 variable name
    );

/**
 * @brief Set a shell environment variable.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_shell_setenv(
    const unsigned short  *name,         ///< UCS-2 variable name
    const unsigned short  *value,        ///< UCS-2 value
    bool                   volatile_var  ///< true = volatile (session only)
    );

/**
 * @brief Get the SHELL_FILE_HANDLE for the running image's standard
 *     input, as published by EFI_SHELL_PARAMETERS_PROTOCOL on this
 *     image's handle.
 *
 * Returns the handle the shell wired up — for `tool1 | tool2`
 * invocations this points to the captured LHS output, so reading
 * from it via @ref axl_backend_file_read consumes the piped bytes.
 * For non-redirected interactive launches the handle typically
 * still exists but is tied to the keyboard.
 *
 * Returns NULL when the shell-params protocol isn't published on
 * this image (cross-volume launches, BDS contexts, or any
 * non-Shell-2.0 launch path).
 */
AxlFileHandle
axl_backend_shell_stdin(void);

/**
 * @brief Get the SHELL_FILE_HANDLE for the running image's standard
 *     output, as published by EFI_SHELL_PARAMETERS_PROTOCOL on this
 *     image's handle.
 *
 * Writing bytes to this handle via @ref axl_backend_file_write
 * sends them through whatever the shell wired up — a file (`>`),
 * a pipe RHS (`|`), or the firmware console (no redirection).
 * Bypasses the CHAR16 console-output path used by axl_print, so
 * raw binary bytes survive intact across pipes.
 *
 * Returns NULL when the shell-params protocol isn't published on
 * this image. Symmetric with @ref axl_backend_shell_stdin.
 */
AxlFileHandle
axl_backend_shell_stdout(void);

/**
 * @brief Get the current working directory.
 *
 * @return UCS-2 path (pointer to Shell-owned storage), or NULL.
 */
const unsigned short *
axl_backend_shell_getcwd(void);

/**
 * @brief Change the current working directory.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_shell_chdir(
    const unsigned short  *path  ///< UCS-2 directory path
    );

/**
 * @brief Execute a shell command string.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_shell_execute(
    const unsigned short  *command  ///< UCS-2 command line
    );

// ===================================================================
// Wide-string operations
// ===================================================================

/**
 * @brief Get UCS-2 string length.
 *
 * @return number of characters (not including NUL).
 */
size_t
axl_backend_wcslen(
    const unsigned short  *s  ///< UCS-2 string
    );

/**
 * @brief Compare two UCS-2 strings.
 *
 * @return <0, 0, or >0.
 */
int
axl_backend_wcscmp(
    const unsigned short  *a,  ///< first string
    const unsigned short  *b   ///< second string
    );

/**
 * @brief Copy UCS-2 string with size limit.
 */
void
axl_backend_wcscpy(
    unsigned short        *dst,        ///< destination buffer
    const unsigned short  *src,        ///< source string
    size_t                 dst_count   ///< destination buffer size in characters
    );

/**
 * @brief Convert UCS-2 character to uppercase.
 *
 * @return uppercase character.
 */
unsigned short
axl_backend_towupper(
    unsigned short  c  ///< character to convert
    );

/**
 * @brief Find substring in UCS-2 string.
 *
 * @return pointer to first occurrence, or NULL if not found.
 */
const unsigned short *
axl_backend_wcsstr(
    const unsigned short  *haystack,  ///< string to search
    const unsigned short  *needle     ///< substring to find
    );

// ===================================================================
// Events and timers (used by axl-loop.c, axl-task-pool.c)
// ===================================================================

/* AxlEventHandle is now declared in <axl/axl-event.h>. */
#include <axl/axl-event.h>

/// Timer modes for axl_backend_event_set_timer (matches EFI_TIMER_DELAY)
#define AXL_TIMER_CANCEL    0
#define AXL_TIMER_PERIODIC  1
#define AXL_TIMER_RELATIVE  2

/**
 * @brief Create a timer event.
 *
 * Creates an EVT_TIMER event at TPL_APPLICATION with no notify.
 * Use axl_backend_event_set_timer to configure the timer.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_event_create_timer(
    AxlEventHandle  *event  ///< (out) receives event handle
    );

/**
 * @brief Create a non-timer event.
 *
 * Creates a plain event (type 0) at TPL_APPLICATION with no notify.
 * Used for protocol notifications and non-blocking AP dispatch.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_event_create(
    AxlEventHandle  *event  ///< (out) receives event handle
    );

/**
 * @brief Close an event. NULL-safe.
 *
 * DIAG-WRAPPED 2026-04-27: every call site routes through
 * axl_backend_event_close_dbg with __FILE__/__LINE__ so a debug ring
 * can spot double-closes (the suspected source of the
 * test-http.sh CoreCloseEvent #GP). The wrapped function still does
 * the same gBS->CloseEvent on the underlying handle.
 */
void
axl_backend_event_close_dbg(
    AxlEventHandle  event,    ///< event to close
    const char     *file,     ///< caller __FILE__
    int             line      ///< caller __LINE__
    );

#define axl_backend_event_close(event) \
    axl_backend_event_close_dbg((event), __FILE__, __LINE__)

/**
 * @brief Configure a timer event.
 *
 * @param type  AXL_TIMER_PERIODIC, AXL_TIMER_RELATIVE, or AXL_TIMER_CANCEL
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_event_set_timer(
    AxlEventHandle  event,           ///< timer event
    int             type,            ///< TimerPeriodic, TimerRelative, etc.
    uint64_t        interval_100ns   ///< interval in 100ns units
    );

/**
 * @brief Wait for one of several events to fire (blocking).
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_event_wait(
    size_t          count,        ///< number of events
    AxlEventHandle  *events,      ///< array of event handles
    size_t          *fired_index  ///< (out) index of signaled event
    );

/**
 * @brief Check if an event is signaled (non-blocking).
 *
 * @return 0 if signaled, 1 if not ready, -1 on error.
 */
int
axl_backend_event_check(
    AxlEventHandle  event  ///< event to check
    );

/**
 * @brief Register for protocol install notification.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_event_register_protocol_notify(
    void            *guid,          ///< protocol GUID
    AxlEventHandle   event,         ///< event to signal on install
    void           **registration   ///< (out) registration key
    );

// ===================================================================
// Console input
// ===================================================================

/**
 * @brief Get the WaitForKey event from ConIn.
 *
 * This is a borrowed event — do NOT close it.
 *
 * @return event handle, or NULL if unavailable.
 */
AxlEventHandle
axl_backend_console_wait_for_key(
    void
    );

/**
 * @brief Read a keystroke from ConIn.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_console_read_key(
    uint16_t  *scan_code,    ///< (out) scan code (0 for printable)
    uint16_t  *unicode_char  ///< (out) unicode character (0 for special)
    );

/**
 * @brief Read a keystroke from ConIn including shift-state info.
 *
 * Uses SimpleTextInputEx if available; falls back to ReadKeyStroke
 * (with @p shift_state always set to 0) when the firmware doesn't
 * publish ConsoleInHandle's ex protocol. The latter is what raw
 * serial consoles deliver — TerminalDxe carries no shift bits over
 * the wire — so a 0 result there is correct, not lossy.
 *
 * @return AXL_OK on success, AXL_ERR on error or no key available.
 */
int
axl_backend_console_read_key_ex(
    uint16_t  *scan_code,     ///< (out) scan code (0 for printable)
    uint16_t  *unicode_char,  ///< (out) unicode character (0 for special)
    uint32_t  *shift_state    ///< (out) KeyShiftState bits (0 if unavailable)
    );

/**
 * @brief Check if the shell Ctrl-C break flag is set.
 *
 * @return true if break requested.
 */
bool
axl_backend_shell_break_flag(
    void
    );

/**
 * @brief Get the shell ExecutionBreak event handle.
 *
 * Returns the EFI_EVENT that the shell signals on Ctrl-C. Can be
 * added to a WaitForEvent array to wake on break without polling.
 *
 * @return event handle, or NULL if shell is unavailable.
 */
AxlEventHandle
axl_backend_shell_break_event(
    void
    );

// ===================================================================
// App termination
// ===================================================================

/**
 * @brief Terminate the current image via gBS->Exit. Does not return.
 *
 * Convention: rc == 0 -> EFI_SUCCESS; any other value -> EFI_ABORTED.
 * The caller is expected to have already run _axl_cleanup; this
 * helper only bridges into the firmware exit service.
 */
__attribute__((noreturn))
void
axl_backend_boot_exit(
    int rc
    );

// ===================================================================
// MP Services (used by axl-task-pool.c)
// ===================================================================

/// Opaque MP context (backend manages protocol + AP enumeration)
typedef struct AxlMpContext AxlMpContext;

/// AP worker procedure (EFIAPI calling convention required by firmware)
typedef void (EFIAPI *AxlApProc)(void *arg);

/**
 * @brief Initialize MP services and enumerate available APs.
 *
 * Locates MP Services protocol, identifies BSP, builds AP list.
 *
 * @return context handle (caller must free with mp_cleanup),
 *         or NULL if MP not available (single-core fallback).
 */
AxlMpContext *
axl_backend_mp_init(
    size_t  *worker_count  ///< (out) number of usable APs
    );

/**
 * @brief Dispatch a worker procedure on an AP (non-blocking).
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_mp_start_ap(
    AxlMpContext  *ctx,       ///< MP context
    size_t         ap_index,  ///< AP index (0..worker_count-1)
    AxlApProc      proc,      ///< worker procedure
    void          *arg        ///< argument passed to proc
    );

/**
 * @brief Get the firmware processor number for an AP index.
 *
 * @return processor number (for logging).
 */
size_t
axl_backend_mp_get_ap_number(
    AxlMpContext  *ctx,       ///< MP context
    size_t         ap_index   ///< AP index (0..worker_count-1)
    );

/**
 * @brief Free MP context. NULL-safe.
 */
void
axl_backend_mp_cleanup(
    AxlMpContext  *ctx  ///< MP context
    );

// ===================================================================
// Generic EFI protocol call wrapper
// ===================================================================

/**
 * Wraps UEFI function pointer calls.  The second argument (n) is
 * ignored — it existed for gnu-efi's uefi_call_wrapper and is kept
 * for call-site compatibility.
 *
 * Usage: axl_efi_call(Protocol->Method, argcount, arg1, arg2, ...)
 *        axl_efi_call(axl_bs()->BootSvc, argcount, arg1, ...)
 */
#define axl_efi_call(fn, n, ...) (fn)(__VA_ARGS__)

// ===================================================================
// Miscellaneous Boot Services
// ===================================================================

/**
 * @brief Busy-wait for the specified duration.
 */
void
axl_backend_stall(
    uint64_t  microseconds  ///< duration in microseconds
    );

/**
 * @brief Signal an event. NULL-safe.
 */
void
axl_backend_event_signal(
    AxlEventHandle  event  ///< event to signal
    );

#endif /* AXL_BACKEND_H */
