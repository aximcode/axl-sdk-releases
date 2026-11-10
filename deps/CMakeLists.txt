# deps -- vendored code. mbedTLS (and AXL's platform glue for it) closes
# libaxl.a, as the Makefile appends MBEDTLS_SOURCES last; the other vendored
# libraries are added by the module that uses them (lzma: src/data,
# ed25519-ref10: src/net, libvterm: src/vterm). deps/mbedtls is a submodule
# with a CMakeLists.txt of its own, which is deliberately NOT used: the build
# compiles its sources with AXL's flags and AXL's config header.
target_sources(axl PRIVATE
    ${CMAKE_SOURCE_DIR}/src/net/axl-mbedtls-platform.c
    mbedtls/library/aes.c
    mbedtls/library/asn1parse.c
    mbedtls/library/asn1write.c
    mbedtls/library/base64.c
    mbedtls/library/bignum.c
    mbedtls/library/bignum_core.c
    mbedtls/library/chacha20.c
    mbedtls/library/chachapoly.c
    mbedtls/library/cipher.c
    mbedtls/library/cipher_wrap.c
    mbedtls/library/constant_time.c
    mbedtls/library/ctr_drbg.c
    mbedtls/library/ecdh.c
    mbedtls/library/ecdsa.c
    mbedtls/library/ecp.c
    mbedtls/library/ecp_curves.c
    mbedtls/library/entropy.c
    mbedtls/library/error.c
    mbedtls/library/gcm.c
    mbedtls/library/poly1305.c
    mbedtls/library/hkdf.c
    mbedtls/library/md.c
    mbedtls/library/oid.c
    mbedtls/library/pem.c
    mbedtls/library/pk.c
    mbedtls/library/pkparse.c
    mbedtls/library/pkwrite.c
    mbedtls/library/pk_ecc.c
    mbedtls/library/pk_wrap.c
    mbedtls/library/rsa.c
    mbedtls/library/rsa_alt_helpers.c
    mbedtls/library/platform.c
    mbedtls/library/platform_util.c
    mbedtls/library/sha256.c
    mbedtls/library/sha512.c
    mbedtls/library/ssl_cache.c
    mbedtls/library/ssl_ciphersuites.c
    mbedtls/library/ssl_client.c
    mbedtls/library/ssl_cookie.c
    mbedtls/library/ssl_msg.c
    mbedtls/library/ssl_ticket.c
    mbedtls/library/ssl_tls.c
    mbedtls/library/ssl_tls12_client.c
    mbedtls/library/ssl_tls12_server.c
    mbedtls/library/version.c
    mbedtls/library/x509.c
    mbedtls/library/x509_create.c
    mbedtls/library/x509_crt.c
    mbedtls/library/x509write.c
    mbedtls/library/x509write_crt.c)

# ...and after mbedTLS, as the Makefile's `LIB_OBJS +=` appends it: the legacy
# IP4Config driver image as a blob (src/net/CMakeLists.txt).
axl_embed_blob(legacy_ip4cfg ${CMAKE_BINARY_DIR}/build/axl-legacy-ip4cfg.efi
    INTO axl DEPENDS axl_image_legacy_ip4cfg)

