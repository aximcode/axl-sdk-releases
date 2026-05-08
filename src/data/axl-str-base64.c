/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-str-base64.c
    Base64 (RFC 4648) encode / decode. Owns the encode alphabet
    and decode table — split out of axl-str.c per
    docs/Style-Cleanup-Plan.md Pass C so the file's own state
    (the two static tables) lives next to the only code that
    uses it.
**/

#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("str");

static const char b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t b64_dec[128] = {
    /* 0x00-0x0F */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x10-0x1F */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x20-0x2F */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  62,0xFF,0xFF,0xFF,  63,
    /* 0x30-0x3F */   52,  53,  54,  55,  56,  57,  58,  59,  60,  61,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x40-0x4F */ 0xFF,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
    /* 0x50-0x5F */   15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x60-0x6F */ 0xFF,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    /* 0x70-0x7F */   41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,0xFF,0xFF,0xFF,0xFF,0xFF
};

char *
axl_base64_encode(const void *data, size_t len)
{
    const uint8_t *in;
    size_t         out_len;
    char          *out;
    size_t         i;
    size_t         j;
    uint32_t       trip;

    if (data == NULL || len == 0) {
        out = (char *)axl_malloc(1);
        if (out != NULL) {
            out[0] = '\0';
        }
        return out;
    }

    in = (const uint8_t *)data;
    out_len = 4 * ((len + 2) / 3);
    out = (char *)axl_malloc(out_len + 1);
    if (out == NULL) {
        axl_warning("base64_encode allocation failed");
        return NULL;
    }

    j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        trip = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[j++] = b64_enc[(trip >> 18) & 0x3F];
        out[j++] = b64_enc[(trip >> 12) & 0x3F];
        out[j++] = b64_enc[(trip >>  6) & 0x3F];
        out[j++] = b64_enc[(trip      ) & 0x3F];
    }

    if (i < len) {
        trip = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            trip |= (uint32_t)in[i+1] << 8;
        }
        out[j++] = b64_enc[(trip >> 18) & 0x3F];
        out[j++] = b64_enc[(trip >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_enc[(trip >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }

    out[j] = '\0';
    return out;
}

int
axl_base64_decode(const char *b64, void **out, size_t *out_len)
{
    size_t   in_len;
    size_t   pad;
    size_t   raw_len;
    uint8_t *raw;
    size_t   i;
    size_t   j;
    uint8_t  a, b, c, d;

    if (b64 == NULL || out == NULL || out_len == NULL) {
        return AXL_ERR;
    }

    in_len = axl_strlen(b64);
    if (in_len == 0) {
        *out = axl_malloc(1);
        *out_len = 0;
        return (*out != NULL) ? AXL_OK : AXL_ERR;
    }

    if ((in_len % 4) != 0) {
        return AXL_ERR;
    }

    pad = 0;
    if (b64[in_len - 1] == '=') { pad++; }
    if (b64[in_len - 2] == '=') { pad++; }

    raw_len = (in_len / 4) * 3 - pad;
    raw = (uint8_t *)axl_malloc(raw_len + 1);
    if (raw == NULL) {
        axl_warning("base64_decode allocation failed");
        return AXL_ERR;
    }

    j = 0;
    for (i = 0; i < in_len; i += 4) {
        a = ((uint8_t)b64[i]   < 128) ? b64_dec[(uint8_t)b64[i]]   : 0xFF;
        b = ((uint8_t)b64[i+1] < 128) ? b64_dec[(uint8_t)b64[i+1]] : 0xFF;
        c = (b64[i+2] != '=' && (uint8_t)b64[i+2] < 128) ? b64_dec[(uint8_t)b64[i+2]] : 0xFF;
        d = (b64[i+3] != '=' && (uint8_t)b64[i+3] < 128) ? b64_dec[(uint8_t)b64[i+3]] : 0xFF;

        if (a == 0xFF || b == 0xFF) {
            axl_free(raw);
            return AXL_ERR;
        }

        raw[j++] = (uint8_t)((a << 2) | (b >> 4));
        if (j < raw_len) {
            if (c == 0xFF && b64[i+2] != '=') { axl_free(raw); return AXL_ERR; }
            raw[j++] = (uint8_t)((b << 4) | (c >> 2));
        }
        if (j < raw_len) {
            if (d == 0xFF && b64[i+3] != '=') { axl_free(raw); return AXL_ERR; }
            raw[j++] = (uint8_t)((c << 6) | d);
        }
    }

    *out = raw;
    *out_len = (size_t)raw_len;
    return AXL_OK;
}
