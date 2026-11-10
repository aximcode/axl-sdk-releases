/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * sshd-test-hook.c -- strong definitions of test seams, linked ONLY into the
 * test build of sshd (sshd-t.efi: its launcher and its driver,
 * sshd-t-dxe.efi). No shipped sshd image links this file: the library's
 * _axl_console_device_test_point, _axl_ssh_host_key_test_point and
 * _axl_tcp_test_point and sshd-svc.c's _sshd_test_point are weak and always
 * false there --
 * test-efi-pack-release-qemu.sh asserts it by symbol SIZE: every test point
 * in a shipped image must be the weak always-false stub's size.
 *
 * Each point is armed by a file on the boot volume, so the test's own Shell
 * script can arm and disarm it between commands:
 *
 *   fs0:\sshd-fault-disconnect  axl_console_device_uninstall's disconnect
 *                               is refused: the take-over cannot be undone
 *   fs0:\sshd-fault-setup       sshd's setup fails right after it took the
 *                               console
 *   fs0:\sshd-fault-hk-read     the host-key variable reads as a firmware
 *                               error (nv-read)
 *   fs0:\sshd-fault-hk-read-dxe ...in the DRIVER only (this file compiled
 *                               with SSHD_TEST_HOOK_DRIVER): the launcher's
 *                               pre-check reads it fine, so the driver's own
 *                               backstop is what refuses
 *   fs0:\sshd-fault-hk-store    storing a new host key fails (nv-write)
 *   fs0:\sshd-fault-hk-store-once  ...once: the file is deleted as the
 *                               write is failed, so the next write works
 *   fs0:\sshd-fault-hk-readback ...or does not read back (nv-readback)
 *   fs0:\sshd-fault-hk-keep     ...and that write is not deleted again, so
 *                               it stays in the variable (nv-cleanup-refused)
 *   fs0:\sshd-fault-hk-delete   deleting it is refused, as for an
 *                               authenticated variable (nv-delete-denied)
 *   fs0:\sshd-tcp-stats         not a fault: AxlTcp logs each connection's
 *                               receive counts as it closes (recv-stats;
 *                               test-sshd-linger-qemu.sh)
 *
 * A file is looked up on every call. The points are reached at
 * TPL_APPLICATION (the driver's entry and its Unload, the launcher) and, for
 * the console device's, also at TPL_CALLBACK from sshd's on_tick (the
 * release after the Shell exits). A SimpleFileSystem read is legal at both.
 * The console device's TPL_CALLBACK caller is never armed by a test.
 * _axl_tcp_test_point is different: it is reached from axl_tcp_close, which
 * in sshd's driver-pumped loop runs at TPL_CALLBACK, and
 * test-sshd-linger-qemu.sh arms it there on purpose -- one file lookup per
 * closed connection, legal at that level.
 */

#include <axl.h>
#include <axl/axl-fs.h>
#include <axl/axl-str.h>

#include "../../src/util/axl-console-device-internal.h"
#include "../../src/net/axl-ssh-host-key-internal.h"
#include "../../src/net/axl-tcp-internal.h"
#include "../../tools/sshd-svc.h"

static bool
armed(const char *path)
{
    AxlFsEntry e;
    return axl_file_info(path, &e) == AXL_OK;
}

/* armed(), and disarmed as it fires: a fault for exactly one call. */
static bool
armed_once(const char *path)
{
    if (!armed(path)) {
        return false;
    }
    if (axl_file_delete(path) != AXL_OK) {
        axl_printf("sshd-test-hook: cannot disarm %s\n", path);
    }
    return true;
}

bool
_axl_console_device_test_point(const char *point)
{
    return axl_strcmp(point, "disconnect") == 0 && armed("fs0:\\sshd-fault-disconnect");
}

bool
_axl_tcp_test_point(const char *point)
{
    return axl_strcmp(point, "recv-stats") == 0 && armed("fs0:\\sshd-tcp-stats");
}

bool
_sshd_test_point(const char *point)
{
    return axl_strcmp(point, "setup") == 0 && armed("fs0:\\sshd-fault-setup");
}

bool
_axl_ssh_host_key_test_point(const char *point)
{
    static const struct {
        const char *point;
        const char *file;
    } map[] = {
        { "nv-read",          "fs0:\\sshd-fault-hk-read" },
        { "nv-write",         "fs0:\\sshd-fault-hk-store" },
        { "nv-readback",      "fs0:\\sshd-fault-hk-readback" },
        { "nv-cleanup-refused", "fs0:\\sshd-fault-hk-keep" },
        { "nv-delete-denied", "fs0:\\sshd-fault-hk-delete" },
    };
#ifdef SSHD_TEST_HOOK_DRIVER
    if (axl_strcmp(point, "nv-read") == 0 && armed("fs0:\\sshd-fault-hk-read-dxe")) {
        return true;
    }
#endif
    if (axl_strcmp(point, "nv-write") == 0 && armed_once("fs0:\\sshd-fault-hk-store-once")) {
        return true;
    }
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (axl_strcmp(point, map[i].point) == 0) {
            return armed(map[i].file);
        }
    }
    return false;
}
