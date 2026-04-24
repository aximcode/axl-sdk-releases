/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-websocket.c
    WebSocket frame protocol (RFC 6455 section 5).
    Frame parsing, building, and handshake accept key computation.
    Used by axl-http-server.c for WebSocket support.
**/

#include <axl/axl-str.h>
#include <axl/axl-digest.h>
#include <axl/axl-log.h>
#include "axl-net-internal.h"

AXL_LOG_DOMAIN("ws");

#define WS_MAGIC_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// ===================================================================
// Handshake accept key (RFC 6455 section 4.2.2)
// ===================================================================

/**
 * Compute the Sec-WebSocket-Accept value from the client's
 * Sec-WebSocket-Key header.
 *
 * Returns a newly allocated base64 string (caller frees with axl_free).
 * Returns NULL on error.
 */
char *
ws_compute_accept_key(const char *client_key)
{
    uint8_t digest[20];
    char concat[128];

    if (client_key == NULL) {
        return NULL;
    }

    /* Concatenate client key + magic GUID */
    size_t key_len = axl_strlen(client_key);
    size_t guid_len = axl_strlen(WS_MAGIC_GUID);
    if (key_len + guid_len >= sizeof(concat)) {
        return NULL;
    }

    axl_memcpy(concat, client_key, key_len);
    axl_memcpy(concat + key_len, WS_MAGIC_GUID, guid_len);

    /* SHA-1 hash */
    if (axl_compute_checksum_digest(AXL_CHECKSUM_SHA1,
                                    concat, key_len + guid_len,
                                    digest, sizeof(digest)) != 0) {
        return NULL;
    }

    /* Base64 encode */
    return axl_base64_encode(digest, sizeof(digest));
}

// ===================================================================
// Frame parsing (RFC 6455 section 5.2)
// ===================================================================

/**
 * Parse a WebSocket frame header from raw bytes.
 *
 * Returns the header size (2-14 bytes) on success, 0 if the buffer
 * is too small to contain a complete header.
 */
size_t
ws_parse_header(const uint8_t *buf, size_t len, WsFrameHeader *out)
{
    if (len < 2) {
        return 0;
    }

    out->fin    = (buf[0] & 0x80) != 0;
    out->opcode = buf[0] & 0x0F;
    out->masked = (buf[1] & 0x80) != 0;

    size_t payload_len = buf[1] & 0x7F;
    size_t header_len = 2;

    if (payload_len == 126) {
        if (len < 4) {
            return 0;
        }
        payload_len = ((size_t)buf[2] << 8) | (size_t)buf[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (len < 10) {
            return 0;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | (size_t)buf[2 + i];
        }
        header_len = 10;
    }

    if (out->masked) {
        if (len < header_len + 4) {
            return 0;
        }
        axl_memcpy(out->mask, buf + header_len, 4);
        header_len += 4;
    }

    if (len < header_len + payload_len) {
        return 0;  /* incomplete frame */
    }

    out->payload_len = payload_len;
    out->header_len = header_len;
    return header_len;
}

/**
 * Unmask a WebSocket payload in-place.
 * Client-to-server frames are always masked (RFC 6455 section 5.3).
 */
void
ws_unmask(uint8_t *data, size_t len, const uint8_t mask[4])
{
    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask[i % 4];
    }
}

// ===================================================================
// Frame building (server-to-client, unmasked)
// ===================================================================

/**
 * Build a WebSocket frame into the given buffer.
 * Server frames are never masked. Returns total frame size,
 * or 0 if the buffer is too small.
 */
size_t
ws_build_frame(uint8_t opcode, const void *payload, size_t payload_len,
               void *out, size_t out_size)
{
    uint8_t *buf = (uint8_t *)out;
    size_t header_len;

    if (payload_len < 126) {
        header_len = 2;
    } else if (payload_len <= 0xFFFF) {
        header_len = 4;
    } else {
        header_len = 10;
    }

    if (out_size < header_len + payload_len) {
        axl_warning("ws frame buffer too small: need %zu, have %zu",
                    header_len + payload_len, out_size);
        return 0;
    }

    /* FIN bit + opcode */
    buf[0] = 0x80 | (opcode & 0x0F);

    /* Payload length (no mask bit — server frames are unmasked) */
    if (payload_len < 126) {
        buf[1] = (uint8_t)payload_len;
    } else if (payload_len <= 0xFFFF) {
        buf[1] = 126;
        buf[2] = (uint8_t)(payload_len >> 8);
        buf[3] = (uint8_t)(payload_len);
    } else {
        buf[1] = 127;
        for (int i = 0; i < 8; i++) {
            buf[2 + i] = (uint8_t)(payload_len >> (56 - i * 8));
        }
    }

    if (payload != NULL && payload_len > 0) {
        axl_memcpy(buf + header_len, payload, payload_len);
    }

    return header_len + payload_len;
}
