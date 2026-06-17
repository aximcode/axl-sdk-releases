Cryptography
============

AXL's cryptographic primitives are spread across a few headers by
dependency, not gathered behind one ``libcrypto``-style include. This
page is the map.

Two tiers:

- **Standalone** primitives have no external dependency and are
  available in every build — hashing, HMAC, randomness, and Authenticode
  image verification.
- **mbedTLS-backed** primitives require an ``AXL_TLS=1`` build (which
  links the vendored mbedTLS): TLS and public-key signature
  verification. Without ``AXL_TLS=1`` they fail closed (see
  ``axl_pk_available()`` / ``axl_tls_available()``).

``<axl/axl-crypto.h>`` is the home for mbedTLS-backed primitives beyond
TLS; it grows on demand as consumers need them (today: public-key
signature verification).

.. list-table:: Primitives at a glance
   :header-rows: 1
   :widths: 34 30 16 20

   * - Primitive
     - Header
     - Backend
     - Reference
   * - Hashing & checksums (MD5, SHA-1, SHA-256, CRC-32, Adler-32)
     - ``<axl/axl-digest.h>``
     - standalone
     - :doc:`data`
   * - HMAC (RFC 2104)
     - ``<axl/axl-hmac.h>``
     - standalone
     - :doc:`data`
   * - Cryptographic random bytes (``EFI_RNG_PROTOCOL``)
     - ``<axl/axl-rng.h>``
     - standalone
     - :doc:`rng`
   * - Deterministic PRNG (``GRand``-style)
     - ``<axl/axl-rand.h>``
     - standalone
     - :doc:`rand`
   * - Authenticode image-signature verification
     - ``<axl/axl-image-verify.h>``
     - standalone
     - :doc:`image`
   * - TLS 1.2 server / client
     - ``<axl/axl-tls.h>``
     - needs ``AXL_TLS=1``
     - :doc:`tls`
   * - Public-key signature verification (ECDSA-P256)
     - ``<axl/axl-crypto.h>``
     - needs ``AXL_TLS=1``
     - below

Public-key signature verification
---------------------------------

``axl_pk_verify`` checks a detached signature over a message against a
public key the consumer ships — the building block for signed firmware
updates, signed config blobs, or Secure-Boot-style image checks. It is
pure verification: there is no signing side (the private key is held
offline by the vendor and never ships).

``AXL_PK_ECDSA_P256`` (ECDSA over NIST P-256 with SHA-256) is supported:
the public key is a DER ``SubjectPublicKeyInfo``, the signature is a DER
ECDSA signature, and the message is hashed with SHA-256 internally.
``AXL_PK_ED25519`` is reserved but unsupported by the current mbedTLS
build (which does not enable PSA crypto) and returns ``AXL_ERR``.

Any non-``AXL_OK`` result means "not verified, untrusted" — fail closed.
Use ``axl_pk_available()`` to distinguish "verification not compiled in"
from "signature invalid".

.. doxygenfile:: axl-crypto.h
