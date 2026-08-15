/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Minimal time.h shim for mbedTLS in freestanding UEFI environment.
 *
 * Provides time_t for freestanding builds and sets the glibc/musl
 * guard macros so the system time.h (if pulled in transitively by
 * mbedTLS via sys/socket.h) doesn't redefine it.
 */
#ifndef AXL_MBEDTLS_TIME_H
#define AXL_MBEDTLS_TIME_H

#ifndef __time_t_defined
#define __time_t_defined 1
#ifndef _TIME_T
#define _TIME_T
#endif
typedef long long time_t;
#endif

typedef long long mbedtls_time_t;

#ifdef __cplusplus
extern "C" {
#endif

time_t time(time_t *timer);

#ifdef __cplusplus
}
#endif

#endif
