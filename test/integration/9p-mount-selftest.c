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
 *   MOUNT: XDIR-RENAME=1        -- a move between directories by Trename
 *   MOUNT: XDIR-EXDEV=1         -- one the server answers EXDEV is
 *                                   AXL_UNSUPPORTED from axl_file_rename,
 *                                   nothing changed
 *   MOUNT: XDIR-MOVE=1          -- ...and axl_file_move copies it instead
 *   MOUNT: MTIME=1              -- axl_file_set_mtime by Tsetattr
 *   MOUNT: RENAME-INPLACE=1     -- a rename within one directory
 *   MOUNT: TRUNCATE=1           -- axl_file_truncate by Tsetattr(SIZE)
 *   MOUNT: RENAME-CASE=1        -- mnt2.txt renamed to MNT2.txt
 *   MOUNT: LONG-LIST=1          -- /long lists its 400-byte, 200-unit name
 *                                   whole, and skips a 300-unit name (over
 *                                   FAT's 255) and an 800-byte one
 *   MOUNT: DONE                 -- always printed before exit
 */
#include <axl.h>

#define MOUNT_WRITE_CONTENT "mount-write-ok"
#define READONLY_CONTENT    "read-only content\n"
#define MOUNT_MTIME         1767225600ULL   /* 2026-01-01 00:00:00 UTC */

/* True when @p path reads over the raw client as exactly @p want. */
static bool
raw_holds(Axl9pClient *c, const char *path, const char *want)
{
    AxlBytes *b = NULL;
    if (axl_9p_read_file(c, path, &b) != AXL_OK || b == NULL) {
        return false;
    }
    size_t      n = 0;
    const void *d = axl_bytes_get_data(b, &n);
    bool        ok = n == axl_strlen(want) && axl_memcmp(d, want, n) == 0;
    axl_bytes_unref(b);
    return ok;
}

/* True when @p path does not read over the raw client at all. */
static bool
raw_gone(Axl9pClient *c, const char *path)
{
    AxlBytes *b = NULL;
    if (axl_9p_read_file(c, path, &b) == AXL_OK) {
        axl_bytes_unref(b);
        return false;
    }
    return true;
}

/* Provider version 2 through the mount: renames by Trename on the file's
   own fid, times and sizes by Tsetattr. p9-server.py lets a Trename move
   between directories only below /mv (as one POSIX filesystem would) and
   answers EXDEV elsewhere; it refuses Tlcreate in /mv/b, so a COPY into it
   fails and only a real rename can satisfy MOUNT: XDIR-RENAME. */
static void
mount_v2_checks(Axl9pClient *c, const char *fs)
{
    char a[96];
    char b[96];
    AxlFsEntry e;

    axl_snprintf(a, sizeof(a), "%s:\\mv\\a\\x.txt", fs);
    axl_snprintf(b, sizeof(b), "%s:\\mv\\b\\x.txt", fs);
    if (axl_file_rename(a, b) == AXL_OK && raw_holds(c, "/mv/b/x.txt", "move-me\n")
        && raw_gone(c, "/mv/a/x.txt")) {
        axl_printf("MOUNT: XDIR-RENAME=1\n");
    }

    axl_snprintf(a, sizeof(a), "%s:\\dir\\a.txt", fs);
    axl_snprintf(b, sizeof(b), "%s:\\a2.txt", fs);
    if (axl_file_rename(a, b) == AXL_UNSUPPORTED && raw_holds(c, "/dir/a.txt", "alpha\n")
        && raw_gone(c, "/a2.txt")) {
        axl_printf("MOUNT: XDIR-EXDEV=1\n");
    }

    axl_snprintf(a, sizeof(a), "%s:\\dir\\b.txt", fs);
    axl_snprintf(b, sizeof(b), "%s:\\b2.txt", fs);
    if (axl_file_move(a, b) == AXL_OK && raw_holds(c, "/b2.txt", "bravo\n")
        && raw_gone(c, "/dir/b.txt")) {
        axl_printf("MOUNT: XDIR-MOVE=1\n");
    }

    axl_snprintf(a, sizeof(a), "%s:\\hello.txt", fs);
    if (axl_file_set_mtime(a, MOUNT_MTIME) == AXL_OK
        && axl_file_info(a, &e) == AXL_OK && e.mtime_unix == MOUNT_MTIME) {
        axl_printf("MOUNT: MTIME=1\n");
    }

    axl_snprintf(a, sizeof(a), "%s:\\mnttest.txt", fs);
    if (axl_file_rename(a, "mnt2.txt") == AXL_OK
        && raw_holds(c, "/mnt2.txt", MOUNT_WRITE_CONTENT) && raw_gone(c, "/mnttest.txt")) {
        axl_printf("MOUNT: RENAME-INPLACE=1\n");
    }

    axl_snprintf(a, sizeof(a), "%s:\\mnt2.txt", fs);
    if (axl_file_truncate(a, 5) == AXL_OK && raw_holds(c, "/mnt2.txt", "mount")) {
        axl_printf("MOUNT: TRUNCATE=1\n");
    }

    /* A rename that changes only the letter case. */
    if (axl_file_rename(a, "MNT2.txt") == AXL_OK
        && raw_holds(c, "/MNT2.txt", "mount") && raw_gone(c, "/mnt2.txt")) {
        axl_printf("MOUNT: RENAME-CASE=1\n");
    }

    /* /long holds a 200 x U+00E9 name -- 400 bytes, 200 units: past the 256
       bytes AxlFsEntry.name once held, inside FAT's 255 units -- a
       300-character ASCII name (300 units: no FAT holds it) and an
       800-byte one: listed whole, skipped, skipped. */
    axl_snprintf(a, sizeof(a), "%s:\\long", fs);
    AxlDir *d     = axl_dir_open(a);
    size_t  n     = 0;
    bool    whole = false;
    while (d != NULL && axl_dir_read(d, &e)) {
        n++;
        whole = axl_strlen(e.name) == 400 && (uint8_t)e.name[0] == 0xC3
                && (uint8_t)e.name[399] == 0xA9;
    }
    axl_dir_close(d);
    if (n == 1 && whole) {
        axl_printf("MOUNT: LONG-LIST=1\n");
    }
}

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

        mount_v2_checks(c, matched_fs);
    }

    axl_bytes_unref(oracle);
    axl_9p_unmount(vol);
    axl_9p_disconnect(c);
    axl_printf("MOUNT: DONE\n");
    return matched ? 0 : 1;
}
