/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file http-plain-selftest.c
    Plain-HTTP-only client fixture. References the HTTP client but NEVER TLS.

    libaxl always contains mbedTLS; this binary must contain NONE of it — that
    is the test (see test-tls-strippable.sh): it proves the client's TLS path
    is reachable only through the ops indirection (axl-http-client-tls.h), so a
    consumer that never calls axl_tls_init() lets --gc-sections drop mbedTLS.

    The LINK output is the assertion; this never has to run.
**/

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AXL_AUTOPTR(AxlHttpClient) client = axl_http_client_new();
    if (client == NULL) {
        return 1;
    }

    /* Plain http:// only — exercises the client's request path (which contains
       the now-indirected https branch) without ever referencing TLS. */
    AxlHttpClientResponse *resp = NULL;
    axl_http_get(client, "http://127.0.0.1/", &resp);
    axl_http_client_response_free(resp);
    return 0;
}
