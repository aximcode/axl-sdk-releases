/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * shell-status-selftest.c -- drive axl_shell_execute_status over the cases
 * docs/AXL-Test-Guest-Pool-Design.md §2.4 measured, one exact line each:
 *
 *   CASE|<name>|<RAN|NOT_RUN|UNKNOWN>|0x<status>|nonest=<0|1>|reset=<0|1>
 *   CASE|<name>|DENIED                       (the line refused: an exit)
 *   CASE|<name>|ERR                          (the call itself failed)
 *
 * Usage: shell-status-selftest.efi <mode> <fs>       e.g. nonest fs0:
 *   nonest   every case, in a Shell booted -nonest with fs0: current
 *   nested   the same lines in a default (nesting) Shell
 *   nocwd    one unknown command, run before any current directory exists
 *   break    signal the Shell's break event and return 0 -- run INSIDE a
 *            script, it plays the Ctrl-C that interrupts that script
 *
 * Driven by test/integration/test-shell-status-qemu.sh, which stages the
 * files the cases name. UEFI is used directly for what no AXL API does:
 * signalling the Shell's ExecutionBreak event (AXL_ALLOW_UEFI, as every test
 * in this tree builds with).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

static const char *
outcome_name(
    AxlShellOutcome o
    )
{
    switch (o) {
    case AXL_SHELL_OUTCOME_RAN:     return "RAN";
    case AXL_SHELL_OUTCOME_NOT_RUN: return "NOT_RUN";
    default:                        return "UNKNOWN";
    }
}

static void
run(
    const char *name,
    const char *line
    )
{
    AxlShellResult r;
    int rc = axl_shell_execute_status(line, &r);
    if (rc != AXL_OK) {
        axl_printf("CASE|%s|%s\n", name, rc == AXL_DENIED ? "DENIED" : "ERR");
        return;
    }
    axl_printf("CASE|%s|%s|0x%llx|nonest=%d|reset=%d\n", name,
               outcome_name(r.outcome), (unsigned long long)r.status,
               r.nonest ? 1 : 0, r.reset_ok ? 1 : 0);
}

/* run() with "<fs>" in front of @p rest. */
static void
run_fs(
    const char *name,
    const char *fs,
    const char *rest
    )
{
    char line[256];
    axl_snprintf(line, sizeof(line), "%s%s", fs, rest);
    run(name, line);
}

static int
signal_break(void)
{
    EFI_SHELL_PROTOCOL *shell = NULL;
    if (axl_protocol_find_guid((const AxlGuid *)&gEfiShellProtocolGuid,
                               (void **)&shell) != AXL_OK || shell == NULL) {
        axl_printf("shell-status-selftest: no EFI_SHELL_PROTOCOL\n");
        return 1;
    }
    gBS->SignalEvent(shell->ExecutionBreak);
    return 0;
}

int
main(
    int   argc,
    char *argv[]
    )
{
    if (argc < 2) {
        axl_printf("usage: shell-status-selftest.efi <nonest|nested|nocwd|break> [fs]\n");
        return 2;
    }
    const char *mode = argv[1];
    if (axl_strcmp(mode, "break") == 0) {
        return signal_break();
    }
    if (axl_strcmp(mode, "nocwd") == 0) {
        run("nocwd-unknown", "nosuchcmd");
        return 0;
    }
    if (argc < 3) {
        axl_printf("shell-status-selftest: %s needs <fs>\n", mode);
        return 2;
    }
    const char *fs = argv[2];

    run("echo", "echo hi");
    run("unknown", "nosuchcmd");
    run_fs("missing-efi", fs, "\\missing.efi");
    run_fs("wrong-arch", fs, "\\wrong.efi");
    run_fs("directory", fs, "\\adir");
    run_fs("app-34", fs, "\\exit-status-selftest.efi 34");
    run_fs("app-aborted", fs, "\\exit-status-selftest.efi 8000000000000015");
    run("goto", "goto nowhere");
    run_fs("nsh-exit3", fs, "\\s3.nsh");
    run_fs("nsh-echo", fs, "\\s0.nsh");
    run_fs("nsh-break", fs, "\\sbreak.nsh");
    run("drive", fs);
    run("comment", "# x");
    char line[256];
    axl_snprintf(line, sizeof(line), "echo a | %s\\exit-status-selftest.efi 5", fs);
    run("pipe", line);
    axl_snprintf(line, sizeof(line), "echo a > %s\\o1 > %s\\o2", fs, fs);
    run("double-redirect", line);
    run("unmapped-drive", "fs99:");
    run("exit", "exit 5");
    run("exit-quoted", "\"exit\" 5");
    run("exit-part-quoted", "ex\"it\" 5");
    run("exit-escaped", "e^xit 5");
    run("exit-glued", "\"exit\"5");
    run("at-echo", "@echo hi");
    run("at-space-echo", "@ echo hi");
    run("alias-exit-define", "alias axlquit exit");
    run("alias-exit", "axlquit 5");
    run("empty", "");
    /* Last: one quoted word "exit 5" is not the exit command. Should the
       Shell disagree it would end here, and the DONE line would be missing. */
    run("exit-in-one-word", "\"exit 5\"");
    axl_printf("SHELL-STATUS-DONE\n");
    return 0;
}
