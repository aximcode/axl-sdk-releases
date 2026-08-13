/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log.c
    AxlLog core — level filtering, domain overrides, console output
    with optional timestamps and func/line, and handler dispatch.

    Migrated from AxlLog.c(EDK2-style) to GLib-style API.
**/

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-format.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MAX_DOMAINS   8
#define MAX_HANDLERS  8
#define DOMAIN_LEN    16
#define MSG_BUF_SIZE  512

// Default console attribute (light gray on black)
#define DEFAULT_ATTR  EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)

/* Sentinel return values for parse_level_keyword. Both are negative
 * integers far below AXL_LOG_ERROR (=0) so the filter check
 * `level > effective_level` always evaluates to "filtered" if either
 * is stored as a domain/global level — but they MUST be distinct
 * from each other so apply_one_entry can tell them apart. */
#define LEVEL_PARSE_UNKNOWN  (-100)   /* "garbage" → ignore */
#define LEVEL_OFF            (-10)    /* "off" / "none" / "silent" */

// ---------------------------------------------------------------------------
// buf_write adapter for axl_vformat
// ---------------------------------------------------------------------------

typedef struct {
    char   *buf;
    size_t  pos;
    size_t  size;
} BufCtx;

static void
buf_write(const char *data, size_t len, void *ctx)
{
    BufCtx *bc = (BufCtx *)ctx;
    size_t avail = bc->size - bc->pos - 1;
    if (len > avail) {
        len = avail;
    }
    axl_memcpy(bc->buf + bc->pos, data, len);
    bc->pos += len;
    bc->buf[bc->pos] = '\0';
}

// ---------------------------------------------------------------------------
// Global State
// ---------------------------------------------------------------------------

static int            mGlobalLevel      = AXL_LOG_INFO;
static bool           mConsoleEnabled   = true;
static bool           mConsoleTimestamp  = true;
static bool           mConsoleColor     = true;

static char           mDomains[MAX_DOMAINS][DOMAIN_LEN];
static int            mDomainLevels[MAX_DOMAINS];
static size_t         mDomainCount      = 0;

static AxlLogHandler  mHandlers[MAX_HANDLERS];
static void          *mHandlerData[MAX_HANDLERS];
static const char    *mHandlerDomains[MAX_HANDLERS];
static int            mHandlerMaxLevel[MAX_HANDLERS];
static bool           mHandlerFiltered[MAX_HANDLERS];
static size_t         mHandlerCount     = 0;

static int            mFatalLevel       = -1;   // disabled

/* Lazy-init guard: ensure_env_init_once() reads AXL_LOG_LEVEL on the
   first log emission and applies the global/per-domain levels it
   parsed. The flag prevents re-parsing on every subsequent call. */
static bool           mEnvInitDone      = false;
static bool           mFatalTriggered   = false;
static EFI_HANDLE     mFatalImageHandle = NULL;

// ---------------------------------------------------------------------------
// Level Prefixes
// ---------------------------------------------------------------------------

static const unsigned short *mLevelPrefix[] = {
    L"[ERROR] ",
    L"[WARN]  ",
    L"[INFO]  ",
    L"[DEBUG] ",
    L"[TRACE] "
};

static size_t mLevelColor[] = {
    EFI_TEXT_ATTR(EFI_LIGHTRED,  EFI_BLACK),   // ERROR
    EFI_TEXT_ATTR(EFI_YELLOW,    EFI_BLACK),   // WARNING
    EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK),   // INFO
    EFI_TEXT_ATTR(EFI_DARKGRAY,  EFI_BLACK),   // DEBUG
    EFI_TEXT_ATTR(EFI_DARKGRAY,  EFI_BLACK)    // TRACE
};

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

static int
get_effective_level(const char *domain)
{
    if (domain != NULL) {
        for (size_t i = 0; i < mDomainCount; i++) {
            if (axl_strcmp(domain, mDomains[i]) == 0) {
                return mDomainLevels[i];
            }
        }
    }
    return mGlobalLevel;
}

static void
print_console_timestamp(const AxlRealtime *stamp)
{
    if (stamp == NULL) {
        return;
    }
    const AxlRealtime time = *stamp;

    char buf[24];
    int pos = 0;
    buf[pos++] = '0' + (time.hour / 10);
    buf[pos++] = '0' + (time.hour % 10);
    buf[pos++] = ':';
    buf[pos++] = '0' + (time.minute / 10);
    buf[pos++] = '0' + (time.minute % 10);
    buf[pos++] = ':';
    buf[pos++] = '0' + (time.second / 10);
    buf[pos++] = '0' + (time.second % 10);
    /* Sub-second precision. EFI_TIME.Nanosecond is allowed by spec but
       most firmware (OVMF, vendor BMC firmware) leaves it 0. Prefer the
       backend's monotonic counter for a real microsecond-resolution
       fractional field. Falls back to Nanosecond if monotonic isn't
       available, then to no-fraction at all.

       Caveat: the wallclock seconds (HH:MM:SS) are RTC-derived and
       tick at integer-second boundaries; the fractional comes from
       a separate counter with an unrelated epoch. Within a single
       wallclock second the fractional can appear to go backwards
       — e.g. `12:34:05.998500` followed by `12:34:05.012300` is
       possible if the RTC tick to second 06 hasn't fired yet but
       the monotonic-us counter has wrapped past 1_000_000. Acceptable
       for human-eyeball debugging (you see *some* fractional change
       between adjacent lines, which is what was missing before).
       For machine ordering use AxlLogEntry.timestamp from the ring
       handler, which uses raw monotonic-us and is strictly
       monotonic. */
    /* Already normalized by the dispatcher (see log_dispatch) -- render it.
       Taking our own monotonic reading here is what made the console and the
       file sink print different fractions for one record. */
    unsigned usec = time.nanosecond / 1000;
    if (usec > 0) {
        buf[pos++] = '.';
        buf[pos++] = '0' + ((usec / 100000) % 10);
        buf[pos++] = '0' + ((usec / 10000) % 10);
        buf[pos++] = '0' + ((usec / 1000) % 10);
        buf[pos++] = '0' + ((usec / 100) % 10);
        buf[pos++] = '0' + ((usec / 10) % 10);
        buf[pos++] = '0' + (usec % 10);
    }
    buf[pos++] = ' ';
    buf[pos] = '\0';

    unsigned short wide[24];
    axl_utf8_to_ucs2_buf(buf, wide, 24);
    axl_backend_console_write_err(wide);
}

// ---------------------------------------------------------------------------
// Core Log Dispatch
// ---------------------------------------------------------------------------

static void
log_dispatch(int level, const char *domain, const char *func,
             int line, const char *msg_buf)
{
    unsigned short wide[MSG_BUF_SIZE];

    /* Stamp the record ONCE, here, and hand the same value to the console and
     * to every sink. Each sink used to read the clock itself, which cost N
     * firmware GetTime calls per record for N sinks and -- worse -- let two
     * sinks land on opposite sides of a second boundary, so a serial
     * transcript and a file log disagreed about when one record happened.
     *
     * Only read it if something will render it: a service that suppresses the
     * console and attaches only the ring sink used to make ZERO firmware
     * GetTime calls per record, and an RTC read is CMOS I/O plus a UIP wait on
     * EDK2's PcRtc -- not free, and this runs on every record that clears the
     * level filter. */
    AxlRealtime        stamp;
    const AxlRealtime *stampp = NULL;
    if ((mConsoleEnabled && mConsoleTimestamp) || mHandlerCount > 0) {
        if (axl_time_realtime(&stamp) == AXL_OK) {
            /* Substitute the sub-second fraction HERE, once, rather than in
             * each renderer. Firmware leaves Nanosecond at 0 on every platform
             * we test on, so each sink's own fallback fired per record and
             * took its OWN monotonic reading -- three different fractions for
             * one record. Agreeing on the second while disagreeing on the
             * microseconds is the same corruption one digit further down.
             *
             * Trigger on the firmware reporting NOTHING, not on the value
             * rounding to zero: a genuine 0 < nanosecond < 1000 is a real
             * reading and beats an unrelated epoch's fraction. As ever, the
             * fraction is sub-second PRECISION, not an ordering key. */
            if (stamp.nanosecond == 0) {
                uint64_t mono = axl_backend_get_monotonic_us();
                if (mono > 0) {
                    stamp.nanosecond = (uint32_t)((mono % 1000000u) * 1000u);
                }
            }
            stampp = &stamp;
        }
    }

    // Console output
    if (mConsoleEnabled && axl_st() != NULL &&
        (axl_st()->StdErr != NULL || axl_st()->ConOut != NULL)) {
        if (mConsoleColor && level <= AXL_LOG_TRACE) {
            axl_backend_console_set_attr_err(mLevelColor[level]);
        }

        if (mConsoleTimestamp) {
            print_console_timestamp(stampp);
        }

        if (level <= AXL_LOG_TRACE) {
            axl_backend_console_write_err((const unsigned short *)mLevelPrefix[level]);
        }

        if (domain != NULL) {
            unsigned short wide_domain[DOMAIN_LEN];
            axl_utf8_to_ucs2_buf(domain, wide_domain, DOMAIN_LEN);
            axl_backend_console_write_err(wide_domain);

            if (func != NULL && line > 0 && level >= AXL_LOG_DEBUG) {
                /* Format :func:line */
                char loc[128];
                BufCtx bc = { loc, 0, sizeof (loc) };
                loc[0] = ':';
                bc.pos = 1;
                size_t flen = axl_strlen(func);
                if (flen > 80) {
                    flen = 80;
                }
                axl_memcpy(loc + bc.pos, func, flen);
                bc.pos += flen;
                loc[bc.pos++] = ':';
                /* Format line number */
                char num[12];
                int npos = 0;
                int tmp = line;
                if (tmp == 0) {
                    num[npos++] = '0';
                } else {
                    char rev[12];
                    int rpos = 0;
                    while (tmp > 0) {
                        rev[rpos++] = '0' + (tmp % 10);
                        tmp /= 10;
                    }
                    while (rpos > 0) {
                        num[npos++] = rev[--rpos];
                    }
                }
                axl_memcpy(loc + bc.pos, num, npos);
                bc.pos += npos;
                loc[bc.pos] = '\0';

                unsigned short wide_loc[128];
                axl_utf8_to_ucs2_buf(loc, wide_loc, 128);
                axl_backend_console_write_err(wide_loc);
            }
            axl_backend_console_write_err((const unsigned short *)L": ");
        } else if (func != NULL && line > 0 && level >= AXL_LOG_DEBUG) {
            char loc[128];
            size_t pos = 0;
            size_t flen = axl_strlen(func);
            if (flen > 80) {
                flen = 80;
            }
            axl_memcpy(loc, func, flen);
            pos = flen;
            loc[pos++] = ':';
            char num[12];
            int npos = 0;
            int tmp = line;
            if (tmp == 0) {
                num[npos++] = '0';
            } else {
                char rev[12];
                int rpos = 0;
                while (tmp > 0) {
                    rev[rpos++] = '0' + (tmp % 10);
                    tmp /= 10;
                }
                while (rpos > 0) {
                    num[npos++] = rev[--rpos];
                }
            }
            axl_memcpy(loc + pos, num, npos);
            pos += npos;
            loc[pos++] = ':';
            loc[pos++] = ' ';
            loc[pos] = '\0';

            unsigned short wide_loc[128];
            axl_utf8_to_ucs2_buf(loc, wide_loc, 128);
            axl_backend_console_write_err(wide_loc);
        }

        axl_utf8_to_ucs2_buf(msg_buf, wide, MSG_BUF_SIZE);
        axl_backend_console_write_err(wide);
        axl_backend_console_write_err((const unsigned short *)L"\r\n");

        if (mConsoleColor) {
            axl_backend_console_set_attr_err(DEFAULT_ATTR);
        }
    }

    // Dispatch to registered handlers
    for (size_t i = 0; i < mHandlerCount; i++) {
        if (mHandlers[i] == NULL) {
            continue;
        }
        if (mHandlerFiltered[i]) {
            if (level > mHandlerMaxLevel[i]) {
                continue;
            }
            if (mHandlerDomains[i] != NULL && domain != NULL &&
                axl_strcmp(domain, mHandlerDomains[i]) != 0) {
                continue;
            }
            if (mHandlerDomains[i] != NULL && domain == NULL) {
                continue;
            }
        }
        mHandlers[i](level, domain, msg_buf, stampp, mHandlerData[i]);
    }

    // Fatal level check
    if (mFatalLevel != -1 && level <= mFatalLevel) {
        mFatalTriggered = true;
        if (mFatalImageHandle != NULL && axl_bs() != NULL) {
            axl_bs()->Exit(mFatalImageHandle, EFI_ABORTED, 0, NULL);
        }
    }
}

// Forward decl — implementation is below the public-API section.
static void ensure_env_init_once(void);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_log_full(int level, const char *domain, const char *func,
             int line, const char *fmt, ...)
{
    /* Apply AXL_LOG_LEVEL on first emission (idempotent). */
    ensure_env_init_once();

    /* Early-return BEFORE the va_list declaration. clang-tidy's
       valist.Uninitialized analyzer otherwise traces the
       get_effective_level → axl_strcmp call from inside the
       declared-but-unstarted lifetime and false-positives an
       "uninitialized va_list" diagnostic. Functionally identical;
       cheaper too, since the level filter skips the buffer alloc. */
    if (level > get_effective_level(domain)) {
        return;
    }

    va_list args;
    char    msg_buf[MSG_BUF_SIZE];
    BufCtx  bc = { msg_buf, 0, sizeof (msg_buf) };

    va_start(args, fmt);
    axl_vformat(buf_write, &bc, fmt, args);
    va_end(args);

    log_dispatch(level, domain, func, line, msg_buf);
}

void
axl_log(int level, const char *domain, const char *fmt, ...)
{
    ensure_env_init_once();
    if (level > get_effective_level(domain)) {
        return;
    }

    va_list args;
    char    msg_buf[MSG_BUF_SIZE];
    BufCtx  bc = { msg_buf, 0, sizeof (msg_buf) };

    va_start(args, fmt);
    axl_vformat(buf_write, &bc, fmt, args);
    va_end(args);

    log_dispatch(level, domain, NULL, 0, msg_buf);
}

void
axl_log_set_level(int level)
{
    /* Apply env-baseline first so programmatic calls take precedence
     * over AXL_LOG_LEVEL — RUST_LOG semantics (env is baseline,
     * code wins). Idempotent / no-op if env is unset. */
    ensure_env_init_once();
    mGlobalLevel = level;
}

void
axl_log_set_domain_level(const char *domain, int level)
{
    ensure_env_init_once();
    if (domain == NULL) {
        return;
    }

    // Check if domain already has an override
    for (size_t i = 0; i < mDomainCount; i++) {
        if (axl_strcmp(domain, mDomains[i]) == 0) {
            if (level == -1) {
                // Clear: shift remaining entries down
                if (i < mDomainCount - 1) {
                    axl_memcpy(&mDomains[i], &mDomains[i + 1],
                             (mDomainCount - i - 1) * DOMAIN_LEN);
                    axl_memcpy(&mDomainLevels[i], &mDomainLevels[i + 1],
                             (mDomainCount - i - 1) * sizeof (int));
                }
                mDomainCount--;
            } else {
                mDomainLevels[i] = level;
            }
            return;
        }
    }

    // New domain override
    if (level == -1 || mDomainCount >= MAX_DOMAINS) {
        return;
    }

    axl_strlcpy(mDomains[mDomainCount], domain, DOMAIN_LEN);
    mDomainLevels[mDomainCount] = level;
    mDomainCount++;
}

// ---------------------------------------------------------------------------
// AXL_LOG_LEVEL — env-var-driven configuration
// ---------------------------------------------------------------------------

/* Case-insensitive keyword equality on a length-bounded slice
 * against a NUL-terminated reference. Avoids a heap copy. */
static bool
ci_equal(const char *s, size_t len, const char *kw)
{
    if (axl_strlen(kw) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char a = s[i];
        char b = kw[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) {
            return false;
        }
    }
    return true;
}

static int
parse_level_keyword(const char *s, size_t len)
{
    /* Case-insensitive — `DEBUG`, `Debug`, and `debug` all parse the
     * same. Matches RUST_LOG / G_MESSAGES_DEBUG ergonomics. */
    struct { const char *kw; int level; } map[] = {
        { "off",     LEVEL_OFF       },
        { "none",    LEVEL_OFF       },
        { "silent",  LEVEL_OFF       },
        { "error",   AXL_LOG_ERROR   },
        { "err",     AXL_LOG_ERROR   },
        { "warning", AXL_LOG_WARNING },
        { "warn",    AXL_LOG_WARNING },
        { "info",    AXL_LOG_INFO    },
        { "debug",   AXL_LOG_DEBUG   },
        { "trace",   AXL_LOG_TRACE   },
        { NULL, 0 }
    };
    for (size_t i = 0; map[i].kw != NULL; i++) {
        if (ci_equal(s, len, map[i].kw)) {
            return map[i].level;
        }
    }
    return LEVEL_PARSE_UNKNOWN;
}

/**
 * Parse one "domain:level" entry; @a end is past-the-end of the
 * entry (just before a comma, or the trailing NUL). Bare "level"
 * (no colon) sets the global default.
 */
static void
apply_one_entry(const char *p, const char *end)
{
    /* Skip leading whitespace */
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    /* Trim trailing whitespace */
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (p == end) {
        return;
    }

    /* Aliases: bare "all" / "off" / "none" / "silent" set the global
     * default before any colon-parsing. */
    size_t total_len = (size_t)(end - p);
    if (total_len == 3 && axl_strncmp(p, "all", 3) == 0) {
        axl_log_set_level(AXL_LOG_DEBUG);
        return;
    }

    /* Look for ':' separator */
    const char *colon = NULL;
    for (const char *q = p; q < end; q++) {
        if (*q == ':') { colon = q; break; }
    }

    const char *level_str;
    size_t      level_len;
    const char *domain_str = NULL;
    size_t      domain_len = 0;

    if (colon == NULL) {
        /* Bare level → global default */
        level_str = p;
        level_len = total_len;
    } else {
        domain_str = p;
        domain_len = (size_t)(colon - p);
        level_str  = colon + 1;
        level_len  = (size_t)(end - level_str);
        /* Trim spaces around the level part */
        while (level_len > 0 && (*level_str == ' ' || *level_str == '\t')) {
            level_str++; level_len--;
        }
        while (level_len > 0 &&
               (level_str[level_len - 1] == ' ' ||
                level_str[level_len - 1] == '\t'))
        {
            level_len--;
        }
    }

    int level = parse_level_keyword(level_str, level_len);
    if (level == LEVEL_PARSE_UNKNOWN) {
        return;     /* Unknown — silently ignore (no log churn at init). */
    }
    /* `level` is now either a valid AXL_LOG_* level (0..4) or
     * LEVEL_OFF (-10). Both are safe to pass to axl_log_set_level
     * (anything < AXL_LOG_ERROR filters even axl_error since the
     * filter check is `msg_level > stored_level`). */

    if (domain_str == NULL) {
        /* Global default. */
        axl_log_set_level(level);
        return;
    }

    if (domain_len == 1 && *domain_str == '*') {
        /* Wildcard: same as bare level. */
        axl_log_set_level(level);
        return;
    }

    /* Per-domain. axl_log_set_domain_level needs a NUL-terminated
     * string; copy onto a stack buffer bounded by DOMAIN_LEN. */
    char domain_buf[DOMAIN_LEN];
    if (domain_len >= DOMAIN_LEN) {
        domain_len = DOMAIN_LEN - 1;
    }
    for (size_t i = 0; i < domain_len; i++) {
        domain_buf[i] = domain_str[i];
    }
    domain_buf[domain_len] = '\0';
    axl_log_set_domain_level(domain_buf, level);
}

void
axl_log_init_from_env(void)
{
    /* Read the raw shell value rather than axl_getenv, which returns an
     * OWNED heap copy this module has no way to release: AxlLog must not
     * call axl_free (axl-mem.c logs through us, and closing that loop is
     * the circular dependency the whole module layout exists to avoid).
     * Not freeing it is not an option either — it leaked once per image
     * for as long as AXL_LOG_LEVEL was set, which the teardown leak
     * report duly showed. The backend hands back a BORROWED pointer into
     * the shell's own storage, and axl_ucs2_to_utf8_buf decodes it into
     * this frame, so the whole path allocates nothing. */
    char buf[256];
    const unsigned short *wide =
        axl_backend_shell_getenv((const unsigned short *)L"AXL_LOG_LEVEL");
    if (wide == NULL) {
        return;
    }

    /* Refuse a value too long for the frame instead of taking the prefix.
     * axl_ucs2_to_utf8_buf truncates at a CODEPOINT boundary, which for a
     * comma-separated list lands mid-ENTRY: "...,mem:off" clipped to
     * "...,mem:of" parses as an unknown keyword, which this function
     * deliberately ignores in silence — so a half-applied configuration
     * would look exactly like a fully-applied one. Ignoring the whole
     * value is at least ONE coherent behavior, and it is documented.
     * 255 bytes is far past what the parser can act on anyway: the table
     * holds 8 domains of at most 15 characters. */
    size_t need = 1;
    for (const unsigned short *p = wide; *p != 0; p++) {
        need += (*p < 0x80) ? 1 : (*p < 0x800) ? 2 : 3;
    }
    if (need > sizeof(buf)) {
        return;
    }

    axl_ucs2_to_utf8_buf(wide, buf, sizeof(buf));
    const char *v = buf;
    if (*v == '\0') {
        return;
    }
    /* Walk comma-separated entries. */
    const char *p = v;
    while (*p != '\0') {
        const char *q = p;
        while (*q != '\0' && *q != ',') {
            q++;
        }
        apply_one_entry(p, q);
        p = (*q == ',') ? q + 1 : q;
    }
}

/* One-shot guard used by the dispatcher to apply env-var config on
 * first log emission. Init order is irrelevant — any caller paying
 * attention to log levels passes through axl_log_full / axl_log,
 * which both call get_effective_level which sees the configured
 * state on second-and-later calls. (mEnvInitDone is declared with
 * the rest of the file-scope state near the top.) */
static void
ensure_env_init_once(void)
{
    if (mEnvInitDone) {
        return;
    }
    mEnvInitDone = true;
    axl_log_init_from_env();
}

int
axl_log_add_handler(AxlLogHandler handler, void *data)
{
    if (handler == NULL || mHandlerCount >= MAX_HANDLERS) {
        return AXL_ERR;
    }

    mHandlers[mHandlerCount]      = handler;
    mHandlerData[mHandlerCount]   = data;
    mHandlerDomains[mHandlerCount] = NULL;
    mHandlerMaxLevel[mHandlerCount] = AXL_LOG_TRACE;
    mHandlerFiltered[mHandlerCount] = false;
    mHandlerCount++;
    return AXL_OK;
}

int
axl_log_add_domain_handler(const char *domain, int max_level,
                           AxlLogHandler handler, void *data)
{
    if (handler == NULL || mHandlerCount >= MAX_HANDLERS) {
        return AXL_ERR;
    }

    mHandlers[mHandlerCount]      = handler;
    mHandlerData[mHandlerCount]   = data;
    mHandlerDomains[mHandlerCount] = domain;
    mHandlerMaxLevel[mHandlerCount] = max_level;
    mHandlerFiltered[mHandlerCount] = true;
    mHandlerCount++;
    return AXL_OK;
}

void
axl_log_remove_handler(AxlLogHandler handler)
{
    for (size_t i = 0; i < mHandlerCount; i++) {
        if (mHandlers[i] == handler) {
            if (i < mHandlerCount - 1) {
                size_t remaining = mHandlerCount - i - 1;
                axl_memcpy(&mHandlers[i], &mHandlers[i + 1],
                         remaining * sizeof (AxlLogHandler));
                axl_memcpy(&mHandlerData[i], &mHandlerData[i + 1],
                         remaining * sizeof (void *));
                axl_memcpy(&mHandlerDomains[i], &mHandlerDomains[i + 1],
                         remaining * sizeof (const char *));
                axl_memcpy(&mHandlerMaxLevel[i], &mHandlerMaxLevel[i + 1],
                         remaining * sizeof (int));
                axl_memcpy(&mHandlerFiltered[i], &mHandlerFiltered[i + 1],
                         remaining * sizeof (bool));
            }
            mHandlerCount--;
            return;
        }
    }
}

void
axl_log_set_console_enabled(bool enable)
{
    mConsoleEnabled = enable;
}

void
axl_log_suppress_console(void)
{
    axl_log_set_console_enabled(false);
}

void
axl_log_set_console_timestamp(bool enable)
{
    mConsoleTimestamp = enable;
}

void
axl_log_set_console_color(bool enable)
{
    mConsoleColor = enable;
}

void
axl_log_set_fatal_level(int level)
{
    mFatalLevel = level;
}

void
axl_log_set_fatal_image_handle(void *image_handle)
{
    mFatalImageHandle = (EFI_HANDLE)image_handle;
}
