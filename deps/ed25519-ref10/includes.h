/* Not upstream. deps/ed25519-ref10/ed25519.c is byte-identical to
   OpenSSH's, and its first line is `#include "includes.h"` -- OpenSSH's
   autoconf umbrella, which pulls that project's whole build world. This
   file exists so the vendored source compiles UNEDITED against it.

   It is deliberately empty of declarations: ed25519.c includes <string.h>
   and "crypto_api.h" itself, and everything else it needs comes from the
   trimmed crypto_api.h beside this file. Supplying an empty shim rather
   than deleting the include is what keeps a resync a copy rather than a
   merge -- see README.md and the design spec's §6b.

   OPENSSL_HAS_ED25519 must stay UNDEFINED: ed25519.c guards its entire
   body with `#ifndef OPENSSL_HAS_ED25519`, so defining it compiles the
   file to nothing. Nothing in this tree defines it; the unit test that
   calls crypto_sign_ed25519_keypair_from_seed would fail to link if
   anything ever did. */

#ifndef AXL_ED25519_REF10_INCLUDES_H
#define AXL_ED25519_REF10_INCLUDES_H

#endif /* AXL_ED25519_REF10_INCLUDES_H */
