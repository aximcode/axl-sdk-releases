/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-ssh-host-key-internal.h -- what the two halves of the host-key module
 * (axl-ssh-host-key.c, provider-free; axl-ssh-host-key-load.c,
 * provider-bound) share, plus the module's test seam.
 */

#ifndef AXL_SSH_HOST_KEY_INTERNAL_H
#define AXL_SSH_HOST_KEY_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-ssh-host-key.h>

/* Ed25519 PKCS#8 v1 (RFC 8410): this fixed prefix, then the 32-byte seed --
   the form axl_pk_key_get_private_der() writes and the provider loads. */
#define AXL_SSH_HK_PKCS8_PREFIX_LEN  16u
#define AXL_SSH_HK_SEED_LEN          32u

extern const uint8_t axl_ssh_hk_pkcs8_prefix[AXL_SSH_HK_PKCS8_PREFIX_LEN];

/* Reasons more than one function gives, spelled once. */
#define AXL_SSH_HK_WHY_NOT_KEY     "not a private key file"
#define AXL_SSH_HK_WHY_TOO_LARGE   "file too large to be a host key"
#define AXL_SSH_HK_WHY_MALFORMED   "truncated or malformed key"
#define AXL_SSH_HK_WHY_HALVES      "corrupt key (public and private halves differ)"
#define AXL_SSH_HK_WHY_PASSPHRASE  "passphrase-protected host keys are not supported"
#define AXL_SSH_HK_WHY_PKCS8 \
    "PKCS#8 keys are not supported; convert with ssh-keygen -p -N '' -f FILE"
#define AXL_SSH_HK_WHY_READ        "cannot read the variable"

/* True when @a pkcs8 (AXL_SSH_HOST_KEY_PKCS8_LEN bytes) has the Ed25519
   prefix. */
bool
axl_ssh_hk_pkcs8_is_ed25519(
    const uint8_t *pkcs8);

/* What is wrong with a stored value read as the firmware reports it -- @a
   attrs the RAW EFI_VARIABLE_* word -- as one of axl_ssh_host_key_nv_read()'s
   reasons, or NULL with @a out filled. Pure, so the attribute words OVMF
   cannot create (an authenticated variable needs a signed payload) are unit
   tested directly (midpoint review MP5). */
const char *
axl_ssh_hk_stored_check(
    uint32_t             attrs,
    const uint8_t       *data,
    size_t               size,
    AxlSshHostKeyRecord *out);

/*
 * The host-key module's test seam: true makes the library act at @p point as
 * a test asks.
 *   "nv-read"      axl_ssh_host_key_nv_read() reports a firmware read error;
 *   "nv-write"     axl_ssh_host_key_nv_write()'s SetVariable fails;
 *   "nv-readback"  its read-back reads as different;
 *   "nv-cleanup-refused"  nv_write's delete of a write that did not read back
 *                  fails, so that write stays in the variable;
   "nv-delete-denied"  axl_ssh_host_key_nv_delete()'s delete is refused, as
                  the firmware refuses one of an authenticated variable.
 *
 * The library's definition is WEAK and always false, so production carries no
 * trace of a test control; a test image links a strong one
 * (test/unit/axl-test-ssh.c, test/integration/sshd-test-hook.c). Same pattern
 * as _axl_service_stage_test_point.
 *
 * EVERY TEST SEAM IS NAMED `*_test_point`: test-efi-pack-release-qemu.sh
 * finds the seams in a shipped image by that suffix alone, and fails one
 * whose size is not the weak always-false stub's. A seam named otherwise is
 * invisible to it.
 */
bool
_axl_ssh_host_key_test_point(
    const char *point);

#endif /* AXL_SSH_HOST_KEY_INTERNAL_H */
