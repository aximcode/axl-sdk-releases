/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* io-streams.c — emits one marker per stream; driven under redirect
 * operators by test-io-redirect-qemu.sh to prove sink separation. */
#include <axl.h>

AXL_LOG_DOMAIN("io-streams");

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;
    axl_print("OUT:stdout\n");        /* -> ConOut (stdout) */
    axl_printerr("ERR:stderr\n");     /* -> stderr sink */
    axl_warning("LOG:warn");          /* diagnostic log */
    axl_write(axl_stderr_raw, "RAW:err\n", 8);   /* binary -> shell StdErr handle */
    return 0;
}
