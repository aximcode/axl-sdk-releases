Cryptography
============

AXL's cryptographic primitives are spread across a few headers by
dependency, not gathered behind one ``libcrypto``-style include. This
page is the map.

Two tiers:

- **Standalone** primitives are AXL's own implementations — with one
  exception: SHA-512 forwards to mbedTLS (see the table below), because
  wrapping mbedTLS's SHA-512 costs the same bytes as a fourth
  hand-written compressor and adds no new cryptographic code to audit.
  "Standalone" in the table's *Backend* column names which code computes
  the result, not what an image links: ``axl-digest.c``'s dispatcher
  strongly references every algorithm, so any image touching *any*
  checksum — even MD5 alone — links the mbedTLS SHA-512 object too.
- **mbedTLS-backed** primitives — TLS and public-key signature
  verification — use the vendored mbedTLS, which is compiled into every
  build. ``axl_pk_available()`` / ``axl_tls_available()`` remain public
  API but are guaranteed true and report nothing worth branching on.
  The predicate that can still answer no is ``axl_pk_alg_available()``,
  which reports whether this *image* linked a given algorithm.
  ``--gc-sections`` drops the **TLS stack** — the handshake,
  X.509 and the cipher suites — from an image that never calls
  ``axl_tls_init()``; it does not drop the SHA-512 adapter above, which
  the digest dispatcher references unconditionally.

``<axl/axl-crypto.h>`` is the home for mbedTLS-backed primitives beyond
TLS; it has grown well past the original public-key signature
verification: AEAD (AES-GCM, ChaCha20-Poly1305), ECDH (P-256,
X25519), AES-CTR, and the ``AxlPkKey`` handle API (keygen, DER
import/export, sign/verify) all live here now too.

.. list-table:: Primitives at a glance
   :header-rows: 1
   :widths: 34 30 16 20

   * - Primitive
     - Header
     - Backend
     - Reference
   * - Hashing & checksums (MD5, SHA-1, SHA-256, SHA-512, CRC-32, Adler-32)
     - ``<axl/axl-digest.h>``
     - standalone (SHA-512: mbedTLS)
     - :doc:`data`
   * - HMAC (RFC 2104)
     - ``<axl/axl-hmac.h>``
     - standalone (SHA-512: mbedTLS)
     - :doc:`data`
   * - PBKDF2-HMAC-SHA256 (RFC 8018)
     - ``<axl/axl-digest.h>``
     - standalone
     - :doc:`data`
   * - Constant-time secret compare
     - ``<axl/axl-crypto.h>``
     - standalone
     - below
   * - SCRAM-SHA-256 server auth (RFC 7677)
     - ``<axl/axl-scram.h>``
     - standalone
     - :doc:`scram`
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
     - mbedTLS-backed
     - :doc:`tls`
   * - Public-key signature verification (ECDSA-P256)
     - ``<axl/axl-crypto.h>``
     - mbedTLS-backed
     - below
   * - JOSE — JWS / JWT / JWK signed tokens
     - ``<axl/axl-jose.h>``
     - mbedTLS-backed
     - :doc:`jose`

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
``AXL_PK_ED25519`` is reserved but unsupported and ``axl_pk_verify()``
returns ``AXL_ERR`` for it: mbedTLS 3.6.3 has no twisted-Edwards curve
at all, so no build configuration can supply it today. Use
``axl_pk_alg_available()`` to check which algorithms a given image
actually has, rather than assuming from the enum alone.

Any non-``AXL_OK`` result means "not verified, untrusted" — fail closed.
Use ``axl_pk_alg_available()`` to distinguish "this image did not link
that algorithm" from "signature invalid"; ``axl_pk_available()`` cannot
make that distinction, because it is always true.

.. doxygenfile:: axl-crypto.h
