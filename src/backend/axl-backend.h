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
 * @brief Write a UCS-2 string to standard error. NULL-safe.
 *
 *        On the console -- neither stdout nor stderr redirected -- the
 *        line goes through gST->ConOut, the stream stdout uses, so the
 *        two share one cursor as on a Unix terminal (gST->StdErr is a
 *        separate splitter whose cursor the Shell's prompt never sees),
 *        and also to every StdErr device that is not a ConOut device.
 *        Redirected (`2>`, `>`, `|`), or with the ConOut/StdErr pair no
 *        longer the one axl_backend_console_err_init() recorded, it goes to
 *        gST->StdErr, so `2>` captures it and `>` does not. ConOut when
 *        there is no StdErr.
 */
void
axl_backend_console_write_err(
    const unsigned short  *str  ///< UCS-2 string to output
    );

/**
 * @brief Record the console standard error belongs to: the
 *        gST->ConOut/StdErr pair now. axl_backend_console_write_err()
 *        writes through ConOut only while that pair is unchanged, so a
 *        resident image writing while another command runs under `>` does
 *        not write into that command's file. Called by axl_stream_init()
 *        at image start; the first write calls it when nothing has.
 */
void
axl_backend_console_err_init(
    void
    );

/**
 * @brief axl_backend_console_write_err() in a text attribute: each
 *        output it reaches is set to @a attr for the string and put back
 *        to its own attribute afterwards. NULL-safe.
 */
void
axl_backend_console_write_err_attr(
    const unsigned short  *str,  ///< UCS-2 string to output
    uint32_t               attr  ///< attribute bitmask for the string
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

/**
 * @brief The raw cycle counter: one RDTSC on x64, one CNTPCT_EL0 read on
 *     aarch64, and 0 on any other architecture.
 *
 * No conversion, no calibration, no firmware call: safe at any TPL,
 * including inside a TPL_NOTIFY notify. Compare two readings against a
 * tick count derived from axl_backend_counter_hz().
 */
uint64_t
axl_backend_counter_ticks(void);

/**
 * @brief Counter ticks per second for axl_backend_counter_ticks(), or 0
 *     when unknown.
 *
 * On x64 the first call in a process may calibrate (a ~10 ms gBS->Stall)
 * or read the calibration another image published, so call it at
 * TPL_APPLICATION, never from a notify.
 */
uint64_t
axl_backend_counter_hz(void);

/**
 * @brief The cycle-counter frequency a set of calibration samples gives.
 *
 * Each sample is the number of counter ticks across one `gBS->Stall`: the
 * @a short_ticks ones of @a short_us microseconds, the @a long_ticks ones
 * of @a long_us. The frequency is the least long sample minus the least
 * short one, over the difference of the two lengths, so a cost every Stall
 * carries whatever its length -- the call, the reference timer's reads --
 * cancels exactly instead of reading as extra ticks. The x86 calibration
 * in axl_backend_clock_gettime's first call is the one caller; this is its
 * arithmetic, apart from the firmware, so a test can hand it samples.
 *
 * The least long sample alone, over @a long_us, can only overstate the
 * frequency (the fixed cost and any overshoot both add ticks), while an
 * overshoot on the short Stalls makes the difference understate it --
 * without bound when every short sample is disturbed. The result is
 * therefore never above the long-alone value and never more than 2 % below
 * it: a disturbed short Stall leaves the clock at most 2 % fast. When no
 * short sample is non-zero the difference is taken against 0 (so the
 * long-alone value is returned); when the least long sample is not larger
 * than the least short one, the 2 % floor is.
 *
 * @return ticks per second; 0 only when no frequency follows at all:
 *     @a count is 0, @a long_ticks is NULL or holds no non-zero sample,
 *     @a long_us is not longer than @a short_us, or the least long sample
 *     comes to under one tick per second.
 */
uint64_t
axl_backend_counter_freq_from_samples(
    const uint64_t *short_ticks,  ///< ticks across each short Stall
    const uint64_t *long_ticks,   ///< ticks across each long Stall
    size_t          count,        ///< samples in each array
    uint64_t        short_us,     ///< length of each short Stall, microseconds
    uint64_t        long_us       ///< length of each long Stall, microseconds
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
 * @return AXL_OK on success; AXL_NOT_FOUND when the volume resolved and the
 *     file system reported no such file (EFI_NOT_FOUND from the open) -- an
 *     unmapped volume is NOT this; AXL_ERR on any other failure.
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
 * Asks the firmware for at most AXL_FS_READ_CHUNK (64 KB) per Shell
 * ReadFile / EFI_FILE_PROTOCOL.Read call, looping until @p size bytes are
 * read: on BMC virtual media larger single reads have been measured coming
 * back corrupt (src/fs/axl-fs-read-chunk.h).
 *
 * The loop also stops at a SHORT read, fewer bytes than that call asked
 * for: for a file that is end of file (a 0-byte read is its limit case); a
 * directory returns one entry per call and a pipe what it holds, so a
 * request of up to one chunk is exactly one call. A caller that needs the
 * whole file must compare *size with what it asked for.
 *
 * An error after some bytes were read is NOT reported by this call: it
 * returns AXL_OK with *size = the bytes read before the failing call, since
 * the file position has already passed them. The next call retries the
 * device from the failing chunk and reports the error if the fault recurs
 * (the POSIX read() contract): the error is not sticky, and media whose
 * fault was transient simply delivers the rest. Only a failure of the first call returns AXL_ERR, with *size 0.
 *
 * @return AXL_OK on success (*size = bytes read, possibly fewer than asked);
 *     AXL_ERR when nothing could be read.
 */
int
axl_backend_file_read(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to read / bytes read
    void          *buf      ///< destination buffer
    );

/**
 * @brief Read ONE directory entry (an EFI_FILE_INFO) in one firmware call.
 *
 * Unlike axl_backend_file_read this keeps the firmware's probe contract: an
 * entry larger than @p buf is not consumed, and its size is reported so the
 * caller can read it with a larger buffer.
 *
 * @return AXL_OK with *size the entry's size (0 at the end of the
 *     directory); AXL_NO_RESOURCES when @p buf is too small, *size then the
 *     size the entry needs and nothing consumed; AXL_ERR otherwise.
 */
int
axl_backend_dir_read_entry(
    AxlFileHandle  handle,  ///< directory handle
    size_t        *size,    ///< (in/out) buffer size / entry size
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
 * @brief Is an already-open handle a directory?
 *
 * @return AXL_OK with @p is_dir set, or AXL_ERR when the file info cannot
 *     be read.
 */
int
axl_backend_file_handle_is_dir(
    AxlFileHandle   handle,  ///< open file handle
    bool           *is_dir   ///< [out] true for a directory
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
 * @brief Get file metadata through an already-open handle.
 *
 * axl_backend_file_stat for a handle: it describes the file the handle is
 * open on wherever it now is, and can also report its CURRENT basename.
 *
 * @return AXL_OK on success, AXL_ERR on error (including a file deleted
 *     through another handle, which the FAT driver answers with a device
 *     error).
 */
int
axl_backend_file_handle_stat(
    AxlFileHandle   handle,      ///< open file handle
    uint64_t       *size,        ///< [out] file size (NULL OK)
    uint64_t       *alloc_size,  ///< [out] allocation size (NULL OK)
    uint64_t       *mtime_unix,  ///< [out] modification time, Unix seconds (NULL OK, 0 = unknown)
    bool           *is_dir,      ///< [out] directory flag (NULL OK)
    bool           *read_only,   ///< [out] read-only attribute (NULL OK)
    unsigned short *name,        ///< [out] current basename, UCS-2, NUL-terminated, cut at a character boundary to fit (NULL OK)
    size_t          name_cap,    ///< capacity of @p name in characters
    bool           *name_cut     ///< [out] true when @p name was cut to fit (NULL OK)
    );

/**
 * @brief Write a file's modification time through an open handle.
 *
 * Writes @p mtime_unix as UTC calendar fields with the time zone
 * unspecified, leaving every other field as it was. Unverified: the caller
 * re-reads the time to learn what the filesystem kept.
 *
 * @return AXL_OK if SetInfo accepted it, AXL_ERR otherwise.
 */
int
axl_backend_file_set_mtime(
    AxlFileHandle  handle,      ///< open file handle (write access)
    uint64_t       mtime_unix   ///< Unix seconds, UTC
    );

/**
 * @brief Days since 1970-01-01 to a Gregorian year, month and day.
 *
 * The inverse of the backend's days-from-civil conversion; shared so the
 * library holds one calendar conversion in each direction.
 */
void
axl_backend_civil_from_days(
    int64_t   z,  ///< days since the Unix epoch
    int      *y,  ///< [out] year
    unsigned *m,  ///< [out] month, 1..12
    unsigned *d   ///< [out] day, 1..31
    );

/**
 * Convert a 16-byte EFI_TIME value to Unix epoch seconds. The input
 * is read out of EFI_FILE_INFO.ModificationTime (or any of the other
 * EFI_TIME fields) by either the backend or the axl-fs.c dir-entry
 * parser. Returns 0 (caller's "unknown" sentinel) if the wire bytes
 * indicate an unset / zero date.
 *
 * Treats TimeZone as UTC when EFI_UNSPECIFIED_TIMEZONE (0x07FF) is
 * stored; otherwise ADDS TimeZone-minutes to convert to UTC: UEFI 2.11
 * section 8.3 defines Localtime = UTC - TimeZone. Daylight bit is ignored
 * (the timezone offset already accounts for it on platforms that bother
 * to set it). A pre-1970 time is 0.
 */
uint64_t
axl_backend_efi_time_to_unix(
    const void  *efi_time_16  ///< pointer to 16 bytes in EFI_TIME layout
    );

/**
 * The inverse of axl_backend_efi_time_to_unix: Unix seconds (UTC) as the
 * 16 bytes of an EFI_TIME -- UTC calendar fields, nanoseconds 0, TimeZone
 * unspecified (0x07FF), Daylight 0 -- which that function reads back
 * unchanged. Every file time AXL writes is written this way.
 */
void
axl_backend_unix_to_efi_time(
    uint64_t  secs,         ///< Unix seconds, UTC
    void     *efi_time_16   ///< [out] 16 bytes in EFI_TIME layout
    );

/// Capacity, in UCS-2 characters with the NUL, of the volume-relative path
/// axl_backend_path_locate writes.
#define AXL_BACKEND_VOL_PATH_MAX  512u

/**
 * @brief Resolve a path to the filesystem it lands on and its place there.
 *
 * Resolution is the backend's own, the same one axl_backend_file_open
 * uses: the current directory, either separator, map-name case and
 * aliases. Two paths are on the same volume exactly when their @p volume
 * handles are equal. Nothing is opened, so the path need not exist.
 *
 * @p rel receives the path below the volume root with `\` separators and
 * no leading or trailing one ("" is the root). `.` and `..` components are
 * passed through unresolved.
 *
 * @return AXL_OK; AXL_ERR when the volume does not resolve or @p rel does
 *     not fit in @p rel_cap characters.
 */
int
axl_backend_path_locate(
    const unsigned short  *path,     ///< UCS-2 path
    void                 **volume,   ///< [out] the volume's SimpleFileSystem EFI_HANDLE
    unsigned short        *rel,      ///< [out] volume-relative path
    size_t                 rel_cap   ///< capacity of @p rel in characters
    );

/**
 * @brief Delete the entry an open handle refers to, and close the handle.
 *
 * The handle is closed in every case and set to NULL.
 *
 * @return AXL_OK if the entry was deleted; AXL_ERR otherwise (including
 *     the firmware's "closed but not deleted" warning).
 */
int
axl_backend_file_handle_delete(
    AxlFileHandle  *handle   ///< open handle, write access; NULL on return
    );

/**
 * @brief Rename or move the entry an open handle refers to.
 *
 * One SetInfo carrying the entry's current metadata (times included) and
 * @p file_name: a bare name renames within the directory, a `\`-anchored
 * path moves within the volume. Unverified -- the caller re-reads the name.
 *
 * @return AXL_OK if SetInfo succeeded; AXL_UNSUPPORTED if the filesystem
 *     answered EFI_UNSUPPORTED (it cannot take @p file_name; nothing
 *     changed); AXL_ERR otherwise.
 */
int
axl_backend_file_handle_rename(
    AxlFileHandle          handle,     ///< open handle, write access
    const unsigned short  *file_name   ///< new name or `\`-anchored path
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
 * @brief axl_backend_shell_stdout(), and WHOSE handle it is: @p *owner is 0
 *     when it is this image's own (or NULL), else the token of the launcher
 *     dispatch that lends it through the stdio bridge. Every launcher's
 *     console handle is the same Shell file, so the handle alone cannot
 *     tell one launcher from the next; the pair can.
 */
AxlFileHandle
axl_backend_shell_stdout_owned(
    uint64_t  *owner   ///< [out] 0, or the lending dispatch's token
    );

/**
 * @brief axl_backend_shell_stderr() and whose handle it is, as
 *     axl_backend_shell_stdout_owned().
 */
AxlFileHandle
axl_backend_shell_stderr_owned(
    uint64_t  *owner   ///< [out] 0, or the lending dispatch's token
    );

/**
 * @brief Whether @p h IS the Shell's console file for stdout (@p err false)
 *        or stderr (@p err true): the handle EFI_SHELL_PROTOCOL's
 *        OpenFileByName returns for ">o" / ">e". That file prints what it
 *        is given as a NUL-terminated UCS-2 string, whatever the size, so
 *        raw bytes must not be written to it. False for NULL, for any other
 *        file (`NUL`, `>v name`, a real file, a pipe), and when the Shell
 *        cannot be asked.
 */
bool
axl_backend_shell_handle_is_console(
    AxlFileHandle  h,    ///< a Shell std handle, or NULL
    bool           err   ///< true for stderr (">e"), false for stdout (">o")
    );

/**
 * @brief Get the current working directory.
 *
 * @return UCS-2 path, valid until the next call (do not free), or NULL.
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
 * @brief Backend of axl_timer_resolution_begin(): arguments already checked
 *     (@p period_us in range, @p out_id non-NULL).
 *
 * @return AXL_OK, AXL_UNSUPPORTED or AXL_ERR, as the public call.
 */
int
axl_backend_timer_resolution_begin(
    uint32_t  period_us,       ///< wanted tick, microseconds
    uint32_t *out_id,          ///< [out] the request
    uint32_t *out_granted_us   ///< [out, optional] the tick afterwards
    );

/**
 * @brief Backend of axl_timer_resolution_end().
 *
 * @return AXL_OK, or AXL_INVALID if @p id is not this image's active request.
 */
int
axl_backend_timer_resolution_end(
    uint32_t id   ///< a request from axl_backend_timer_resolution_begin()
    );

/**
 * @brief The firmware's timer period in microseconds; 0 when unknown.
 *
 * @return the period, or 0.
 */
uint32_t
axl_backend_timer_period_us(void);

/**
 * @brief Execute a shell command string and report what the Shell said.
 *
 * Modern Shell only (`EFI_SHELL_PROTOCOL`). `*status` is preset to a
 * sentinel and @p written says whether `Execute` overwrote it -- EDK2's
 * nested mode always writes it (the nested Shell's exit code), a `-nonest`
 * Shell only when the command ran. @p execute_status is
 * `Execute`'s own return.
 *
 * @return AXL_OK when `Execute` was called (whatever it returned); AXL_ERR
 *     when there is no `EFI_SHELL_PROTOCOL` or an argument is NULL.
 */
int
axl_backend_shell_execute_status(
    const unsigned short  *command,         ///< UCS-2 command line
    uint64_t              *status,          ///< [out] the StatusCode Execute wrote (valid when *written)
    bool                  *written,         ///< [out] whether Execute wrote it
    uint64_t              *execute_status   ///< [out] Execute's own return
    );

/**
 * @brief Look up a Shell alias.
 *
 * @return the alias's UCS-2 value (Shell-owned), or NULL when there is no such
 *     alias or no `EFI_SHELL_PROTOCOL`.
 */
const unsigned short *
axl_backend_shell_get_alias(
    const unsigned short  *name   ///< UCS-2 alias name
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

/* Longest volume name a caller joins a path onto ("FS0", "fs12", a SetMap
   alias), NUL included -- for axl_backend_volume_name_for_handle, which fails
   rather than truncate, so an over-long alias degrades to the prefix-less path,
   not a wrong one. ONE size for every caller (axl_app_*, axl_driver_locate): a
   smaller one in one place lost candidates the other found. */
#define AXL_BACKEND_VOLUME_NAME_MAX  64u

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
 * @brief Total size of a device path in bytes, its END node included.
 *
 * Bounded walk (256 nodes); a node claiming Length < 4 makes it malformed.
 *
 * @return the size, or 0 for a malformed chain.
 */
size_t
axl_backend_dp_size(
    const void  *dp  ///< opaque EFI_DEVICE_PATH_PROTOCOL
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
 * @brief Does a shell map name resolve to this device path?
 *
 * Consults EFI_SHELL_PROTOCOL.GetDevicePathFromMap and compares the result
 * with @p device_path byte for byte. The shell stores a byte copy of the path
 * a name was set for, so a name set for @p device_path compares equal.
 * @p name must be ':'-terminated.
 *
 * On the old EFI 1.x shell the same comparison is made against
 * SHELL_ENVIRONMENT.GetMap's device path for @p name.
 *
 * @return true if @p name is mapped to @p device_path; false if it is not
 *     mapped, is mapped to another device, or there is no shell at all.
 */
bool
axl_backend_shell_map_is(
    const unsigned short  *name,        ///< ':'-terminated UCS-2 mapping name
    void                  *device_path  ///< opaque EFI_DEVICE_PATH_PROTOCOL
    );

/**
 * @brief Remove every shell map name for a device path.
 *
 * Takes each name from the shell's own list for @p device_path
 * (GetMapFromDevicePath) and removes it with SetMap(NULL, name), until the
 * device has none. Names are handled whole, in UCS-2, so any length or
 * character works, and a name that has moved to another device is never
 * touched.
 *
 * @return AXL_OK when no name maps to @p device_path any more, including with
 *     no shell at all (there are no names then); AXL_ERR when one may still:
 *     the shell refused, it is the old EFI 1.x shell (which cannot remove a
 *     name), the shell lacks GetMapFromDevicePath or SetMap, an allocation
 *     failed, or the device still had names after 32 were removed — or when
 *     @p device_path is NULL.
 */
int
axl_backend_shell_unmap_device(
    void    *device_path,  ///< opaque EFI_DEVICE_PATH_PROTOCOL
    size_t  *removed       ///< [out, optional] how many names were removed
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
 * @brief Copy a UCS-2 string whole, or not at all.
 *
 * @return true with @p src and its NUL copied into @p dst; false (and
 *     @p dst untouched) when it would not fit in @p cap characters.
 */
bool
axl_backend_wcs_copy(
    unsigned short        *dst,  ///< [out] destination
    size_t                 cap,  ///< capacity of @p dst in characters
    const unsigned short  *src   ///< NUL-terminated UCS-2 source
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
 * @brief Register a notify to run on every idle pass of the foreground's
 *     gBS->WaitForEvent.
 *
 * Joins EDK2's idle-loop event group (gIdleLoopEventGuid), which the DXE
 * core signals from gBS->WaitForEvent on every pass that finds none of
 * the waiter's events signalled; EDK2's CpuDxe member then halts the CPU
 * until the next interrupt. With @p before_halt the notify runs at
 * TPL_NOTIFY, at the pass itself, on EDK2 (see the implementation for the
 * ordering it relies on) -- and may then do no more than read
 * axl_backend_counter_ticks() and store. Without it, it runs at
 * TPL_CALLBACK after the halt, ahead of any timer notify the waking
 * interrupt queued. Either may run very often while the foreground waits.
 * It is an EDK2 mechanism, not a UEFI one: on firmware that does not
 * signal the group the notify never runs.
 *
 * Pair with `axl_backend_event_close`.
 *
 * @return AXL_OK on success, AXL_ERR on error or table-full.
 */
int
axl_backend_event_create_idle_notify(
    void  (*notify)(void *ctx),  ///< notify function
    void   *ctx,                 ///< opaque context passed to @p notify
    bool    before_halt,         ///< TPL_NOTIFY, before the halt (else TPL_CALLBACK)
    AxlEventHandle *event        ///< (out) receives event handle
    );

/**
 * @brief Register a notify to run whenever a protocol is installed.
 *
 * Creates an `EVT_NOTIFY_SIGNAL` event at `TPL_CALLBACK` and hands it
 * to `RegisterProtocolNotify`, so the firmware invokes @p notify(@p ctx)
 * each time a new instance of @p guid appears. This is the LOOPLESS
 * counterpart to `axl_loop_add_protocol_notify`, which creates a bare
 * waitable event and relies on an `AxlLoop` to poll it — a resident DXE
 * driver that returns from its entry point pumps no loop and cannot use
 * that one.
 *
 * The notify runs at `TPL_CALLBACK`, so it MAY allocate (pool allocation
 * is legal at or below `TPL_NOTIFY`) and must stay short, for the same
 * reason `axl_backend_event_create_notify_timer` documents.
 *
 * Pair every successful call with `axl_backend_event_close`.
 *
 * @return AXL_OK on success, AXL_ERR on error or table-full.
 */
int
axl_backend_event_create_notify_protocol(
    const void *guid,               ///< protocol GUID to watch
    void      (*notify)(void *ctx), ///< notify function (TPL_CALLBACK)
    void       *ctx,                ///< opaque context passed to @p notify
    AxlEventHandle *event           ///< (out) receives event handle
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

/**
 * @brief gBS->StartImage on @p handle, counted as a child of this image.
 *
 * While it runs, the child -- not this image -- is the firmware's current
 * image, so a gBS->Exit from this image (one of its event callbacks calling
 * axl_exit) FAILS. _axl_cleanup_note_exit reads the count to know that such
 * an exit will not unload this image and must not tear it down. Every place
 * libaxl starts an image goes through here, and every call that can make the
 * FIRMWARE start one on this image's behalf -- ConnectController, RamDisk
 * Register, the Shell's Execute -- is bracketed with
 * axl_backend_child_image_enter/leave. (The pack stub calls StartImage
 * itself, and is exempt: it is raw UEFI and links no AXL runtime.) A
 * consumer calling gBS->StartImage itself under AXL_ALLOW_UEFI is not seen.
 *
 * @return the EFI_STATUS StartImage returned.
 */
uint64_t
axl_backend_start_image(
    void *handle   ///< the loaded image to start
    );

/**
 * @brief Bracket a call that runs images of its own -- the Shell's Execute --
 *        as one child (see axl_backend_start_image).
 */
void axl_backend_child_image_enter(void);
void axl_backend_child_image_leave(void);

/** @brief True while an image this one started (or a Shell command it ran)
 *         is running. */
bool axl_backend_child_images_running(void);

/**
 * @brief Defer an exit that cannot happen now, by unwinding the callback.
 *
 * While a child image runs, the CHILD is the firmware's current image, so a
 * gBS->Exit from one of this image's notify callbacks fails -- and an exit
 * must not return, so axl_exit used to spin there at callback TPL, which
 * starves the child: a deadlock. Instead, when a child is running and the
 * call is inside an AXL notify callback, this records @p rc and unwinds to
 * that callback's trampoline (axl_backend_notify_unwind): the callback is
 * abandoned, the child runs on, and when the last child returns
 * (axl_backend_exit_if_deferred, after the starting function's cleanup)
 * @p exit_fn(rc) runs from the caller's own context, where the exit succeeds.
 *
 * @return false when the exit can happen now (no child, or not inside a
 *     notify callback); it does not return otherwise.
 */
bool axl_backend_exit_defer(int rc, void (*exit_fn)(int rc));

/**
 * @brief Perform an exit axl_backend_exit_defer recorded, once no child
 *        remains. Does not return when it does.
 *
 * Called by every libaxl function that started a child, AFTER it has
 * released what it owns that teardown cannot reclaim (firmware pool, a
 * child's load options, the AxlImage) -- never from inside the counting
 * itself, which would skip that. Composite functions call it last; the
 * pieces they are built from use non-checking internals.
 */
void axl_backend_exit_if_deferred(void);

/** @brief Notify nesting depth: AXL notify callbacks currently active. */
unsigned axl_backend_notify_depth(void);

/** @brief True when the innermost AXL notify callback was entered at a depth
 *         above @p newer_than and is running at the TPL it was called at --
 *         the only callback a deferred exit may unwind. */
bool axl_backend_notify_unwind_ok(unsigned newer_than);

/** @brief Abandon the current notify callback and return from its
 *         trampoline to the firmware. Returns only outside one. */
void axl_backend_notify_unwind(void);

/** @brief Have @p state saved on every notify callback's entry and put back
 *         when that callback is abandoned (one counter; AxlLoop's callback
 *         depth). */
void axl_backend_notify_track_state(unsigned *state);

// ===================================================================
// MP Services (used by axl-task-pool.c)
// ===================================================================

/// Opaque MP context (backend manages protocol + AP enumeration)
typedef struct AxlMpContext AxlMpContext;

/// AP worker procedure (EFIAPI calling convention required by firmware)
typedef void (EFIAPI *AxlApProc)(void *arg);

/**
 * How long the BSP waits on an AP to do anything -- answer a probe,
 * acknowledge quit, or be retired by the firmware -- in wall-clock
 * microseconds. One value for every such wait, because they all wait on
 * the same thing: a processor that, on a virtual machine, makes no
 * progress at all while its host thread is descheduled. See
 * axl-task-pool.c's tunables for the measurements behind it.
 */
#define AXL_AP_ANSWER_TIMEOUT_US  10000000u

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
 * @brief Give up on AP @a ap_index's dispatch: the caller has no
 *        confirmation that its procedure returned, so cleanup must not
 *        wait for the firmware to retire it.
 *
 * axl_backend_mp_cleanup then checks that AP's completion event once and
 * leaks it if unsignalled, rather than spending AXL_AP_ANSWER_TIMEOUT_US
 * on an AP that has already failed to answer for that long. Out-of-range
 * indices and a NULL context are ignored.
 */
void
axl_backend_mp_abandon_ap(
    AxlMpContext  *ctx,       ///< MP context
    size_t         ap_index   ///< AP index (0..worker_count-1)
    );

/**
 * @brief Free MP context. NULL-safe.
 *
 * Waits up to AXL_AP_ANSWER_TIMEOUT_US for the firmware to retire each
 * dispatched AP that was not abandoned (axl_backend_mp_abandon_ap), so
 * its completion event can be closed rather than leaked.
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
