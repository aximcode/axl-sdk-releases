/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tpm-internal.h
    Private TPM2 wire-marshaling shared by the AxlTpm module files
    (axl-tpm.c EK read, axl-tpm-seal.c seal/unseal). Not shipped to SDK
    consumers — everything public is in <axl/axl-tpm.h>.

    TPM2 structures are big-endian on the wire; these are bounds-checked
    write/read cursors plus the algorithm/handle constants both files use.
**/

#ifndef AXL_TPM_INTERNAL_H
#define AXL_TPM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* TPM2 constants shared across commands (TCG TPM 2.0 Structures). */
#define TPM_ST_SESSIONS        0x8002u
#define TPM_ST_NO_SESSIONS     0x8001u
#define TPM_CC_CREATE_PRIMARY  0x00000131u
#define TPM_CC_FLUSH_CONTEXT   0x00000165u
#define TPM_RS_PW              0x40000009u
#define TPM_ALG_ECC            0x0023u
#define TPM_ALG_SHA256         0x000Bu
#define TPM_ALG_AES            0x0006u
#define TPM_ALG_CFB            0x0043u
#define TPM_ALG_NULL           0x0010u
#define TPM_ECC_NIST_P256      0x0003u

// ---- big-endian write cursor ----
typedef struct {
    uint8_t *p;
    size_t   cap;
    size_t   len;
    bool     ok;
} TpmWr;

static inline void
wr8(TpmWr *w, uint8_t v)
{
    if (w->len < w->cap) {
        w->p[w->len++] = v;
    } else {
        w->ok = false;
    }
}
static inline void wr16(TpmWr *w, uint16_t v) { wr8(w, (uint8_t)(v >> 8)); wr8(w, (uint8_t)v); }
static inline void wr32(TpmWr *w, uint32_t v) { wr16(w, (uint16_t)(v >> 16)); wr16(w, (uint16_t)v); }
static inline void
wr_zeros(TpmWr *w, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        wr8(w, 0);
    }
}
static inline void
wr_bytes(TpmWr *w, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        wr8(w, b[i]);
    }
}

// ---- big-endian read cursor (bounds-checked) ----
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         off;
    bool           ok;
} TpmRd;

static inline uint8_t
rd8(TpmRd *r)
{
    if (r->off < r->len) {
        return r->p[r->off++];
    }
    r->ok = false;
    return 0;
}
static inline uint16_t rd16(TpmRd *r) { uint16_t h = rd8(r); return (uint16_t)((h << 8) | rd8(r)); }
static inline uint32_t rd32(TpmRd *r) { uint32_t h = rd16(r); return (h << 16) | rd16(r); }
static inline void
rd_skip(TpmRd *r, size_t n)
{
    if (r->off + n <= r->len) {
        r->off += n;
    } else {
        r->ok  = false;
        r->off = r->len;
    }
}

#endif /* AXL_TPM_INTERNAL_H */
