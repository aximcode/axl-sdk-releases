/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-ssh-host-key-weak.c
    axl_ssh_host_key_nv_load_or_create() never stores a key made from weak
    entropy.

    A SEPARATE binary because the condition is per image and permanent: the
    DRBG is seeded once, on first use, and an image whose DRBG was ever fed
    from the clock stays that way. So this binary's FIRST draw of randomness
    must be the one under test. It links a strong _axl_rng_test_point that
    makes EFI_RNG_PROTOCOL read as failing, and opts into the clock fallback
    with axl_crypto_set_weak_entropy_allowed(true) -- the configuration a
    platform with no RNG would run -- before anything draws.

    The control is AxlTestSsh's test_ssh_host_key_nv_load_or_create: the
    same call on an image seeded from the real RNG reads GENERATED and
    stores the key.
**/

#include <axl.h>
#include "axl-test.h"

/* Strong over the library's weak, always-false definition: every RNG read
   in this image fails, as on a platform without EFI_RNG_PROTOCOL. */
bool
_axl_rng_test_point(const char *point);

bool
_axl_rng_test_point(const char *point)
{
    return axl_strcmp(point, "fail") == 0;
}

static void
test_weak_entropy_key_is_not_stored(void)
{
    AxlPkKey           *key    = NULL;
    AxlSshHostKeyOrigin origin = AXL_SSH_HOST_KEY_LOADED;
    const char         *why    = NULL;
    AxlSshHostKeyRecord rec;
    uint8_t             probe[4];

    /* The premise: the RNG really reads as absent here. */
    test_check(axl_rng_bytes(probe, sizeof probe) != AXL_OK,
               "weak entropy: the RNG test point makes the RNG fail");
    test_check(axl_ssh_host_key_nv_delete() == AXL_OK,
               "weak entropy: no stored key to begin with");

    axl_crypto_set_weak_entropy_allowed(true);
    int rc = axl_ssh_host_key_nv_load_or_create(&key, &origin, &why);
    test_check(rc == AXL_OK && key != NULL && origin == AXL_SSH_HOST_KEY_EPHEMERAL
               && why != NULL
               && axl_strcmp(why, "weak entropy; the key is not saved") == 0,
               "weak entropy: the key serves as EPHEMERAL, named");
    test_check(axl_ssh_host_key_nv_read(&rec, NULL) == AXL_NOT_FOUND,
               "weak entropy: the key is not stored");
    axl_pk_key_free(key);

    /* A later call in the same image is still tainted: the DRBG keeps its
       clock seed for the life of the image, so this one is not stored
       either -- even with the fallback switched off again. */
    axl_crypto_set_weak_entropy_allowed(false);
    key = NULL;
    rc  = axl_ssh_host_key_nv_load_or_create(&key, &origin, &why);
    test_check(rc == AXL_OK && origin == AXL_SSH_HOST_KEY_EPHEMERAL
               && axl_ssh_host_key_nv_read(&rec, NULL) == AXL_NOT_FOUND,
               "weak entropy: the taint lasts for the life of the image");
    axl_pk_key_free(key);
    axl_ssh_host_key_record_clear(&rec);
}

static int
test_ssh_host_key_weak_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlSshHostKeyWeak");
    axl_pk_enable_ed25519();
    test_weak_entropy_key_is_not_stored();
    return test_print_results();
}

AXL_APP(test_ssh_host_key_weak_main)
