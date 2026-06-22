/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-scram-internal.h
    Private test seam for the AxlScram engine.

    Not shipped to SDK consumers. `axl_scram_server_first` generates a
    random server nonce, which makes the server-first message
    non-deterministic and impossible to pin to a published vector. This
    seam lets the unit test supply a fixed server nonce so the exchange
    matches the RFC 7677 wire bytes exactly. Production code uses the
    public `axl_scram_server_first`.
**/

#ifndef AXL_SCRAM_INTERNAL_H
#define AXL_SCRAM_INTERNAL_H

#include <axl/axl-scram.h>

/* Like axl_scram_server_first, but with a caller-supplied server nonce
   (printable, comma-free ASCII) instead of a fresh random one. */
AXL_WARN_UNUSED int
_axl_scram_server_first_nonce(
    const AxlScramCredential *cred,
    const char              *client_first,
    size_t                   client_first_len,
    const char              *server_nonce,
    size_t                   server_nonce_len,
    char                    *out_server_first,
    size_t                   out_server_first_size,
    AxlScramState           *out_state
);

/* Like axl_scram_client_first, but with a caller-supplied client nonce
   instead of a fresh random one (for pinning to a published vector). */
AXL_WARN_UNUSED int
_axl_scram_client_first_nonce(
    const char          *username,
    const char          *client_nonce,
    size_t               client_nonce_len,
    char                *out_client_first,
    size_t               out_size,
    AxlScramClientState *out_state
);

#endif /* AXL_SCRAM_INTERNAL_H */
