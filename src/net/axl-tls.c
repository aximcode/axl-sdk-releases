/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tls.c
    TLS support — ported from SoftBMC TlsShim.c.

    When AXL_HAVE_TLS is defined, provides full TLS 1.2 via mbedTLS.
    Otherwise, all functions return -1/NULL/false (stubs).
**/

#include <axl/axl-net.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

#ifndef AXL_HAVE_TLS

// ===================================================================
// Stub implementations (TLS not compiled in)
// ===================================================================

bool axl_tls_available(void) { return false; }
int  axl_tls_init(void) { return AXL_ERR; }
void axl_tls_cleanup(void) {}
int  axl_tls_generate_self_signed(const char *cn,
    const AxlIPv4Address *ips, size_t ip_count,
    void **cert, size_t *cert_len, void **key, size_t *key_len)
{ (void)cn;(void)ips;(void)ip_count;(void)cert;(void)cert_len;(void)key;(void)key_len; return AXL_ERR; }
int  axl_tls_server_set_cert(const void *c, size_t cl, const void *k, size_t kl)
{ (void)c;(void)cl;(void)k;(void)kl; return AXL_ERR; }
AxlTlsContext *axl_tls_accept(AxlTcp *s) { (void)s; return NULL; }
AxlTlsContext *axl_tls_connect(AxlTcp *s, const char *h) { (void)s;(void)h; return NULL; }
AxlTlsStatus axl_tls_handshake(AxlTlsContext *c) { (void)c; return AXL_TLS_ERR; }
AxlTlsStatus axl_tls_handshake_async(AxlTlsContext *c, AxlLoop *l)
{ (void)c; (void)l; return AXL_TLS_ERR; }
AxlTlsStatus axl_tls_read(AxlTlsContext *c, void *b, size_t s, size_t *o)
{ (void)c;(void)b;(void)s;(void)o; return AXL_TLS_ERR; }
int  axl_tls_write(AxlTlsContext *c, const void *d, size_t l)
{ (void)c;(void)d;(void)l; return AXL_ERR; }
int  axl_tls_write_async(AxlTlsContext *c, const void *d, size_t l,
    AxlLoop *loop, AxlTcpCallback cb, void *data)
{ (void)c;(void)d;(void)l;(void)loop;(void)cb;(void)data; return AXL_ERR; }
void axl_tls_free(AxlTlsContext *c) { (void)c; }
void axl_tls_stage_data(AxlTlsContext *c, const void *d, size_t l)
{ (void)c;(void)d;(void)l; }
bool axl_tls_pending(AxlTlsContext *c) { (void)c; return false; }

#else /* AXL_HAVE_TLS */

// ===================================================================
// Full TLS implementation via mbedTLS
// ===================================================================

#include "../backend/axl-backend.h"
#include "axl-tcp-internal.h"   /* axl_tcp_send_in_flight */
#include "axl-http-client-tls.h" /* register the HTTP client's TLS ops */
#include <axl/axl-atexit.h>
#include <axl/axl-log.h>
#include <axl/axl-debug.h>

#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>

AXL_LOG_DOMAIN("tls");

/* Forward declaration for platform entropy */
int axl_mbedtls_entropy_poll(void *data, unsigned char *output,
                             size_t len, size_t *olen);

#define TLS_SEND_TIMEOUT_MS  10000
#define TLS_HANDSHAKE_BUF    16384

// ---------------------------------------------------------------------------
// Global TLS state
// ---------------------------------------------------------------------------

static mbedtls_ssl_config       g_tls_config;
static mbedtls_x509_crt         g_server_cert;
static mbedtls_pk_context        g_server_key;
static mbedtls_ctr_drbg_context  g_ctr_drbg;
static mbedtls_entropy_context   g_entropy;
static bool                      g_initialized = false;

// ---------------------------------------------------------------------------
// Per-connection TLS context
// ---------------------------------------------------------------------------

struct AxlTlsContext {
    mbedtls_ssl_context  ssl;
    AxlTcp              *sock;

    /* Staging buffer for BIO recv (zero-copy) */
    uint8_t             *stage_buf;
    size_t               stage_len;
    size_t               stage_off;

    /* Buffered output for handshake batching and multi-record app writes.
       out_cap is the current capacity of out_buf — normally
       TLS_HANDSHAKE_BUF, but axl_tls_write_async temporarily points these
       at a larger heap buffer when a single write spans multiple records
       (a response body > one TLS record). */
    uint8_t             *out_buf;
    size_t               out_len;
    size_t               out_cap;
    bool                 buffered_mode;
};

// ---------------------------------------------------------------------------
// BIO callbacks (bridge mbedTLS ↔ AXL TCP)
// ---------------------------------------------------------------------------

static int
tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    AxlTlsContext *tc = (AxlTlsContext *)ctx;

    if (tc->buffered_mode) {
        /* Accumulate output for a single TCP send (handshake flight or a
           multi-record app write). */
        if (tc->out_len + len > tc->out_cap) {
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }
        axl_memcpy(tc->out_buf + tc->out_len, buf, len);
        tc->out_len += len;
        return (int)len;
    }

    /* Direct send via TCP */
    int rc = axl_tcp_send(tc->sock, buf, len, TLS_SEND_TIMEOUT_MS);
    if (rc != AXL_OK) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return (int)len;
}

static int
tls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    AxlTlsContext *tc = (AxlTlsContext *)ctx;

    if (tc->stage_buf == NULL || tc->stage_off >= tc->stage_len) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    size_t avail = tc->stage_len - tc->stage_off;
    size_t n = (len < avail) ? len : avail;
    axl_memcpy(buf, tc->stage_buf + tc->stage_off, n);
    tc->stage_off += n;
    return (int)n;
}

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

static void axl_tls_cleanup_thunk(void *unused);

bool
axl_tls_available(void)
{
    return true;
}

/* HTTP client TLS ops — registered with the client from axl_tls_init() so the
   client never needs a static axl_tls_* reference. Field order/signatures
   match AxlHttpClientTlsOps. */
static const AxlHttpClientTlsOps g_http_client_tls_ops = {
    .connect         = axl_tls_connect,
    .free            = axl_tls_free,
    .stage_data      = axl_tls_stage_data,
    .handshake       = axl_tls_handshake,
    .write           = axl_tls_write,
    .read            = axl_tls_read,
    .handshake_async = axl_tls_handshake_async,
    .write_async     = axl_tls_write_async,
};

int
axl_tls_init(void)
{
    if (g_initialized) {
        return AXL_OK;
    }

    mbedtls_ssl_config_init(&g_tls_config);
    mbedtls_x509_crt_init(&g_server_cert);
    mbedtls_pk_init(&g_server_key);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    mbedtls_entropy_init(&g_entropy);

    /* Register our entropy source */
    int ret = mbedtls_entropy_add_source(&g_entropy,
        axl_mbedtls_entropy_poll, NULL, 32,
        MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (ret != 0) {
        axl_warning("entropy source registration failed: -0x%04x",
                   (unsigned)-ret);
    }

    /* Seed CTR-DRBG */
    ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
                                &g_entropy,
                                (const unsigned char *)"AXL", 3);
    if (ret != 0) {
        axl_warning("CTR-DRBG seed failed: -0x%04x", (unsigned)-ret);
        return AXL_ERR;
    }

    /* Default server config (TLS 1.2) */
    ret = mbedtls_ssl_config_defaults(&g_tls_config,
        MBEDTLS_SSL_IS_SERVER,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        axl_warning("SSL config defaults failed: -0x%04x", (unsigned)-ret);
        return AXL_ERR;
    }

    mbedtls_ssl_conf_rng(&g_tls_config, mbedtls_ctr_drbg_random, &g_ctr_drbg);

    /* Wire the always-linked HTTP client to this (now-linked) TLS module, so
       its https path works. A consumer that never calls axl_tls_init (or
       axl_http_server_use_tls, which does) never reaches here, leaving the
       client's ops NULL and letting --gc-sections strip mbedTLS. */
    axl_http_client_set_tls_ops(&g_http_client_tls_ops);

    g_initialized = true;
    axl_atexit(axl_tls_cleanup_thunk, NULL);
    axl_info("initialized (mbedTLS)");
    return AXL_OK;
}

void
axl_tls_cleanup(void)
{
    if (!g_initialized) {
        return;
    }

    mbedtls_ssl_config_free(&g_tls_config);
    mbedtls_x509_crt_free(&g_server_cert);
    mbedtls_pk_free(&g_server_key);
    mbedtls_ctr_drbg_free(&g_ctr_drbg);
    mbedtls_entropy_free(&g_entropy);

    g_initialized = false;
}

/// Atexit thunk — drops the void* arg so axl_tls_cleanup keeps a
/// caller-friendly signature.
static void
axl_tls_cleanup_thunk(void *unused)
{
    (void)unused;
    axl_tls_cleanup();
}

// ---------------------------------------------------------------------------
// Certificate generation (ported from SoftBMC TlsShim.c)
// ---------------------------------------------------------------------------

int
axl_tls_generate_self_signed(
    const char           *cn,
    const AxlIPv4Address *ips,
    size_t                ip_count,
    void                **cert_der,
    size_t               *cert_len,
    void                **key_der,
    size_t               *key_len
    )
{
    if (!g_initialized || cn == NULL ||
        cert_der == NULL || cert_len == NULL ||
        key_der == NULL || key_len == NULL) {
        return AXL_ERR;
    }

    *cert_der = NULL;
    *cert_len = 0;
    *key_der = NULL;
    *key_len = 0;

    int ret;

    /* Generate ECDSA P-256 key pair */
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        axl_warning("pk_setup failed: -0x%04x", (unsigned)-ret);
        mbedtls_pk_free(&pk);
        return AXL_ERR;
    }

    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                              mbedtls_pk_ec(pk),
                              mbedtls_ctr_drbg_random, &g_ctr_drbg);
    if (ret != 0) {
        axl_warning("key generation failed: -0x%04x", (unsigned)-ret);
        mbedtls_pk_free(&pk);
        return AXL_ERR;
    }

    /* Export private key to DER */
    uint8_t key_buf[256];
    ret = mbedtls_pk_write_key_der(&pk, key_buf, sizeof(key_buf));
    if (ret < 0) {
        axl_warning("key export failed: -0x%04x", (unsigned)-ret);
        mbedtls_pk_free(&pk);
        return AXL_ERR;
    }
    /* mbedTLS writes backwards — data starts at key_buf + sizeof - ret */
    size_t klen = (size_t)ret;
    uint8_t *kdata = key_buf + sizeof(key_buf) - klen;

    /* Build subject name */
    char subject[128];
    axl_snprintf(subject, sizeof(subject), "CN=%s,O=AximCode", cn);

    /* Create self-signed certificate */
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);

    ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
    if (ret != 0) goto cert_fail;

    ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
    if (ret != 0) goto cert_fail;

    /* Random serial (8 bytes, MSB cleared to keep positive) */
    uint8_t serial_buf[8];
    mbedtls_ctr_drbg_random(&g_ctr_drbg, serial_buf, sizeof(serial_buf));
    serial_buf[0] &= 0x7F;
    mbedtls_x509write_crt_set_serial_raw(&crt, serial_buf, sizeof(serial_buf));

    /* Validity: now to +10 years (dynamic from system time) */
    {
        char not_before[16], not_after[16];
        EFI_TIME t;
        uint16_t start_year = 2025, end_year = 2035;

        if (gRT != NULL &&
            axl_efi_call(gRT->GetTime, 2, &t, NULL) == 0 &&
            t.Year >= 2020)
        {
            start_year = t.Year;
            end_year   = t.Year + 10;
        }
        axl_snprintf(not_before, sizeof(not_before),
                     "%04u0101000000", (unsigned)start_year);
        axl_snprintf(not_after, sizeof(not_after),
                     "%04u0101000000", (unsigned)end_year);
        ret = mbedtls_x509write_crt_set_validity(&crt,
            not_before, not_after);
    }
    if (ret != 0) goto cert_fail;

    /* SubjectAltName: localhost + provided IPs */
    {
        uint8_t san_buf[256];
        size_t san_len = 0;
        uint8_t *p = san_buf + sizeof(san_buf);

        /* DNS: localhost */
        const char *dns = "localhost";
        size_t dns_len = axl_strlen(dns);
        p -= dns_len;
        axl_memcpy(p, dns, dns_len);
        p -= 1; *p = (uint8_t)dns_len;  /* length */
        p -= 1; *p = 0x82;              /* context tag [2] = dNSName */
        san_len += dns_len + 2;

        /* IP: 127.0.0.1 */
        p -= 4;
        p[0] = 127; p[1] = 0; p[2] = 0; p[3] = 1;
        p -= 1; *p = 4;       /* length */
        p -= 1; *p = 0x87;    /* context tag [7] = iPAddress */
        san_len += 6;

        /* Provided IPs */
        for (size_t i = 0; i < ip_count && ips != NULL; i++) {
            if (ips[i].addr[0] == 127 || ips[i].addr[0] == 0) {
                continue;
            }
            p -= 4;
            axl_memcpy(p, ips[i].addr, 4);
            p -= 1; *p = 4;
            p -= 1; *p = 0x87;
            san_len += 6;
        }

        /* Wrap in SEQUENCE */
        if (san_len > 0) {
            uint8_t seq_buf[256];
            size_t seq_off = sizeof(san_buf) - san_len;
            uint8_t *san_data = san_buf + seq_off;

            /* Build DER SEQUENCE header + content */
            size_t total = 0;
            if (san_len < 128) {
                seq_buf[0] = 0x30;          /* SEQUENCE tag */
                seq_buf[1] = (uint8_t)san_len;
                axl_memcpy(seq_buf + 2, san_data, san_len);
                total = san_len + 2;
            } else {
                seq_buf[0] = 0x30;
                seq_buf[1] = 0x81;
                seq_buf[2] = (uint8_t)san_len;
                axl_memcpy(seq_buf + 3, san_data, san_len);
                total = san_len + 3;
            }

            /* OID for SubjectAltName: 2.5.29.17 */
            ret = mbedtls_x509write_crt_set_extension(&crt,
                MBEDTLS_OID_SUBJECT_ALT_NAME,
                MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
                0, seq_buf, total);
            if (ret != 0) {
                axl_debug("SAN extension failed: -0x%04x", (unsigned)-ret);
            }
        }
    }

    /* Write certificate to DER */
    {
        uint8_t cert_buf[2048];
        ret = mbedtls_x509write_crt_der(&crt, cert_buf, sizeof(cert_buf),
                                        mbedtls_ctr_drbg_random, &g_ctr_drbg);
        if (ret < 0) {
            goto cert_fail;
        }

        size_t clen = (size_t)ret;
        uint8_t *cdata = cert_buf + sizeof(cert_buf) - clen;

        *cert_der = axl_memdup(cdata, clen);
        *cert_len = clen;
        *key_der = axl_memdup(kdata, klen);
        *key_len = klen;

        if (*cert_der == NULL || *key_der == NULL) {
            axl_free(*cert_der);
            axl_free(*key_der);
            *cert_der = NULL;
            *key_der = NULL;
            goto cert_fail;
        }
    }

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);

    axl_info("generated self-signed cert: %s (%zu bytes)", subject, *cert_len);
    return AXL_OK;

cert_fail:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// Server certificate configuration
// ---------------------------------------------------------------------------

int
axl_tls_server_set_cert(
    const void *cert_der,
    size_t      cert_len,
    const void *key_der,
    size_t      key_len
    )
{
    if (!g_initialized || cert_der == NULL || key_der == NULL) {
        return AXL_ERR;
    }

    int ret;

    /* Parse certificate */
    mbedtls_x509_crt_free(&g_server_cert);
    mbedtls_x509_crt_init(&g_server_cert);

    ret = mbedtls_x509_crt_parse_der(&g_server_cert,
                                     cert_der, cert_len);
    if (ret != 0) {
        axl_warning("cert parse failed: -0x%04x", (unsigned)-ret);
        return AXL_ERR;
    }

    /* Parse private key */
    mbedtls_pk_free(&g_server_key);
    mbedtls_pk_init(&g_server_key);

    ret = mbedtls_pk_parse_key(&g_server_key,
                               key_der, key_len,
                               NULL, 0,
                               mbedtls_ctr_drbg_random, &g_ctr_drbg);
    if (ret != 0) {
        axl_warning("key parse failed: -0x%04x", (unsigned)-ret);
        return AXL_ERR;
    }

    /* Attach to config */
    ret = mbedtls_ssl_conf_own_cert(&g_tls_config,
                                    &g_server_cert, &g_server_key);
    if (ret != 0) {
        axl_warning("conf_own_cert failed: -0x%04x", (unsigned)-ret);
        return AXL_ERR;
    }

    axl_info("server certificate loaded");
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Per-connection context
// ---------------------------------------------------------------------------

AxlTlsContext *
axl_tls_accept(AxlTcp *sock)
{
    if (!g_initialized || sock == NULL) {
        return NULL;
    }

    AxlTlsContext *ctx = axl_new(AxlTlsContext);
    if (ctx == NULL) {
        return NULL;
    }

    ctx->sock = sock;
    ctx->out_buf = axl_malloc(TLS_HANDSHAKE_BUF);
    if (ctx->out_buf == NULL) {
        axl_free(ctx);
        return NULL;
    }
    ctx->out_cap = TLS_HANDSHAKE_BUF;
    ctx->out_len = 0;
    ctx->buffered_mode = true;

    mbedtls_ssl_init(&ctx->ssl);

    int ret = mbedtls_ssl_setup(&ctx->ssl, &g_tls_config);
    if (ret != 0) {
        axl_warning("ssl_setup failed: -0x%04x", (unsigned)-ret);
        axl_free(ctx->out_buf);
        axl_free(ctx);
        return NULL;
    }

    mbedtls_ssl_set_bio(&ctx->ssl, ctx, tls_bio_send, tls_bio_recv, NULL);

    return ctx;
}

AxlTlsContext *
axl_tls_connect(AxlTcp *sock, const char *hostname)
{
    if (!g_initialized || sock == NULL) {
        return NULL;
    }

    /* Need a client config */
    static mbedtls_ssl_config client_config;
    static bool client_config_init = false;

    if (!client_config_init) {
        mbedtls_ssl_config_init(&client_config);
        int ret = mbedtls_ssl_config_defaults(&client_config,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
            return NULL;
        }
        mbedtls_ssl_conf_rng(&client_config,
                             mbedtls_ctr_drbg_random, &g_ctr_drbg);
        /* No certificate verification (trust on first use) */
        mbedtls_ssl_conf_authmode(&client_config, MBEDTLS_SSL_VERIFY_NONE);
        client_config_init = true;
    }

    AxlTlsContext *ctx = axl_new(AxlTlsContext);
    if (ctx == NULL) {
        return NULL;
    }

    ctx->sock = sock;
    ctx->out_buf = axl_malloc(TLS_HANDSHAKE_BUF);
    if (ctx->out_buf == NULL) {
        axl_free(ctx);
        return NULL;
    }
    ctx->out_cap = TLS_HANDSHAKE_BUF;
    ctx->out_len = 0;
    ctx->buffered_mode = true;

    mbedtls_ssl_init(&ctx->ssl);

    int ret = mbedtls_ssl_setup(&ctx->ssl, &client_config);
    if (ret != 0) {
        axl_free(ctx->out_buf);
        axl_free(ctx);
        return NULL;
    }

    if (hostname != NULL) {
        mbedtls_ssl_set_hostname(&ctx->ssl, hostname);
    }

    mbedtls_ssl_set_bio(&ctx->ssl, ctx, tls_bio_send, tls_bio_recv, NULL);

    return ctx;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

/* Log a failed handshake at the right level. A failed CLIENT handshake is
   NORMAL operation for a public TLS listener — browsers abort speculative /
   preconnect sockets constantly, and a single page load can produce ~20 peer
   fatal-alert failures. Demote those routine peer-driven terminations (peer
   fatal alert, clean close-notify, EOF, reset) to debug; keep warning for codes
   that smell like a local problem worth a human's attention. (Genuine local
   misconfiguration — bad cert/key, no usable ciphersuite — already surfaces as
   its own warning at setup time.) The custom BIO (tls_bio_recv) surfaces
   SSL-level codes only — mbedTLS net_sockets isn't used — so a peer reset
   arrives as CONN_EOF, not MBEDTLS_ERR_NET_CONN_RESET. */
static void
log_handshake_failure(int ret)
{
    switch (ret) {
    case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:  /* peer sent a fatal alert */
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:    /* peer closed cleanly */
    case MBEDTLS_ERR_SSL_CONN_EOF:             /* peer hung up mid-handshake */
        axl_debug("handshake terminated by peer: -0x%04x", (unsigned)-ret);
        break;
    default:
        axl_warning("handshake failed: -0x%04x", (unsigned)-ret);
        break;
    }
}

AxlTlsStatus
axl_tls_handshake(AxlTlsContext *ctx)
{
    if (ctx == NULL) {
        return AXL_TLS_ERR;
    }

    int ret = mbedtls_ssl_handshake(&ctx->ssl);

    if (ret == 0) {
        /* Handshake complete — flush buffered output and switch to direct */
        if (ctx->buffered_mode && ctx->out_len > 0) {
            axl_tcp_send(ctx->sock, ctx->out_buf, ctx->out_len,
                         TLS_SEND_TIMEOUT_MS);
            ctx->out_len = 0;
        }
        ctx->buffered_mode = false;
        return AXL_TLS_OK;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
        ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        /* Flush any buffered handshake output */
        if (ctx->buffered_mode && ctx->out_len > 0) {
            axl_tcp_send(ctx->sock, ctx->out_buf, ctx->out_len,
                         TLS_SEND_TIMEOUT_MS);
            ctx->out_len = 0;
        }
        return AXL_TLS_WANT_MORE;  /* need more data */
    }

    log_handshake_failure(ret);
    return AXL_TLS_ERR;
}

// ---------------------------------------------------------------------------
// Read / Write
// ---------------------------------------------------------------------------

AxlTlsStatus
axl_tls_read(
    AxlTlsContext *ctx,
    void          *buf,
    size_t         size,
    size_t        *out_len
    )
{
    if (ctx == NULL || buf == NULL || out_len == NULL) {
        return AXL_TLS_ERR;
    }

    *out_len = 0;

    int ret = mbedtls_ssl_read(&ctx->ssl, buf, size);

    if (ret > 0) {
        *out_len = (size_t)ret;
        return AXL_TLS_OK;
    }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return AXL_TLS_ERR;  /* connection closed */
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        return AXL_TLS_WANT_MORE;  /* need more data */
    }

    return AXL_TLS_ERR;
}

int
axl_tls_write(
    AxlTlsContext *ctx,
    const void    *data,
    size_t         len
    )
{
    if (ctx == NULL || data == NULL || len == 0) {
        return AXL_ERR;
    }

    /* mbedtls_ssl_write emits at most one record per call, so loop until
       every byte is written (each call sends its record directly via the
       non-buffered BIO). Without this a payload larger than one TLS record
       — e.g. a client request body > 16 KiB — would be truncated. */
    size_t off = 0;
    while (off < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl,
                                    (const uint8_t *)data + off, len - off);
        if (ret <= 0) {
            return AXL_ERR;
        }
        off += (size_t)ret;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Async write (encrypt + async TCP send)
// ---------------------------------------------------------------------------

/* Wrapper context for axl_tls_write_async completion */
typedef struct {
    AxlTcpCallback  user_cb;
    void           *user_data;
    void           *enc_buf;
} TlsWriteAsyncCtx;

static bool
tls_write_async_done(AxlTcp *sock, AxlStatus status, void *data)
{
    TlsWriteAsyncCtx *wctx = (TlsWriteAsyncCtx *)data;
    AxlTcpCallback    cb      = wctx->user_cb;
    void             *cb_data = wctx->user_data;
    axl_free(wctx->enc_buf);
    axl_free(wctx);
    if (cb != NULL) {
        cb(sock, status, cb_data);  /* TLS write is one-shot */
    }
    return false;
}

int
axl_tls_write_async(
    AxlTlsContext *ctx,
    const void    *data,
    size_t         len,
    AxlLoop       *loop,
    AxlTcpCallback cb,
    void          *cb_data
    )
{
    if (ctx == NULL || data == NULL || len == 0 || loop == NULL) {
        return AXL_ERR;
    }

    /* Floor against TLS-stream desync (the ws-broadcast-over-TLS bug):
       axl_tcp_send_async is strictly one-send-in-flight, and mbedtls_ssl_write
       below ADVANCES the TLS write sequence number and emits an encrypted
       record. If we encrypted now and the send were then rejected (a prior
       send still pending), that record would be dropped with the seqno already
       consumed — desyncing the stream and breaking the connection. So bail
       BEFORE touching the SSL context when a send is in flight. Returns a
       DISTINCT AXL_BUSY (not AXL_ERR) so a caller can re-queue/retry; the SSL
       context is left untouched. Well-behaved callers serialize via their own
       outbound queue (see the WS per-connection queue) and never hit this. */
    if (axl_tcp_send_in_flight(ctx->sock)) {
        return AXL_BUSY;
    }

    /* mbedtls_ssl_write emits at most one record (<= OUT_CONTENT_LEN
       plaintext) per call, so a body larger than one record needs several
       calls. Accumulate every record's ciphertext into one buffer and send
       it in a single TCP write. For a write that fits in one record this
       is the existing out_buf; for a larger one, point the accumulation at
       a right-sized temporary so tls_bio_send doesn't overflow. */
    uint8_t *saved_buf = ctx->out_buf;
    size_t   saved_cap = ctx->out_cap;
    uint8_t *big_buf    = NULL;

    if (len > TLS_HANDSHAKE_BUF / 2) {
        /* Worst-case ciphertext: plaintext + per-record framing overhead
           (header + IV + AEAD tag, well under 128 B/record). */
        size_t nrec = (len / MBEDTLS_SSL_OUT_CONTENT_LEN) + 1;
        size_t cap  = len + nrec * 128 + 256;
        big_buf = axl_malloc(cap);
        if (big_buf == NULL) {
            return AXL_ERR;
        }
        ctx->out_buf = big_buf;
        ctx->out_cap = cap;
    }

    ctx->buffered_mode = true;
    ctx->out_len = 0;

    /* Invariant guard (the 4563aabf desync): we are about to advance the TLS
       write sequence number via mbedtls_ssl_write, which is irreversible. The
       floor above already returned AXL_BUSY if a TCP send were in flight, so
       this must hold here. If a future edit reaches the seqno advance without
       that floor, catch it at the cause — not as a downstream stream desync a
       consumer's integration surfaces days later. */
    AXL_DEBUG_ASSERT_MSG(!axl_tcp_send_in_flight(ctx->sock),
                         "TLS seqno advance with a TCP send in flight");

    /* Write all bytes, one record per mbedtls_ssl_write (it returns the
       count written, which is capped at one record). */
    size_t off = 0;
    bool   ok  = true;
    while (off < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl,
                                    (const uint8_t *)data + off, len - off);
        if (ret <= 0) {
            ok = false;
            break;
        }
        off += (size_t)ret;
    }

    ctx->buffered_mode = false;

    size_t enc_len = ctx->out_len;
    ctx->out_len = 0;

    void *enc_copy = (ok && enc_len > 0)
                     ? axl_memdup(ctx->out_buf, enc_len)
                     : NULL;

    /* Restore the handshake-sized out_buf. */
    if (big_buf != NULL) {
        ctx->out_buf = saved_buf;
        ctx->out_cap = saved_cap;
        axl_free(big_buf);
    }

    if (enc_copy == NULL) {
        return AXL_ERR;
    }

    TlsWriteAsyncCtx *wctx = axl_new(TlsWriteAsyncCtx);
    if (wctx == NULL) {
        axl_free(enc_copy);
        return AXL_ERR;
    }
    wctx->user_cb   = cb;
    wctx->user_data = cb_data;
    wctx->enc_buf   = enc_copy;

    if (axl_tcp_send_async(ctx->sock, enc_copy, enc_len, loop, NULL,
                           tls_write_async_done, wctx) != AXL_OK) {
        axl_free(enc_copy);
        axl_free(wctx);
        return AXL_ERR;
    }

    return AXL_OK;
}

/* Flush the buffered handshake output (out_buf) asynchronously on @p loop,
   taking ownership of a copy so the buffer can be reused immediately.
   @p out_len is consumed only once the send is accepted, so a rejected
   submission (e.g. a prior flight's send still in flight) leaves the
   buffered bytes intact rather than dropping them. This assumes lock-step
   handshake flights — true for a server handshake, where each flight
   follows a client round-trip and the prior send has long drained. */
static int
handshake_flush_async(AxlTlsContext *ctx, AxlLoop *loop)
{
    if (!ctx->buffered_mode || ctx->out_len == 0) {
        return AXL_OK;
    }
    size_t n    = ctx->out_len;
    void  *copy = axl_memdup(ctx->out_buf, n);
    if (copy == NULL) {
        return AXL_ERR;
    }
    TlsWriteAsyncCtx *wctx = axl_new(TlsWriteAsyncCtx);
    if (wctx == NULL) {
        axl_free(copy);
        return AXL_ERR;
    }
    wctx->user_cb   = NULL;
    wctx->user_data = NULL;
    wctx->enc_buf   = copy;
    if (axl_tcp_send_async(ctx->sock, copy, n, loop, NULL,
                           tls_write_async_done, wctx) != AXL_OK) {
        axl_free(copy);
        axl_free(wctx);
        return AXL_ERR;   /* out_len left intact — bytes not lost */
    }
    ctx->out_len = 0;     /* consumed only on a successful submission */
    return AXL_OK;
}

AxlTlsStatus
axl_tls_handshake_async(AxlTlsContext *ctx, AxlLoop *loop)
{
    if (ctx == NULL || loop == NULL) {
        return AXL_TLS_ERR;
    }

    int ret = mbedtls_ssl_handshake(&ctx->ssl);

    if (ret == 0) {
        if (handshake_flush_async(ctx, loop) != AXL_OK) {
            return AXL_TLS_ERR;
        }
        ctx->buffered_mode = false;
        return AXL_TLS_OK;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
        ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return (handshake_flush_async(ctx, loop) == AXL_OK) ? AXL_TLS_WANT_MORE : AXL_TLS_ERR;
    }

    log_handshake_failure(ret);
    return AXL_TLS_ERR;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void
axl_tls_free(AxlTlsContext *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* Generate the close_notify alert into the output buffer, NOT onto the
       wire: a synchronous send here (the default BIO path) spins an
       ephemeral AxlLoop, which cannot make progress when the connection is
       being torn down from inside a resident driver-tick loop (raised TPL)
       — and that nested loop's source ids collide with the outer loop's
       (the same hazard as commit adbf5461), silently killing the server's
       accept source so no further connections are accepted. The alert is
       advisory; the TCP FIN that follows (axl_tcp_close) plus HTTP's own
       message framing convey the close. buffered_mode makes tls_bio_send
       accumulate instead of send; the buffer is freed unsent below. */
    ctx->buffered_mode = true;
    ctx->out_len = 0;
    mbedtls_ssl_close_notify(&ctx->ssl);

    mbedtls_ssl_free(&ctx->ssl);
    axl_free(ctx->out_buf);
    axl_free(ctx);
}

/**
 * @brief Stage data for the next TLS read or handshake step.
 *
 * Points the BIO recv callback at the provided buffer (zero-copy).
 * Call before axl_tls_handshake() or axl_tls_read().
 */
void
axl_tls_stage_data(
    AxlTlsContext *ctx,
    const void    *data,
    size_t         len
    )
{
    if (ctx == NULL) {
        return;
    }
    ctx->stage_buf = (uint8_t *)data;
    ctx->stage_len = len;
    ctx->stage_off = 0;
}

bool
axl_tls_pending(
    AxlTlsContext *ctx
    )
{
    if (ctx == NULL) {
        return false;
    }
    /* More TLS records from the same TCP segment may still be staged
       (stage_off < stage_len), or part of the current record may have
       been decrypted but not yet read (get_bytes_avail). Either way a
       further axl_tls_read can make progress without new transport
       input, so the caller must drain before idling on the transport. */
    return ctx->stage_off < ctx->stage_len
        || mbedtls_ssl_get_bytes_avail(&ctx->ssl) > 0;
}

#endif /* AXL_HAVE_TLS */
