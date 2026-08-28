SCRAM-SHA-256 authentication
============================

Server-side SCRAM-SHA-256 (RFC 5802 / RFC 7677): authenticate a client
over a password without the server ever storing — or seeing on the wire
— a password-equivalent. The home for a BMC / web console that wants
salted, challenge/response browser login instead of sending the password
(even over TLS).

At enrollment the server runs ``axl_scram_sha256_derive`` and keeps only
``{salt, iterations, StoredKey, ServerKey}`` — never the password. At
login the client proves knowledge of the password through a two-message
challenge/response the server drives with ``axl_scram_server_first`` then
``axl_scram_server_final``; the engine verifies the client proof with a
constant-time compare (``axl_consttime_equal``) and returns a server
signature the client checks in turn.

This is plain SCRAM-SHA-256 (gs2 header ``n,,``, no channel binding):
a browser client driving it from JavaScript cannot read the TLS
channel-binding hash, so run it inside TLS for transport security. The
two login messages arrive on two separate requests, so ``AxlScramState``
is a plain serializable value the caller parks between the steps.

A matching **client** engine (``axl_scram_client_first`` /
``axl_scram_client_final`` / ``axl_scram_client_verify``) is provided for
an axl consumer that authenticates *to* a SCRAM server — a client tool,
agt, or a test driver — including verifying the server's signature for
mutual authentication. It holds the password only for the exchange and
never sends it.

Built on the digest / HMAC / PBKDF2 / base64 / RNG primitives, all of
which compute using AXL's own SHA-256 implementation.

API Reference
-------------

.. doxygenfile:: axl-scram.h
