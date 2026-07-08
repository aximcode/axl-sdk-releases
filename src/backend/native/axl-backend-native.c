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
#include "axl-backend-native-efi1x.h"  /* old EFI 1.x shell path resolution */
#include "axl-stdio-bridge.h"  /* AxlStdioBridge, bridge install/uninstall */
#include <axl/axl-driver.h>    /* axl_protocol_install, axl_protocol_uninstall */
#include <axl/axl-sys.h>       /* axl_protocol_find_guid */
#include <axl/axl-atexit.h>    /* axl_atexit */
#include <axl/axl-input.h>     /* AXL_INPUT_MOD_* — read_key_ex normalizes to these */
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
 * @brief Write a UCS-2 string to the error console. NULL-safe.
 *
 * Targets gST->StdErr so the UEFI shell's `2>` redirect (which swaps
 * gST->StdErr, symmetric to `>` swapping gST->ConOut) captures it and a
 * plain `>` does not. Falls back to gST->ConOut when StdErr is absent
 * (minimal firmware / BDS).
 */
void
axl_backend_console_write_err(
    const unsigned short  *str
    )
{
    if (gST == NULL || str == NULL) {
        return;
    }
    if (gST->StdErr != NULL) {
        gST->StdErr->OutputString(gST->StdErr, (CHAR16 *)str);
    } else if (gST->ConOut != NULL) {
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
 * @brief Set the error console's text attribute (color/style).
 *
 * Targets gST->StdErr, symmetric to axl_backend_console_write_err(), so
 * the escape bytes an ANSI/serial console (TerminalDxe) emits for
 * SetAttribute land on the same sink as the text they color. Falls
 * back to gST->ConOut when StdErr is absent (minimal firmware / BDS).
 */
void
axl_backend_console_set_attr_err(
    uint32_t  attr
    )
{
    if (gST == NULL) {
        return;
    }
    if (gST->StdErr != NULL) {
        gST->StdErr->SetAttribute(gST->StdErr, attr);
    } else if (gST->ConOut != NULL) {
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

void
axl_backend_console_set_page_break(
    bool  enable
    )
{
    /* Paging is a shell service — the shell's console logger wraps ConOut,
       so flipping this switch paginates our ConOut-bound output with no
       SDK-side pager. get_shell() is NULL on the legacy EFI 1.x shell (it
       publishes SHELL_ENVIRONMENT, not EFI_SHELL_PROTOCOL) and in a
       non-shell context; both leave paging unavailable — a no-op. The
       member NULL-checks guard a pre-page-break shell protocol revision. */
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL) {
        return;
    }
    if (enable) {
        /* Suppress paging inside a script (startup.nsh / `.nsh`): the shell
           still pauses ConOut after each screenful even in batch context,
           and with no human to press the continue key that hangs the run.
           BatchIsActive() is TRUE for the whole time a script-launched image
           executes, so gate on it. Interactive `-b` (BatchIsActive FALSE)
           still pages normally. */
        if (shell->BatchIsActive != NULL && shell->BatchIsActive()) {
            return;
        }
        if (shell->EnablePageBreak != NULL) {
            shell->EnablePageBreak();
        }
    } else if (shell->DisablePageBreak != NULL) {
        shell->DisablePageBreak();
    }
}

uint32_t
axl_backend_console_text_mode_count(
    void
    )
{
    if (gST == NULL || gST->ConOut == NULL || gST->ConOut->Mode == NULL) {
        return 0;
    }
    /* MaxMode is INT32; a non-positive value means no usable modes. */
    INT32 max = gST->ConOut->Mode->MaxMode;
    return (max > 0) ? (uint32_t)max : 0;
}

int
axl_backend_console_text_query_mode(
    uint32_t   index,
    uint32_t  *columns,
    uint32_t  *rows
    )
{
    if (gST == NULL || gST->ConOut == NULL || gST->ConOut->QueryMode == NULL
        || columns == NULL || rows == NULL) {
        return AXL_ERR;
    }
    UINTN      cols = 0, r = 0;
    EFI_STATUS status =
        gST->ConOut->QueryMode(gST->ConOut, (UINTN)index, &cols, &r);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    *columns = (uint32_t)cols;
    *rows    = (uint32_t)r;
    return AXL_OK;
}

int
axl_backend_console_text_current_mode(
    void
    )
{
    if (gST == NULL || gST->ConOut == NULL || gST->ConOut->Mode == NULL) {
        return -1;
    }
    return (int)gST->ConOut->Mode->Mode;   /* -1 when no mode is set */
}

int
axl_backend_console_text_set_mode(
    uint32_t  index
    )
{
    if (gST == NULL || gST->ConOut == NULL || gST->ConOut->SetMode == NULL) {
        return AXL_ERR;
    }
    EFI_STATUS status = gST->ConOut->SetMode(gST->ConOut, (UINTN)index);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
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

/**
 * @brief Set the current date/time in firmware.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_backend_set_time(
    const AxlTime  *time  ///< time to program into the RTC
    )
{
    EFI_TIME    efi_time = { 0 };
    EFI_STATUS  status;

    if (time == NULL) {
        return AXL_ERR;
    }

    efi_time.Year       = time->year;
    efi_time.Month      = time->month;
    efi_time.Day        = time->day;
    efi_time.Hour       = time->hour;
    efi_time.Minute     = time->minute;
    efi_time.Second     = time->second;
    efi_time.Nanosecond = time->nanosecond;
    efi_time.Daylight   = time->daylight;
    /* Inverse of the GetTime mapping: our INT16_MIN "unspecified"
       sentinel becomes EFI's EFI_UNSPECIFIED_TIMEZONE (2047). */
    efi_time.TimeZone   = (time->timezone_minutes == INT16_MIN)
        ? 2047
        : time->timezone_minutes;

    status = gRT->SetTime(&efi_time);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
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
        /* Old EFI 1.x shell: resolve + open natively. The resulting
           EFI_FILE_PROTOCOL* IS the handle — the same shape a
           SHELL_FILE_HANDLE has on the modern shell, so the handle-based
           ops below stay shell-agnostic. */
        EFI_FILE_PROTOCOL *file = NULL;
        if (axl_efi1x_file_open(path, mode, attributes, &file) != AXL_OK) {
            return AXL_ERR;
        }
        *handle = (AxlFileHandle)file;
        return AXL_OK;
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
    } else {
        /* Old EFI 1.x shell: the handle is an EFI_FILE_PROTOCOL*. */
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)*handle;
        file->Close(file);
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

    if (handle == NULL || size == NULL || buf == NULL) {
        return AXL_ERR;
    }

    usize = *size;
    shell = get_shell();
    if (shell != NULL) {
        status = shell->ReadFile((SHELL_FILE_HANDLE)handle, &usize, buf);
    } else {
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
        status = file->Read(file, &usize, buf);
    }
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

    if (handle == NULL || size == NULL || buf == NULL) {
        return AXL_ERR;
    }

    usize = *size;
    shell = get_shell();
    if (shell != NULL) {
        status = shell->WriteFile((SHELL_FILE_HANDLE)handle, &usize,
                                   (VOID *)buf);
    } else {
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
        status = file->Write(file, &usize, (VOID *)buf);
    }
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

    if (handle == NULL || pos == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell != NULL) {
        status = shell->GetFilePosition((SHELL_FILE_HANDLE)handle, &efi_pos);
    } else {
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
        status = file->GetPosition(file, &efi_pos);
    }
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

    if (handle == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell != NULL) {
        status = shell->SetFilePosition((SHELL_FILE_HANDLE)handle, pos);
    } else {
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
        status = file->SetPosition(file, pos);
    }
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
        /* Old EFI 1.x shell: open with write access, then Delete (which
           closes the handle whether it succeeds or fails). Works for both
           files and empty directories. */
        EFI_FILE_PROTOCOL *file = NULL;
        if (axl_efi1x_file_open(path,
                                AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE,
                                0, &file) != AXL_OK) {
            return AXL_ERR;
        }
        status = file->Delete(file);
        return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
    }

    status = shell->DeleteFileByName((CHAR16 *)path);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/* GetFileInfo for an already-open handle, shell-agnostic. On the modern shell
   the handle is a SHELL_FILE_HANDLE and GetFileInfo allocates the EFI_FILE_INFO
   for us; on the old shell it is an EFI_FILE_PROTOCOL* and we run the standard
   size-probe-then-read GetInfo pair. Either way the result is an
   axl_backend_alloc'd buffer the caller frees with axl_backend_free — the
   shell's GetFileInfo uses the same firmware pool allocator. Returns NULL on
   error. */
static EFI_FILE_INFO *
native_file_info(
    AxlFileHandle  handle
    )
{
    if (handle == NULL) {
        return NULL;
    }

    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell != NULL) {
        return (EFI_FILE_INFO *)shell->GetFileInfo((SHELL_FILE_HANDLE)handle);
    }

    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
    EFI_GUID           guid = gEfiFileInfoGuid;
    UINTN              sz   = 0;
    EFI_STATUS         st   = file->GetInfo(file, &guid, &sz, NULL);
    if (st != EFI_BUFFER_TOO_SMALL || sz == 0) {
        return NULL;
    }
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)axl_backend_alloc(sz);
    if (info == NULL) {
        return NULL;
    }
    st = file->GetInfo(file, &guid, &sz, info);
    if (EFI_ERROR(st)) {
        axl_backend_free(info);
        return NULL;
    }
    return info;
}

/* SetFileInfo for an already-open handle, shell-agnostic (mirror of
   native_file_info). */
static EFI_STATUS
native_set_file_info(
    AxlFileHandle         handle,
    const EFI_FILE_INFO  *info
    )
{
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell != NULL) {
        return shell->SetFileInfo((SHELL_FILE_HANDLE)handle, info);
    }
    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
    EFI_GUID           guid = gEfiFileInfoGuid;
    return file->SetInfo(file, &guid, (UINTN)info->Size, (VOID *)info);
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

    if (handle == NULL) {
        return -1;
    }

    shell = get_shell();
    if (shell != NULL) {
        status = shell->GetFileSize((SHELL_FILE_HANDLE)handle, &size);
        return EFI_ERROR(status) ? -1 : (int64_t)size;
    }

    /* Old EFI 1.x shell: read it out of the file info. */
    EFI_FILE_INFO *info = native_file_info(handle);
    if (info == NULL) {
        return -1;
    }
    int64_t result = (int64_t)info->FileSize;
    axl_backend_free(info);
    return result;
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
    AxlFileHandle  handle = NULL;
    bool           is_dir = false;

    if (axl_backend_file_open(path, AXL_FILE_MODE_READ, 0, &handle) != AXL_OK) {
        return false;
    }

    EFI_FILE_INFO *info = native_file_info(handle);
    if (info != NULL) {
        is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        axl_backend_free(info);
    }
    axl_backend_file_close(&handle);
    return is_dir;
}

int
axl_backend_file_rename(
    const unsigned short  *old_path,
    const unsigned short  *new_path
    )
{
    AxlFileHandle        handle = NULL;
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;
    size_t               new_len;
    size_t               info_size;
    EFI_FILE_INFO       *new_info;
    size_t               i;

    if (old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }

    /* Open the existing file (shell-agnostic — resolves fsN:/relative). */
    if (axl_backend_file_open(old_path,
                              AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE,
                              0, &handle) != AXL_OK) {
        return AXL_ERR;
    }

    /* Get current file info */
    info = native_file_info(handle);
    if (info == NULL) {
        axl_backend_file_close(&handle);
        return AXL_ERR;
    }

    /* Build new info with the new filename */
    for (new_len = 0; new_path[new_len] != 0; new_len++) {}
    info_size = sizeof(EFI_FILE_INFO) + (new_len + 1) * sizeof(CHAR16);

    new_info = (EFI_FILE_INFO *)axl_backend_alloc(info_size);
    if (new_info == NULL) {
        axl_backend_free(info);
        axl_backend_file_close(&handle);
        return AXL_ERR;
    }

    /* Copy metadata, replace filename */
    *new_info = *info;
    new_info->Size = info_size;
    for (i = 0; i <= new_len; i++) {
        new_info->FileName[i] = (CHAR16)new_path[i];
    }

    status = native_set_file_info(handle, new_info);
    axl_backend_free(new_info);
    axl_backend_free(info);
    axl_backend_file_close(&handle);

    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_backend_file_set_size(
    AxlFileHandle  handle,
    uint64_t       size
    )
{
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;

    if (handle == NULL) {
        return AXL_ERR;
    }

    /* GetFileInfo returns a fresh allocation we mutate and write back —
       same SetFileInfo round-trip the rename path uses. The struct
       (filename tail included) is preserved; only FileSize changes. */
    info = native_file_info(handle);
    if (info == NULL) {
        return AXL_ERR;
    }
    info->FileSize = size;
    status = native_set_file_info(handle, info);
    axl_backend_free(info);

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

    if (path == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        /* Old EFI 1.x shell: Open with CREATE|DIRECTORY makes the directory
           (and opens it); close the handle. */
        EFI_FILE_PROTOCOL *dir = NULL;
        if (axl_efi1x_file_open(path,
                                AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE
                                    | AXL_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY, &dir) != AXL_OK) {
            return AXL_ERR;
        }
        dir->Close(dir);
        return AXL_OK;
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
    /* DeleteFileByName works for both files and empty directories; the old
       shell's file_delete opens with write access and calls Delete, which
       likewise removes an empty directory. */
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
    AxlFileHandle        handle = NULL;
    EFI_FILE_INFO       *info;

    if (path == NULL) {
        return AXL_ERR;
    }

    if (axl_backend_file_open(path, AXL_FILE_MODE_READ, 0, &handle) != AXL_OK) {
        return AXL_ERR;
    }

    info = native_file_info(handle);
    if (info == NULL) {
        axl_backend_file_close(&handle);
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
    axl_backend_file_close(&handle);
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
        /* Old EFI 1.x shell: SHELL_ENVIRONMENT.GetEnv. */
        return axl_efi1x_getenv(name);
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

    if (name == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        /* Old EFI 1.x shell: no SetEnv member — drive the shell's own `set`
           command through Execute (the mkrd `map -r` pattern). */
        return axl_efi1x_setenv(name, value, volatile_var);
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
static SHELL_FILE_HANDLE  mShellStdErr       = NULL;
static bool               mShellStdProbed    = false;

/* Shared probe — looks up EFI_SHELL_PARAMETERS_PROTOCOL once and
   caches StdIn, StdOut, and StdErr handles. All three helpers below
   trigger the same probe on first call. */
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
        mShellStdErr = sp->StdErr;
    }
}

// ===================================================================
// Stdio-bridge — carries launcher shell handles across image boundary
// ===================================================================

/* uuid c8f517d7-36cc-458d-98d6-b116825e30bf — fixed identity of the
   stdio-bridge protocol. */
const AxlGuid AXL_STDIO_BRIDGE_GUID =
    AXL_GUID(0xc8f517d7, 0x36cc, 0x458d,
             0x98, 0xd6, 0xb1, 0x16, 0x82, 0x5e, 0x30, 0xbf);

const AxlGuid AXL_DISPATCH_TOKEN_GUID =
    AXL_GUID(0x02dd6813, 0xd275, 0x4734,
             0x98, 0xf8, 0xc7, 0xf6, 0x03, 0x31, 0x95, 0x8d);

/* Dedicated persistent handle the one dispatch-token cell is installed on.
   The cell lives in pool memory (image-independent) and is never uninstalled —
   infra that must outlive the images that use it, like the fixed bridge GUID
   identity. The cell pointer isn't cached; every consult locates it fresh via
   dispatch_cell() (cross-image-correct; a transient locate miss degrades to
   safe EOF-fallback rather than a stale deref). */
static AxlHandle         mDispatchHandle = NULL;

static AxlStdioBridge  mBridge;
static AxlHandle       mBridgeHandle  = NULL;   /* install handle; NULL = not installed */
static uint32_t        mBridgeAtexit  = 0;      /* atexit cookie; 0 = not yet registered */

static void
bridge_atexit(
    void  *data
    )
{
    (void)data;
    axl_backend_stdio_bridge_uninstall();
}

int
axl_backend_dispatch_token_ensure(void)
{
    /* Already resident (this image or another) — reuse it. */
    void *found = NULL;
    if (axl_protocol_find_guid(&AXL_DISPATCH_TOKEN_GUID, &found) == AXL_OK
        && found != NULL) {
        return AXL_OK;
    }
    /* Create the singleton cell in pool memory on a fresh handle. */
    void *mem = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiBootServicesData,
                                    sizeof(AxlDispatchToken), &mem))
        || mem == NULL) {
        return AXL_ERR;
    }
    ((AxlDispatchToken *)mem)->current = 0;
    mDispatchHandle = NULL;   /* NULL => allocate a fresh handle */
    if (axl_protocol_install(&AXL_DISPATCH_TOKEN_GUID, mem, &mDispatchHandle)
        != AXL_OK) {
        gBS->FreePool(mem);
        return AXL_ERR;
    }
    return AXL_OK;
}

/* Locate the live dispatch-token cell (any image's), or NULL if none. */
static AxlDispatchToken *
dispatch_cell(void)
{
    void *found = NULL;
    if (axl_protocol_find_guid(&AXL_DISPATCH_TOKEN_GUID, &found) == AXL_OK) {
        return (AxlDispatchToken *)found;
    }
    return NULL;
}

/* Is the bridge's launcher's dispatch still current? Forward decl — used by
   the reaper below and the read-path lookup. Definition follows. */
static bool bridge_launcher_alive(const AxlStdioBridge *b);

/* Uninstall every installed bridge instance whose launcher image has exited.
   Cross-image and best-effort: it enumerates the whole DB, so a live image can
   reap bridges leaked by launchers that skipped their atexit uninstall
   (--minimal-runtime / gBS->Exit). UninstallProtocolInterface matches the
   (guid, iface) pointer and never dereferences the freed interface, so a stale
   instance is safe to drop. */
static void
bridge_reap_dead(void)
{
    EFI_GUID    guid    = *(EFI_GUID *)&AXL_STDIO_BRIDGE_GUID;
    UINTN       count   = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(
            ByProtocol, &guid, NULL, &count, &handles))
        || count == 0 || handles == NULL) {
        return;
    }
    for (UINTN i = 0; i < count; i++) {
        void *iface = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &guid, &iface))
            || iface == NULL) {
            continue;
        }
        AxlStdioBridge *b = (AxlStdioBridge *)iface;
        if (!bridge_launcher_alive(b)) {
            gBS->UninstallProtocolInterface(handles[i], &guid, iface);
        }
    }
    gBS->FreePool(handles);
}

void
axl_backend_stdio_bridge_reap(void)
{
    /* No active dispatch on this teardown path (do -u / unload): clear the
       marker so EVERY installed bridge (all tokens != 0) is dead and reaped,
       including a final leaked instance whose token still matches a stale
       current. */
    AxlDispatchToken *cell = dispatch_cell();
    if (cell != NULL) {
        cell->current = 0;
    }
    bridge_reap_dead();
}

int
axl_backend_stdio_bridge_install(void)
{
    /* Capture THIS image's shell handles.  If we have none, nothing to
       bridge — a successful no-op (e.g. not launched from a shell). */
    AxlFileHandle in  = axl_backend_shell_stdin();
    AxlFileHandle out = axl_backend_shell_stdout();
    AxlFileHandle err = axl_backend_shell_stderr();
    if (in == NULL && out == NULL && err == NULL) {
        return AXL_OK;
    }
    /* Fresh per-dispatch token: firmware-global monotonic, unique across
       images (a per-image counter would collide). 0 is the "no dispatch"
       sentinel; if the counter's first value of the boot is 0, re-call to
       consume it and take the next value. Re-calling (not fabricating t=1)
       is what keeps uniqueness: the counter advances, so no later dispatch
       can be handed the same value. Terminates immediately — a monotonic
       counter never returns 0 twice. */
    uint64_t t = 0;
    while (t == 0) {
        gBS->GetNextMonotonicCount(&t);
    }
    /* Publish the token to the driver-resident cell BEFORE reaping, so the
       reap-at-install below uses the NEW token as its liveness reference and
       correctly sweeps prior leaked bridges (all bearing older tokens). No
       resident driver => no cell => reap drops every not-yet-installed
       instance, which is fine (nothing consults a bridge without a driver). */
    {
        AxlDispatchToken *cell = dispatch_cell();
        if (cell != NULL) {
            cell->current = t;
        }
    }
    /* Sweep bridges leaked by prior launchers before publishing ours. Each
       launcher is a fresh image, so the mBridgeHandle refresh below only
       catches a re-install within THIS image; the cross-image dead instances
       (the accumulating leak) are cleared here. */
    bridge_reap_dead();
    /* Refresh: uninstall a stale one first (handles change per invocation). */
    if (mBridgeHandle != NULL) {
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE_GUID, &mBridge);
        mBridgeHandle = NULL;
    }
    mBridge.stdin_h        = in;
    mBridge.stdout_h       = out;
    mBridge.stderr_h       = err;
    mBridge.launcher_image = (void *)gImageHandle;
    mBridge.token          = t;
    mBridge.pending_status = 0;
    mBridge.has_pending    = false;
    /* The published interface is &mBridge — a static in THIS (launcher)
       image. It MUST be uninstalled before the image unloads, or the
       firmware protocol DB keeps a dangling pointer into freed image
       memory. The bridge_atexit handler below guarantees that: CRT0 runs
       atexit (-> uninstall) before returning the launcher image to firmware. */
    if (axl_protocol_install(&AXL_STDIO_BRIDGE_GUID, &mBridge, &mBridgeHandle)
        != AXL_OK) {
        mBridgeHandle = NULL;
        return AXL_ERR;
    }
    if (mBridgeAtexit == 0) {
        mBridgeAtexit = axl_atexit(bridge_atexit, NULL);
    }
    return AXL_OK;
}

void
axl_backend_stdio_bridge_uninstall(void)
{
    if (mBridgeHandle != NULL) {
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE_GUID, &mBridge);
        mBridgeHandle = NULL;
    }
}

/* A bridge is live iff its per-dispatch token equals the driver-resident
   AxlDispatchToken.current the launcher stamped this dispatch. The reference
   lives in driver memory (not the freed, recyclable bridge), so this is robust
   against the correlated pool recycling that defeated the old LoadedImage-proto
   match. Reading b->token is a value compare on mapped memory — stdin_h and the
   status cell are only touched AFTER a live match, preserving the v2.6.1 UAF
   fix. current==0 (no active dispatch) or no cell => nothing is live. */
static bool
bridge_launcher_alive(
    const AxlStdioBridge  *b
    )
{
    if (b == NULL) {
        return false;
    }
    AxlDispatchToken *cell = dispatch_cell();
    if (cell == NULL || cell->current == 0) {
        return false;
    }
    return b->token == cell->current;
}

/* Live (uncached) bridge lookup — only used on the no-local-shell-params
   (driver) path. Reaps stale instances first (self-heal: a naive find-first
   would dereference whatever LocateProtocol returned first — a prior
   launcher's leaked stale bridge is older, so returned first, and its
   handles are freed), then returns the newest LIVE survivor's bridge
   instance (or NULL when none are live). Shared by every per-stream lookup
   below (stdin, stderr) so each just picks the field it needs off the same
   live instance. */
static AxlStdioBridge *
bridge_find_live(void)
{
    bridge_reap_dead();

    EFI_GUID    guid    = *(EFI_GUID *)&AXL_STDIO_BRIDGE_GUID;
    UINTN       count   = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(
            ByProtocol, &guid, NULL, &count, &handles))
        || count == 0 || handles == NULL) {
        return NULL;
    }
    AxlStdioBridge *live = NULL;
    for (UINTN i = 0; i < count; i++) {
        void *iface = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &guid, &iface))
            || iface == NULL) {
            continue;
        }
        AxlStdioBridge *b = (AxlStdioBridge *)iface;
        if (bridge_launcher_alive(b)) {
            live = b;   /* DB order is oldest-first; newest live wins */
        }
    }
    gBS->FreePool(handles);
    return live;
}

static AxlFileHandle
bridge_lookup_stdin(void)
{
    AxlStdioBridge *b = bridge_find_live();
    return (b != NULL) ? b->stdin_h : NULL;
}

static AxlFileHandle
bridge_lookup_stdout(void)
{
    AxlStdioBridge *b = bridge_find_live();
    return (b != NULL) ? b->stdout_h : NULL;
}

static AxlFileHandle
bridge_lookup_stderr(void)
{
    AxlStdioBridge *b = bridge_find_live();
    return (b != NULL) ? b->stderr_h : NULL;
}

AxlFileHandle
axl_backend_shell_stdin(void)
{
    probe_shell_std_handles();
    if (mShellStdIn != NULL) {
        return (AxlFileHandle)mShellStdIn;   /* app/launcher: own params */
    }
    return bridge_lookup_stdin();            /* driver: live bridge consult */
}

bool
axl_backend_stdin_is_interactive(void)
{
    AxlFileHandle h = axl_backend_shell_stdin();
    if (h == NULL) {
        /* No shell StdIn wiring (BDS / non-shell context, or a driver
           with no live bridge). Nothing to read interactively through
           the shell — the stream layer surfaces this as EOF rather than
           blocking on a keyboard, so report non-interactive to match. */
        return false;
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL || shell->GetFileSize == NULL) {
        return false;   /* can't probe → keep the safe raw-byte path */
    }
    /* A redirected file (`< f`) or pipe RHS (`|`) is a real file: GetFileSize
       succeeds with a byte count. The console pseudo-file rejects the query.
       Verified on OVMF/EDK2 across typed / `<` / `|`. */
    UINT64     size = 0;
    EFI_STATUS st   = shell->GetFileSize((SHELL_FILE_HANDLE)h, &size);
    if (!EFI_ERROR(st)) {
        return false;   /* has a byte size ⇒ redirected file/pipe */
    }
    /* GetFileSize failed. Bias toward the safe raw-byte path unless a SECOND,
       independent probe corroborates "console": a real file also returns file
       info, the console pseudo-file returns none. Requiring BOTH signals keeps
       a GetFileSize-hostile firmware from misclassifying a pipe as interactive
       — which would block on a keyboard instead of reading the piped bytes,
       strictly worse than the byte path (a false "piped" only degrades to the
       raw console read, it does not hang). */
    if (shell->GetFileInfo == NULL) {
        /* Can't obtain the corroborating signal — do NOT claim interactive on
           the single failed probe (that would risk blocking a pipe on the
           keyboard); fall back to the safe raw-byte path. */
        return false;
    }
    EFI_FILE_INFO *fi = shell->GetFileInfo((SHELL_FILE_HANDLE)h);
    if (fi != NULL) {
        gBS->FreePool(fi);
        return false;   /* has file info ⇒ treat as a file, not the console */
    }
    return true;   /* no size AND no file info ⇒ interactive console */
}

AxlFileHandle
axl_backend_shell_stdout(void)
{
    probe_shell_std_handles();
    if (mShellStdOut != NULL) {
        return (AxlFileHandle)mShellStdOut;  /* app/launcher: own params */
    }
    return bridge_lookup_stdout();           /* driver: live bridge consult */
}

AxlFileHandle
axl_backend_shell_stderr(void)
{
    probe_shell_std_handles();
    if (mShellStdErr != NULL) {
        return (AxlFileHandle)mShellStdErr;  /* app/launcher: own params */
    }
    return bridge_lookup_stderr();           /* driver: live bridge consult */
}

const unsigned short *
axl_backend_shell_getcwd(
    void
    )
{
    EFI_SHELL_PROTOCOL  *shell;

    shell = get_shell();
    if (shell == NULL) {
        /* Old EFI 1.x shell: SHELL_ENVIRONMENT.CurDir. */
        return axl_efi1x_getcwd();
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
    if (command == NULL) {
        return AXL_ERR;
    }

    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell != NULL) {
        EFI_STATUS status = shell->Execute(NULL, (CHAR16 *)command, NULL, NULL);
        return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
    }

    /* Old EFI 1.x shell: no EFI_SHELL_PROTOCOL. Run the command through the
       SHELL_ENVIRONMENT protocol's Execute — the same in-shell context, so a
       `map -r` here refreshes the very map the interactive shell resolves
       `fsN:` against (this is how the legacy mkramdisk mapped its disks: it
       drove the shell's own `map` command rather than any programmatic API). */
    EFI_SHELL_ENVIRONMENT *se = axl_efi1x_shell_env();
    if (se != NULL && se->Execute != NULL) {
        /* Handle by value, per the EFI Toolkit ShellExecute convention. A
           resident driver's own handle has no SHELL_INTERFACE and Execute
           would reject it (EFI_INVALID_PARAMETER), so borrow a shell-launched
           one. */
        EFI_STATUS status = se->Execute(axl_efi1x_shell_parent(),
                                        (CHAR16 *)command, FALSE);
        return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
    }
    return AXL_ERR;
}

// ===================================================================
// Shell map reverse-lookup
//
// On the old EFI 1.x shell these fall back to the SHELL_ENVIRONMENT-based
// helpers in axl-backend-native-efi1x.c (GetMap has no reverse direction,
// so those iterate fs0..fsN and byte-compare).
// ===================================================================

int
axl_backend_shell_map_name(
    void   *device_path,
    char   *out,
    size_t  out_size
    )
{
    if (device_path == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL || shell->GetMapFromDevicePath == NULL) {
        /* Old EFI 1.x shell: no GetMapFromDevicePath. The efi1x reverse lookup
           yields exactly the lowercase fs<n> this function reports (resolves
           only once the disk is in the shell's map, i.e. after `map -r`). */
        return axl_efi1x_map_fs_name_from_dp(device_path, out, out_size);
    }

    /* GetMapFromDevicePath advances the pointer past the matched volume
       portion; pass a local copy so the caller's device path is untouched. */
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)device_path;
    const unsigned short *map = (const unsigned short *)
        shell->GetMapFromDevicePath(&dp);
    if (map == NULL) {
        return AXL_ERR;
    }

    /* `map` is one or more ';'-separated aliases in UCS-2, e.g. "FS1:;F1:".
       Take the first "fs<digits>" token, lowercased, without the ':'. The
       shell lists the friendliest (FSn:) alias first, but scanning for the
       fs<n> token explicitly is robust to ordering. */
    for (const unsigned short *p = map; *p != 0; ) {
        const unsigned short *tok = p;
        while (*p != 0 && *p != (unsigned short)';') {
            p++;
        }
        size_t toklen = (size_t)(p - tok);
        if (toklen >= 3
            && (tok[0] == 'f' || tok[0] == 'F')
            && (tok[1] == 's' || tok[1] == 'S')
            && tok[2] >= '0' && tok[2] <= '9') {
            size_t j = 0;
            for (size_t k = 0; k < toklen && j + 1 < out_size; k++) {
                unsigned short c = tok[k];
                if (c == (unsigned short)':') {
                    break;
                }
                out[j++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            }
            out[j] = '\0';
            return (j > 0) ? AXL_OK : AXL_ERR;
        }
        if (*p == (unsigned short)';') {
            p++;
        }
    }
    return AXL_ERR;   /* no fs<n> alias maps to this device path */
}

int
axl_backend_shell_map_alias(
    void   *device_path,
    char   *out,
    size_t  out_size
    )
{
    if (device_path == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL || shell->GetMapFromDevicePath == NULL) {
        /* Old EFI 1.x shell: no GetMapFromDevicePath. Reverse-look-up the
           disk's FS<n> through SHELL_ENVIRONMENT.GetMap instead. Resolves only
           once the disk is actually in the shell's map (after `map -r`). */
        return axl_efi1x_map_fs_name_from_dp(device_path, out, out_size);
    }

    /* GetMapFromDevicePath advances the pointer; pass a local copy. */
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)device_path;
    const unsigned short *map = (const unsigned short *)
        shell->GetMapFromDevicePath(&dp);
    if (map == NULL) {
        return AXL_ERR;
    }

    /* `map` is one or more ';'-separated aliases, e.g. "RAMDISK:;FS9:". Take
       the FIRST token verbatim (minus the trailing ':') — any form, fs<n> or a
       custom SetMap name. This is the "what is it mapped as" query, distinct
       from map_name's fs<n>-only filter. */
    const unsigned short *p = map;
    size_t j = 0;
    while (*p != 0 && *p != (unsigned short)';' && *p != (unsigned short)':') {
        if (j + 1 >= out_size) {
            return AXL_ERR;   /* alias longer than the buffer — don't return a
                                 truncated, un-resolvable name as success */
        }
        out[j++] = (char)*p++;
    }
    out[j] = '\0';
    return (j > 0) ? AXL_OK : AXL_ERR;
}

bool
axl_backend_shell_map_exists(
    const unsigned short  *name
    )
{
    if (name == NULL) {
        return false;
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell != NULL && shell->GetDevicePathFromMap != NULL) {
        /* GetDevicePathFromMap returns the firmware-owned device path for the
           mapping, or NULL if no such mapping exists. */
        return shell->GetDevicePathFromMap((const CHAR16 *)name) != NULL;
    }

    /* Old EFI 1.x shell: no GetDevicePathFromMap. Ask SHELL_ENVIRONMENT.GetMap
       instead (axl_efi1x_map_exists strips any ':' the caller appended and
       tries the case foldings the old shell stores names in). */
    return axl_efi1x_map_exists(name);
}

int
axl_backend_shell_set_map(
    void                  *device_path,
    const unsigned short  *name
    )
{
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL || shell->SetMap == NULL) {
        return AXL_UNSUPPORTED;
    }
    if (device_path == NULL || name == NULL) {
        return AXL_ERR;
    }
    /* SetMap adds the mapping to the shell's global map list (not a nested
       shell like Execute), so a name set here by a child image is usable by
       the launching shell/script immediately — no `map -r` needed. Requires a
       ':'-terminated name; the caller supplies it. */
    EFI_STATUS st = shell->SetMap(
        (const EFI_DEVICE_PATH_PROTOCOL *)device_path, (const CHAR16 *)name);
    return EFI_ERROR(st) ? AXL_ERR : AXL_OK;
}

int
axl_backend_shell_unmap(
    const unsigned short  *name
    )
{
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL || shell->SetMap == NULL) {
        return AXL_UNSUPPORTED;
    }
    if (name == NULL) {
        return AXL_ERR;
    }
    /* SetMap with a NULL device path deletes the named mapping from the
       shell's global map (UEFI Shell 2.2 EFI_SHELL_PROTOCOL.SetMap). */
    EFI_STATUS st = shell->SetMap(NULL, (const CHAR16 *)name);
    return EFI_ERROR(st) ? AXL_ERR : AXL_OK;
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
        set_state(ex, &toggle);
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

    /* If THIS image is a resident driver (no shell params of its own) serving
       a launcher dispatch, reflect the status into the launcher's bridge cell
       so the launcher's apply/CRT0 returns it. A normal app/launcher (has
       shell params) skips this and uses its own g_exit_status. */
    probe_shell_std_handles();
    if (mShellStdIn == NULL) {
        /* Assumes SYNCHRONOUS dispatch (the shared-driver pattern has no
           event loop): only one launcher is ever mid-dispatch at a time, so
           bridge_find_live() unambiguously returns THAT launcher's bridge. */
        AxlStdioBridge *b = bridge_find_live();
        if (b != NULL) {
            b->pending_status = status;
            b->has_pending    = true;
        }
    }
}

bool
axl_backend_bridge_take_exit_status(uint64_t *out)
{
    /* Read THIS (launcher) image's own bridge cell — the driver wrote it
       through the installed protocol interface (== &mBridge here). */
    if (mBridgeHandle == NULL || !mBridge.has_pending) {
        return false;
    }
    if (out != NULL) {
        *out = mBridge.pending_status;
    }
    mBridge.has_pending = false;
    return true;
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
    if (rc == 0) {
        return (uint64_t)EFI_SUCCESS;
    }
    /* Map a non-zero main() return to a small POSIX-style exit code (1..255,
       high bit clear) so the shell surfaces it as `%lasterror%=N`. This
       REPLACES the old EFI_ABORTED (0x15) collapse, which made every ordinary
       failure — grep no-match, a bad flag, a missing file — read as an
       "Aborted" crash and gave a script comparing `%lasterror% == 1` a value it
       could never match. Consumers needing an exact / EFI_ERROR-detectable
       status still arm one via axl_set_exit_status (the verbatim path above).
       Mask to a byte so a stray large/negative rc can't emit a giant value or
       alias down to 0 (a false success). */
    unsigned code = (unsigned)rc & 0xFFu;
    if (code == 0u) {
        code = 1u;
    }
    return (uint64_t)code;
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
