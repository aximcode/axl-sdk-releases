/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-inet-address.h:
 *
 * IPv4 address and socket address types. AxlInetAddress wraps an IP
 * address with parsing, formatting, and comparison. AxlSocketAddress
 * pairs an address with a port number for use with AxlSocket.
 *
 * @code
 * AxlInetAddress *addr = axl_inet_address_new_from_string("192.168.1.1");
 * AxlSocketAddress *sa = axl_socket_address_new(addr, 8080);
 * // sa now owns addr — do not free addr separately
 * axl_socket_address_free(sa);
 * @endcode
 */

#ifndef AXL_INET_ADDRESS_H
#define AXL_INET_ADDRESS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlInetAddress AxlInetAddress;

// ---------------------------------------------------------------------------
// AxlInetAddress — IPv4 address
// ---------------------------------------------------------------------------

/**
 * @brief Create an address from a dotted-decimal string.
 *
 * Parses strings like "192.168.1.1". Each octet must be 0-255.
 *
 * @return new address, or NULL on invalid input.
 */
AxlInetAddress *
axl_inet_address_new_from_string(
    const char *str  ///< dotted-decimal IPv4 string (e.g. "10.0.0.1")
);

/**
 * @brief Create an address from raw bytes.
 *
 * @return new address, or NULL on allocation failure.
 */
AxlInetAddress *
axl_inet_address_new_from_bytes(
    const uint8_t *bytes  ///< 4-byte IPv4 address in network order
);

/**
 * @brief Create the any-address (0.0.0.0).
 *
 * @return new address, or NULL on allocation failure.
 */
AxlInetAddress *
axl_inet_address_new_any(void);

/**
 * @brief Create the loopback address (127.0.0.1).
 *
 * @return new address, or NULL on allocation failure.
 */
AxlInetAddress *
axl_inet_address_new_loopback(void);

/**
 * @brief Free an address. NULL-safe.
 */
void
axl_inet_address_free(
    AxlInetAddress *addr  ///< address to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlInetAddress, axl_inet_address_free)
#endif

/**
 * @brief Get the dotted-decimal string representation.
 *
 * The string is lazily cached — the first call formats it, subsequent
 * calls return the cached buffer. The pointer is valid for the
 * lifetime of @p addr.
 *
 * @return internal string (e.g. "192.168.1.1"), or NULL on error.
 */
const char *
axl_inet_address_to_string(
    AxlInetAddress *addr  ///< address
);

/**
 * @brief Get the raw 4-byte address.
 *
 * @return pointer to internal 4-byte array, valid for lifetime of @p addr.
 */
const uint8_t *
axl_inet_address_to_bytes(
    const AxlInetAddress *addr  ///< address
);

/**
 * @brief Check if two addresses are equal.
 *
 * @return true if both addresses have the same octets.
 */
bool
axl_inet_address_equal(
    const AxlInetAddress *a,  ///< first address
    const AxlInetAddress *b   ///< second address
);

/**
 * @brief Check if this is the any-address (0.0.0.0).
 *
 * @return true if all octets are zero.
 */
bool
axl_inet_address_is_any(
    const AxlInetAddress *addr  ///< address
);

/**
 * @brief Check if this is the loopback address (127.0.0.1).
 *
 * @return true if addr is 127.0.0.1.
 */
bool
axl_inet_address_is_loopback(
    const AxlInetAddress *addr  ///< address
);

// ---------------------------------------------------------------------------
// AxlSocketAddress — address + port pair
// ---------------------------------------------------------------------------

typedef struct AxlSocketAddress AxlSocketAddress;

/**
 * @brief IPv4 address (4 bytes). Legacy — prefer AxlInetAddress for new code.
 */
typedef struct {
    uint8_t addr[4];
} AxlIPv4Address;

/**
 * @brief Create a socket address from an IP address and port.
 *
 * Takes ownership of @p addr — the caller must not free it after
 * this call. Free the socket address with axl_socket_address_free(),
 * which also frees the contained AxlInetAddress.
 *
 * @return new socket address, or NULL on failure (addr is freed on failure).
 */
AxlSocketAddress *
axl_socket_address_new(
    AxlInetAddress *addr,  ///< IP address (ownership transferred)
    uint16_t        port   ///< port number
);

/**
 * @brief Create a socket address by parsing "host:port" or "host".
 *
 * If no port is present in the string, @p default_port is used.
 * The host part must be a dotted-decimal IPv4 address.
 *
 * @return new socket address, or NULL on parse failure.
 */
AxlSocketAddress *
axl_socket_address_new_from_string(
    const char *str,          ///< "192.168.1.1:8080" or "192.168.1.1"
    uint16_t    default_port  ///< port used when string has no ":port"
);

/**
 * @brief Free a socket address and its contained AxlInetAddress. NULL-safe.
 */
void
axl_socket_address_free(
    AxlSocketAddress *sa  ///< socket address to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlSocketAddress, axl_socket_address_free)
#endif

/**
 * @brief Get the IP address component.
 *
 * The returned pointer is borrowed — do not free it. It remains
 * valid for the lifetime of @p sa.
 *
 * @return borrowed pointer, valid for lifetime of @p sa.
 */
AxlInetAddress *
axl_socket_address_get_address(
    const AxlSocketAddress *sa  ///< socket address
);

/**
 * @brief Get the port number.
 *
 * @return port number.
 */
uint16_t
axl_socket_address_get_port(
    const AxlSocketAddress *sa  ///< socket address
);

/**
 * @brief Extract as legacy AxlIPv4Address + port.
 *
 * Convenience for interop with existing UDP APIs that take
 * AxlIPv4Address and port separately.
 */
void
axl_socket_address_to_ipv4(
    const AxlSocketAddress *sa,  ///< socket address
    AxlIPv4Address         *out_addr,  ///< [out] receives IPv4 address
    uint16_t               *out_port   ///< [out] receives port number
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_INET_ADDRESS_H */
