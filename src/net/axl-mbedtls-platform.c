/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-mbedtls-platform.c
    mbedTLS platform shim — bridges mbedTLS to AXL runtime.
    Compiled into every build: mbedTLS is unconditional since the
    AXL_TLS flag was removed.

    Standard C functions (memcpy, strlen, etc.) come from newlib. This
    file only handles mbedTLS-specific platform hooks: memory
    allocator, printf, fprintf, time, and entropy.

    It also implements the two public entropy-policy calls declared in
    <axl/axl-crypto.h> -- axl_crypto_set_weak_entropy_allowed() and
    axl_crypto_is_weak_entropy_allowed(). They live beside the entropy
    poll rather than with the rest of AxlCrypto because the poll is
    their only reader, which keeps the flag and the branch that honors
    it in one translation unit.
**/

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/entropy.h>
#include "../backend/axl-backend.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-format.h>
#include <axl/axl-time.h>
#include <axl/axl-crypto.h>
#include <axl/axl-rng.h>

/* "crypto", not "tls": this file's only log line is the entropy poll, and
   that poll seeds the DRBG behind SSH host keys, Ed25519 and ECDH key
   generation and PK signing as well as TLS. Filing a warning about SSH key
   material under a "tls" domain would send an operator looking in the wrong
   place -- and the domain is what AXL_LOG_LEVEL selects on. */
AXL_LOG_DOMAIN("crypto");

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

/* Spelled with `time_t` rather than `long long` so the definition matches
   whichever <time.h> is on the path. They are the same width today but not
   the same TYPE: include/compat's shim typedefs `time_t` as `long long`,
   while newlib's is `long` (`_TIME_T_` in sys/_types.h) — and a bare-metal
   toolchain build against newlib's real header therefore rejected this with
   "conflicting types for 'time'". `time_t` is correct under both.
   docs/AXL-Libc-Substrate-Design.md §4.1b. */
time_t
time(time_t *timer)
{
    time_t      result = 0;
    AxlTimespec ts;

    if (axl_clock_gettime(AXL_CLOCK_REALTIME, &ts) == AXL_OK
        && ts.tv_sec >= 0) {
        /* mbedTLS expects Unix seconds; pre-1970 timestamps would
           confuse certificate-validity arithmetic, so clamp to 0
           (matches the previous behavior which gated on
           t.Year >= 1970 before doing any conversion). The new
           tv_sec >= 0 check is slightly stricter — it also rejects
           1970-01-01 dates that go negative after a negative
           tz_minutes offset is added (UEFI: UTC = local + TimeZone)
           — but the difference is
           a fraction of a day on the epoch boundary, harmless for
           certificate validity windows that span years. */
        result = (time_t)ts.tv_sec;
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
// Entropy: EFI_RNG_PROTOCOL, and what happens when it is not there.
//
// This is the ONLY entropy source registered with mbedTLS (see
// axl-crypto-rng.c and axl-tls.c, which both add it with a threshold of
// 32 and MBEDTLS_ENTROPY_SOURCE_STRONG; MBEDTLS_NO_PLATFORM_ENTROPY means
// mbedtls_entropy_init() adds none of its own). So every private key the
// SDK produces traces back to this function.
//
// mbedTLS credits whatever a source reports through *olen toward that
// threshold, without inspecting the bytes. Reporting a clock-seeded
// xorshift as gathered entropy therefore satisfies the threshold and seeds
// the DRBG -- which is why the fallback below reports NOTHING unless the
// consumer has explicitly opted in.
// ---------------------------------------------------------------------------

/* Default false: no hardware RNG means no key, rather than a key derived
   from a wall clock. Lives in this TU so the flag and its only reader are
   the same object -- a consumer that calls the setter necessarily links the
   poll that honors it. */
static bool g_weak_entropy_allowed;

/* Set the first time the clock fallback actually FEEDS the pool, and never
   cleared: a DRBG seeded from a clock stays derived from it for the life of
   the image, whatever the policy flag says afterwards. Its reader is
   axl_crypto_entropy_was_weak(), which axl_ssh_host_key_nv_load_or_create()
   asks before persisting a key -- the policy flag answers a different
   question ("may it happen?", not "did it?"). */
static bool g_weak_entropy_used;

bool
axl_crypto_entropy_was_weak(
    void
    )
{
    return g_weak_entropy_used;
}

void
axl_crypto_set_weak_entropy_allowed(
    bool allowed
    )
{
    g_weak_entropy_allowed = allowed;
}

bool
axl_crypto_is_weak_entropy_allowed(
    void
    )
{
    return g_weak_entropy_allowed;
}

/// Fill @p output with @p len bytes from a xorshift64 generator started
/// from the real-time clock and the firmware's monotonic counter. NOT an
/// entropy source: the state space is bounded by how precisely an attacker
/// can bound boot time. Reachable only through the opt-in above.
static void
entropy_from_clock(
    unsigned char *output,
    size_t         len
    )
{
    uint64_t seed = 0;
    AxlTime  et;
    /* Through the backend, not gRT->GetTime directly: the backend carries the
     * RTC re-entrancy guard, and entropy seeding is reachable from a TLS
     * handshake driven off an event callback. */
    if (axl_backend_get_time(&et) == AXL_OK) {
        seed = et.nanosecond;
        seed ^= ((uint64_t)et.second << 32) | (et.minute << 16) | et.hour;
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
}

int
axl_mbedtls_entropy_poll(
    void           *data,
    unsigned char  *output,
    size_t          len,
    size_t         *olen
    )
{
    (void)data;

    *olen = 0;

    /* Through axl_rng_bytes() rather than a second, local EFI_RNG_PROTOCOL
       binding. The local one was WRONG and silently so: it took
       ((RNG_GET_RNG *)proto)[0], but member 0 of EFI_RNG_PROTOCOL is
       GetInfo and GetRNG is member 1 (UEFI 2.11 §37.5). So it called
       GetInfo(This, NULL, output), which answers EFI_INVALID_PARAMETER for
       a NULL size pointer -- on EVERY platform, including ones whose RNG
       works. The hardware branch never once succeeded, and the fallback
       below silently absorbed it, which is precisely what fail-closed and
       one binding per protocol exist to prevent. */
    if (axl_rng_bytes(output, len) == AXL_OK) {
        *olen = len;
        return 0;
    }

    if (!g_weak_entropy_allowed) {
        /* MBEDTLS_ERR_ENTROPY_SOURCE_FAILED rather than "return 0 with
           *olen = 0". Both end in a failed seed, but they cost very
           differently: entropy_gather_internal() propagates a source
           ERROR immediately, whereas a zero-length success leaves the
           threshold unmet and mbedtls_entropy_func() re-polls
           ENTROPY_MAX_LOOP (256) times before giving up -- 256 protocol
           lookups and 256 log lines to reach the same verdict. The error
           is also what the library documents this condition as. */
        /* log-level: the caller mbedTLS reports this to is not the caller
           who needs to know. By the time it surfaces it is an
           axl_pk_key_new() NULL or an axl_tls_init() AXL_ERR, neither of
           which says WHY, and the why is a security property of the
           platform. */
        axl_error("no usable EFI_RNG_PROTOCOL: refusing to seed the DRBG"
                  " from a clock -- no key will be generated."
                  " axl_crypto_set_weak_entropy_allowed(true) accepts"
                  " predictable keys on a platform that has no RNG");
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    /* Opted in. Loud EVERY time, not once and not at debug: an image built
       this way is generating guessable key material for as long as it runs,
       and a single line at startup is easy to miss in a boot log. */
    /* log-level: nothing in the return value says these bytes came from a
       clock -- mbedTLS sees an ordinary successful poll. This line is the
       only record that the key being generated is predictable. */
    axl_warning("no usable EFI_RNG_PROTOCOL: seeding from the clock"
                " because weak entropy was explicitly allowed --"
                " keys generated now are predictable");
    entropy_from_clock(output, len);
    g_weak_entropy_used = true;
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
