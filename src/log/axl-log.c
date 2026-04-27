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
print_console_timestamp(void)
{
    AxlTime time;

    if (axl_backend_get_time(&time) != 0) {
        return;
    }

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
    buf[pos++] = '.';
    unsigned usec = time.nanosecond / 1000;
    buf[pos++] = '0' + ((usec / 100000) % 10);
    buf[pos++] = '0' + ((usec / 10000) % 10);
    buf[pos++] = '0' + ((usec / 1000) % 10);
    buf[pos++] = '0' + ((usec / 100) % 10);
    buf[pos++] = '0' + ((usec / 10) % 10);
    buf[pos++] = '0' + (usec % 10);
    buf[pos++] = ' ';
    buf[pos] = '\0';

    unsigned short wide[24];
    axl_utf8_to_ucs2_buf(buf, wide, 24);
    axl_backend_console_write(wide);
}

// ---------------------------------------------------------------------------
// Core Log Dispatch
// ---------------------------------------------------------------------------

static void
log_dispatch(int level, const char *domain, const char *func,
             int line, const char *msg_buf)
{
    unsigned short wide[MSG_BUF_SIZE];

    // Console output
    if (mConsoleEnabled && axl_st() != NULL && axl_st()->ConOut != NULL) {
        if (mConsoleColor && level <= AXL_LOG_TRACE) {
            axl_backend_console_set_attr(mLevelColor[level]);
        }

        if (mConsoleTimestamp) {
            print_console_timestamp();
        }

        if (level <= AXL_LOG_TRACE) {
            axl_backend_console_write((const unsigned short *)mLevelPrefix[level]);
        }

        if (domain != NULL) {
            unsigned short wide_domain[DOMAIN_LEN];
            axl_utf8_to_ucs2_buf(domain, wide_domain, DOMAIN_LEN);
            axl_backend_console_write(wide_domain);

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
                axl_backend_console_write(wide_loc);
            }
            axl_backend_console_write((const unsigned short *)L": ");
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
            axl_backend_console_write(wide_loc);
        }

        axl_utf8_to_ucs2_buf(msg_buf, wide, MSG_BUF_SIZE);
        axl_backend_console_write(wide);
        axl_backend_console_write((const unsigned short *)L"\r\n");

        if (mConsoleColor) {
            axl_backend_console_set_attr(DEFAULT_ATTR);
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
        mHandlers[i](level, domain, msg_buf, mHandlerData[i]);
    }

    // Fatal level check
    if (mFatalLevel != -1 && level <= mFatalLevel) {
        mFatalTriggered = true;
        if (mFatalImageHandle != NULL && axl_bs() != NULL) {
            axl_bs()->Exit(mFatalImageHandle, EFI_ABORTED, 0, NULL);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_log_full(int level, const char *domain, const char *func,
             int line, const char *fmt, ...)
{
    va_list args;
    char msg_buf[MSG_BUF_SIZE];
    BufCtx bc = { msg_buf, 0, sizeof (msg_buf) };

    if (level > get_effective_level(domain)) {
        return;
    }

    va_start(args, fmt);
    axl_vformat(buf_write, &bc, fmt, args);
    va_end(args);

    log_dispatch(level, domain, func, line, msg_buf);
}

void
axl_log(int level, const char *domain, const char *fmt, ...)
{
    va_list args;
    char msg_buf[MSG_BUF_SIZE];
    BufCtx bc = { msg_buf, 0, sizeof (msg_buf) };

    if (level > get_effective_level(domain)) {
        return;
    }

    va_start(args, fmt);
    axl_vformat(buf_write, &bc, fmt, args);
    va_end(args);

    log_dispatch(level, domain, NULL, 0, msg_buf);
}

void
axl_log_set_level(int level)
{
    mGlobalLevel = level;
}

void
axl_log_set_domain_level(const char *domain, int level)
{
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

int
axl_log_add_handler(AxlLogHandler handler, void *data)
{
    if (handler == NULL || mHandlerCount >= MAX_HANDLERS) {
        return -1;
    }

    mHandlers[mHandlerCount]      = handler;
    mHandlerData[mHandlerCount]   = data;
    mHandlerDomains[mHandlerCount] = NULL;
    mHandlerMaxLevel[mHandlerCount] = AXL_LOG_TRACE;
    mHandlerFiltered[mHandlerCount] = false;
    mHandlerCount++;
    return 0;
}

int
axl_log_add_domain_handler(const char *domain, int max_level,
                           AxlLogHandler handler, void *data)
{
    if (handler == NULL || mHandlerCount >= MAX_HANDLERS) {
        return -1;
    }

    mHandlers[mHandlerCount]      = handler;
    mHandlerData[mHandlerCount]   = data;
    mHandlerDomains[mHandlerCount] = domain;
    mHandlerMaxLevel[mHandlerCount] = max_level;
    mHandlerFiltered[mHandlerCount] = true;
    mHandlerCount++;
    return 0;
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
axl_log_suppress_console(void)
{
    mConsoleEnabled = false;
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
