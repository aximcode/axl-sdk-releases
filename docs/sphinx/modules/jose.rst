AxlJose — JOSE (JWS / JWT / JWK)
==================================

JSON Object Signing and Encryption: JWS Compact sign/verify (RFC
7515), JWT registered-claim validation (RFC 7519), and JWK/JWK-Set
parse/export/``kid`` resolution (RFC 7517), built on :doc:`crypto`.
Supported algorithms are ES256, ES384, RS256, PS256, and HS256; JWE
and non-compact JWS serialization are out of scope. Like
``axl-tls.h`` / ``axl-crypto.h``, this header requires an
``AXL_TLS=1`` build — see :doc:`crypto` for the tier overview.

Verification is allow-list only: the caller passes a mandatory list
of accepted algorithms rather than trusting the token's own ``alg``,
``none`` is not representable, and the key is bound to its algorithm
family, so RS256/HS256 key confusion is structurally impossible.

Header: ``<axl/axl-jose.h>``

API Reference
-------------

.. doxygenfile:: axl-jose.h
