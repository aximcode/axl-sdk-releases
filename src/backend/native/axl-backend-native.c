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
#include "../../fs/axl-fs-read-chunk.h"  /* AXL_FS_READ_CHUNK */
#include "axl-backend-native-efi1x.h"  /* old EFI 1.x shell path resolution */
#include "axl-backend-native-nosh.h"   /* no shell at all (BDS boot option) */
#include "axl-backend-native-shell.h"  /* modern-shell paths that park nothing */
#include "axl-backend-native-lines.h"  /* axl_text_out_lines */
#include "axl-stdio-bridge.h"  /* AxlStdioBridge, bridge install/uninstall */
#include <axl/axl-driver.h>    /* axl_protocol_install, axl_protocol_uninstall */
#include <axl/axl-sys.h>       /* axl_protocol_find_guid */
#include <axl/axl-atexit.h>    /* axl_atexit */
#include <axl/axl-input.h>     /* AXL_INPUT_MOD_* — read_key_ex normalizes to these */
#include <axl/axl-log.h>
#include <axl/axl-str.h>       /* axl_snprintf */
#include <stdarg.h>

AXL_LOG_DOMAIN("backend");

/* Runaway guard for axl_backend_shell_unmap_device: more names than any one
   device carries. */
#define AXL_UNMAP_MAX_NAMES  32u

/* Longest `name:` volume prefix shell_path_volume_resolves looks up, with
   its NUL. Shell map names are short (`fs0:`, `blk12:`, a consistent name
   such as `HD0a1b:`); a longer prefix is not a mapping. */
#define AXL_SHELL_MAP_NAME_MAX  64u

/* EFI_TIME.TimeZone meaning "local time, zone unknown" (UEFI 2.11 §8.3).
   AXL reads it as UTC, so a time written with it reads back unchanged. */
#define AXL_EFI_TIME_ZONE_UNSPECIFIED  0x07FF

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

static EFI_SHELL_PROTOCOL *
get_shell(void)
{
    /* Re-locate on EVERY call rather than caching the interface pointer.  A cached
       EFI_SHELL_PROTOCOL dangles the moment Shell.efi unloads -- e.g. the user types
       `exit` while a RESIDENT driver's loop is still running (fbcon).  The loop polls
       the shell's Ctrl-C break every dispatch (axl_backend_shell_break_flag ->
       shell->ExecutionBreak), so a stale cached pointer means the next poll derefs a
       freed, poison-filled protocol and #GPs in CoreCheckEvent.  The shell uninstalls
       its protocol on exit, so LocateProtocol then returns an error and callers safely
       see NULL.  LocateProtocol is a cheap handle-DB lookup -- fine on the poll path. */
    EFI_SHELL_PROTOCOL *shell = NULL;
    EFI_GUID            guid  = gEfiShellProtocolGuid;
    if (EFI_ERROR(gBS->LocateProtocol(&guid, NULL, (VOID **)&shell))) {
        return NULL;
    }
    return shell;
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
        axl_text_out_lines(gST, gST->ConOut, &gEfiConsoleOutDeviceGuid, NULL,
                           (const CHAR16 *)str);
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

/* Re-entrancy guard for the RTC service family. UEFI 2.11 section 8.1,
 * Table 8.1 makes GetTime / SetTime / GetWakeupTime / SetWakeupTime mutually
 * exclusive as a GROUP, so one flag covers all of them -- guarding GetTime
 * against only itself would still let a nested SetTime through.
 *
 * SCOPE, stated honestly: this covers the INTERRUPTED case on the BSP, which
 * is the reachable one (a timer notify preempting a caller's own clock read).
 * Section 8.1 also forbids a runtime service that is "active on another
 * processor", and a plain flag cannot cover that -- an AxlTaskProc running on
 * an AP (see axl-task-pool.c) that reads the clock is a caller error under
 * that same rule, and was one before this guard existed.
 *
 * volatile because these are written by one flow of control and read by
 * another that preempts it asynchronously -- the same reason
 * g_axl_interrupted is volatile in axl-signal.c. */
static volatile bool  mRtcBusy      = false;
static volatile bool  mHaveLastTime = false;
static AxlTime        mLastTime;

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

    /* RE-ENTRANCY GUARD. UEFI 2.11 section 8.1, Table 8.1 forbids re-entering
     * GetTime() while a GetTime/SetTime/GetWakeupTime/SetWakeupTime call is
     * busy -- and "busy" explicitly includes INTERRUPTED, not merely
     * in-progress on another agent.
     *
     * That is reachable here, not theoretical: code at TPL_APPLICATION is
     * inside gRT->GetTime(), the timer interrupt fires, a notify function runs
     * at TPL_CALLBACK and logs, and the log handler stamps a timestamp -- so
     * GetTime() is entered while the interrupted call is still open. The spec
     * says the result is undefined; EDK2's PcRtc trips its EfiAcquireLock
     * ASSERT on a DEBUG build.
     *
     * Serve the nested call from the last good reading instead. That value is
     * as old as the last COMPLETED read -- not one call old -- so an app whose
     * only other clock read is infrequent can be served a correspondingly old
     * stamp. Still better than undefined behaviour, and better than failing
     * the nested call, which would drop the timestamp off exactly the log
     * records emitted from notify context.
     *
     * No atomics: notifies are dispatched on the BSP and TPL provides the
     * mutual exclusion, so this is a plain flag on a single flow of control. */
    if (gRT == NULL) {
        return AXL_ERR;
    }

    if (mRtcBusy) {
        if (!mHaveLastTime) {
            return AXL_ERR;
        }
        *time = mLastTime;
        return AXL_OK;
    }

    mRtcBusy = true;
    status = gRT->GetTime(&efi_time, NULL);
    if (EFI_ERROR(status)) {
        mRtcBusy = false;
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

    /* Publish the cache and only THEN release the guard. Clearing the flag
     * first would leave a window in which a notify takes a fresh, newer
     * reading and publishes it, after which this call overwrites the cache
     * with its own OLDER value -- so a later nested read could be stamped
     * earlier than a record already written. Ordering is what prevents a
     * timestamp going backwards. */
    mLastTime     = *time;
    mHaveLastTime = true;
    mRtcBusy      = false;
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

    /* Same family, same exclusion (see axl_backend_get_time). A write cannot
     * be served from cache the way a read can, so a nested call fails rather
     * than silently not setting the clock. */
    if (mRtcBusy) {
        return AXL_ERR;
    }

    mRtcBusy = true;
    status = gRT->SetTime(&efi_time);
    if (!EFI_ERROR(status)) {
        /* The caller just told us what the clock reads, so publish that
         * rather than invalidating -- it leaves no window in which a nested
         * read has nothing to answer with. On FAILURE the clock did not
         * change, so the existing cache is still valid and is kept; throwing
         * it away would punish a rejected write with a dead timestamp. */
        mLastTime     = *time;
        mHaveLastTime = true;
    }
    mRtcBusy = false;
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

/* EFI_TIME -> AxlTime, shared by the clock and alarm readers so the two
   cannot drift in how they map the timezone sentinel. */
static void
efi_time_to_axl(
    const EFI_TIME  *in,
    AxlTime         *out
    )
{
    out->year       = in->Year;
    out->month      = in->Month;
    out->day        = in->Day;
    out->hour       = in->Hour;
    out->minute     = in->Minute;
    out->second     = in->Second;
    out->nanosecond = in->Nanosecond;
    out->daylight   = in->Daylight;
    out->timezone_minutes = (in->TimeZone == 2047)
        ? INT16_MIN
        : (int16_t)in->TimeZone;
}

/**
 * @brief Read the RTC wake alarm.
 */
int
axl_backend_get_wakeup(
    bool     *enabled,
    bool     *pending,
    AxlTime  *when
    )
{
    EFI_TIME    efi_time = { 0 };
    BOOLEAN     efi_enabled = FALSE;
    BOOLEAN     efi_pending = FALSE;
    EFI_STATUS  status;

    if (gRT == NULL) {
        return AXL_ERR;
    }
    /* Same RTC group as GetTime/SetTime (UEFI 2.11 Table 8.1). Unlike a
     * clock read there is no cached answer to fall back on -- an alarm has
     * no last-known-good equivalent -- so a nested call fails outright. */
    if (mRtcBusy) {
        return AXL_ERR;
    }

    mRtcBusy = true;
    status = gRT->GetWakeupTime(&efi_enabled, &efi_pending, &efi_time);
    mRtcBusy = false;

    /* EFI_UNSUPPORTED is a fact about the platform, not a failure: this box
     * has no wake timer. Kept distinct so a caller can say "cannot" rather
     * than "broken". */
    if (status == EFI_UNSUPPORTED) {
        return AXL_UNSUPPORTED;
    }
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    if (enabled != NULL) {
        *enabled = (efi_enabled != FALSE);
    }
    if (pending != NULL) {
        *pending = (efi_pending != FALSE);
    }
    if (when != NULL) {
        efi_time_to_axl(&efi_time, when);
    }
    return AXL_OK;
}

/**
 * @brief Arm (when != NULL) or disarm (when == NULL) the RTC wake alarm.
 */
int
axl_backend_set_wakeup(
    const AxlTime  *when
    )
{
    EFI_TIME    efi_time = { 0 };
    EFI_STATUS  status;

    if (gRT == NULL) {
        return AXL_ERR;
    }
    if (mRtcBusy) {
        return AXL_ERR;
    }

    if (when != NULL) {
        efi_time.Year       = when->year;
        efi_time.Month      = when->month;
        efi_time.Day        = when->day;
        efi_time.Hour       = when->hour;
        efi_time.Minute     = when->minute;
        efi_time.Second     = when->second;
        efi_time.Nanosecond = when->nanosecond;
        efi_time.Daylight   = when->daylight;
        efi_time.TimeZone   = (when->timezone_minutes == INT16_MIN)
            ? 2047
            : when->timezone_minutes;
    }

    mRtcBusy = true;
    /* Disarm passes Enable=FALSE with a NULL time, which the spec permits
     * only in that direction -- an arm with no time is a caller error the
     * firmware would reject anyway. */
    status = (when != NULL)
        ? gRT->SetWakeupTime(TRUE, &efi_time)
        : gRT->SetWakeupTime(FALSE, NULL);
    mRtcBusy = false;

    if (status == EFI_UNSUPPORTED) {
        return AXL_UNSUPPORTED;
    }
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
   - x86_64: calibrate against short gBS->Stall samples on first call
     (short and long Stalls, differenced; see get_counter_freq_hz), then
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

/* TSC calibration: this many pairs of a short and a long Stall, the
   least of each kept and differenced (get_counter_freq_hz says why). Five
   pairs of 100 + 1900 us cost the 10 ms five 2 ms Stalls did. */
#define AXL_TSC_CAL_PAIRS     5u
#define AXL_TSC_CAL_SHORT_US  100u
#define AXL_TSC_CAL_LONG_US   1900u
#endif

/* The MINIMUM non-zero sample, not the mean: every error a sample carries
   is an overshoot (see get_counter_freq_hz), so the smallest is the least
   disturbed, and one bad sample among several costs nothing. */
static uint64_t
least_sample(const uint64_t *ticks, size_t count)
{
    uint64_t least = 0;

    for (size_t i = 0; ticks != NULL && i < count; i++) {
        if (ticks[i] != 0 && (least == 0 || ticks[i] < least)) {
            least = ticks[i];
        }
    }
    return least;
}

/* How far under the long Stall alone the differenced frequency may fall,
   as a divisor: 1/50, 2 %. The long Stall alone overstates by the fixed
   cost every Stall carries -- measured at 0.6 % on an idle virtualised
   host -- and a floor closer than that would put the truth below it and
   undo the differencing. 2 % is over three times that cost, room for it to
   grow under load, and the differenced value read at worst 1.0 % fast on
   a loaded host before the floor existed; a disturbed SHORT Stall can now
   make the clock run at most 2 % fast instead of without bound. */
#define AXL_COUNTER_FREQ_FAST_SLACK_DIV  50u

/* Ticks per @a us microseconds scaled to a second, in two parts so the
   scaling cannot overflow: the remainder is below @a us, and @a us is
   microseconds. */
static uint64_t
ticks_per_second(uint64_t ticks, uint64_t us)
{
    return ticks / us * 1000000u + ticks % us * 1000000u / us;
}

uint64_t
axl_backend_counter_freq_from_samples(const uint64_t *short_ticks,
                                      const uint64_t *long_ticks,
                                      size_t count, uint64_t short_us,
                                      uint64_t long_us)
{
    uint64_t least_short = least_sample(short_ticks, count);
    uint64_t least_long  = least_sample(long_ticks, count);

    if (least_long == 0 || short_us >= long_us) {
        return 0;
    }
    /* The long Stall alone can only OVERSTATE the frequency: its fixed cost
       and any overshoot both add ticks. An overshoot on the short Stall
       errs the other way -- the difference shrinks and the clock runs fast,
       without bound if every short sample was disturbed -- so the
       difference is kept between that ceiling and a floor just under it
       (AXL_COUNTER_FREQ_FAST_SLACK_DIV). No short sample leaves the
       ceiling, and a short sample no smaller than the long the floor --
       never 0. */
    uint64_t ceiling = ticks_per_second(least_long, long_us);
    uint64_t lowest  = ceiling - ceiling / AXL_COUNTER_FREQ_FAST_SLACK_DIV;
    uint64_t diff    = 0;
    if (least_long > least_short) {
        diff = ticks_per_second(least_long - least_short, long_us - short_us);
    }
    if (diff > ceiling) {
        return ceiling;
    }
    return diff < lowest ? lowest : diff;
}

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

    /* First publisher: calibrate against gBS->Stall, AXL_TSC_CAL_PAIRS
       pairs of a short and a long Stall, keeping the least of each. A Stall
       never returns early -- it polls a reference timer until at least the
       time asked for has passed -- so every error a sample carries is an
       OVERSHOOT: the vCPU descheduled in or around it, an SMI, a long
       interrupt. An overshoot counts extra ticks, overestimates the
       frequency, and makes this clock run SLOW for the rest of the boot,
       since the value is published below and every later process adopts
       it. One 10 ms sample was measured at 0.77x, and once 0.13x, real time
       on a loaded virtualised host, which stretched every bound timed on
       this clock (login grace, penalties, hook budgets) by the same factor.
       Linux calibrates the TSC the same way for the same reason: several
       passes against the PIT and the HPET/PM timer, keeping the minimum
       (pit_hpet_ptimer_calibrate_cpu, arch/x86/kernel/tsc.c).

       Even the least sample carries what EVERY Stall costs, whatever its
       length: the call and the reference timer's reads, each a VM exit
       when virtualised. Read off one length, that cost alone left the
       clock 0.6 % slow on an idle virtualised host -- enough that a timer
       firing at a period's end read the clock just short of it, every
       time. The frequency is therefore the DIFFERENCE of the least long
       and the least short sample over the difference of their lengths,
       which cancels it (axl_backend_counter_freq_from_samples). The short
       Stall is short so that it is rarely disturbed itself: an overshoot
       there makes the clock run fast, which the long Stall alone bounds to
       2 % (AXL_COUNTER_FREQ_FAST_SLACK_DIV). */
    uint64_t short_ticks[AXL_TSC_CAL_PAIRS];
    uint64_t long_ticks[AXL_TSC_CAL_PAIRS];
    for (size_t i = 0; i < AXL_TSC_CAL_PAIRS; i++) {
        uint64_t before = read_cycle_counter();
        gBS->Stall(AXL_TSC_CAL_SHORT_US);
        uint64_t mid = read_cycle_counter();
        gBS->Stall(AXL_TSC_CAL_LONG_US);
        long_ticks[i]  = read_cycle_counter() - mid;
        short_ticks[i] = mid - before;
    }
    uint64_t freq = axl_backend_counter_freq_from_samples(
        short_ticks, long_ticks, AXL_TSC_CAL_PAIRS, AXL_TSC_CAL_SHORT_US,
        AXL_TSC_CAL_LONG_US);
    if (freq == 0) {
        return 0;   /* no usable counter */
    }

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

   @p tz_minutes is EFI_TIME.TimeZone's value -- UEFI 2.11 section 8.3:
   Localtime = UTC - TimeZone, so UTC = local + TimeZone (EDK2's CryptoPkg
   TimerWrapper adds it the same way) -- or INT16_MIN to mean "unspecified,
   treat as already-UTC". EFI's 0x07FF sentinel must be translated to
   INT16_MIN by the caller (axl_backend_get_time does this for AxlTime;
   axl_backend_efi_time_to_unix does it inline). */
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
        secs += (int64_t)tz_minutes * 60;
    }
    return secs;
}

uint64_t
axl_backend_counter_ticks(void)
{
#if defined(__x86_64__) || defined(__aarch64__)
    return read_cycle_counter();
#else
    return 0;
#endif
}

uint64_t
axl_backend_counter_hz(void)
{
#if defined(__x86_64__) || defined(__aarch64__)
    return get_counter_freq_hz();
#else
    return 0;
#endif
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

/* True when NEITHER shell is present — a BDS boot option, a DXE driver on
   minimal firmware. This is the ONLY condition under which the positional
   fs<n> namespace may be used: it is measured to diverge from what a shell
   calls the same volume, so handing a positional name to a live shell's
   OpenFileByName would resolve a DIFFERENT volume. */
static bool
no_shell_at_all(
    void
    )
{
    return get_shell() == NULL && axl_efi1x_shell_env() == NULL;
}

/* Resolve + open @p path with no EFI_SHELL_PROTOCOL: through the old EFI 1.x
   shell's map when that shell is live, else straight off the firmware's
   SimpleFileSystem handles (a BDS boot option, where no shell exists at all).

   Both produce an EFI_FILE_PROTOCOL *, which is the same handle shape a
   SHELL_FILE_HANDLE has on the modern shell — so every handle-based op below
   stays shell-agnostic. Shared by the open / delete / mkdir entry points,
   which differ only in the mode and attributes they ask for.

   @return AXL_OK with @p out set; AXL_NOT_FOUND when the volume resolved
       and the file is absent; AXL_ERR otherwise. */
static int
no_shell_file_open(
    const unsigned short  *path,
    uint64_t               mode,
    uint64_t               attributes,
    EFI_FILE_PROTOCOL    **out
    )
{
    if (!no_shell_at_all()) {
        return axl_efi1x_file_open(path, mode, attributes, out);
    }
    return axl_nosh_file_open(path, mode, attributes, out);
}

/* Does the volume @p path names resolve under the modern shell? A `name:`
   prefix (before any separator) must be a live mapping; a path without one
   resolves against the current directory, which must exist. The shell's
   OpenFileByName answers EFI_NOT_FOUND for an unmapped volume exactly as
   for an absent file, and only this tells the two apart. */
static bool
shell_path_volume_resolves(
    EFI_SHELL_PROTOCOL    *shell,
    const unsigned short  *path
    )
{
    size_t colon = 0;
    bool   found = false;
    for (size_t i = 0; path[i] != 0; i++) {
        if (path[i] == (unsigned short)'\\' || path[i] == (unsigned short)'/') {
            break;
        }
        if (path[i] == (unsigned short)':') {
            colon = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return axl_shell_cwd(shell) != NULL;
    }
    if (colon == 0 || colon + 2 > AXL_SHELL_MAP_NAME_MAX) {
        return false;
    }
    unsigned short name[AXL_SHELL_MAP_NAME_MAX];
    for (size_t i = 0; i <= colon; i++) {
        name[i] = path[i];
    }
    name[colon + 1] = 0;
    return axl_backend_shell_map_exists(name);
}

/* @p path, or -- when it holds a '/' -- a copy with every '/' turned into
   '\'. A UEFI path separates with '\' only: the modern shell's
   OpenFileByName fails "fs0:/x" outright and the EFI 1.x shell's map
   lookup splits on '\' alone, so every entry point that takes a path
   converts here, before the shell-mode dispatch, and no name can hold a
   '/' to lose (FAT refuses it). *@p copy receives the allocation, NULL when
   none was needed; the caller frees it on every exit.

   @return the path to use, or NULL when the copy could not be allocated. */
static const unsigned short *
path_backslashed(
    const unsigned short  *path,
    unsigned short       **copy
    )
{
    size_t n     = 0;
    bool   slash = false;

    *copy = NULL;
    for (; path[n] != 0; n++) {
        if (path[n] == (unsigned short)'/') {
            slash = true;
        }
    }
    if (!slash) {
        return path;
    }
    unsigned short *c = axl_backend_alloc((n + 1) * sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    for (size_t i = 0; i <= n; i++) {
        c[i] = (path[i] == (unsigned short)'/') ? (unsigned short)'\\' : path[i];
    }
    *copy = c;
    return c;
}

/* Is @p path a name EFI_SHELL_PROTOCOL.OpenFileByName answers itself
   rather than resolving to a file (ShellProtocol.c EfiShellOpenFileByName):
   the standard streams `>i` `>o` `>e`, the null file `NUL`/`NULL` (any
   case), and a shell variable `>v<name>`. None of them parks a buffer, so
   they stay on the Shell. */
static bool
shell_special_name(
    const unsigned short  *path
    )
{
    static const char *const nul[] = { "NUL", "NULL" };

    if (path[0] == (unsigned short)'>') {
        unsigned short c = path[1];
        return (c == 'v')
               || ((c == 'i' || c == 'o' || c == 'e') && path[2] == 0);
    }
    for (size_t k = 0; k < sizeof(nul) / sizeof(nul[0]); k++) {
        size_t i = 0;
        while (nul[k][i] != '\0'
               && (path[i] & ~(unsigned short)0x20) == (unsigned short)nul[k][i]) {
            i++;
        }
        if (nul[k][i] == '\0' && path[i] == 0) {
            return true;
        }
    }
    return false;
}

/* EFI_SHELL_PROTOCOL.OpenFileByName, minus the buffer it parks on every
   call: a regular file is resolved and opened AXL-side exactly as the Shell
   would (axl-backend-native-shell.h); the names the Shell answers itself
   still go to it. */
static EFI_STATUS
shell_open_by_name(
    EFI_SHELL_PROTOCOL    *shell,
    const unsigned short  *path,
    uint64_t               mode,
    SHELL_FILE_HANDLE     *fh
    )
{
    if (shell_special_name(path)) {
        return shell->OpenFileByName((CHAR16 *)path, fh, mode);
    }
    return axl_shell_path_open(shell, (const CHAR16 *)path, mode, 0,
                               (EFI_FILE_PROTOCOL **)fh);
}

/* axl_backend_file_open on a path whose separators are all '\'. */
static int
file_open_backslashed(
    const unsigned short  *path,
    uint64_t               mode,
    uint64_t               attributes,
    AxlFileHandle         *handle
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        /* No modern shell: resolve + open natively (old shell's map, or
           the raw SimpleFileSystem handles when there is no shell at all).
           The resulting EFI_FILE_PROTOCOL* IS the handle — the same shape a
           SHELL_FILE_HANDLE has on the modern shell, so the handle-based
           ops below stay shell-agnostic. */
        EFI_FILE_PROTOCOL *file = NULL;
        int rc = no_shell_file_open(path, mode, attributes, &file);
        if (rc != AXL_OK) {
            return rc;   /* AXL_NOT_FOUND passes through */
        }
        *handle = (AxlFileHandle)file;
        return AXL_OK;
    }

    status = shell_open_by_name(shell, path, mode, &fh);
    if (EFI_ERROR(status)) {
        /* EFI_NOT_FOUND is the normal "file doesn't exist" case and
         * floods the log when callers probe many candidate paths
         * (e.g., axl_driver_locate walking every mounted volume).
         * Real errors — media, permissions — still surface. */
        if (status != EFI_NOT_FOUND) {
            axl_debug("file open failed: status=0x%llx",
                      (unsigned long long)status);
            return AXL_ERR;
        }
        return shell_path_volume_resolves(shell, path) ? AXL_NOT_FOUND
                                                       : AXL_ERR;
    }
    *handle = (AxlFileHandle)fh;
    return AXL_OK;
}

/**
 * @brief Open a file by UCS-2 path; either separator.
 *
 * @return AXL_OK on success; AXL_NOT_FOUND when the volume resolved and the
 *     file system reported no such file; AXL_ERR on any other failure.
 */
int
axl_backend_file_open(
    const unsigned short  *path,        ///< UCS-2 file path
    uint64_t               mode,        ///< EFI file mode flags
    uint64_t               attributes,  ///< EFI file attributes
    AxlFileHandle         *handle       ///< (out) receives file handle
    )
{
    unsigned short *copy = NULL;

    if (path == NULL || handle == NULL) {
        return AXL_ERR;
    }
    const unsigned short *p = path_backslashed(path, &copy);
    if (p == NULL) {
        return AXL_ERR;
    }
    int rc = file_open_backslashed(p, mode, attributes, handle);
    axl_backend_free(copy);
    return rc;
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

int
axl_backend_dir_read_entry(
    AxlFileHandle  handle,
    size_t        *size,
    void          *buf
    )
{
    if (handle == NULL || size == NULL || buf == NULL) {
        return AXL_ERR;
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    UINTN               usize = *size;
    EFI_STATUS          status;
    if (shell != NULL) {
        status = shell->ReadFile((SHELL_FILE_HANDLE)handle, &usize, buf);
    } else {
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
        status = file->Read(file, &usize, buf);
    }
    if (status == EFI_BUFFER_TOO_SMALL) {
        *size = usize;
        return AXL_NO_RESOURCES;
    }
    if (EFI_ERROR(status)) {
        *size = 0;
        return AXL_ERR;
    }
    *size = usize;
    return AXL_OK;
}

/**
 * @brief Read from a file, at most AXL_FS_READ_CHUNK per firmware call.
 *
 * The contract (the short-read stop, partial success before an error,
 * *size on error) is in axl-backend.h; the chunk size and why are in
 * axl-fs-read-chunk.h.
 *
 * @return AXL_OK on success, including a partial read cut short by an
 *     error; AXL_ERR when the first call fails.
 */
int
axl_backend_file_read(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to read / bytes read
    void          *buf      ///< destination buffer
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status = EFI_SUCCESS;
    size_t               want;
    size_t               got = 0;

    if (handle == NULL || size == NULL || buf == NULL) {
        return AXL_ERR;
    }

    want  = *size;
    shell = get_shell();
    while (got < want) {
        UINTN ask   = (want - got < AXL_FS_READ_CHUNK) ? want - got
                                                       : AXL_FS_READ_CHUNK;
        UINTN usize = ask;
        if (shell != NULL) {
            status = shell->ReadFile((SHELL_FILE_HANDLE)handle, &usize,
                                     (uint8_t *)buf + got);
        } else {
            EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
            status = file->Read(file, &usize, (uint8_t *)buf + got);
        }
        if (EFI_ERROR(status)) {
            break;
        }
        got += usize;
        if (usize < ask) {
            break;
        }
    }
    /* Bytes already read are the caller's even when a later chunk fails:
       the file position has moved past them, so dropping them here would
       lose them for good. The next call retries from the failing chunk and
       reports the error if the fault recurs (the POSIX read() contract;
       nothing here is sticky). */
    *size = got;
    return (EFI_ERROR(status) && got == 0) ? AXL_ERR : AXL_OK;
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
 * @brief Flush a file handle's pending data through to the volume.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_flush(
    AxlFileHandle  handle  ///< file handle (open for write)
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    if (handle == NULL) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell != NULL) {
        status = shell->FlushFile((SHELL_FILE_HANDLE)handle);
    } else {
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)handle;
        status = file->Flush(file);
    }
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
    AxlFileHandle h = NULL;
    int           rc;

    if (path == NULL) {
        return AXL_ERR;
    }

    /* Open-then-delete, NOT shell->DeleteFileByName().
     *
     * EDK2's EfiShellDeleteFileByName opens its target with
     * EfiShellCreateFile (ShellPkg/Application/Shell/ShellProtocol.c), so
     * deleting a name that does not exist CREATED a zero-length file,
     * deleted it, and honestly reported EFI_SUCCESS: the caller could not
     * tell "removed" from "never there", and a pure cleanup call performed
     * a WRITE. axl_backend_file_open never creates without CREATE, on the
     * modern shell, the old shell and with no shell at all, and the delete
     * goes through the one handle routine that reads the firmware's
     * "closed but not deleted" warning as the failure it is -- which the
     * no-shell branch here used to read as success. */
    rc = axl_backend_file_open(path, AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE,
                               0, &h);
    if (rc != AXL_OK) {
        return (rc == AXL_NOT_FOUND) ? AXL_NOT_FOUND : AXL_ERR;
    }
    return axl_backend_file_handle_delete(&h);
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

int
axl_backend_file_handle_is_dir(
    AxlFileHandle   handle,
    bool           *is_dir
    )
{
    if (handle == NULL || is_dir == NULL) {
        return AXL_ERR;
    }
    EFI_FILE_INFO *info = native_file_info(handle);
    if (info == NULL) {
        return AXL_ERR;
    }
    *is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
    axl_backend_free(info);
    return AXL_OK;
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

/* Append @p n characters of @p src to the volume-relative path in @p rel,
   normalizing as it goes: either separator becomes '\', a run of them one,
   and none leads. A trailing one is the caller's to strip.

   @return false when the result would not fit in @p cap characters. */
static bool
vol_path_append(
    unsigned short       *rel,
    size_t                cap,
    size_t               *len,
    const unsigned short *src,
    size_t                n
    )
{
    for (size_t i = 0; i < n; i++) {
        unsigned short c = src[i];
        if (c == (unsigned short)'/' || c == (unsigned short)'\\') {
            if (*len == 0 || rel[*len - 1] == (unsigned short)'\\') {
                continue;
            }
            c = (unsigned short)'\\';
        }
        if (*len + 1 >= cap) {
            return false;
        }
        rel[(*len)++] = c;
    }
    rel[*len] = 0;
    return true;
}

/* The modern shell: the volume and remainder GetDevicePathFromFilePath
   would resolve the path to (the map name in any case and any alias, or
   the current directory), without opening anything -- and without the
   buffers that call parks (axl-backend-native-shell.h). The remainder is
   the one file-path node the Shell would build, normalized here. */
static int
shell_path_locate(
    EFI_SHELL_PROTOCOL    *shell,
    const unsigned short  *path,
    void                 **volume,
    unsigned short        *rel,
    size_t                 rel_cap
    )
{
    AxlShellPath sp;
    if (axl_shell_path_resolve(shell, (const CHAR16 *)path, &sp) != AXL_OK) {
        return AXL_ERR;
    }

    static const unsigned short sep = (unsigned short)'\\';
    size_t len = 0;
    int    rc  = AXL_OK;
    rel[0] = 0;
    if (!vol_path_append(rel, rel_cap, &len, &sep, 1)
        || !vol_path_append(rel, rel_cap, &len, sp.rest,
                            axl_backend_wcslen(sp.rest))) {
        rc = AXL_ERR;
    }
    while (rc == AXL_OK && len > 0 && rel[len - 1] == (unsigned short)'\\') {
        rel[--len] = 0;
    }
    if (rc == AXL_OK) {
        *volume = sp.volume;
    }
    axl_shell_path_done(&sp);
    return rc;
}

int
axl_backend_path_locate(
    const unsigned short  *path,
    void                 **volume,
    unsigned short        *rel,
    size_t                 rel_cap
    )
{
    EFI_SHELL_PROTOCOL *shell;
    unsigned short     *copy = NULL;
    int                 rc;

    if (path == NULL || volume == NULL || rel == NULL || rel_cap == 0) {
        return AXL_ERR;
    }
    *volume = NULL;
    rel[0]  = 0;

    const unsigned short *p = path_backslashed(path, &copy);
    if (p == NULL) {
        return AXL_ERR;
    }
    shell = get_shell();
    if (shell != NULL) {
        rc = shell_path_locate(shell, p, volume, rel, rel_cap);
    } else if (!no_shell_at_all()) {
        rc = axl_efi1x_path_locate(p, volume, rel, rel_cap);
    } else {
        rc = axl_nosh_path_locate(p, volume, rel, rel_cap);
    }
    axl_backend_free(copy);
    return rc;
}

int
axl_backend_file_handle_delete(
    AxlFileHandle  *handle
    )
{
    EFI_SHELL_PROTOCOL *shell;
    EFI_STATUS          status;

    if (handle == NULL || *handle == NULL) {
        return AXL_ERR;
    }
    shell = get_shell();
    if (shell != NULL) {
        /* DeleteFile closes the handle whatever it answers. */
        status = shell->DeleteFile((SHELL_FILE_HANDLE)*handle);
    } else {
        EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)*handle;
        status = file->Delete(file);
    }
    *handle = NULL;
    /* EFI_WARN_DELETE_FAILURE is a warning EFI_ERROR() reads as success:
       the handle closed, the file survived. Test for success outright. */
    return (status == EFI_SUCCESS) ? AXL_OK : AXL_ERR;
}

int
axl_backend_file_handle_rename(
    AxlFileHandle          handle,
    const unsigned short  *file_name
    )
{
    EFI_FILE_INFO *info;
    EFI_FILE_INFO *new_info;
    EFI_STATUS     status;
    size_t         new_len;
    size_t         info_size;

    if (handle == NULL || file_name == NULL) {
        return AXL_ERR;
    }

    info = native_file_info(handle);
    if (info == NULL) {
        return AXL_ERR;
    }

    /* The current metadata, times included, with the name replaced. The
       times are the entry's own, so the driver writes back what it has.

       Sized EXACTLY: the fixed fields up to FileName, then the name and its
       NUL, nothing after. sizeof(EFI_FILE_INFO) is not that -- it counts
       FileName[1] and the struct's tail padding, 8 bytes past offset 80 --
       and OVMF's VirtioFsDxe (SimpleFsSetInfo.c ValidateInfoStructure)
       takes everything after FileName's offset as the name field and
       requires its LAST CHAR16 to be NUL. The old sizing left 8 bytes of
       uninitialised pool there, so every rename on a virtiofs volume was
       EFI_INVALID_PARAMETER. EnhancedFatDxe does not check, which hid it. */
    for (new_len = 0; file_name[new_len] != 0; new_len++) {}
    size_t fixed = SIZE_OF_EFI_FILE_INFO;
    info_size = fixed + (new_len + 1) * sizeof(CHAR16);
    new_info  = (EFI_FILE_INFO *)axl_backend_alloc(info_size);
    if (new_info == NULL) {
        axl_backend_free(info);
        return AXL_ERR;
    }
    /* The fixed fields only: the buffer may be shorter than
       sizeof(EFI_FILE_INFO) for a one-character name. */
    for (size_t i = 0; i < fixed; i++) {
        ((UINT8 *)new_info)[i] = ((const UINT8 *)info)[i];
    }
    new_info->Size = info_size;
    for (size_t i = 0; i <= new_len; i++) {
        new_info->FileName[i] = (CHAR16)file_name[i];
    }

    status = native_set_file_info(handle, new_info);
    axl_backend_free(new_info);
    axl_backend_free(info);

    if (status == EFI_UNSUPPORTED) {
        return AXL_UNSUPPORTED;
    }
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

/* axl_backend_file_mkdir on a path whose separators are all '\'. */
static int
file_mkdir_backslashed(
    const unsigned short  *path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        /* No modern shell: Open with CREATE|DIRECTORY makes the directory
           (and opens it); close the handle. */
        EFI_FILE_PROTOCOL *dir = NULL;
        if (no_shell_file_open(path,
                               AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE
                                   | AXL_FILE_MODE_CREATE,
                               EFI_FILE_DIRECTORY, &dir) != AXL_OK) {
            return AXL_ERR;
        }
        dir->Close(dir);
        return AXL_OK;
    }

    /* EFI_SHELL_PROTOCOL.CreateFile, minus the buffer it parks: only a
       `>v` shell-variable name is the Shell's own; a file is opened
       READ|WRITE|CREATE with the attributes, as CreateFile opens it. */
    if (path[0] == (unsigned short)'>' && path[1] == (unsigned short)'v') {
        status = shell->CreateFile((CHAR16 *)path, EFI_FILE_DIRECTORY, &fh);
    } else {
        status = axl_shell_path_open(shell, (const CHAR16 *)path,
                                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE
                                         | EFI_FILE_MODE_CREATE,
                                     EFI_FILE_DIRECTORY,
                                     (EFI_FILE_PROTOCOL **)&fh);
    }
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    shell->CloseFile(fh);
    return AXL_OK;
}

int
axl_backend_file_mkdir(
    const unsigned short  *path
    )
{
    unsigned short *copy = NULL;

    if (path == NULL) {
        return AXL_ERR;
    }
    const unsigned short *p = path_backslashed(path, &copy);
    if (p == NULL) {
        return AXL_ERR;
    }
    int rc = file_mkdir_backslashed(p);
    axl_backend_free(copy);
    return rc;
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

/* Inverse of days_from_civil (Howard Hinnant's civil_from_days, public
   domain): days since 1970-01-01 to a Gregorian year/month/day, for the
   full proleptic Gregorian range. The one copy: axl_time_set_unix and the
   file-time writer below both convert through it. */
void
axl_backend_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era  = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                  // [0, 146096]
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);       // [0, 365]
    unsigned mp  = (5 * doy + 2) / 153;                           // [0, 11]
    *d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
    *m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
    *y = (int)((int64_t)yoe + era * 400 + (*m <= 2));
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

void
axl_backend_unix_to_efi_time(uint64_t secs, void *efi_time_16)
{
    int       y;
    unsigned  mo, d;
    uint64_t  rem = secs % 86400u;
    EFI_TIME *out = efi_time_16;

    axl_backend_civil_from_days((int64_t)(secs / 86400u), &y, &mo, &d);
    *out = (EFI_TIME){
        .Year       = (UINT16)y,
        .Month      = (UINT8)mo,
        .Day        = (UINT8)d,
        .Hour       = (UINT8)(rem / 3600u),
        .Minute     = (UINT8)((rem % 3600u) / 60u),
        .Second     = (UINT8)(rem % 60u),
        .Nanosecond = 0,
        .TimeZone   = AXL_EFI_TIME_ZONE_UNSPECIFIED,
        .Daylight   = 0,
    };
}

int
axl_backend_file_handle_stat(
    AxlFileHandle   handle,
    uint64_t       *size,
    uint64_t       *alloc_size,
    uint64_t       *mtime_unix,
    bool           *is_dir,
    bool           *read_only,
    unsigned short *name,
    size_t          name_cap,
    bool           *name_cut
    )
{
    EFI_FILE_INFO *info = native_file_info(handle);

    if (name_cut != NULL) {
        *name_cut = false;
    }

    if (info == NULL) {
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
    /* A name longer than @p name is CUT, at a character boundary -- never
       between the halves of a surrogate pair -- rather than failing the
       whole stat. A caller that must compare the full name passes a buffer
       for the longest name it accepts. */
    if (name != NULL && name_cap > 0) {
        const unsigned short *src = (const unsigned short *)info->FileName;
        size_t n = 0;
        while (n + 1 < name_cap && src[n] != 0) {
            name[n] = src[n];
            n++;
        }
        if (src[n] != 0 && n > 0 && name[n - 1] >= 0xD800u
            && name[n - 1] <= 0xDBFFu) {
            n--;   /* a high surrogate whose low half did not fit */
        }
        if (name_cut != NULL) {
            *name_cut = (src[n] != 0);
        }
        name[n] = 0;
    }
    axl_backend_free(info);
    return AXL_OK;
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
    AxlFileHandle handle = NULL;
    int           rc;

    if (path == NULL) {
        return AXL_ERR;
    }
    if (axl_backend_file_open(path, AXL_FILE_MODE_READ, 0, &handle) != AXL_OK) {
        return AXL_ERR;
    }
    rc = axl_backend_file_handle_stat(handle, size, alloc_size, mtime_unix,
                                      is_dir, read_only, NULL, 0, NULL);
    axl_backend_file_close(&handle);
    return rc;
}

int
axl_backend_file_set_mtime(
    AxlFileHandle  handle,
    uint64_t       mtime_unix
    )
{
    EFI_FILE_INFO *info;
    EFI_STATUS     status;

    if (handle == NULL) {
        return AXL_ERR;
    }
    info = native_file_info(handle);
    if (info == NULL) {
        return AXL_ERR;
    }
    /* A zero time means "keep it" to the EDK2-derived FAT driver (Info.c),
       so only the modification time is changed. Size, attributes and the
       name go back exactly as read, which makes this neither a resize nor
       a rename. */
    info->CreateTime     = (EFI_TIME){ 0 };
    info->LastAccessTime = (EFI_TIME){ 0 };
    axl_backend_unix_to_efi_time(mtime_unix, &info->ModificationTime);
    status = native_set_file_info(handle, info);
    axl_backend_free(info);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
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

    if (name == NULL || name[0] == 0) {
        return AXL_ERR;
    }

    shell = get_shell();
    if (shell == NULL) {
        /* Old EFI 1.x shell: no SetEnv member — drive the shell's own `set`
           command through Execute (the mkrd `map -r` pattern). */
        return axl_efi1x_setenv(name, value, volatile_var);
    }

    /* An empty value deletes. EDK2's SetEnv returns EFI_NOT_FOUND for a var
       that is not set (its own header says "no action"); deleting what is not
       there succeeds, as POSIX unsetenv and the old-shell path do. */
    if ((value == NULL || value[0] == 0) &&
        shell->GetEnv((CONST CHAR16 *)name) == NULL) {
        return AXL_OK;
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
 * this is the captured LHS output stream. Read LIVE on every call, never
 * cached: the Shell uninstalls the protocol and closes a redirect's file
 * when the launch command ends, and an image that writes after that (a
 * resident one) must not use a handle the Shell has freed.
 *
 * @return SHELL_FILE_HANDLE cast as AxlFileHandle, or NULL when the
 *     shell-params protocol isn't published on this image (cross-
 *     volume launches, BDS contexts, non-Shell-2.0 launches).
 */
static SHELL_FILE_HANDLE  mShellStdIn        = NULL;
static SHELL_FILE_HANDLE  mShellStdOut       = NULL;
static SHELL_FILE_HANDLE  mShellStdErr       = NULL;

/* Shared probe -- looks up EFI_SHELL_PARAMETERS_PROTOCOL on this image and
   sets StdIn, StdOut and StdErr from it, or all three NULL when it is not
   (or no longer) there. One HandleProtocol per call. */
static void
probe_shell_std_handles(void)
{
    mShellStdIn  = NULL;
    mShellStdOut = NULL;
    mShellStdErr = NULL;
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

const AxlGuid AXL_STDIO_BRIDGE2_GUID =
    AXL_GUID(0xce4cacac, 0xe9c8, 0x4efc,
             0x92, 0x6b, 0x66, 0xbe, 0x58, 0x76, 0xdb, 0x55);

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

/* This launcher's bridge: installed as itself under AXL_STDIO_BRIDGE2_GUID
   and, for drivers built before it, its `base` under AXL_STDIO_BRIDGE_GUID,
   both on mBridgeHandle. */
static AxlStdioBridge2 mBridge;
static AxlHandle       mBridgeHandle  = NULL;   /* install handle; NULL = not installed */

/* Driver side: the interfaces THIS image published for launchers to dispatch
   through (axl_shared_driver_publish). A bridge aimed elsewhere is not ours. */
static const void     *mBridgeTargets[AXL_BRIDGE_TARGETS_MAX];
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

/* The bridge under @p guid on @p handle as the AxlStdioBridge every layout
   starts its liveness fields with: the interface itself under the original
   GUID, its `base` under BRIDGE2. NULL when there is none. */
static AxlStdioBridge *
bridge_base_on(
    EFI_HANDLE   handle,
    EFI_GUID    *guid,
    bool         v2,
    void       **iface
    )
{
    *iface = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(handle, guid, iface)) || *iface == NULL) {
        return NULL;
    }
    return v2 ? &((AxlStdioBridge2 *)*iface)->base : (AxlStdioBridge *)*iface;
}

/* Uninstall every installed bridge instance whose launcher image has exited.
   Cross-image and best-effort: it enumerates the whole DB, so a live image can
   reap bridges leaked by launchers that skipped their atexit uninstall
   (--minimal-runtime / gBS->Exit). UninstallProtocolInterface matches the
   (guid, iface) pointer and never dereferences the freed interface, so a stale
   instance is safe to drop. */
static void
bridge_reap_dead_of(
    const AxlGuid  *which,
    bool            v2
    )
{
    EFI_GUID    guid    = *(EFI_GUID *)which;
    UINTN       count   = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(
            ByProtocol, &guid, NULL, &count, &handles))
        || count == 0 || handles == NULL) {
        return;
    }
    for (UINTN i = 0; i < count; i++) {
        void           *iface = NULL;
        AxlStdioBridge *b     = bridge_base_on(handles[i], &guid, v2, &iface);
        if (b != NULL && !bridge_launcher_alive(b)) {
            gBS->UninstallProtocolInterface(handles[i], &guid, iface);
        }
    }
    gBS->FreePool(handles);
}

/* Both layouts: an older launcher installs the original only, a newer one
   both, and either can leak. */
static void
bridge_reap_dead(void)
{
    bridge_reap_dead_of(&AXL_STDIO_BRIDGE2_GUID, true);
    bridge_reap_dead_of(&AXL_STDIO_BRIDGE_GUID, false);
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

bool
axl_backend_stdio_bridge_target(
    const void  *iface,
    bool         published
    )
{
    if (iface == NULL) {
        return true;
    }
    for (size_t i = 0; i < AXL_BRIDGE_TARGETS_MAX; i++) {
        if (published ? mBridgeTargets[i] == NULL : mBridgeTargets[i] == iface) {
            mBridgeTargets[i] = published ? iface : NULL;
            return true;
        }
    }
    return !published;
}

/* Whether @p target is an interface this image published. */
static bool
bridge_target_is_mine(
    const void  *target
    )
{
    for (size_t i = 0; target != NULL && i < AXL_BRIDGE_TARGETS_MAX; i++) {
        if (mBridgeTargets[i] == target) {
            return true;
        }
    }
    return false;
}

int
axl_backend_stdio_bridge_install(
    const void  *target
    )
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
    axl_backend_stdio_bridge_uninstall();
    mBridge.size                = (uint32_t)sizeof(mBridge);
    mBridge.base.stdin_h        = in;
    mBridge.base.stdout_h       = out;
    mBridge.base.stderr_h       = err;
    mBridge.base.launcher_image = (void *)gImageHandle;
    mBridge.base.token          = t;
    mBridge.base.pending_status = 0;
    mBridge.base.has_pending    = false;
    mBridge.target              = target;
    /* The published interfaces are &mBridge and &mBridge.base — a static in
       THIS (launcher) image. They MUST be uninstalled before the image
       unloads, or the firmware protocol DB keeps a dangling pointer into
       freed image memory. The bridge_atexit handler below guarantees that:
       CRT0 runs atexit (-> uninstall) before returning the launcher image to
       firmware. BRIDGE2 first: it is the one a driver of this age reads. */
    if (axl_protocol_install(&AXL_STDIO_BRIDGE2_GUID, &mBridge, &mBridgeHandle)
        != AXL_OK) {
        mBridgeHandle = NULL;
        return AXL_ERR;
    }
    if (axl_protocol_install(&AXL_STDIO_BRIDGE_GUID, &mBridge.base, &mBridgeHandle)
        != AXL_OK) {
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE2_GUID, &mBridge);
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
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE_GUID, &mBridge.base);
        axl_protocol_uninstall(mBridgeHandle, &AXL_STDIO_BRIDGE2_GUID, &mBridge);
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
   (driver) path. Returns the newest LIVE bridge AIMED AT THIS IMAGE (its
   target is an interface this image published), or NULL: a launcher lends
   its handles to the driver it dispatches into, never to another resident
   image writing on its own clock. @p reap first drops dead instances
   (self-heal for leaked bridges; the stdin path); without it the lookup only
   reads -- no firmware call at all in an image that published nothing, one
   LocateProtocol and out when no dispatch is current -- which is what the
   per-line stdout/stderr path uses. Liveness is the token
   compare on mapped memory either way, so a dead instance never matches.
   Only AXL_STDIO_BRIDGE2_GUID is read, and `target` only when the
   launcher's `size` covers it: an older launcher's original-layout bridge
   ends where `target` would begin. */
static AxlStdioBridge2 *
bridge_find_live(
    bool  reap
    )
{
    /* First, with no firmware call: an image that published nothing to
       dispatch into -- an application, a service -- reads no bridge. */
    bool aimed = false;
    for (size_t i = 0; i < AXL_BRIDGE_TARGETS_MAX && !aimed; i++) {
        aimed = mBridgeTargets[i] != NULL;
    }
    if (!aimed) {
        return NULL;
    }
    AxlDispatchToken *cell = dispatch_cell();
    if (cell == NULL || cell->current == 0) {
        return NULL;
    }
    if (reap) {
        bridge_reap_dead();
    }

    EFI_GUID    guid    = *(EFI_GUID *)&AXL_STDIO_BRIDGE2_GUID;
    UINTN       count   = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(
            ByProtocol, &guid, NULL, &count, &handles))
        || count == 0 || handles == NULL) {
        return NULL;
    }
    AxlStdioBridge2 *live = NULL;
    for (UINTN i = 0; i < count; i++) {
        void           *iface = NULL;
        AxlStdioBridge *base  = bridge_base_on(handles[i], &guid, true, &iface);
        AxlStdioBridge2 *b    = iface;
        if (base != NULL && bridge_launcher_alive(base)
            && b->size >= sizeof(AxlStdioBridge2)
            && bridge_target_is_mine(b->target)) {
            live = b;   /* DB order is oldest-first; newest live wins */
        }
    }
    gBS->FreePool(handles);
    return live;
}

static AxlFileHandle
bridge_lookup_stdin(void)
{
    AxlStdioBridge2 *b = bridge_find_live(true);
    return (b != NULL) ? b->base.stdin_h : NULL;
}

/* The live bridge's stdout (@p err false) or stderr handle, and in
   @p *owner the token of the dispatch that lent it (0 with no bridge). */
static AxlFileHandle
bridge_lookup_out(
    bool       err,
    uint64_t  *owner
    )
{
    AxlStdioBridge2 *b = bridge_find_live(false);
    *owner = (b != NULL) ? b->base.token : 0;
    if (b == NULL) {
        return NULL;
    }
    return err ? b->base.stderr_h : b->base.stdout_h;
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

/* Shared classifier behind the stdin/stdout interactive predicates: is the
   given shell std handle the interactive CONSOLE, or a redirected file / pipe?
   NULL handle (BDS / non-shell / no live bridge) → not interactive: the stream
   layer surfaces that as EOF (stdin) or the raw byte path (stdout) rather than
   blocking or mangling. */
static bool
handle_is_interactive_console(AxlFileHandle h)
{
    if (h == NULL) {
        return false;
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL || shell->GetFileSize == NULL) {
        return false;   /* can't probe → keep the safe byte/raw path */
    }
    /* A redirected file (`> f` / `< f`) or a pipe (`|`) is a real file:
       GetFileSize succeeds with a byte count. The console pseudo-file rejects
       the query. Verified on OVMF/EDK2 across typed / `<` / `>` / `|`. */
    UINT64     size = 0;
    EFI_STATUS st   = shell->GetFileSize((SHELL_FILE_HANDLE)h, &size);
    if (!EFI_ERROR(st)) {
        return false;   /* has a byte size ⇒ redirected file/pipe */
    }
    /* GetFileSize failed. Bias toward the safe byte path unless a SECOND,
       independent probe corroborates "console": a real file also returns file
       info, the console pseudo-file returns none. Requiring BOTH signals keeps
       a GetFileSize-hostile firmware from misclassifying a file/pipe as
       interactive — a false "piped" only degrades to the safe path, it does
       not hang (stdin) or mangle (stdout). */
    if (shell->GetFileInfo == NULL) {
        return false;
    }
    EFI_FILE_INFO *fi = shell->GetFileInfo((SHELL_FILE_HANDLE)h);
    if (fi != NULL) {
        gBS->FreePool(fi);
        return false;   /* has file info ⇒ treat as a file, not the console */
    }
    return true;   /* no size AND no file info ⇒ interactive console */
}

bool
axl_backend_stdin_is_interactive(void)
{
    return handle_is_interactive_console(axl_backend_shell_stdin());
}

bool
axl_backend_stdout_is_interactive(void)
{
    return handle_is_interactive_console(axl_backend_shell_stdout());
}

// ===================================================================
// Error console
// ===================================================================

/* Standard error on a console is the SAME stream as standard output, as on a
   Unix terminal: written through the console's ConOut splitter, whatever
   stdout is redirected to.

   Why not gST->StdErr: the firmware's StdErr and ConOut are two console
   splitters, each keeping its own cursor record (ConSplitter's TextOutMode).
   A StdErr line moves the devices' cursors and not ConOut's record, and the
   Shell draws its prompt from that record -- SetCursorPosition(0,
   ConOut->Mode->CursorRow) -- so the prompt landed on the first error line
   and wrote over it. ConOut's record is its FIRST device's cursor, and where
   that device is not in StdErr (OVMF's display: ErrOut is the serial terminal
   only) it never moves for an error line at all.

   Why the splitter and not gST->ConOut: under the Shell gST->ConOut is the
   Shell's console logger, which keeps the scroll-back, stops at each
   screenful under page break and waits for a key (and on Ctrl-S), and is
   not written to be entered from a timer callback -- where a resident
   image's log line comes from. The logger shares the splitter's Mode
   (ConsoleLogger.c, OurConOut.Mode = gST->ConOut->Mode), so a line written
   to the splitter moves the very record the prompt is placed from. Under
   `>` gST->ConOut is the file, but the logger -- installed as a
   SimpleTextOut on the Shell's own image handle, the one carrying
   EFI_SHELL_PROTOCOL -- still names the splitter by that Mode. Error lines
   are therefore not in the Shell's scroll-back, as they never were.

   A device in StdErr but not ConOut -- the serial terminal while a take-over
   holds the screen, a port only ErrOut names -- still gets every error line,
   written to it directly (stderr_only_devices_write).

   "The console" is decided by IDENTITY: standard error is the console only
   when its Shell handle IS the Shell's console file -- the handle
   EFI_SHELL_PROTOCOL's OpenFileByName returns for ">e". "Has no file size"
   is not enough: `NUL` and an environment variable (`2>v name`) have none
   either. The handle judged is the live one (stderr_is_console_now): a
   resident driver's comes from whichever launcher dispatches into it now.
   With no live handle, the verdict taken at image start applies
   (axl_backend_console_err_init: the image's own handle, or its parent
   launcher's). With nothing to compare -- no EFI_SHELL_PARAMETERS_PROTOCOL
   (the EFI 1.x shell, a BDS launch), no EFI_SHELL_PROTOCOL to ask, no
   splitter found -- the line goes to gST->StdErr as it always did. So does
   it under `2>`. */
static bool                             mErrInitDone      = false;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mErrSplitter      = NULL;  ///< found once; NULL = none
static bool                             mErrStartConsole  = false; ///< verdict with no live handle

/* The loaded image whose memory holds @p p: its base, and its size in
   @p size. NULL when no image does. */
static const unsigned char *
image_holding(
    const void  *p,
    UINT64      *size
    )
{
    EFI_HANDLE          *handles = NULL;
    UINTN                count   = 0;
    const unsigned char *found   = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiLoadedImageProtocolGuid, NULL,
                                          &count, &handles))) {
        return NULL;
    }
    for (UINTN i = 0; i < count && found == NULL; i++) {
        EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
        if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiLoadedImageProtocolGuid,
                                           (VOID **)&li))
            && li != NULL && li->ImageBase != NULL
            && (const unsigned char *)p >= (const unsigned char *)li->ImageBase
            && (const unsigned char *)p < (const unsigned char *)li->ImageBase
                                          + li->ImageSize) {
            found = li->ImageBase;
            *size = li->ImageSize;
        }
    }
    gBS->FreePool(handles);
    return found;
}

/* Whether the Shell handle @p h is the Shell's console file named @p name
   (">o" or ">e"). False whenever that cannot be asked.

   Nested Shells: a command run in a Shell started from the Shell inherits
   the OUTER Shell's console file (the inner Shell hands on the StdErr it was
   given, ShellParametersProtocol.c), while OpenFileByName answers with the
   inner Shell's own. The console files are statics of the Shell binary, so
   the outer one is the same code and the same static in another image of
   the same binary: at the same offset in an image of the same size, with
   the same Revision, and with Write and Close at the same offsets from
   their own image's base. That counts as the console too.

   The equality test is a StrCmp in the Shell and allocates nothing, so it
   is made on every call. Only the image walk is cached -- keyed on the
   handle, the Shell and the answer it was compared with, as a Shell's
   static can be freed with the Shell and its pages reused -- and the key
   is stored LAST, so a writer at a raised TPL never sees a key with the
   previous verdict. */
static bool
shell_handle_is_outer_console(
    EFI_SHELL_PROTOCOL  *shell,
    AxlFileHandle        h,
    SHELL_FILE_HANDLE    console
    )
{
    static EFI_SHELL_PROTOCOL *key_shell[2];
    static SHELL_FILE_HANDLE   key_console[2];
    static AxlFileHandle       key_h[2];
    static bool                verdict[2];
    for (int i = 0; i < 2; i++) {
        if (key_h[i] == h && key_shell[i] == shell && key_console[i] == console) {
            return verdict[i];
        }
    }
    UINT64                   size_c = 0;
    UINT64                   size_h = 0;
    const unsigned char     *base_c = image_holding(console, &size_c);
    const unsigned char     *base_h = image_holding(h, &size_h);
    const EFI_FILE_PROTOCOL *fc     = (const EFI_FILE_PROTOCOL *)console;
    const EFI_FILE_PROTOCOL *fh     = (const EFI_FILE_PROTOCOL *)h;
    bool same = base_c != NULL && base_h != NULL && base_c != base_h && size_c == size_h
                && (const unsigned char *)console - base_c == (const unsigned char *)h - base_h
                && fh->Revision == fc->Revision
                && (const unsigned char *)(UINTN)fh->Write - base_h
                   == (const unsigned char *)(UINTN)fc->Write - base_c
                && (const unsigned char *)(UINTN)fh->Close - base_h
                   == (const unsigned char *)(UINTN)fc->Close - base_c;
    /* One slot per answer (">o", ">e" have different statics): the slot
       already holding this answer, else the first while it is empty, else
       the second. */
    int slot = (key_console[0] == console || key_console[0] == NULL) ? 0 : 1;
    key_h[slot]       = NULL;
    verdict[slot]     = same;
    key_shell[slot]   = shell;
    key_console[slot] = console;
    key_h[slot]       = h;
    return same;
}

static bool
shell_handle_is_console_file(
    EFI_SHELL_PROTOCOL  *shell,
    AxlFileHandle        h,
    const CHAR16        *name
    )
{
    SHELL_FILE_HANDLE console = NULL;
    if (h == NULL || shell == NULL || shell->OpenFileByName == NULL) {
        return false;
    }
    if (EFI_ERROR(shell->OpenFileByName((CHAR16 *)name, &console, EFI_FILE_MODE_WRITE))) {
        return false;
    }
    if ((AxlFileHandle)console == h) {
        return true;
    }
    return shell_handle_is_outer_console(shell, h, console);
}

/* The first SimpleTextOut, other than @p skip, whose Mode is @p mode (any
   Mode when NULL), among the handles carrying @p on. NULL when none. */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *
text_out_find(
    EFI_GUID                         *on,
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *skip,
    SIMPLE_TEXT_OUTPUT_MODE          *mode
    )
{
    EFI_HANDLE                      *handles = NULL;
    UINTN                            count   = 0;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *found   = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, on, NULL, &count, &handles))) {
        return NULL;
    }
    for (UINTN i = 0; i < count && found == NULL; i++) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t = NULL;
        if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiSimpleTextOutputProtocolGuid,
                                           (VOID **)&t))
            && t != NULL && t != skip && (mode == NULL || t->Mode == mode)) {
            found = t;
        }
    }
    gBS->FreePool(handles);
    return found;
}

/* The StdErr the Shell gave this image's PARENT -- the launcher an AXL
   service driver or a shared driver was loaded from, running while the
   driver starts. NULL when the parent has no Shell parameters. */
static AxlFileHandle
parent_image_stderr(
    void
    )
{
    EFI_LOADED_IMAGE_PROTOCOL     *li = NULL;
    EFI_SHELL_PARAMETERS_PROTOCOL *sp = NULL;
    if (gImageHandle == NULL
        || EFI_ERROR(gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid,
                                         (VOID **)&li))
        || li == NULL || li->ParentHandle == NULL
        || EFI_ERROR(gBS->HandleProtocol(li->ParentHandle, &gEfiShellParametersProtocolGuid,
                                         (VOID **)&sp))
        || sp == NULL) {
        return NULL;
    }
    return (AxlFileHandle)sp->StdErr;
}

void
axl_backend_console_err_init(
    void
    )
{
    if (gST == NULL) {
        return;
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    AxlFileHandle       err   = axl_backend_shell_stderr();
    mErrInitDone     = true;
    mErrSplitter     = NULL;
    /* With no Shell handle of its own and no live bridge -- a driver
       starting inside its launcher -- standard error is the launcher's. */
    mErrStartConsole = shell_handle_is_console_file(
        shell, (err != NULL) ? err : parent_image_stderr(), L">e");
    if (shell == NULL) {
        return;
    }
    /* The Shell's console logger, then the splitter that shares its Mode. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *logger =
        text_out_find(&gEfiShellProtocolGuid, NULL, NULL);
    if (logger != NULL && logger->Mode != NULL) {
        mErrSplitter = text_out_find(&gEfiSimpleTextOutputProtocolGuid, logger,
                                     logger->Mode);
    }
}

/* Whether standard error is the console NOW. The live handle is the image's
   own Shell StdErr, or -- for a resident driver -- the StdErr the CURRENT
   launcher lends it through the stdio bridge, which changes from one
   launcher to the next: judged on every write (shell_handle_is_console_file). With
   no live handle (a driver writing on its own clock, between launchers),
   the verdict taken at image start. */
static bool
stderr_is_console_now(
    void
    )
{
    AxlFileHandle h = axl_backend_shell_stderr();
    if (h == NULL) {
        return mErrStartConsole;
    }
    return shell_handle_is_console_file(get_shell(), h, L">e");
}

bool
axl_backend_shell_handle_is_console(
    AxlFileHandle  h,
    bool           err
    )
{
    return shell_handle_is_console_file(get_shell(), h, err ? L">e" : L">o");
}

/* Write @p str, in @p attr when @p colour, to one text output, putting its
   attribute back afterwards. @p fanout and @p device say which devices it
   reaches (axl_text_out_lines). */
static void
text_out_write(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out,
    EFI_GUID                        *fanout,
    EFI_HANDLE                       device,
    const unsigned short            *str,
    bool                             colour,
    uint32_t                         attr
    )
{
    INT32 prev = (out->Mode != NULL) ? out->Mode->Attribute : 0;
    if (colour) {
        out->SetAttribute(out, attr);
    }
    axl_text_out_lines(gST, out, fanout, device, (const CHAR16 *)str);
    if (colour) {
        out->SetAttribute(out, (UINTN)prev);
    }
}

/* The error line to every StdErr device that is not also a ConOut device:
   ConSplitter fans a splitter out to exactly the handles carrying its tag,
   so these are the devices a ConOut write misses. */
static void
stderr_only_devices_write(
    const unsigned short *str,
    bool                  colour,
    uint32_t              attr
    )
{
    EFI_HANDLE  stack[8];
    EFI_HANDLE *handles = stack;
    UINTN       size    = sizeof(stack);

    EFI_STATUS st = gBS->LocateHandle(ByProtocol, &gEfiStandardErrorDeviceGuid, NULL,
                                      &size, handles);
    if (st == EFI_BUFFER_TOO_SMALL) {
        handles = axl_backend_alloc(size);
        if (handles == NULL) {
            return;
        }
        st = gBS->LocateHandle(ByProtocol, &gEfiStandardErrorDeviceGuid, NULL,
                               &size, handles);
    }
    if (!EFI_ERROR(st)) {
        for (UINTN i = 0; i < size / sizeof(EFI_HANDLE); i++) {
            VOID                            *tag = NULL;
            EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = NULL;
            if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiConsoleOutDeviceGuid, &tag))
                && !EFI_ERROR(gBS->HandleProtocol(handles[i],
                                                  &gEfiSimpleTextOutputProtocolGuid,
                                                  (VOID **)&out))
                && out != NULL) {
                text_out_write(out, NULL, handles[i], str, colour, attr);
            }
        }
    }
    if (handles != stack) {
        axl_backend_free(handles);
    }
}

/* One error line to wherever standard error is (above). */
static void
console_write_err_impl(
    const unsigned short *str,
    bool                  colour,
    uint32_t              attr
    )
{
    if (gST == NULL || str == NULL) {
        return;
    }
    if (gST->StdErr == NULL || gST->StdErr == gST->ConOut) {
        /* No separate error console, or a console tap aliased the two. */
        if (gST->ConOut != NULL) {
            text_out_write(gST->ConOut, &gEfiConsoleOutDeviceGuid, NULL, str, colour, attr);
        }
        return;
    }
    if (!mErrInitDone) {
        axl_backend_console_err_init();   /* no stream init ran: first use */
    }
    if (mErrSplitter != NULL && stderr_is_console_now()) {
        text_out_write(mErrSplitter, &gEfiConsoleOutDeviceGuid, NULL, str, colour, attr);
        stderr_only_devices_write(str, colour, attr);
    } else {
        text_out_write(gST->StdErr, &gEfiStandardErrorDeviceGuid, NULL, str, colour, attr);
    }
}

/**
 * @brief Write a UCS-2 string to standard error. NULL-safe.
 */
void
axl_backend_console_write_err(
    const unsigned short  *str
    )
{
    console_write_err_impl(str, false, 0);
}

/**
 * @brief Write a UCS-2 string to standard error in a text attribute.
 */
void
axl_backend_console_write_err_attr(
    const unsigned short  *str,
    uint32_t               attr
    )
{
    console_write_err_impl(str, true, attr);
}

AxlFileHandle
axl_backend_shell_stdout_owned(
    uint64_t  *owner
    )
{
    probe_shell_std_handles();
    if (mShellStdOut != NULL) {
        *owner = 0;
        return (AxlFileHandle)mShellStdOut;  /* app/launcher: own params */
    }
    return bridge_lookup_out(false, owner);  /* driver: live bridge consult */
}

AxlFileHandle
axl_backend_shell_stderr_owned(
    uint64_t  *owner
    )
{
    probe_shell_std_handles();
    if (mShellStdErr != NULL) {
        *owner = 0;
        return (AxlFileHandle)mShellStdErr;  /* app/launcher: own params */
    }
    return bridge_lookup_out(true, owner);   /* driver: live bridge consult */
}

AxlFileHandle
axl_backend_shell_stdout(void)
{
    uint64_t owner;
    return axl_backend_shell_stdout_owned(&owner);
}

AxlFileHandle
axl_backend_shell_stderr(void)
{
    uint64_t owner;
    return axl_backend_shell_stderr_owned(&owner);
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
    /* Cached across calls; GetCurDir parks a buffer every time it is asked
       (axl-backend-native-shell.h). */
    return (const unsigned short *)axl_shell_cwd(shell);
}

int
axl_backend_shell_chdir(
    const unsigned short  *path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;
    unsigned short      *copy = NULL;

    shell = get_shell();
    if (shell == NULL || path == NULL) {
        return AXL_ERR;
    }

    const unsigned short *p = path_backslashed(path, &copy);
    if (p == NULL) {
        return AXL_ERR;
    }
    status = shell->SetCurDir(NULL, (CONST CHAR16 *)p);
    axl_backend_free(copy);
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
        /* ParentImageHandle is EFI_HANDLE* (not EFI_HANDLE) and the modern
           EDK2 shell's Execute() dereferences it to seed the spawned
           command's own LoadedImage->ParentHandle -- a NULL pointer here
           (as opposed to a NULL *contents*) makes Execute() bail with
           EFI_INVALID_PARAMETER before the command ever runs, on real
           OVMF/EDK2 shells. Pass this image's own handle by address, the
           same convention the old EFI 1.x branch below uses. */
        axl_backend_child_image_enter();
        EFI_STATUS status = shell->Execute(&gImageHandle, (CHAR16 *)command,
                                           NULL, NULL);
        axl_backend_child_image_leave();
        axl_backend_exit_if_deferred();
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
        axl_backend_child_image_enter();
        EFI_STATUS status = se->Execute(axl_efi1x_shell_parent(),
                                        (CHAR16 *)command, FALSE);
        axl_backend_child_image_leave();
        axl_backend_exit_if_deferred();
        return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
    }
    return AXL_ERR;
}

/* An app can return any 64-bit value, this one included; the result's
   status_written is then wrong only for a command that ran and returned
   exactly this. */
#define AXL_SHELL_STATUS_UNSET 0xA5A5A5A5A5A5A5A5ULL

int
axl_backend_shell_execute_status(
    const unsigned short  *command,
    uint64_t              *status,
    bool                  *written,
    uint64_t              *execute_status
    )
{
    if (command == NULL || status == NULL || written == NULL
        || execute_status == NULL) {
        return AXL_ERR;
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL) {
        return AXL_ERR;
    }
    /* &gImageHandle, not NULL: see axl_backend_shell_execute. */
    EFI_STATUS code = (EFI_STATUS)AXL_SHELL_STATUS_UNSET;
    axl_backend_child_image_enter();
    EFI_STATUS rc = shell->Execute(&gImageHandle, (CHAR16 *)command, NULL, &code);
    axl_backend_child_image_leave();
    axl_backend_exit_if_deferred();
    *written        = ((uint64_t)code != AXL_SHELL_STATUS_UNSET);
    *status         = *written ? (uint64_t)code : 0;
    *execute_status = (uint64_t)rc;
    return AXL_OK;
}

const unsigned short *
axl_backend_shell_get_alias(
    const unsigned short  *name
    )
{
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL || name == NULL || shell->GetAlias == NULL) {
        return NULL;
    }
    return (const unsigned short *)shell->GetAlias((CONST CHAR16 *)name, NULL);
}

/* No-op stand-in for ConOut->OutputString, installed while a "quiet" shell
   command runs so the command's text is dropped without a `> nul` redirect. */
static EFI_STATUS EFIAPI
swallow_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    (void)String;
    return EFI_SUCCESS;
}

int
axl_backend_shell_execute_quiet(
    const unsigned short  *command
    )
{
    if (command == NULL) {
        return AXL_ERR;
    }

    /* Swallow console text by swapping only the OutputString slot of the live
       ConOut (single-threaded UEFI, so no concurrency hazard) — Mode, cursor,
       and attributes stay valid for a command that reads them. Callers invoke
       ConOut->OutputString(ConOut, ...) through the pointer each time, never a
       cached copy, so the swap takes effect and the restore is clean. The
       command runs unaltered and in-context, unlike a `> nul` redirect. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *conout = axl_st()->ConOut;
    EFI_TEXT_STRING saved = NULL;
    if (conout != NULL) {
        saved = conout->OutputString;
        conout->OutputString = swallow_output_string;
    }

    int rc = axl_backend_shell_execute(command);

    if (conout != NULL) {
        conout->OutputString = saved;
    }
    return rc;
}

// ===================================================================
// Shell map reverse-lookup
//
// On the old EFI 1.x shell these fall back to the SHELL_ENVIRONMENT-based
// helpers in axl-backend-native-efi1x.c (GetMap has no reverse direction,
// so those iterate fs0..fsN and byte-compare).
// ===================================================================

size_t
axl_backend_dp_size(
    const void  *dp
    )
{
    const uint8_t *p = (const uint8_t *)dp;
    size_t total = 0;
    for (unsigned n = 0; n < 256 && p != NULL; n++) {
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));
        if (len < 4) {
            return 0;
        }
        total += len;
        if (p[0] == 0x7f) {   /* END node — included in the total */
            break;
        }
        p += len;
    }
    return total;
}

bool
axl_backend_dp_equal(
    const void  *a,
    const void  *b
    )
{
    if (a == NULL || b == NULL) {
        return false;
    }
    size_t sa = axl_backend_dp_size(a);
    if (sa == 0 || sa != axl_backend_dp_size(b)) {
        return false;
    }
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < sa; i++) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

int
axl_backend_volume_name_for_handle(
    void   *device_handle,
    char   *out,
    size_t  out_size
    )
{
    if (device_handle == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    /* With no shell anywhere, name the volume positionally — matched on the
       handle itself, which for a file-loaded image is exact where a
       device-path comparison is merely careful. */
    if (no_shell_at_all()) {
        return axl_nosh_volume_name_for_handle(device_handle, out, out_size);
    }

    /* A shell is live, so its map is the ONLY naming that may be returned:
       a path built from the name has to resolve through that same shell.
       Falling back to the positional fs<n> here would be a correctness bug
       for any volume the shell has not mapped (a fresh RAM disk, a hot-plug,
       anything since the last `map -r`) — the two orders diverge, so the
       name would resolve to a DIFFERENT volume. Report "unnamed" instead and
       let the caller degrade to a prefix-less path. */
    EFI_DEVICE_PATH_PROTOCOL *dp      = NULL;
    EFI_GUID                  dp_guid = gEfiDevicePathProtocolGuid;
    if (EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)device_handle, &dp_guid,
                                      (VOID **)&dp))
        || dp == NULL)
    {
        return AXL_ERR;
    }
    return axl_backend_shell_map_alias(dp, out, out_size);
}

/* Name a volume by device path when there is no modern shell: the old EFI 1.x
   shell's map if that shell is live, else the positional fs<n>. Shared by the
   map_name and map_alias lookups — with no shell there is no distinction
   between "the fs<n> name" and "the first alias", since fs<n> is the only
   name that exists. */
static int
shell_map_name_no_modern_shell(
    void   *device_path,
    char   *out,
    size_t  out_size
    )
{
    if (!no_shell_at_all()) {
        return axl_efi1x_map_fs_name_from_dp(device_path, out, out_size);
    }
    return axl_nosh_map_fs_name_from_dp(device_path, out, out_size);
}

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
        return shell_map_name_no_modern_shell(device_path, out, out_size);
    }

    /* Probe FS0..FS63 first: GetMapFromDevicePath parks its result on the
       Shell's per-command free list, and axl_volume_enumerate asks this for
       every volume on every call. When exactly one probed name maps to the
       volume it is the answer whatever the list order
       (axl-backend-native-shell.h); otherwise ask the Shell. */
    unsigned only = 0;
    if (axl_shell_fs_probe(shell, device_path, &only) == 1) {
        int n = axl_snprintf(out, out_size, "fs%u", only);
        return (n > 0 && (size_t)n < out_size) ? AXL_OK : AXL_ERR;
    }

    /* GetMapFromDevicePath advances the pointer past the matched volume
       portion; pass a local copy so the caller's device path is untouched.
       The list it returns is NOT freed, here or in map_alias and
       unmap_device: the shell owns it. Its header comment says the caller
       frees it, but the function returns it through AddBufferToFreeList
       (ShellProtocol.c), and the shell frees that list itself once the
       command finishes (Shell.c). Freeing it here is a double free: the
       shell's own free then trips the DXE core's pool-header ASSERT
       (MdeModulePkg Pool.c, measured on a debug OVMF with --debugcon) and
       the shell hangs after the app returns. */
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)device_path;
    const unsigned short *map = (const unsigned short *)
        shell->GetMapFromDevicePath(&dp);
    if (map == NULL) {
        return AXL_ERR;
    }

    /* `map` is one or more ';'-separated aliases in UCS-2, e.g. "FS1:;F1:".
       Take the first "fs<digits>" token, lowercased, without the ':'. The
       shell lists the friendliest (FSn:) alias first, but scanning for the
       fs<n> token explicitly is robust to ordering. A token that does not fit
       in @p out is an error, never a shortened name that would resolve to
       another volume; @p out is written only on success. */
    int rc = AXL_ERR;   /* no fs<n> alias maps to this device path */
    for (const unsigned short *p = map; *p != 0; ) {
        const unsigned short *tok = p;
        while (*p != 0 && *p != (unsigned short)';') {
            p++;
        }
        size_t toklen = (size_t)(p - tok);
        /* "fs" and digits only, up to the ':' -- the shape the shell
           generates. A custom name that merely starts that way (a label
           "fs1a") is not the volume's fsN. */
        size_t n = 2;
        while (n < toklen && tok[n] >= '0' && tok[n] <= '9') {
            n++;
        }
        if (toklen >= 3
            && (tok[0] == 'f' || tok[0] == 'F')
            && (tok[1] == 's' || tok[1] == 'S')
            && n > 2
            && (n == toklen || tok[n] == (unsigned short)':')) {
            if (n < out_size) {
                for (size_t k = 0; k < n; k++) {
                    unsigned short c = tok[k];
                    out[k] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                }
                out[n] = '\0';
                rc = AXL_OK;
            }
            break;
        }
        if (*p == (unsigned short)';') {
            p++;
        }
    }
    return rc;
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
        return shell_map_name_no_modern_shell(device_path, out, out_size);
    }

    /* Asked of the Shell every time, parking its answer on the Shell's
       per-command free list: the FIRST name cannot be found without it,
       because a re-set of any name the device has reorders the list unseen
       (axl-backend-native-shell.h). Not a per-file-operation path. */
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)device_path;
    const unsigned short *map = (const unsigned short *)
        shell->GetMapFromDevicePath(&dp);
    if (map == NULL) {
        return AXL_ERR;
    }

    /* `map` is one or more ';'-separated aliases, e.g. "RAMDISK:;FS9:". Take
       the FIRST token verbatim (minus the trailing ':') — any form, fs<n> or a
       custom SetMap name. This is the "what is it mapped as" query, distinct
       from map_name's fs<n>-only filter. An alias longer than the buffer is an
       error, never a truncated, un-resolvable name; @p out is written only on
       success. */
    size_t n = 0;
    while (map[n] != 0 && map[n] != (unsigned short)';'
           && map[n] != (unsigned short)':') {
        n++;
    }
    int rc = AXL_ERR;
    if (n > 0 && n < out_size) {
        for (size_t k = 0; k < n; k++) {
            out[k] = (char)map[k];
        }
        out[n] = '\0';
        rc = AXL_OK;
    }
    return rc;
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

bool
axl_backend_shell_map_is(
    const unsigned short  *name,
    void                  *device_path
    )
{
    if (name == NULL || device_path == NULL) {
        return false;
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL) {
        /* The old EFI 1.x shell answers through SHELL_ENVIRONMENT.GetMap;
           with no shell at all there are no names. */
        return axl_efi1x_map_is(name, device_path);
    }
    if (shell->GetDevicePathFromMap == NULL) {
        return false;
    }
    /* Byte comparison. The shell itself compares device paths as text
       (DevicePathCompare), but stores a byte copy of the path a name was set
       for, so a name set for @p device_path, or generated by a `map -r` for
       the handle it came from, compares equal. */
    const EFI_DEVICE_PATH_PROTOCOL *dp =
        shell->GetDevicePathFromMap((const CHAR16 *)name);
    return dp != NULL && axl_backend_dp_equal(dp, device_path);
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

int
axl_backend_shell_unmap_device(
    void    *device_path,
    size_t  *removed
    )
{
    size_t n  = 0;
    int    rc = AXL_ERR;
    if (removed != NULL) {
        *removed = 0;
    }
    if (device_path == NULL) {
        return AXL_ERR;
    }
    if (no_shell_at_all()) {
        return AXL_OK;   /* no shell, no names */
    }
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell == NULL) {
        /* The old EFI 1.x shell cannot remove a name: one its own `map -r`
           gave this device stays until the next `map -r`. */
        char name[32];
        return (axl_efi1x_map_fs_name_from_dp(device_path, name, sizeof(name))
                    == AXL_OK) ? AXL_ERR : AXL_OK;
    }
    if (shell->GetMapFromDevicePath == NULL || shell->SetMap == NULL) {
        return AXL_ERR;   /* can neither list nor remove this device's names */
    }

    /* Remove the device's first name until it has none. Each name is taken
       from the shell's own list for this device, whole and in UCS-2, so it is
       removed exactly as the shell spells it, whatever its length or
       characters, and a name that has moved to another volume is never
       touched. One lookup past the cap confirms the device has none left. */
    for (;;) {
        EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)device_path;
        const CHAR16 *map = shell->GetMapFromDevicePath(&dp);
        if (map == NULL) {
            rc = AXL_OK;
            break;
        }
        if (n >= AXL_UNMAP_MAX_NAMES) {
            break;
        }
        size_t len = 0;
        while (map[len] != 0 && map[len] != (CHAR16)';') {
            len++;
        }
        CHAR16 *name = axl_backend_alloc((len + 1) * sizeof(CHAR16));
        if (name != NULL) {
            for (size_t k = 0; k < len; k++) {
                name[k] = map[k];
            }
            name[len] = 0;
        }
        /* The list is not freed; see axl_backend_shell_map_name. */
        if (name == NULL) {
            break;
        }
        /* The token keeps its ':' — SetMap(NULL, name) wants the name the way
           the shell stores it. */
        EFI_STATUS st = shell->SetMap(NULL, name);
        axl_backend_free(name);
        if (EFI_ERROR(st)) {
            break;
        }
        n++;
    }
    if (removed != NULL) {
        *removed = n;
    }
    return rc;
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

bool
axl_backend_wcs_copy(
    unsigned short        *dst,
    size_t                 cap,
    const unsigned short  *src
    )
{
    size_t n = axl_backend_wcslen(src);
    if (n + 1 > cap) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        dst[i] = src[i];
    }
    return true;
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
           bridge_find_live(true) unambiguously returns THAT launcher's bridge. */
        AxlStdioBridge2 *b = bridge_find_live(true);
        if (b != NULL) {
            b->base.pending_status = status;
            b->base.has_pending    = true;
        }
    }
}

bool
axl_backend_bridge_take_exit_status(uint64_t *out)
{
    /* Read THIS (launcher) image's own bridge cell — the driver wrote it
       through the installed protocol interface: &mBridge.base under either
       GUID, whichever age of driver it was. */
    if (mBridgeHandle == NULL || !mBridge.base.has_pending) {
        return false;
    }
    if (out != NULL) {
        *out = mBridge.base.pending_status;
    }
    mBridge.base.has_pending = false;
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

/* How many images this one has started and not yet regained control from,
   and the notify depth at each start (innermost last), so a deferred exit
   unwinds only to a callback entered after the innermost child started. */
#define AXL_CHILD_NEST_MAX 16
static unsigned mChildImages;
static unsigned mChildNotifyDepth[AXL_CHILD_NEST_MAX];

/* An exit requested while a child ran (axl_backend_exit_defer), performed by
   axl_backend_exit_if_deferred once no child remains. The status is resolved
   at the FIRST request and pinned: a later axl_exit_status overwrites the
   armed status, and must not change what the first request decided. */
static bool     mExitDeferred;
static int      mDeferredRc;
static uint64_t mDeferredStatus;
static void   (*mDeferredExit)(int rc);

void
axl_backend_child_image_enter(void)
{
    if (mChildImages < AXL_CHILD_NEST_MAX) {
        mChildNotifyDepth[mChildImages] = axl_backend_notify_depth();
    }
    mChildImages++;
}

void
axl_backend_child_image_leave(void)
{
    if (mChildImages > 0) {
        mChildImages--;
    }
}

bool
axl_backend_child_images_running(void)
{
    return mChildImages > 0;
}

bool
axl_backend_exit_defer(
    int    rc,
    void (*exit_fn)(int rc)
    )
{
    if (mChildImages == 0) {
        return false;
    }
    unsigned innermost = (mChildImages <= AXL_CHILD_NEST_MAX)
                         ? mChildNotifyDepth[mChildImages - 1]
                         : mChildNotifyDepth[AXL_CHILD_NEST_MAX - 1];
    if (!axl_backend_notify_unwind_ok(innermost)) {
        return false;
    }
    /* The first request decides, as an exit does. A callback that runs again
       while the child still runs (the tick fires on) asks again; it is
       unwound again, without re-deciding. */
    if (!mExitDeferred) {
        mExitDeferred   = true;
        mDeferredRc     = rc;
        mDeferredStatus = axl_backend_resolve_exit_status(rc);
        mDeferredExit   = exit_fn;
    }
    axl_backend_notify_unwind();
    return false;   /* unreachable: the unwind does not return */
}

void
axl_backend_exit_if_deferred(void)
{
    if (mChildImages != 0 || !mExitDeferred) {
        return;
    }
    void (*fn)(int rc) = mDeferredExit;

    mExitDeferred = false;
    axl_backend_set_exit_status(mDeferredStatus);
    fn(mDeferredRc);
}

uint64_t
axl_backend_start_image(
    void *handle
    )
{
    UINTN      exit_data_size = 0;
    EFI_STATUS status;

    axl_backend_child_image_enter();
    status = gBS->StartImage((EFI_HANDLE)handle, &exit_data_size, NULL);
    axl_backend_child_image_leave();
    return (uint64_t)status;
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
