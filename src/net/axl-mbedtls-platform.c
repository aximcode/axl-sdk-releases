/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-mbedtls-platform.c
    mbedTLS platform shim — bridges mbedTLS to AXL runtime.
    Only compiled when AXL_TLS=1.

    Standard C functions (memcpy, strlen, etc.) are provided by
    src/mem/axl-intrinsics.c. This file only handles mbedTLS-specific
    platform hooks: memory allocator, printf, fprintf, time, and entropy.
**/

#include <stddef.h>
#include <stdarg.h>
#include <mbedtls/platform_time.h>
#include "../backend/axl-backend.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-format.h>
#include <axl/axl-time.h>

AXL_LOG_DOMAIN("tls");

// ---------------------------------------------------------------------------
// Memory: mbedTLS calloc/free → axl_calloc/axl_free
// ---------------------------------------------------------------------------

void *
axl_mbedtls_calloc(size_t n, size_t size)
{
    return axl_calloc(n, size);
}

void
axl_mbedtls_free(void *ptr)
{
    axl_free(ptr);
}

// ---------------------------------------------------------------------------
// Printf / snprintf / fprintf
// ---------------------------------------------------------------------------

int
axl_mbedtls_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

typedef struct {
    char   *buf;
    size_t  size;
    size_t  pos;
} SnprintfCtx;

static void
snprintf_write(const char *data, size_t len, void *ctx)
{
    SnprintfCtx *sc = (SnprintfCtx *)ctx;
    for (size_t i = 0; i < len && sc->pos + 1 < sc->size; i++) {
        sc->buf[sc->pos++] = data[i];
    }
    if (sc->size > 0) {
        sc->buf[sc->pos] = '\0';
    }
}

int
axl_mbedtls_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    if (buf == NULL || size == 0 || fmt == NULL) {
        return 0;
    }
    SnprintfCtx sc = { buf, size, 0 };
    va_list args;
    va_start(args, fmt);
    axl_vformat(snprintf_write, &sc, fmt, args);
    va_end(args);
    return (int)sc.pos;
}

int
mbedtls_platform_fprintf(void *stream, const char *fmt, ...)
{
    (void)stream;
    (void)fmt;
    return 0;
}

// ---------------------------------------------------------------------------
// Time: mbedTLS needs time() for certificate validation. Delegate to
// axl_clock_gettime(REALTIME) so there's exactly one EFI_TIME →
// Unix-seconds Gregorian conversion in the codebase.
// ---------------------------------------------------------------------------

long long
time(long long *timer)
{
    long long   result = 0;
    AxlTimespec ts;

    if (axl_clock_gettime(AXL_CLOCK_REALTIME, &ts) == AXL_OK
        && ts.tv_sec >= 0) {
        /* mbedTLS expects Unix seconds; pre-1970 timestamps would
           confuse certificate-validity arithmetic, so clamp to 0
           (matches the previous behavior which gated on
           t.Year >= 1970 before doing any conversion). The new
           tv_sec >= 0 check is slightly stricter — it also rejects
           1970-01-01 dates that go negative after a positive
           tz_minutes offset is subtracted — but the difference is
           a fraction of a day on the epoch boundary, harmless for
           certificate validity windows that span years. */
        result = (long long)ts.tv_sec;
    }

    if (timer != NULL) {
        *timer = result;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Millisecond timer for mbedTLS
// ---------------------------------------------------------------------------

mbedtls_ms_time_t
mbedtls_ms_time(void)
{
    /* Use AXL's monotonic millisecond counter */
    return (mbedtls_ms_time_t)axl_time_get_ms();
}

// ---------------------------------------------------------------------------
// Entropy: UEFI RNG protocol with software fallback
// ---------------------------------------------------------------------------

int
axl_mbedtls_entropy_poll(
    void           *data,
    unsigned char  *output,
    size_t          len,
    size_t         *olen
    )
{
    (void)data;

    /* Try hardware RNG via EFI_RNG_PROTOCOL */
    EFI_GUID rng_guid = { 0x3152bca5, 0xeade, 0x433d,
        { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } };
    void *rng_proto = NULL;

    EFI_STATUS status = axl_bs()->LocateProtocol(
        &rng_guid, NULL, &rng_proto);

    if (status == 0 && rng_proto != NULL) {
        typedef EFI_STATUS (EFIAPI *RNG_GET_RNG)(void *, void *, size_t, void *);
        RNG_GET_RNG get_rng = ((RNG_GET_RNG *)rng_proto)[0];
        status = axl_efi_call(get_rng, 4,
                              rng_proto, NULL, (size_t)len, output);
        if (status == 0) {
            *olen = len;
            return 0;
        }
    }

    /* Fallback: weak entropy from time + monotonic counter */
    axl_debug("hardware RNG unavailable, using weak entropy fallback");
    uint64_t seed = 0;
    EFI_TIME et;
    if (gRT != NULL && axl_efi_call(gRT->GetTime, 2, &et, NULL) == 0) {
        seed = et.Nanosecond;
        seed ^= ((uint64_t)et.Second << 32) | (et.Minute << 16) | et.Hour;
    }

    uint64_t mono = 0;
    if (axl_bs() != NULL) {
        axl_efi_call(axl_bs()->GetNextMonotonicCount, 1, &mono);
    }
    seed ^= mono;

    for (size_t i = 0; i < len; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        output[i] = (unsigned char)(seed & 0xFF);
    }

    *olen = len;
    return 0;
}

// ---------------------------------------------------------------------------
// mbedtls_platform_zeroize — override (MBEDTLS_PLATFORM_ZEROIZE_ALT)
// ---------------------------------------------------------------------------

/* Volatile function pointer prevents the compiler from replacing the
   call with a no-op when it proves buf is dead after return. Same trick
   upstream uses in its own fallback; we can't reach that fallback
   because __GLIBC__ auto-selects the explicit_bzero path on Linux hosts. */
static void *(*const volatile memset_func)(void *, int, size_t) = axl_memset;

void
mbedtls_platform_zeroize(void *buf, size_t len)
{
    if (len > 0) {
        memset_func(buf, 0, len);
    }
}
