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
#include <axl/axl-time.h>   /* AxlTimespec for axl_backend_clock_gettime */

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
 * @brief Write a UCS-2 string to the error console (gST->StdErr).
 *        Falls back to gST->ConOut when StdErr is NULL. NULL-safe.
 */
void
axl_backend_console_write_err(
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
 * @brief Set the error console's text attribute (color/style).
 *        Targets gST->StdErr so escape bytes emitted by ANSI/serial
 *        consoles (TerminalDxe) land on the same sink as
 *        axl_backend_console_write_err() output. Falls back to
 *        gST->ConOut when StdErr is NULL. NULL-safe.
 */
void
axl_backend_console_set_attr_err(
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

/**
 * @brief Toggle the shell's page-break (screen-at-a-time) output mode.
 *
 * Delegates to EFI_SHELL_PROTOCOL.EnablePageBreak / DisablePageBreak. The
 * shell's console logger wraps gST->ConOut, so enabling paginates AXL tool
 * output — which writes to ConOut — with the shell's own prompt and key
 * handling; the shell owns all geometry, interactivity, and redirect logic.
 *
 * No-op (nothing paginated) when no EFI_SHELL_PROTOCOL is reachable: the
 * legacy EFI 1.x shell publishes SHELL_ENVIRONMENT instead, and a non-shell
 * context has no pager at all. Paging is a shell service, not an SDK one.
 */
void
axl_backend_console_set_page_break(
    bool  enable    ///< true = enable page break, false = disable
    );

/**
 * @brief Number of text-output modes the active console enumerates.
 *
 * @return `ConOut->Mode->MaxMode` clamped to non-negative, or 0 if there
 *     is no output console.
 */
uint32_t
axl_backend_console_text_mode_count(
    void
    );

/**
 * @brief Query the geometry of text-output mode @p index.
 *
 * @return AXL_OK with @p columns / @p rows set, or AXL_ERR if there is no
 *     console or `QueryMode` rejected the mode.
 */
int
axl_backend_console_text_query_mode(
    uint32_t   index,    ///< mode number
    uint32_t  *columns,  ///< [out] columns (non-NULL)
    uint32_t  *rows      ///< [out] rows (non-NULL)
    );

/**
 * @brief The active console's current text-output mode index.
 *
 * @return current `Mode->Mode`, or -1 if there is no console or no mode is
 *     currently set.
 */
int
axl_backend_console_text_current_mode(
    void
    );

/**
 * @brief Switch the active console to text-output mode @p index.
 *
 * @return AXL_OK on success, AXL_ERR if there is no console or `SetMode`
 *     failed (the firmware leaves the mode unchanged on failure).
 */
int
axl_backend_console_text_set_mode(
    uint32_t  index  ///< mode number
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
    uint8_t   daylight;           ///< raw EFI_TIME.Daylight bits (bit 0 = DST active)
    uint32_t  nanosecond;
    int16_t   timezone_minutes;   ///< signed minutes from UTC; INT16_MIN if firmware reports EFI_UNSPECIFIED_TIMEZONE
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
 * @brief Set the current date/time in firmware
 *     (EFI_RUNTIME_SERVICES.SetTime).
 *
 * The write counterpart to @ref axl_backend_get_time. The AXL
 * `INT16_MIN` timezone sentinel maps back to EFI's
 * `EFI_UNSPECIFIED_TIMEZONE` (2047); `daylight` is passed through to
 * `EFI_TIME.Daylight`. The firmware validates the fields.
 *
 * @return AXL_OK on success, AXL_ERR on NULL input or firmware
 *     failure.
 */
int
axl_backend_set_time(
    const AxlTime  *time  ///< time to program into the RTC
    );

/**
 * @brief Read the RTC wake alarm
 *     (EFI_RUNTIME_SERVICES.GetWakeupTime).
 *
 * Members of the same mutually-exclusive RTC group as
 * @ref axl_backend_get_time (UEFI 2.11 Table 8.1), and guarded by the
 * same flag. Out parameters are optional.
 *
 * @return AXL_OK; AXL_UNSUPPORTED if the platform has no wake timer;
 *     AXL_ERR on firmware failure or a nested RTC call.
 */
int
axl_backend_get_wakeup(
    bool     *enabled,  ///< (out, optional) alarm armed
    bool     *pending,  ///< (out, optional) alarm has fired
    AxlTime  *when      ///< (out, optional) programmed alarm time
    );

/**
 * @brief Arm or disarm the RTC wake alarm
 *     (EFI_RUNTIME_SERVICES.SetWakeupTime).
 *
 * NULL @p when disarms. Same RTC group exclusion as
 * @ref axl_backend_get_wakeup.
 *
 * @return AXL_OK; AXL_UNSUPPORTED if the platform has no wake timer;
 *     AXL_ERR on firmware failure or a nested RTC call.
 */
int
axl_backend_set_wakeup(
    const AxlTime  *when   ///< alarm time, or NULL to disarm
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

/**
 * @brief Backend primitive for @ref axl_clock_gettime.
 *
 * Single entry point for both monotonic and wallclock reads. The
 * native backend handles cycle-counter calibration (with per-boot
 * caching via a UEFI protocol on x86) and the
 * EFI_TIME → Unix-seconds Gregorian conversion.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments or hardware /
 *     firmware error.
 */
int
axl_backend_clock_gettime(
    AxlClockId    clockid,   ///< clock to read
    AxlTimespec  *out        ///< [out] populated on success
);

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
 * @brief Flush a file handle's pending data through to the volume.
 *
 * Pushes firmware-buffered writes to the media via the real flush
 * primitive (the shell's FlushFile, or EFI_FILE_PROTOCOL.Flush on the
 * old EFI 1.x path). @p handle must be open for writing — the firmware
 * answers a read-only handle with EFI_ACCESS_DENIED, which surfaces
 * here as AXL_ERR.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_flush(
    AxlFileHandle  handle  ///< file handle (open for write)
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
 * @brief Set (truncate or extend) a file's size on an open handle.
 *
 * Updates the file length via SetFileInfo. Shrinking truncates; size
 * 0 empties the file. The handle must be open for writing.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_file_set_size(
    AxlFileHandle  handle,  ///< file handle (open for write)
    uint64_t       size     ///< new file size in bytes
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
    uint64_t              *mtime_unix, ///< [out] modification time, Unix epoch seconds (NULL OK, 0 = unknown)
    bool                  *is_dir,     ///< [out] directory flag (NULL OK)
    bool                  *read_only   ///< [out] read-only flag (NULL OK)
    );

/**
 * Convert a 16-byte EFI_TIME value to Unix epoch seconds. The input
 * is read out of EFI_FILE_INFO.ModificationTime (or any of the other
 * EFI_TIME fields) by either the backend or the axl-fs.c dir-entry
 * parser. Returns 0 (caller's "unknown" sentinel) if the wire bytes
 * indicate an unset / zero date.
 *
 * Treats TimeZone as UTC when EFI_UNSPECIFIED_TIMEZONE (0x07FF) is
 * stored; otherwise subtracts TimeZone-minutes to convert to UTC.
 * Daylight bit is ignored (the timezone offset already accounts for
 * it on platforms that bother to set it).
 */
uint64_t
axl_backend_efi_time_to_unix(
    const void  *efi_time_16  ///< pointer to 16 bytes in EFI_TIME layout
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
 * @brief Is the shell's StdIn an interactive console (not a redirected
 *     file or pipe)?
 *
 * Probes axl_backend_shell_stdin() with EFI_SHELL_PROTOCOL.GetFileSize:
 * a redirected file or pipe reports a size (EFI_SUCCESS); the console
 * pseudo-file rejects the query. Confirmed on OVMF/EDK2 across typed,
 * `< file`, and `| pipe` launches. When GetFileSize fails, a second
 * probe (GetFileInfo) must ALSO report no file for the verdict to be
 * "interactive" — biasing an unfamiliar firmware toward the safe
 * raw-byte path (a false "piped" only degrades a typed line to a raw
 * console read; a false "interactive" would block a pipe on a keyboard).
 *
 * Returns false when no shell StdIn handle is published (BDS / non-shell
 * contexts) — there is nothing to read interactively through the shell,
 * and the stream layer surfaces those as EOF rather than blocking on a
 * keyboard. Backs the public @ref axl_stdin_is_interactive predicate and
 * axl_stdin's console-line-edit fallback.
 *
 * @return true if StdIn is the interactive console; false if redirected
 *     or not connected.
 */
bool
axl_backend_stdin_is_interactive(void);

/**
 * @brief Is the shell's StdOut an interactive console (not a redirected
 *     file or pipe)?
 *
 * Symmetric with @ref axl_backend_stdin_is_interactive, probing
 * axl_backend_shell_stdout() the same way. When false AND a shell StdOut
 * handle is published, the text stdout sink writes UCS-2 to that handle so
 * a `| pipe` carries the tool's output — the shell wires StdOut for a pipe
 * but does NOT swap gST->ConOut, so a ConOut-only write never reaches the
 * downstream stage. When true, the sink uses gST->ConOut so the console
 * subsystem (tap / mirror / device) still sees the bytes.
 *
 * @return true if StdOut is the interactive console; false if redirected,
 *     piped, or not connected.
 */
bool
axl_backend_stdout_is_interactive(void);

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
 * @brief Get the SHELL_FILE_HANDLE for the running image's standard
 *     error, as published by EFI_SHELL_PARAMETERS_PROTOCOL on this
 *     image's handle.
 *
 * Symmetric with @ref axl_backend_shell_stdout. Returns NULL when the
 * shell-params protocol isn't published on this image.
 */
AxlFileHandle
axl_backend_shell_stderr(void);

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

/**
 * @brief Execute a shell command string with its console output swallowed.
 *
 * Runs @p command exactly as @ref axl_backend_shell_execute does, but
 * temporarily points `gST->ConOut->OutputString` at a no-op for the duration,
 * so text the command prints is discarded while its side effects run normally.
 * Unlike a `> nul` redirect, the command is unaltered and runs in-context — on
 * the old EFI 1.x shell that matters, because a redirect pushes the command
 * into a sub-context and loses side effects the interactive shell needs (e.g.
 * `map -r`'s device-path aliases). A no-op difference on the modern shell,
 * whose nested Execute is already off-console.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_shell_execute_quiet(
    const unsigned short  *command  ///< UCS-2 command line
    );

/**
 * @brief Resolve the UEFI Shell filesystem alias (e.g. "fs3") for a
 *        device path.
 *
 * Uses the shell's device-path -> map lookup so the returned name matches
 * what the user and `map` / `vol fsN:` see, and tracks remaps (mkrd, USB
 * hot-plug) — unlike a positional LocateHandle index. Writes the lowercased
 * `fs<n>` alias WITHOUT the trailing ':' (e.g. "fs3") into @p out. On the old
 * EFI 1.x shell (no EFI_SHELL_PROTOCOL) it reverse-looks-up the name through
 * SHELL_ENVIRONMENT.GetMap, so it resolves once the disk is in the shell's map.
 *
 * @return AXL_OK on success; AXL_ERR on bad args, when the device path has no
 *         `fs<n>` mapping, or when the backend has no shell at all (callers
 *         then fall back to a positional name).
 */
int
axl_backend_shell_map_name(
    void   *device_path,  ///< opaque EFI_DEVICE_PATH_PROTOCOL for the volume
    char   *out,          ///< [out] receives lowercased "fsN"
    size_t  out_size      ///< capacity of @p out
    );

/**
 * @brief The device path's FIRST shell alias — any form.
 *
 * Like axl_backend_shell_map_name but returns the first alias
 * GetMapFromDevicePath lists (verbatim, minus the trailing ':') whether it is
 * an `fs<n>` name or a custom SetMap name (e.g. "RD"). For "is this device
 * mapped, and as what?" queries.
 *
 * @return AXL_OK on success; AXL_ERR on bad args, no mapping, or no shell
 *         (the old EFI 1.x shell uses the SHELL_ENVIRONMENT.GetMap fallback).
 */
int
axl_backend_shell_map_alias(
    void   *device_path,  ///< opaque EFI_DEVICE_PATH_PROTOCOL for the volume
    char   *out,          ///< [out] receives the first alias, verbatim, no ':'
    size_t  out_size      ///< capacity of @p out
    );

/**
 * @brief Name the volume a device HANDLE represents, e.g. "FS0" or "fs0".
 *
 * The handle-keyed counterpart of @ref axl_backend_shell_map_alias, for the
 * "what volume did this image come from" query — an image loaded from a file
 * carries its volume as `EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle`, and matching
 * that handle by identity is exact where a device-path comparison is merely
 * careful.
 *
 * Where a shell is live its map is the ONLY naming returned — verbatim, so a
 * caller's path keeps the spelling the user sees, and a volume the shell has
 * not mapped reports AXL_ERR rather than a positional name the shell would
 * resolve to a different volume. Only with NO shell at all is the volume
 * named positionally (`fs<n>`), matched on the handle by identity. Resolved
 * on each call, so a name captured earlier cannot go stale against a remap.
 *
 * @return AXL_OK with the bare name (no trailing ':') in @p out; AXL_ERR on
 *         bad args, or when nothing names the handle's volume — including a
 *         live shell that has no mapping for it.
 */
int
axl_backend_volume_name_for_handle(
    void   *device_handle,  ///< opaque EFI_HANDLE for the volume
    char   *out,            ///< [out] receives the volume name, no ':'
    size_t  out_size        ///< capacity of @p out
    );

/**
 * @brief Byte-compare two device paths, including their END nodes.
 *
 * Node-length-driven walk, bounded against a malformed chain (a node claiming
 * Length < 4 would otherwise never advance). Two paths are equal when they are
 * the same total size and identical byte for byte.
 *
 * @return true when both paths are well-formed and identical.
 */
bool
axl_backend_dp_equal(
    const void  *a,  ///< opaque EFI_DEVICE_PATH_PROTOCOL
    const void  *b   ///< opaque EFI_DEVICE_PATH_PROTOCOL
    );

/**
 * @brief Remove a shell map alias (SetMap with a NULL device path).
 *
 * Deletes @p name from the shell's global map — used to drop a mapping whose
 * backing device is going away (e.g. a RAM disk being destroyed), so a later
 * `<name>:` doesn't dereference a freed device path.
 *
 * @return AXL_OK on success; AXL_ERR on bad args / SetMap failure;
 *         AXL_UNSUPPORTED when there is no shell.
 */
int
axl_backend_shell_unmap(
    const unsigned short *name   ///< ':'-terminated mapping name to delete
    );

/**
 * @brief Is a shell map name (e.g. "fs2:") currently in use?
 *
 * Consults EFI_SHELL_PROTOCOL.GetDevicePathFromMap. @p name must be
 * ':'-terminated (the shell's mapping-name form).
 *
 * @return true if a mapping exists for @p name; false if not, or when there
 *     is no shell / GetDevicePathFromMap.
 */
bool
axl_backend_shell_map_exists(
    const unsigned short  *name  ///< ':'-terminated UCS-2 mapping name
    );

/**
 * @brief Assign a shell map name to a device path (EFI_SHELL_PROTOCOL.SetMap).
 *
 * Adds the mapping to the shell's GLOBAL map list — not a nested shell like
 * Execute — so a name set from a child image is immediately usable by the
 * launching shell/script without `map -r`. @p name must be ':'-terminated.
 * The device path should carry a filesystem (connected) for the name to be
 * usable as a volume.
 *
 * @return AXL_OK on success; AXL_ERR on bad args or SetMap failure;
 *     AXL_UNSUPPORTED when the backend has no shell / SetMap.
 */
int
axl_backend_shell_set_map(
    void                  *device_path,  ///< opaque EFI_DEVICE_PATH_PROTOCOL
    const unsigned short  *name          ///< ':'-terminated UCS-2 mapping name
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
 * @brief Create a periodic timer with a notify-signal callback.
 *
 * Creates an `EVT_TIMER | EVT_NOTIFY_SIGNAL` event at
 * `TPL_CALLBACK` and arms it as periodic with the given period.
 * The firmware invokes @p notify(@p ctx) on each tick. Used by
 * `axl_loop_attach_driver` to drive an `AxlLoop`'s dispatch from
 * firmware-managed timer events when no foreground caller exists
 * (DXE driver mode).
 *
 * `TPL_CALLBACK` is the lowest TPL legal for an `EVT_NOTIFY_SIGNAL`
 * event — UEFI 2.11 §7.1 only maintains signal queues at
 * `TPL_CALLBACK` and `TPL_NOTIFY` (firmware rejects
 * `TPL_APPLICATION` with `EFI_INVALID_PARAMETER`). At
 * `TPL_CALLBACK` the notify shares the FIFO queue with co-located
 * firmware drivers (TCP4 / MNP / SNP run their state machines at
 * the same level), so each notify must stay short or co-located
 * drivers can't make progress. See `axl_loop_attach_driver`
 * doxygen for the notify-budget rule.
 *
 * Pair every successful call with `axl_backend_event_close` — the
 * close path cancels the timer, lets in-flight notifies drain via
 * `gBS->CloseEvent`, then frees the bridging context. Calling
 * `axl_backend_event_set_timer` afterward is supported (e.g. to
 * change the period) but not required.
 *
 * @return AXL_OK on success, AXL_ERR on error or table-full.
 */
int
axl_backend_event_create_notify_timer(
    void   (*notify)(void *ctx),    ///< notify function (TPL_CALLBACK)
    void    *ctx,                   ///< opaque context passed to @p notify
    uint64_t interval_100ns,        ///< period in 100ns units
    AxlEventHandle *event           ///< (out) receives event handle
    );

/**
 * @brief Register a notify to run just before ExitBootServices.
 *
 * Uses the UEFI 2.9 `EFI_EVENT_GROUP_BEFORE_EXIT_BOOT_SERVICES` group,
 * which is signalled while Boot Services are still fully usable, rather
 * than the ExitBootServices group itself. That distinction matters for
 * anything holding APs: the firmware's own AP-relocation handler is in
 * the ExitBootServices group, notification order within a group is not
 * specified, and losing that race means the firmware spins forever
 * waiting for an AP that is still inside consumer code.
 *
 * Pair with `axl_backend_event_close`.
 *
 * @return AXL_OK on success, AXL_ERR on error or table-full.
 */
int
axl_backend_event_create_before_exit_boot(
    void  (*notify)(void *ctx),  ///< notify function (TPL_CALLBACK)
    void   *ctx,                 ///< opaque context passed to @p notify
    AxlEventHandle *event        ///< (out) receives event handle
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
 * Raised-TPL-safe: `gBS->WaitForEvent` is unavailable above
 * `TPL_APPLICATION` (a nested wait reached from a driver-pump notify
 * dispatched at `TPL_CALLBACK`), so above that level this falls back to a
 * non-blocking `CheckEvent` sweep with a short `Stall` between passes
 * rather than failing or wedging. At `TPL_APPLICATION` (the common
 * foreground path) it falls through to a plain `WaitForEvent`.
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
 * @brief Whether the caller is executing above @c TPL_APPLICATION.
 *
 * @c gBS->WaitForEvent returns @c EFI_UNSUPPORTED above @c TPL_APPLICATION
 * (a nested wait reached from a driver-pump notify at @c TPL_CALLBACK).
 * @ref axl_backend_event_wait uses this to pick its non-blocking
 * @c CheckEvent fallback; the sync network wrappers use it to install a
 * protocol @c Poll() tick only when one is needed (the firmware notify
 * already drives I/O at @c TPL_APPLICATION, so no tick — and no extra CPU
 * — in the common foreground case). Cheap: one @c RaiseTPL / @c RestoreTPL.
 *
 * @return true if the current TPL is above @c TPL_APPLICATION.
 */
bool
axl_backend_at_raised_tpl(void);

/**
 * @brief The task priority level the caller is executing at.
 *
 * UEFI has no read accessor, so this is a raise-to-ceiling / restore
 * pair — cheap, and legal from any level including @c TPL_HIGH_LEVEL.
 *
 * @return the current @c EFI_TPL as a plain integer.
 */
uintptr_t
axl_backend_tpl_current(void);

/**
 * @brief Raise to @p level, returning the level to restore.
 *
 * Clamps: a request at or below the current level leaves the level
 * unchanged and returns it, so the paired @ref axl_backend_tpl_restore
 * never tries to restore UPWARD (which the firmware treats as fatal
 * misuse rather than an error return).
 *
 * @return the previous level, for @ref axl_backend_tpl_restore.
 */
uintptr_t
axl_backend_tpl_raise(
    uintptr_t level   ///< level to raise to
);

/**
 * @brief Restore the level returned by @ref axl_backend_tpl_raise.
 */
void
axl_backend_tpl_restore(
    uintptr_t level   ///< level to restore to
);

/**
 * @brief Enter a brief critical section by raising to a serialization TPL.
 *
 * Raises to a level at or above @c TPL_CALLBACK and returns an opaque token to
 * pass to @ref axl_backend_leave_critical. Bracket a short data-structure update
 * (a pointer swap, a small append) that is shared between a foreground writer at
 * @c TPL_APPLICATION and a driver-pump consumer dispatched at @c TPL_CALLBACK:
 * while raised, neither the pump notify nor a lower-TPL writer can preempt, so the
 * two never interleave. Keep the section short — no blocking, no I/O. Pairs
 * strictly LIFO with @ref axl_backend_leave_critical (restore is a stack).
 *
 * The guard raises to @c TPL_NOTIFY (the highest TPL at which pool allocation is
 * still legal, so a guarded buffer append may grow). The caller must therefore
 * already be at or below @c TPL_NOTIFY — true of any console/loop path (they run
 * at @c TPL_APPLICATION or @c TPL_CALLBACK); it is not a general above-NOTIFY lock.
 *
 * @return an opaque token holding the prior TPL, for @ref axl_backend_leave_critical.
 */
uintptr_t
axl_backend_enter_critical(void);

/**
 * @brief Leave the critical section entered by @ref axl_backend_enter_critical.
 *
 * Restores the TPL captured in @p token. Must be called on the same call stack,
 * LIFO, exactly once per @ref axl_backend_enter_critical.
 *
 * @param token the value @ref axl_backend_enter_critical returned.
 */
void
axl_backend_leave_critical(
    uintptr_t token   ///< token from axl_backend_enter_critical
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

/**
 * @brief Install a protocol interface on a handle.
 *
 * Portability seam under axl_protocol_install. If `*handle` is NULL a
 * fresh handle is allocated and written back.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_install_protocol(
    void           **handle,        ///< (in/out) handle; *handle == NULL allocates a fresh one
    const void      *guid,          ///< protocol GUID (binary-compatible with EFI_GUID)
    void            *iface          ///< interface pointer to publish
    );

/**
 * @brief Uninstall a protocol interface from a handle.
 *
 * Portability seam under axl_protocol_uninstall.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_uninstall_protocol(
    void            *handle,        ///< handle the protocol was installed on
    const void      *guid,          ///< protocol GUID
    void            *iface          ///< interface pointer that was installed
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
 * @brief Read a keystroke from ConIn including modifier/lock state.
 *
 * Uses SimpleTextInputEx if available, translating its KeyShiftState +
 * KeyToggleState into normalized AXL_INPUT_MOD_* bits; falls back to
 * ReadKeyStroke (with @p modifiers always 0) when the firmware doesn't
 * publish ConsoleInHandle's ex protocol. The latter is what raw serial
 * consoles deliver — TerminalDxe carries no shift bits over the wire —
 * so a 0 result there is correct, not lossy.
 *
 * @return AXL_OK on success, AXL_ERR on error or no key available.
 */
int
axl_backend_console_read_key_ex(
    uint16_t  *scan_code,     ///< (out) scan code (0 for printable)
    uint16_t  *unicode_char,  ///< (out) unicode character (0 for special)
    uint32_t  *modifiers      ///< (out) AXL_INPUT_MOD_* bits (0 if unavailable)
    );

/**
 * @brief Enable EFI_KEY_STATE_EXPOSED on ConsoleInHandle's SimpleTextInputEx
 *        so the firmware delivers modifier-only "partial" keystrokes (shift/
 *        ctrl/alt down+up), keeping live modifier state current between
 *        character keys. Best-effort + idempotent; a no-op when there is no
 *        ex protocol (serial console). Resets the lock-toggle LEDs, so call
 *        it only when keyboard input is actually consumed.
 */
void
axl_backend_console_expose_modifiers(
    void
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
 * Exit status is resolved by axl_backend_resolve_exit_status(rc): a pending
 * axl_set_exit_status wins verbatim, else rc maps 0 -> EFI_SUCCESS / nonzero
 * -> a small POSIX-style code (1..255). The caller is expected to have already
 * run _axl_cleanup; this helper only bridges into the firmware exit service.
 */
__attribute__((noreturn))
void
axl_backend_boot_exit(
    int rc
    );

/**
 * @brief Stash a pending verbatim exit status for this image (set by the
 *     public axl_set_exit_status). Honored by both exit paths.
 *
 *     Also reflects into a live launcher's stdio-bridge cell when the
 *     calling image is a resident driver (no shell params of its own).
 */
void
axl_backend_set_exit_status(
    uint64_t status   ///< exact EFI_STATUS (UINTN-width) to exit with
    );

/**
 * @brief Resolve the EFI_STATUS this image should exit with for @p rc.
 *
 * Returns the pending status set by axl_backend_set_exit_status if one is
 * armed, else maps rc == 0 -> EFI_SUCCESS / nonzero -> a small POSIX-style exit
 * code (1..255, high bit clear) so the shell reports `%lasterror%=N` rather
 * than collapsing every failure to EFI_ABORTED (0x15).
 * Used by BOTH the CRT0 return path and axl_backend_boot_exit so the two
 * agree. Returned as a UINTN-width integer (the caller, which has UEFI types,
 * uses it as EFI_STATUS).
 */
uint64_t
axl_backend_resolve_exit_status(
    int rc
    );

/**
 * @brief Disarm any pending exit status (internal / test isolation).
 *
 * Not public API. The unit test calls it after exercising the setter so a
 * pending status can't leak into the test binary's own exit code.
 */
void
axl_backend_clear_exit_status(void);

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
 * @brief Dispatch a persistent worker procedure on an AP.
 *
 * @a proc is expected never to return; the AP runs it until the caller
 * stops it by other means. Dispatch is non-blocking where the firmware
 * allows it, falling back to a short blocking call on firmware that
 * refuses non-blocking mode (ArmPsciMpServicesDxe does so after
 * EFI_EVENT_GROUP_READY_TO_BOOT). Either way this returns promptly with
 * the worker left running.
 *
 * Success means the AP is running @a proc. A @a proc that returns
 * immediately is reported as an error, since it is not usable as a
 * persistent worker.
 *
 * The caller must stop every started worker before axl_backend_mp_cleanup,
 * which reclaims the per-AP firmware state the dispatch left behind.
 *
 * @return AXL_OK if the AP is running @a proc, AXL_ERR otherwise.
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
