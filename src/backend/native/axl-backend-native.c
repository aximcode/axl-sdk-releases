/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native.c
    AxlBackend implementation.

    Uses AXL's own UEFI type headers (include/uefi/).
    Global firmware table pointers (gST, gBS, gRT) are initialized
    by the CRT0 (axl-crt0-native.c) before any backend function is
    called.
**/

#include "axl-backend.h"
#include <axl/axl-input.h>   /* AXL_INPUT_MOD_* — read_key_ex normalizes to these */
#include <axl/axl-log.h>
#include <stdarg.h>

AXL_LOG_DOMAIN("backend");

// ===================================================================
// Global firmware table pointers (set by CRT0)
// ===================================================================

EFI_SYSTEM_TABLE     *gST = NULL;
EFI_BOOT_SERVICES    *gBS = NULL;
EFI_RUNTIME_SERVICES *gRT = NULL;
EFI_HANDLE            gImageHandle = NULL;

// ===================================================================
// Shell protocol (cached on first file operation)
// ===================================================================

static EFI_SHELL_PROTOCOL  *mShell = NULL;
static bool                 mShellLocated = false;

static EFI_SHELL_PROTOCOL *
get_shell(void)
{
    if (!mShellLocated) {
        mShellLocated = true;
        EFI_GUID guid = gEfiShellProtocolGuid;
        gBS->LocateProtocol(&guid, NULL, (VOID **)&mShell);
    }
    return mShell;
}

// ===================================================================
// Memory
// ===================================================================

/**
 * @brief Allocate memory from the firmware allocator.
 *
 * @return pointer to allocated memory, or NULL on failure.
 */
void *
axl_backend_alloc(
    size_t  size  ///< bytes to allocate
    )
{
    VOID       *ptr = NULL;
    EFI_STATUS  status;

    status = gBS->AllocatePool(EfiBootServicesData, (UINTN)size, &ptr);
    return EFI_ERROR(status) ? NULL : ptr;
}

/**
 * @brief Allocate zeroed memory from the firmware allocator.
 *
 * @return pointer to zero-filled memory, or NULL on failure.
 */
void *
axl_backend_alloc_zero(
    size_t  size  ///< bytes to allocate
    )
{
    void *ptr = axl_backend_alloc(size);
    if (ptr != NULL) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < size; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

/**
 * @brief Free firmware-allocated memory. NULL-safe.
 */
void
axl_backend_free(
    void  *ptr  ///< pointer from axl_backend_alloc, or NULL
    )
{
    if (ptr != NULL) {
        gBS->FreePool(ptr);
    }
}

// ===================================================================
// Console
// ===================================================================

/**
 * @brief Write a UCS-2 string to the console. NULL-safe.
 */
void
axl_backend_console_write(
    const unsigned short  *str  ///< UCS-2 string to output
    )
{
    if (gST != NULL && gST->ConOut != NULL && str != NULL) {
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)str);
    }
}

/**
 * @brief Set console text attribute (color/style).
 */
void
axl_backend_console_set_attr(
    uint32_t  attr  ///< attribute bitmask
    )
{
    if (gST != NULL && gST->ConOut != NULL) {
        gST->ConOut->SetAttribute(gST->ConOut, attr);
    }
}

/**
 * @brief Get current console text attribute.
 *
 * @return current attribute bitmask, or 0 if unavailable.
 */
uint32_t
axl_backend_console_get_attr(
    void
    )
{
    if (gST != NULL && gST->ConOut != NULL) {
        return (uint32_t)gST->ConOut->Mode->Attribute;
    }
    return 0;
}

// ===================================================================
// Time
// ===================================================================

/**
 * @brief Get the current date/time from firmware.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_get_time(
    AxlTime  *time  ///< (out) receives current time
    )
{
    EFI_TIME    efi_time;
    EFI_STATUS  status;

    if (time == NULL) {
        return AXL_ERR;
    }

    status = gRT->GetTime(&efi_time, NULL);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    time->year       = efi_time.Year;
    time->month      = efi_time.Month;
    time->day        = efi_time.Day;
    time->hour       = efi_time.Hour;
    time->minute     = efi_time.Minute;
    time->second     = efi_time.Second;
    time->nanosecond = efi_time.Nanosecond;
    time->daylight   = efi_time.Daylight;
    /* UEFI's EFI_UNSPECIFIED_TIMEZONE (2047) becomes our INT16_MIN
       sentinel so consumers can branch cleanly. */
    time->timezone_minutes = (efi_time.TimeZone == 2047)
        ? INT16_MIN
        : (int16_t)efi_time.TimeZone;
    return AXL_OK;
}

/* High-resolution monotonic microseconds. The wallclock from
   gRT->GetTime is only second-resolution on most firmware (OVMF and
   most BMC firmware leave EFI_TIME.Nanosecond=0). Use the architecture's
   cycle counter for sub-second precision. */

#if defined(__x86_64__)
static inline uint64_t
read_cycle_counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
static inline uint64_t
read_cycle_counter(void)
{
    uint64_t v;
    /* CNTPCT_EL0: physical counter. Always accessible at EL0/EL1. */
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
static inline uint64_t
read_counter_freq(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
#endif

/* High-resolution monotonic clock — POSIX-style clock_gettime
   shape, backed by the architecture's cycle counter.

   Counter frequency:

   - aarch64: read CNTFRQ_EL0 (architectural register). Every UEFI
     process reads the same value; no calibration needed.
   - x86_64: calibrate against a 10 ms gBS->Stall on first call, then
     publish the result on a fresh UEFI handle carrying our private
     AXL_TSC_FREQ_PROTOCOL_GUID. Subsequent processes within the same
     boot LocateProtocol → read the cached freq, no re-calibration.
     The interface struct is allocated from EfiBootServicesData pool
     (lives until ExitBootServices), so it survives the publishing
     image being unloaded. Process-local memo avoids repeated
     LocateProtocol on hot paths.

   Frequency = 0 means "no usable counter" — clock_gettime returns
   AXL_ERR for AXL_CLOCK_MONOTONIC. (Earlier "below 1 MHz" giveup is
   gone: the new ns-precision shape can report whatever resolution
   the counter has via axl_clock_getres.)

   UEFI is single-threaded at TPL_APPLICATION so no atomics around
   the static memo. RDTSC isn't serializing — the few-cycle OoO
   window is invisible at nanosecond+ resolution. The x86 path
   assumes invariant TSC (Nehalem+ / all server CPUs); pre-2008
   client CPUs that scaled TSC with DVFS would drift, not targeted. */

#if defined(__x86_64__)
/* {f7a3c5e1-2b91-4d8c-9e57-3a6f8b1c2d4e} — private to axl-sdk for
   per-boot TSC-frequency caching. Layout: a single uint64_t holding
   the calibrated ticks-per-second. */
static const EFI_GUID AXL_TSC_FREQ_PROTOCOL_GUID = {
    0xf7a3c5e1, 0x2b91, 0x4d8c,
    {0x9e, 0x57, 0x3a, 0x6f, 0x8b, 0x1c, 0x2d, 0x4e}
};

typedef struct {
    uint64_t freq_hz;
} AxlTscFreqInterface;
#endif

static uint64_t
get_counter_freq_hz(void)
{
#if defined(__aarch64__)
    return read_counter_freq();
#elif defined(__x86_64__)
    static uint64_t memo = 0;
    if (memo != 0) {
        return memo;
    }

    /* Already published by an earlier process? Sanity-bound the
       cached value before trusting it — a buggy or hostile prior
       publisher could land any uint64_t here, and the gettime
       arithmetic `(ticks % freq) * 1e9` would overflow if freq is
       extreme. Bounds chosen to cover every real CPU AXL targets:
       1 MHz floor rejects pathologically-slow counters; 100 GHz
       ceiling is well above any current or near-future TSC. */
    AxlTscFreqInterface *iface = NULL;
    EFI_STATUS status = gBS->LocateProtocol(
        (EFI_GUID *)&AXL_TSC_FREQ_PROTOCOL_GUID,
        NULL,
        (void **)&iface);
    if (!EFI_ERROR(status) && iface != NULL
        && iface->freq_hz >= 1000000ull
        && iface->freq_hz <= 100000000000ull) {
        memo = iface->freq_hz;
        return memo;
    }

    /* First publisher: calibrate via 10ms Stall. */
    uint64_t before = read_cycle_counter();
    gBS->Stall(10000);
    uint64_t after  = read_cycle_counter();
    uint64_t diff   = after - before;
    if (diff == 0) {
        return 0;   /* no usable counter */
    }
    uint64_t freq = diff * 100;   /* 10ms → *100 = ticks/sec */

    /* Publish for future processes. Allocate from boot-services
       pool so the interface survives image unload (the AXL leak
       tracker would free axl_malloc'd memory at image exit). A
       failure here just means subsequent processes re-calibrate —
       not fatal. */
    void *pool = NULL;
    if (!EFI_ERROR(gBS->AllocatePool(EfiBootServicesData,
                                     sizeof(AxlTscFreqInterface), &pool))
        && pool != NULL) {
        ((AxlTscFreqInterface *)pool)->freq_hz = freq;
        EFI_HANDLE pub_handle = NULL;
        gBS->InstallProtocolInterface(
            &pub_handle,
            (EFI_GUID *)&AXL_TSC_FREQ_PROTOCOL_GUID,
            EFI_NATIVE_INTERFACE,
            pool);
        /* If InstallProtocolInterface fails the pool allocation
           leaks for the rest of the boot — acceptable, this is the
           calibration path not a hot path. */
    }

    memo = freq;
    return memo;
#else
    return 0;
#endif
}

/* days_from_civil is defined further down (used by both
   axl_backend_clock_gettime and axl_backend_efi_time_to_unix). */
static int64_t
days_from_civil(uint32_t y, uint32_t m, uint32_t d);

/* Single source of truth for converting a Gregorian civil date +
   time-of-day + signed timezone offset to Unix seconds. Returns a
   signed result so consumers reading pre-epoch values (rare; only
   meaningful for clock_gettime(REALTIME) when the RTC is set to a
   pre-1970 date) preserve them. File-timestamp consumers clamp at
   the call site.

   The @p tz_minutes argument uses the AXL convention: real signed
   minutes east of UTC, or INT16_MIN to mean "unspecified — treat
   as already-UTC." EFI's 0x07FF sentinel must be translated to
   INT16_MIN by the caller (axl_backend_get_time does this for
   AxlTime; axl_backend_efi_time_to_unix does it inline). */
static int64_t
civil_to_unix_seconds(uint32_t year,
                      uint32_t month,
                      uint32_t day,
                      uint32_t hour,
                      uint32_t minute,
                      uint32_t second,
                      int16_t  tz_minutes)
{
    int64_t days = days_from_civil(year, month, day);
    int64_t secs = days * 86400
                 + (int64_t)hour   * 3600
                 + (int64_t)minute * 60
                 + (int64_t)second;
    if (tz_minutes != INT16_MIN) {
        secs -= (int64_t)tz_minutes * 60;
    }
    return secs;
}

int
axl_backend_clock_gettime(AxlClockId clockid, AxlTimespec *out)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    out->tv_sec  = 0;
    out->tv_nsec = 0;

    if (clockid == AXL_CLOCK_MONOTONIC) {
#if defined(__x86_64__) || defined(__aarch64__)
        uint64_t freq = get_counter_freq_hz();
        if (freq == 0) {
            return AXL_ERR;
        }
        uint64_t ticks = read_cycle_counter();
        out->tv_sec  = (int64_t)(ticks / freq);
        /* (ticks % freq) < freq ≤ ~2^32 in practice, so the multiply
           by 1e9 fits in u64 without overflow. */
        out->tv_nsec = (int32_t)(((ticks % freq) * 1000000000ull) / freq);
        return AXL_OK;
#else
        return AXL_ERR;
#endif
    }

    if (clockid == AXL_CLOCK_REALTIME) {
        AxlTime t;
        if (axl_backend_get_time(&t) != AXL_OK) {
            return AXL_ERR;
        }
        out->tv_sec  = civil_to_unix_seconds(
            t.year, t.month, t.day,
            t.hour, t.minute, t.second,
            t.timezone_minutes);
        out->tv_nsec = (int32_t)t.nanosecond;
        return AXL_OK;
    }

    return AXL_ERR;
}

uint64_t
axl_backend_get_monotonic_us(void)
{
    /* Thin wrapper over axl_backend_clock_gettime for backwards
       compatibility with backend consumers that haven't been
       migrated. */
    AxlTimespec ts;
    if (axl_backend_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) != AXL_OK) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

// ===================================================================
// Low-level platform I/O (for AxlIpmi, future AxlPci/AxlSpd)
// ===================================================================

int
axl_backend_io_read8(uint16_t port, uint8_t *value)
{
    if (value == NULL) {
        return AXL_ERR;
    }
#if defined(__x86_64__) || defined(__i386__)
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    *value = v;
    return AXL_OK;
#else
    (void)port;
    return AXL_ERR;
#endif
}

int
axl_backend_io_write8(uint16_t port, uint8_t value)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
    return AXL_OK;
#else
    (void)port;
    (void)value;
    return AXL_ERR;
#endif
}

// ===================================================================
// File I/O (via EFI_SHELL_PROTOCOL)
// ===================================================================

/**
 * @brief Open a file by UCS-2 path.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_open(
    const unsigned short  *path,        ///< UCS-2 file path
    uint64_t               mode,        ///< EFI file mode flags
    uint64_t               attributes,  ///< EFI file attributes
    AxlFileHandle         *handle       ///< (out) receives file handle
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_STATUS           status;

    (void)attributes;

    if (path == NULL || handle == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        return AXL_ERR;
    }

    status = shell->OpenFileByName((CHAR16 *)path, &fh, mode);
    if (EFI_ERROR(status)) {
        /* EFI_NOT_FOUND is the normal "file doesn't exist" case and
         * floods the log when callers probe many candidate paths
         * (e.g., axl_driver_locate walking every mounted volume).
         * Real errors — media, permissions — still surface. */
        if (status != EFI_NOT_FOUND) {
            axl_debug("file open failed: status=0x%llx",
                      (unsigned long long)status);
        }
        return AXL_ERR;
    }
    *handle = (AxlFileHandle)fh;
    return AXL_OK;
}

/**
 * @brief Close a file handle. NULL-safe.
 *
 * @return 0 on success.
 */
int
axl_backend_file_close(
    AxlFileHandle  *handle  ///< file handle (set to NULL on close)
    )
{
    EFI_SHELL_PROTOCOL  *shell;

    if (handle == NULL || *handle == NULL) {
        return AXL_OK;
    }

    shell = get_shell();
    if (shell != NULL) {
        shell->CloseFile((SHELL_FILE_HANDLE)*handle);
    }
    *handle = NULL;
    return AXL_OK;
}

/**
 * @brief Read from a file.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_read(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to read / bytes read
    void          *buf      ///< destination buffer
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;
    UINTN                usize;

    if (size == NULL || buf == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        return AXL_ERR;
    }

    usize = *size;
    status = shell->ReadFile((SHELL_FILE_HANDLE)handle, &usize, buf);
    *size = usize;
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/**
 * @brief Write to a file.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_write(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to write / bytes written
    const void    *buf      ///< source buffer
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;
    UINTN                usize;

    if (size == NULL || buf == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        return AXL_ERR;
    }

    usize = *size;
    status = shell->WriteFile((SHELL_FILE_HANDLE)handle, &usize,
                               (VOID *)buf);
    *size = usize;
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/**
 * @brief Get current file position.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_get_position(
    AxlFileHandle  handle,  ///< file handle
    uint64_t      *pos      ///< (out) receives position
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;
    UINT64               efi_pos;

    if (pos == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        return AXL_ERR;
    }

    status = shell->GetFilePosition((SHELL_FILE_HANDLE)handle, &efi_pos);
    if (!EFI_ERROR(status)) {
        *pos = efi_pos;
    }
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/**
 * @brief Set file position.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_set_position(
    AxlFileHandle  handle,  ///< file handle
    uint64_t       pos      ///< byte offset from start
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        return AXL_ERR;
    }

    status = shell->SetFilePosition((SHELL_FILE_HANDLE)handle, pos);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/**
 * @brief Delete a file by UCS-2 path.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_delete(
    const unsigned short  *path  ///< UCS-2 file path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    if (path == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        return AXL_ERR;
    }

    status = shell->DeleteFileByName((CHAR16 *)path);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/**
 * @brief Get file size.
 *
 * @return file size in bytes, or -1 on error.
 */
int64_t
axl_backend_file_get_size(
    AxlFileHandle  handle  ///< file handle
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    UINT64               size;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    status = shell->GetFileSize((SHELL_FILE_HANDLE)handle, &size);
    return EFI_ERROR(status) ? -1 : (int64_t)size;
}

/**
 * @brief Check if a path is a directory.
 *
 * @return true if path is a directory.
 */
bool
axl_backend_file_is_dir(
    const unsigned short  *path  ///< UCS-2 path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;
    bool                 is_dir = false;

    shell = get_shell();
    if (shell == NULL) {
        return false;
    }

    status = shell->OpenFileByName((CHAR16 *)path, &fh,
                                    AXL_FILE_MODE_READ);
    if (EFI_ERROR(status)) {
        return false;
    }

    info = (EFI_FILE_INFO *)shell->GetFileInfo(fh);
    if (info != NULL) {
        is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        axl_backend_free(info);
    }
    shell->CloseFile(fh);
    return is_dir;
}

int
axl_backend_file_rename(
    const unsigned short  *old_path,
    const unsigned short  *new_path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;
    size_t               new_len;
    size_t               info_size;
    EFI_FILE_INFO       *new_info;
    size_t               i;

    shell = get_shell();
    if (shell == NULL || old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }

    /* Open the existing file */
    status = shell->OpenFileByName((CHAR16 *)old_path, &fh,
                                    AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    /* Get current file info */
    info = (EFI_FILE_INFO *)shell->GetFileInfo(fh);
    if (info == NULL) {
        shell->CloseFile(fh);
        return AXL_ERR;
    }

    /* Build new info with the new filename */
    for (new_len = 0; new_path[new_len] != 0; new_len++) {}
    info_size = sizeof(EFI_FILE_INFO) + (new_len + 1) * sizeof(CHAR16);

    new_info = (EFI_FILE_INFO *)axl_backend_alloc(info_size);
    if (new_info == NULL) {
        axl_backend_free(info);
        shell->CloseFile(fh);
        return AXL_ERR;
    }

    /* Copy metadata, replace filename */
    *new_info = *info;
    new_info->Size = info_size;
    for (i = 0; i <= new_len; i++) {
        new_info->FileName[i] = (CHAR16)new_path[i];
    }

    status = shell->SetFileInfo(fh, (CONST EFI_FILE_INFO *)new_info);
    axl_backend_free(new_info);
    axl_backend_free(info);
    shell->CloseFile(fh);

    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_backend_file_mkdir(
    const unsigned short  *path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || path == NULL) {
        return AXL_ERR;
    }

    status = shell->CreateFile((CHAR16 *)path, EFI_FILE_DIRECTORY, &fh);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    shell->CloseFile(fh);
    return AXL_OK;
}

int
axl_backend_file_rmdir(
    const unsigned short  *path
    )
{
    /* DeleteFileByName works for both files and empty directories */
    return axl_backend_file_delete(path);
}

/* Days-from-civil per Howard Hinnant — works for any year in the
   proleptic Gregorian calendar; we only need the post-1970 range
   that fits in a uint64_t Unix epoch. */
static int64_t
days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
    int64_t yy   = (int64_t)y - (m <= 2);
    int64_t era  = (yy >= 0 ? yy : yy - 399) / 400;
    uint32_t yoe = (uint32_t)(yy - era * 400);
    uint32_t mp  = (m > 2) ? (m - 3) : (m + 9);
    uint32_t doy = (153 * mp + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t
axl_backend_efi_time_to_unix(const void *efi_time_16)
{
    /* EFI_TIME layout (16 bytes, all little-endian):
         u16 Year, u8 Month, u8 Day, u8 Hour, u8 Minute, u8 Second,
         u8 Pad1, u32 Nanosecond, i16 TimeZone, u8 Daylight, u8 Pad2.
       Match by byte offset rather than casting to EFI_TIME directly
       so axl-fs.c can call this against the slice it pulls out of
       the EFI_FILE_INFO buffer without dragging in UEFI headers. */
    if (efi_time_16 == NULL) {
        return 0;
    }
    const uint8_t *b = (const uint8_t *)efi_time_16;
    uint16_t year   = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    uint8_t  month  = b[2];
    uint8_t  day    = b[3];
    uint8_t  hour   = b[4];
    uint8_t  minute = b[5];
    uint8_t  second = b[6];
    /* TimeZone at offset 12 (int16_t, little-endian). */
    int16_t  tz_minutes = (int16_t)(b[12] | ((uint16_t)b[13] << 8));

    if (year == 0 || month == 0 || day == 0) {
        return 0;
    }

    /* Normalize EFI's 0x07FF "unspecified timezone" sentinel to the
       AXL-side INT16_MIN convention before handing off to the
       shared converter. */
    int16_t axl_tz = (tz_minutes == 0x07FF)
                     ? INT16_MIN
                     : tz_minutes;
    int64_t secs = civil_to_unix_seconds(
        year, month, day, hour, minute, second, axl_tz);
    /* File-timestamp use case has no representation for pre-epoch
       values — clamp negative results to 0 (matches the previous
       behavior of axl_backend_efi_time_to_unix). */
    return secs < 0 ? 0 : (uint64_t)secs;
}

int
axl_backend_file_stat(
    const unsigned short  *path,
    uint64_t              *size,
    uint64_t              *alloc_size,
    uint64_t              *mtime_unix,
    bool                  *is_dir,
    bool                  *read_only
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || path == NULL) {
        return AXL_ERR;
    }

    status = shell->OpenFileByName((CHAR16 *)path, &fh,
                                    AXL_FILE_MODE_READ);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    info = (EFI_FILE_INFO *)shell->GetFileInfo(fh);
    if (info == NULL) {
        shell->CloseFile(fh);
        return AXL_ERR;
    }

    if (size != NULL) {
        *size = info->FileSize;
    }
    if (alloc_size != NULL) {
        *alloc_size = info->PhysicalSize;
    }
    if (mtime_unix != NULL) {
        *mtime_unix = axl_backend_efi_time_to_unix(&info->ModificationTime);
    }
    if (is_dir != NULL) {
        *is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
    }
    if (read_only != NULL) {
        *read_only = (info->Attribute & EFI_FILE_READ_ONLY) != 0;
    }

    axl_backend_free(info);
    shell->CloseFile(fh);
    return AXL_OK;
}

// ===================================================================
// Shell environment, working directory, and command execution
// ===================================================================

const unsigned short *
axl_backend_shell_getenv(
    const unsigned short  *name
    )
{
    EFI_SHELL_PROTOCOL  *shell;

    shell = get_shell();
    if (shell == NULL) {
        return NULL;
    }
    return (const unsigned short *)shell->GetEnv((CONST CHAR16 *)name);
}

int
axl_backend_shell_setenv(
    const unsigned short  *name,
    const unsigned short  *value,
    bool                   volatile_var
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || name == NULL) {
        return AXL_ERR;
    }

    status = shell->SetEnv((CONST CHAR16 *)name, (CONST CHAR16 *)value,
                            volatile_var ? TRUE : FALSE);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/**
 * @brief Get the SHELL_FILE_HANDLE for the running image's stdin.
 *
 * Looks up EFI_SHELL_PARAMETERS_PROTOCOL on the image handle. The
 * StdIn field there is what the shell wired up — for `tool1 | tool2`
 * this is the captured LHS output stream. Cached after the first
 * probe so repeat callers don't pay HandleProtocol overhead.
 *
 * @return SHELL_FILE_HANDLE cast as AxlFileHandle, or NULL when the
 *     shell-params protocol isn't published on this image (cross-
 *     volume launches, BDS contexts, non-Shell-2.0 launches).
 */
static SHELL_FILE_HANDLE  mShellStdIn        = NULL;
static SHELL_FILE_HANDLE  mShellStdOut       = NULL;
static bool               mShellStdProbed    = false;

/* Shared probe — looks up EFI_SHELL_PARAMETERS_PROTOCOL once and
   caches both StdIn and StdOut handles. Both helpers below trigger
   the same probe on first call. */
static void
probe_shell_std_handles(void)
{
    if (mShellStdProbed) {
        return;
    }
    mShellStdProbed = true;
    EFI_SHELL_PARAMETERS_PROTOCOL *sp = NULL;
    EFI_GUID guid = gEfiShellParametersProtocolGuid;
    EFI_STATUS status = gBS->HandleProtocol(
        (EFI_HANDLE)gImageHandle, &guid, (VOID **)&sp);
    if (!EFI_ERROR(status) && sp != NULL) {
        mShellStdIn  = sp->StdIn;
        mShellStdOut = sp->StdOut;
    }
}

AxlFileHandle
axl_backend_shell_stdin(void)
{
    probe_shell_std_handles();
    return (AxlFileHandle)mShellStdIn;
}

AxlFileHandle
axl_backend_shell_stdout(void)
{
    probe_shell_std_handles();
    return (AxlFileHandle)mShellStdOut;
}

const unsigned short *
axl_backend_shell_getcwd(
    void
    )
{
    EFI_SHELL_PROTOCOL  *shell;

    shell = get_shell();
    if (shell == NULL) {
        return NULL;
    }
    return (const unsigned short *)shell->GetCurDir(NULL);
}

int
axl_backend_shell_chdir(
    const unsigned short  *path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || path == NULL) {
        return AXL_ERR;
    }

    status = shell->SetCurDir(NULL, (CONST CHAR16 *)path);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_backend_shell_execute(
    const unsigned short  *command
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || command == NULL) {
        return AXL_ERR;
    }

    status = shell->Execute(NULL, (CHAR16 *)command, NULL, NULL);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

// ===================================================================
// Wide-string operations (self-implemented — no external library)
// ===================================================================

/**
 * @brief Get UCS-2 string length.
 *
 * @return number of characters (not including NUL).
 */
size_t
axl_backend_wcslen(
    const unsigned short  *s  ///< UCS-2 string
    )
{
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

/**
 * @brief Compare two UCS-2 strings.
 *
 * @return <0, 0, or >0.
 */
int
axl_backend_wcscmp(
    const unsigned short  *a,  ///< first string
    const unsigned short  *b   ///< second string
    )
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

/**
 * @brief Copy UCS-2 string with size limit.
 */
void
axl_backend_wcscpy(
    unsigned short        *dst,        ///< destination buffer
    const unsigned short  *src,        ///< source string
    size_t                 dst_count   ///< destination buffer size in characters
    )
{
    size_t i;
    if (dst_count == 0) {
        return;
    }
    for (i = 0; i < dst_count - 1 && src[i] != 0; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

/**
 * @brief Convert UCS-2 character to uppercase.
 *
 * @return uppercase character.
 */
unsigned short
axl_backend_towupper(
    unsigned short  c  ///< character to convert
    )
{
    if (c >= L'a' && c <= L'z') {
        return c - L'a' + L'A';
    }
    return c;
}

/**
 * @brief Find substring in UCS-2 string.
 *
 * @return pointer to first occurrence, or NULL if not found.
 */
const unsigned short *
axl_backend_wcsstr(
    const unsigned short  *haystack,  ///< string to search
    const unsigned short  *needle     ///< substring to find
    )
{
    const unsigned short  *h = haystack;
    const unsigned short  *n = needle;
    size_t                 nlen;

    if (h == NULL || n == NULL) {
        return NULL;
    }

    nlen = 0;
    while (n[nlen] != 0) {
        nlen++;
    }
    if (nlen == 0) {
        return haystack;
    }

    for (; *h != 0; h++) {
        if (*h == *n) {
            size_t i;
            for (i = 1; i < nlen && h[i] == n[i]; i++) {
            }
            if (i == nlen) {
                return (const unsigned short *)h;
            }
        }
    }

    return NULL;
}


// ===================================================================
// Console input
// ===================================================================

/* Defined further down alongside the cached SimpleTextInputEx pointer.
 * Forward-declared here so axl_backend_console_read_key_ex can use it. */
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *
get_simple_ex(void);

AxlEventHandle
axl_backend_console_wait_for_key(
    void
    )
{
    if (gST == NULL || gST->ConIn == NULL) {
        return NULL;
    }
    /* Prefer the Ex protocol's WaitForKeyEx: with EFI_KEY_STATE_EXPOSED
     * enabled (get_simple_ex) it also signals on modifier-only "partial"
     * keystrokes, so live modifier state stays current for pointer-event
     * stamping. read_key_ex reads via the same Ex protocol, so the wait and
     * the read are paired. Falls back to the basic WaitForKey when there is
     * no Ex protocol (e.g. a serial console). */
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = get_simple_ex();
    if (ex != NULL && ex->WaitForKeyEx != NULL) {
        return (AxlEventHandle)ex->WaitForKeyEx;
    }
    return (AxlEventHandle)gST->ConIn->WaitForKey;
}

int
axl_backend_console_read_key(
    uint16_t  *scan_code,
    uint16_t  *unicode_char
    )
{
    EFI_INPUT_KEY  key;
    EFI_STATUS     status;

    if (gST == NULL || gST->ConIn == NULL) {
        return AXL_ERR;
    }

    status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    if (scan_code != NULL) {
        *scan_code = key.ScanCode;
    }
    if (unicode_char != NULL) {
        *unicode_char = key.UnicodeChar;
    }
    return AXL_OK;
}

/* Translate an EFI_KEY_STATE (KeyShiftState + KeyToggleState) into
 * normalized AXL_INPUT_MOD_* bits. Each side maps 1:1; the
 * side-agnostic SHIFT/CTRL/ALT/META names are masks over their L/R
 * bits, so no separate "collapse" step is needed. Bits are honored
 * only when their respective *_VALID flag is set. */
static uint32_t
efi_keystate_to_axl_mods(
    uint32_t  shift_state,
    uint8_t   toggle_state
    )
{
    uint32_t m = 0;
    if (shift_state & EFI_SHIFT_STATE_VALID) {
        if (shift_state & EFI_LEFT_SHIFT_PRESSED)    m |= AXL_INPUT_MOD_LSHIFT;
        if (shift_state & EFI_RIGHT_SHIFT_PRESSED)   m |= AXL_INPUT_MOD_RSHIFT;
        if (shift_state & EFI_LEFT_CONTROL_PRESSED)  m |= AXL_INPUT_MOD_LCTRL;
        if (shift_state & EFI_RIGHT_CONTROL_PRESSED) m |= AXL_INPUT_MOD_RCTRL;
        if (shift_state & EFI_LEFT_ALT_PRESSED)      m |= AXL_INPUT_MOD_LALT;
        if (shift_state & EFI_RIGHT_ALT_PRESSED)     m |= AXL_INPUT_MOD_RALT;
        if (shift_state & EFI_LEFT_LOGO_PRESSED)     m |= AXL_INPUT_MOD_LMETA;
        if (shift_state & EFI_RIGHT_LOGO_PRESSED)    m |= AXL_INPUT_MOD_RMETA;
    }
    if (toggle_state & EFI_TOGGLE_STATE_VALID) {
        if (toggle_state & EFI_CAPS_LOCK_ACTIVE)   m |= AXL_INPUT_MOD_CAPS_LOCK;
        if (toggle_state & EFI_NUM_LOCK_ACTIVE)    m |= AXL_INPUT_MOD_NUM_LOCK;
        if (toggle_state & EFI_SCROLL_LOCK_ACTIVE) m |= AXL_INPUT_MOD_SCROLL_LOCK;
    }
    return m;
}

int
axl_backend_console_read_key_ex(
    uint16_t  *scan_code,
    uint16_t  *unicode_char,
    uint32_t  *modifiers
    )
{
    /* Prefer SimpleTextInputEx so we get modifier + lock state. */
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *simple_ex = get_simple_ex();
    if (simple_ex != NULL) {
        EFI_KEY_DATA  key_data;
        EFI_STATUS    status = simple_ex->ReadKeyStrokeEx(simple_ex, &key_data);
        if (EFI_ERROR(status)) {
            return AXL_ERR;
        }
        if (scan_code != NULL) {
            *scan_code = key_data.Key.ScanCode;
        }
        if (unicode_char != NULL) {
            *unicode_char = key_data.Key.UnicodeChar;
        }
        if (modifiers != NULL) {
            *modifiers = efi_keystate_to_axl_mods(
                key_data.KeyState.KeyShiftState,
                key_data.KeyState.KeyToggleState);
        }
        return AXL_OK;
    }

    /* Fallback: SimpleTextInput has no modifier info. Report 0,
     * which is exactly what serial consoles deliver anyway (TerminalDxe
     * doesn't carry shift bits over the wire). */
    int rc = axl_backend_console_read_key(scan_code, unicode_char);
    if (modifiers != NULL) {
        *modifiers = 0;
    }
    return rc;
}

bool
axl_backend_shell_break_flag(
    void
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        return false;
    }

    status = gBS->CheckEvent(shell->ExecutionBreak);
    return (status == EFI_SUCCESS);
}

// ---------------------------------------------------------------------------
// SimpleTextInputEx access (cached) — used to read keystrokes with their
// KeyShiftState + KeyToggleState, which axl_backend_console_read_key_ex
// normalizes into AXL_INPUT_MOD_* bits. The loop reads ConsoleInHandle
// keys event-driven via WaitForKey/Ex; we need the modifier state on
// dispatch so it can recognize raw serial Ctrl-C ({UnicodeChar=0x03,
// no modifiers}, what TerminalDxe emits — see axl-loop.c's keypress
// dispatch).
//
// We do NOT call SimpleTextInputEx::RegisterKeyNotify. Doing so puts
// OVMF's ConSplitter into a TPL_NOTIFY-level key polling loop that
// preempts our TPL_CALLBACK loop and starves the TCP4 stack — the
// regression that 12679de's first revision introduced (test-http.sh
// dropped from 40/19 → 6/53; QEMU pinned at 100% CPU).
// ---------------------------------------------------------------------------

static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *mSimpleEx       = NULL;
static bool                                mSimpleExLooked = false;

/* SetState is `void *` in the (hand-written) Ex protocol struct; this is its
 * real signature (UEFI 2.11 §12.2.4). Used to enable EFI_KEY_STATE_EXPOSED. */
typedef EFI_STATUS (EFIAPI *AxlInputExSetState)(
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
    UINT8                              *KeyToggleState);

static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *
get_simple_ex(void)
{
    if (!mSimpleExLooked) {
        mSimpleExLooked = true;
        if (gST != NULL && gST->ConsoleInHandle != NULL) {
            EFI_GUID guid = gEfiSimpleTextInputExProtocolGuid;
            gBS->HandleProtocol(gST->ConsoleInHandle, &guid,
                                (void **)&mSimpleEx);
        }
    }
    return mSimpleEx;
}

void
axl_backend_console_expose_modifiers(void)
{
    /* Enable modifier-only "partial" keystrokes so live modifier state stays
     * current between character keys (Shift+wheel, Ctrl+click). Best-effort:
     * if SetState is unsupported the call fails harmlessly and modifiers just
     * track the last full keystroke. The loop poll-reads these partials via
     * ReadKeyStrokeEx — WaitForKeyEx discards them (see axl-loop.c).
     *
     * KNOWN LIMITATION — the caps/num/scroll-lock LEDs are reset here. EDK2's
     * SetState (UsbKbDxe USBKeyboardSetState / Ps2KeyboardDxe) UNCONDITIONALLY
     * clears NumLockOn/CapsOn/ScrollOn, then re-asserts only the lock bits in
     * the passed KeyToggleState, then SetKeyLED(). We pass none (just VALID |
     * EXPOSED), so all three reset. UEFI has NO GetState for toggle state, so
     * the *initial* (pre-attach) lock state is unrecoverable — a read-modify-
     * write is impossible. Mitigation is bounded to caching post-attach
     * toggles, which buys nothing for the initial state. Because the loss is
     * inherent to the protocol, this is called only when a keyboard source is
     * attached (not for every app), and we accept it. Lock toggles made
     * AFTER attach ARE tracked live (they arrive as partials and refresh the
     * modifier state); only the pre-attach snapshot is lost. */
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = get_simple_ex();
    if (ex != NULL && ex->SetState != NULL) {
        AxlInputExSetState set_state = (AxlInputExSetState)ex->SetState;
        UINT8 toggle = EFI_TOGGLE_STATE_VALID | EFI_KEY_STATE_EXPOSED;
        (void)set_state(ex, &toggle);
    }
}

AxlEventHandle
axl_backend_shell_break_event(
    void
    )
{
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL) {
        return NULL;
    }
    return (AxlEventHandle)shell->ExecutionBreak;
}

// ===================================================================
// Miscellaneous Boot Services
// ===================================================================

void
axl_backend_stall(
    uint64_t  microseconds
    )
{
    gBS->Stall((UINTN)microseconds);
}

void
axl_backend_event_signal(
    AxlEventHandle  event
    )
{
    if (event != NULL) {
        gBS->SignalEvent((EFI_EVENT)event);
    }
}

// ===================================================================
// App termination
// ===================================================================

// Pending verbatim exit status (armed by axl_set_exit_status). Per-image:
// these live in this image's libaxl instance, alongside gImageHandle, so the
// status applies to whichever image's CRT0 / axl_exit reads them.
static bool       g_exit_status_armed = false;
static EFI_STATUS g_exit_status       = EFI_SUCCESS;

void
axl_backend_set_exit_status(uint64_t status)
{
    g_exit_status       = (EFI_STATUS)status;
    g_exit_status_armed = true;
}

void
axl_backend_clear_exit_status(void)
{
    g_exit_status_armed = false;
    g_exit_status       = EFI_SUCCESS;
}

uint64_t
axl_backend_resolve_exit_status(int rc)
{
    if (g_exit_status_armed) {
        return (uint64_t)g_exit_status;            // verbatim, including success
    }
    return (uint64_t)((rc == 0) ? EFI_SUCCESS : EFI_ABORTED);
}

void
axl_backend_boot_exit(int rc)
{
    EFI_STATUS status = (EFI_STATUS)axl_backend_resolve_exit_status(rc);
    gBS->Exit(gImageHandle, status, 0, NULL);

    /* gBS->Exit is specified as NORETURN for the image's own handle;
     * if it ever does return (e.g. firmware bug) we still have to not
     * return from this NORETURN function. Spin to satisfy the compiler
     * and trap obvious misbehavior. */
    for (;;) {
        gBS->Stall(1000000);
    }
}
