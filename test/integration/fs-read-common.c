/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* fs-read-common.c — the shared read chain for the resident-driver file-read
 * fixture. Linked into BOTH the launcher (standalone shell app) and the
 * resident driver so an app-context read and a driver-context read run
 * byte-identical code and can be compared in a single run. */
#include <axl.h>

#include "fs-read.h"

void
fsread_report(
    const char  *tag,
    const char  *path
    )
{
    /* file_info first — the reporter says this works from the driver even
       when the stream read does not, so it pins the "resolution works,
       read doesn't" split. */
    AxlFsEntry e;
    if (axl_file_info(path, &e) == AXL_OK) {
        axl_printf("FSREAD:%s-info=OK(%llu)\n", tag,
                   (unsigned long long)e.size);
    } else {
        axl_printf("FSREAD:%s-info=FAIL\n", tag);
    }

    /* The consumer chain: axl_fopen -> axl_text_stream_wrap -> axl_readline. */
    AxlStream *raw = axl_fopen(path, "r");
    if (raw == NULL) {
        axl_printf("FSREAD:%s-open=FAIL\n", tag);
        return;
    }
    axl_printf("FSREAD:%s-open=OK\n", tag);

    AxlStream *txt = axl_text_stream_wrap(raw);
    if (txt == NULL) {
        axl_printf("FSREAD:%s-wrap=FAIL\n", tag);
        axl_fclose(raw);
        return;
    }

    char *line = axl_readline(txt);
    if (line == NULL) {
        axl_printf("FSREAD:%s-line=NULL\n", tag);
    } else {
        size_t n = axl_strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        axl_printf("FSREAD:%s-line=[%s]\n", tag, line);
        /* The consumer's `do -f` then does axl_setenv(var, line). Exercise that
           too — the reported "read empty" symptom (set v2 -> not found) is
           actually setenv failing on the old shell, not the read. Use a
           per-context var name so the app's set can't mask the driver's. */
        char var[32];
        axl_snprintf(var, sizeof(var), "FSRVAR_%s", tag);
        int se = axl_setenv(var, line, true);
        axl_printf("FSREAD:%s-setenv=%s\n", tag, se == AXL_OK ? "OK" : "ERR");
        char *got = axl_getenv(var);
        axl_printf("FSREAD:%s-getenv=[%s]\n", tag, got != NULL ? got : "(null)");
        axl_free(got);
        /* FSRVAR_<tag> is left set so the runner can confirm it persists into
           the shell (the `do -f<file> var` payoff). The unset round-trip runs
           on a THROWAWAY var so it doesn't disturb that: set then unset must
           DELETE (getenv -> NULL), not leave an empty var behind. */
        char tmp[32];
        axl_snprintf(tmp, sizeof(tmp), "FSRTMP_%s", tag);
        axl_setenv(tmp, "x", true);
        axl_unsetenv(tmp);
        char *gone = axl_getenv(tmp);
        axl_printf("FSREAD:%s-unset=%s\n", tag, gone == NULL ? "gone" : "STAY");
        axl_free(gone);
        axl_free(line);
    }
    axl_fclose(txt);   /* wrapper: frees its ctx, does NOT close raw */
    axl_fclose(raw);
}
