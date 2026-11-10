/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-ssh-core.h
 * @brief Role-neutral SSH facts: the algorithms AxlSsh speaks and the limits
 *        it enforces.
 *
 * The half of the SSH surface a server and a client share. The split exists
 * now rather than when the client arrives (P6), because moving a symbol
 * between headers after consumers exist is an API break.
 *
 * **What is deliberately NOT here.** The design sketch put "packet framing,
 * KEXINIT codec, KDF" in this header. Those are implemented, and they stay
 * internal (`src/net/axl-ssh-internal.h`) until a second role actually
 * consumes them: publishing a codec with exactly one caller would fix its
 * shape before the client has had a chance to disagree with it, and the
 * client is precisely the evidence that shape is right. What a *consumer*
 * needs today is which algorithms this speaks and what it refuses — so that
 * is what this header is.
 *
 * AxlSsh negotiates **one algorithm per slot**. That is a security decision,
 * not an unfinished one: an algorithm matrix is a downgrade surface, and
 * there are no legacy peers to accommodate.
 */

#ifndef AXL_SSH_CORE_H
#define AXL_SSH_CORE_H

#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The identification line AxlSsh sends, without its CRLF.
 *
 * RFC 4253 §4.2. A peer sees this as `remote software version`.
 */
#define AXL_SSH_VERSION_STRING  "SSH-2.0-AxlSsh_1.0"

/**
 * @brief Key exchange method — the only one offered or accepted.
 *
 * RFC 8731. The shared secret is X25519; the exchange hash is SHA-256.
 */
#define AXL_SSH_KEX_NAME        "curve25519-sha256"

/**
 * @brief Strict key exchange is offered and enforced (Terrapin,
 *        CVE-2023-48795).
 *
 * AxlSsh appends OpenSSH's `kex-strict-s-v00@openssh.com` pseudo-algorithm to
 * the `kex_algorithms` list of its initial `KEXINIT`, and honours a client's
 * `kex-strict-c-v00@openssh.com`, per OpenSSH's `PROTOCOL` §1.10. Both of
 * that section's rules are in force: any message during key exchange that is
 * not the one required terminates the connection — including
 * `SSH_MSG_IGNORE`, `SSH_MSG_DEBUG` and `SSH_MSG_UNIMPLEMENTED`, which RFC
 * 4253 §11 would otherwise have a server ignore — and both packet sequence
 * numbers reset to zero at `SSH_MSG_NEWKEYS`.
 *
 * `ssh -vvv` reports the negotiation as
 * `kex_choose_conf: will use strict KEX ordering`.
 *
 * The pseudo-name is not a key exchange method and is never selected as one:
 * a peer that offers it and no real method is refused.
 */
#define AXL_SSH_KEX_STRICT      1

/**
 * @brief Host key algorithm — the only one offered or accepted.
 *
 * RFC 8709. Needs the Ed25519 provider, which axl_ssh_server_new() enables
 * itself with axl_pk_enable_ed25519(). If its key cannot be made,
 * axl_ssh_server_new() fails rather than falling back to another algorithm.
 */
#define AXL_SSH_HOSTKEY_NAME    "ssh-ed25519"

/**
 * @brief Cipher — the only one offered or accepted.
 *
 * OpenSSH's `PROTOCOL.chacha20poly1305`. An AEAD, so no separate MAC is
 * negotiated and the MAC name-lists are empty by design.
 */
#define AXL_SSH_CIPHER_NAME     "chacha20-poly1305@openssh.com"

/**
 * @brief Largest total packet AxlSsh will send or accept, in bytes.
 *
 * RFC 4253 §6.1 requires every implementation to handle 35000, and AxlSsh
 * treats that as the ceiling rather than a floor: the length field is the one
 * peer-chosen value read before authentication, so it is bounded before it is
 * used to size anything.
 */
#define AXL_SSH_MAX_PACKET      35000u

/**
 * @brief Disconnect reason codes AxlSsh sends (RFC 4253 §11.1).
 *
 * Every protocol violation ends the connection — there is no attempt to
 * resynchronise a stream whose framing is already in doubt.
 */
typedef enum {
    AXL_SSH_DISCONNECT_PROTOCOL_ERROR        = 2,  /**< Malformed message. */
    AXL_SSH_DISCONNECT_KEY_EXCHANGE_FAILED   = 3,  /**< No common algorithm,
                                                        or a failed exchange. */
    AXL_SSH_DISCONNECT_MAC_ERROR             = 5,  /**< Packet failed its tag. */
    AXL_SSH_DISCONNECT_SERVICE_NOT_AVAILABLE = 7,  /**< Requested service is
                                                        not one we offer. */
    AXL_SSH_DISCONNECT_BY_APPLICATION        = 11, /**< Server shutting down. */
    AXL_SSH_DISCONNECT_NO_MORE_AUTH_METHODS  = 14  /**< Authentication
                                                        exhausted. */
} AxlSshDisconnectReason;

#ifdef __cplusplus
}
#endif

#endif /* AXL_SSH_CORE_H */
