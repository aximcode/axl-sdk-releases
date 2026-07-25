/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* 9p-mount-selftest.c — proves axl_9p_mount end-to-end: connect to the host
 * 9P server, mount it as a UEFI fsN: volume, then read the server's seeded
 * /hello.txt THROUGH the published EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and
 * assert it byte-matches the same file read over the raw 9P client (the
 * oracle) -- exercising the mount bridge's open/getattr/read path, not just
 * the client. Also writes a new file through the mount and re-reads it over
 * the raw client to prove the write path really reaches the server (a mount
 * that only echoed locally would still pass a same-process read-back).
 *
 * A plain shell app (int main), public headers only. Driven by
 * test-9p-qemu.sh with `<host> <port>` (QEMU user-net: the host is
 * 10.0.2.2 from the guest's point of view).
 *
 * Also reads the server-side read-only /readonly.txt through the mount and
 * checks it byte-matches the known content -- regression coverage for
 * mount_open honoring the caller's requested read/write mode instead of
 * forcing O_RDWR on every existing-file open. p9-server.py rejects any
 * write-capable Tlopen on /readonly.txt with EACCES (mimicking a real 9P
 * server refusing write access on a 0444 file), so the old "always RDWR
 * unless the whole mount is read_only" behavior made this read fail.
 *
 * Output contract (exact lines the harness greps for):
 *   MOUNT: volumes=<n>          -- axl_volume_enumerate count
 *   MOUNT: CONTENT=<bytes>      -- hello.txt read through the matched volume
 *   MOUNT: MATCH=1              -- printed ONLY when that read byte-equals
 *                                   the raw-client oracle
 *   MOUNT: WRITE-RB=<bytes>     -- a file written through the mount, read
 *                                   back over the raw client
 *   MOUNT: RO-READ=1            -- printed ONLY when the read-only file's
 *                                   through-the-mount read byte-matches its
 *                                   known content
 *   MOUNT: DONE                 -- always printed before exit
 */
#include <axl.h>

#define MOUNT_WRITE_CONTENT "mount-write-ok"
#define READONLY_CONTENT    "read-only content\n"

int
main(int argc, char **argv)
{
    /* The volume-matching loop below deliberately probes fsN:\hello.txt on
       volumes that don't have it (every volume but ours) -- each a clean
       WARN on the same line-buffered serial console our MOUNT: lines use.
       Silence WARN so it can't interleave into an exact-match line; keep
       ERROR so a real failure still surfaces. */
    axl_log_set_level(AXL_LOG_ERROR);

    if (argc < 3) {
        axl_printf("MOUNT: FAIL usage: 9p-mount-selftest <host> <port>\n");
        return 1;
    }
    const char *host = argv[1];
    uint16_t    port;
    if (axl_str_to_u16(argv[2], 10, &port, NULL) != AXL_OK || port == 0) {
        axl_printf("MOUNT: FAIL port\n");
        return 1;
    }

    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);

    Axl9pClient *c = NULL;
    if (axl_9p_connect(host, port, "axl", "/", &c) != AXL_OK) {
        axl_printf("MOUNT: FAIL connect\n");
        return 1;
    }

    /* Oracle: read /hello.txt over the raw client before mounting, so the
       through-the-volume read below has something independent to match. */
    AxlBytes *oracle = NULL;
    if (axl_9p_read_file(c, "/hello.txt", &oracle) != AXL_OK || oracle == NULL) {
        axl_printf("MOUNT: FAIL read hello.txt\n");
        axl_9p_disconnect(c);
        return 1;
    }
    size_t      oracle_len  = 0;
    const void *oracle_data = axl_bytes_get_data(oracle, &oracle_len);

    void *vol = NULL;
    if (axl_9p_mount(c, false, &vol) != AXL_OK) {
        axl_printf("MOUNT: FAIL mount\n");
        axl_bytes_unref(oracle);
        axl_9p_disconnect(c);
        return 1;
    }

    /* axl_9p_mount publishes through axl_fs_provider_publish, which assigns
       a real "fsN:" shell mapping immediately (SetMap, not a `map -r`
       refresh) -- no extra step needed here before the volume is
       enumerable and openable by path. */
    AxlVolume vols[16];
    size_t    nv = 0;
    axl_volume_enumerate(vols, 16, &nv);
    axl_printf("MOUNT: volumes=%zu\n", nv);

    /* Identify our mount by content, not by handle/device-path: axl_9p_mount
       returns an opaque token, not the underlying handle, so the only public
       signal available here is "does fsN:\hello.txt read back like the
       oracle". Every other enumerated volume either lacks hello.txt (clean
       failure) or serves a different one, so a match is unambiguous. */
    char matched_fs[16] = {0};
    bool matched         = false;
    for (size_t i = 0; i < nv; i++) {
        char path[64];
        axl_snprintf(path, sizeof(path), "%s:\\hello.txt", vols[i].name);
        void  *buf = NULL;
        size_t len = 0;
        if (axl_file_get_contents(path, &buf, &len) == AXL_OK && buf != NULL) {
            if (len == oracle_len && axl_memcmp(buf, oracle_data, len) == 0) {
                axl_printf("MOUNT: CONTENT=%.*s\n", (int)len, (const char *)buf);
                axl_memcpy(matched_fs, vols[i].name, sizeof(matched_fs));
                matched = true;
            }
            axl_free(buf);
        }
        if (matched) {
            break;
        }
    }
    axl_printf("MOUNT: MATCH=%d\n", matched ? 1 : 0);

    if (matched) {
        /* Write path: create a new file through the mount, then verify over
           the raw client that the bytes actually reached the server. */
        char wpath[64];
        axl_snprintf(wpath, sizeof(wpath), "%s:\\mnttest.txt", matched_fs);
        if (axl_file_set_contents(wpath, MOUNT_WRITE_CONTENT,
                                   sizeof(MOUNT_WRITE_CONTENT) - 1) == AXL_OK) {
            AxlBytes *rb = NULL;
            if (axl_9p_read_file(c, "/mnttest.txt", &rb) == AXL_OK && rb != NULL) {
                size_t      rlen = 0;
                const void *rdata = axl_bytes_get_data(rb, &rlen);
                axl_printf("MOUNT: WRITE-RB=%.*s\n", (int)rlen, (const char *)rdata);
                axl_bytes_unref(rb);
            } else {
                axl_printf("MOUNT: FAIL write-readback\n");
            }
        } else {
            axl_printf("MOUNT: FAIL write\n");
        }

        /* Read-only regression: readonly.txt is server-side read-only (the
           attach user lacks write; p9-server.py's Tlopen handler returns
           EACCES for a write-capable open on it). mount_open must open
           existing files O_RDONLY unless the caller actually requested
           write on a non-read_only mount -- the pre-fix code opened every
           existing file O_RDWR (except on a whole-mount-read_only), so the
           server's lopen failed here and this read never happened. */
        char ropath[64];
        axl_snprintf(ropath, sizeof(ropath), "%s:\\readonly.txt", matched_fs);
        void  *robuf = NULL;
        size_t rolen = 0;
        if (axl_file_get_contents(ropath, &robuf, &rolen) == AXL_OK && robuf != NULL) {
            if (rolen == sizeof(READONLY_CONTENT) - 1 &&
                axl_memcmp(robuf, READONLY_CONTENT, rolen) == 0) {
                axl_printf("MOUNT: RO-READ=1\n");
            }
            axl_free(robuf);
        }
    }

    axl_bytes_unref(oracle);
    axl_9p_unmount(vol);
    axl_9p_disconnect(c);
    axl_printf("MOUNT: DONE\n");
    return matched ? 0 : 1;
}
