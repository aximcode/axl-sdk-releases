/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* 9p-server-selftest.c -- the guest half of the Axl9pServer live-socket
 * harness. Seeds a small tree, exports it with axl_9p_server_new, listens on
 * a TCP port QEMU forwards to the host, and pumps the loop until a deadline.
 * test-9p-server-qemu.sh drives it from the host with p9-client.py; THAT
 * client's assertions are the test. Nothing here asserts anything about the
 * server's behaviour -- a guest that graded its own wire protocol would prove
 * nothing about what actually reaches a peer.
 *
 * A plain shell app (int main), public headers only.
 *
 * Export tree (seeded before the listen so the client finds it immediately):
 *   <root>/hello.txt      -- "hello from 9p server\n"  (21 bytes)
 *   <root>/sub/           -- a directory, deliberately NON-empty
 *   <root>/sub/inner.txt  -- "inner\n"
 *
 * <root> is a `9pexport` directory on a freshly created RAM disk when
 * EFI_RAM_DISK_PROTOCOL can be had, and the same directory on the boot volume
 * (fs0:) when it cannot -- firmware without the protocol (and no RamDiskDxe on
 * the volume to load) must still exercise the whole round-trip, so the
 * fallback is a fully populated path rather than a skip. Both are ordinary
 * subdirectories, so the exported tree has exactly the same shape either way
 * and the harness asserts the same lines against both. `SERVER: ROOT=` reports
 * which one ran. MEASURED: neither OVMF (x64) nor AAVMF (aa64) as shipped here
 * publishes the protocol, and no RamDiskDxe.efi is staged on the image, so QEMU
 * always takes the fs0: path -- the RAM-disk branch is for firmware that does
 * publish it (real hardware commonly does), not for this harness.
 *
 * TWO servers are published over the SAME root: a writable one on <port> and a
 * READ-ONLY one (axl_9p_server_new(..., read_only=true, ...)) on <ro_port>.
 * The read-only export is not decoration -- the dispatch gate that answers
 * EROFS to every mutating message type is otherwise unreachable from a peer,
 * and case 38 of the accumulated case list requires driving every gated type
 * over the wire against a genuinely read-only instance. Both run on the one
 * loop, so the same accept/pump machinery serves both.
 *
 * Output contract (exact lines the harness greps for):
 *   SERVER: ROOT=<path> backing=<ramdisk|fs0>   -- export root + which backing
 *   SERVER: LISTENING port=<n> ro=<n>           -- accepting clients on both
 *   SERVER: REAPING                             -- deadline hit, freeing
 *   SERVER: DONE                                -- axl_9p_server_free returned
 *   SERVER: FAIL <stage>                        -- setup failed; nothing served
 *
 * REAPING/DONE bracket axl_9p_server_free deliberately: the harness holds a
 * SECOND client connection open (with a read view, a write stream and a
 * directory iterator on its fids) across the deadline, so the free reaps a
 * LIVE connection rather than an already-drained one. That teardown ordering
 * had never executed before this test existed.
 */
#include <axl.h>

#define GUEST_PORT_DEFAULT   5640u
#define DEADLINE_MS_DEFAULT  40000u

#define EXPORT_LABEL    "NINEP"
#define EXPORT_SUBDIR   "9pexport"
#define EXPORT_FALLBACK "fs0:\\" EXPORT_SUBDIR

#define HELLO_CONTENT "hello from 9p server\n"
#define INNER_CONTENT "inner\n"

/* Quit the loop once the harness has had its round-trip. A one-shot timeout
   rather than a bounded iterate loop so the app's shape matches how a real
   consumer runs a server: sources drive it, and something eventually quits. */
static bool
on_deadline(void *data)
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

/* Pick the export root: a RAM disk when the firmware will give us one, the
   boot volume otherwise. Always succeeds -- the fallback needs no protocol. */
static void
pick_root(char *out, size_t cap, const char **backing)
{
    void *dev_path = NULL;
    char  fsname[16];

    /* No embedded RamDiskDxe blob is passed: a test app has no business
       carrying tens of KB of driver, and firmware that publishes the protocol
       (or has the driver on a volume) is the only case worth the extra
       coverage. Everything else takes the fs0: path below. */
    if (axl_ramdisk_ensure_driver(NULL, 0, NULL) == AXL_OK
        && axl_ramdisk_create(EXPORT_LABEL, 8, &dev_path) == AXL_OK
        && dev_path != NULL) {
        if (axl_volume_map_name(dev_path, fsname, sizeof(fsname)) == AXL_OK) {
            axl_snprintf(out, cap, "%s:\\%s", fsname, EXPORT_SUBDIR);
            *backing = "ramdisk";
            return;
        }
        /* Created but unmappable -- there is no fsN: to build a path from, so
           this run falls back. Give the disk back first: leaving it registered
           would strand its backing memory (and its label) for a volume nothing
           can address, and the leak would be invisible precisely because this
           branch never runs under QEMU. */
        if (axl_ramdisk_destroy(EXPORT_LABEL) != AXL_OK) {
            axl_printf("SERVER: NOTE ramdisk created but neither mappable "
                       "nor destroyable\n");
        }
    }
    axl_strlcpy(out, EXPORT_FALLBACK, cap);
    *backing = "fs0";
}

/* Create the export root and seed it. Tolerates the root already existing
   (a re-run against a RAM disk that survived, or a boot volume that did) but
   not it existing as a file. */
static bool
seed_tree(const char *root)
{
    char path[256];

    if (axl_dir_mkdir(root) != AXL_OK) {
        AxlFsEntry e;
        if (axl_file_info(root, &e) != AXL_OK || !axl_fs_entry_is_dir(&e)) {
            return false;
        }
    }

    axl_snprintf(path, sizeof(path), "%s\\hello.txt", root);
    if (axl_file_set_contents(path, HELLO_CONTENT,
                              sizeof(HELLO_CONTENT) - 1) != AXL_OK) {
        return false;
    }

    axl_snprintf(path, sizeof(path), "%s\\sub", root);
    if (axl_dir_mkdir(path) != AXL_OK) {
        AxlFsEntry e;
        if (axl_file_info(path, &e) != AXL_OK || !axl_fs_entry_is_dir(&e)) {
            return false;
        }
    }

    axl_snprintf(path, sizeof(path), "%s\\sub\\inner.txt", root);
    return axl_file_set_contents(path, INNER_CONTENT,
                                 sizeof(INNER_CONTENT) - 1) == AXL_OK;
}

int
main(int argc, char **argv)
{
    uint16_t port        = (uint16_t)GUEST_PORT_DEFAULT;
    uint32_t deadline_ms = DEADLINE_MS_DEFAULT;
    uint16_t ro_port     = 0;

    if (argc >= 2 && axl_str_to_u16(argv[1], 10, &port, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL port-arg\n");
        return 1;
    }
    if (argc >= 3
        && axl_str_to_u32(argv[2], 10, &deadline_ms, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL deadline-arg\n");
        return 1;
    }
    if (argc >= 4 && axl_str_to_u16(argv[3], 10, &ro_port, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL ro-port-arg\n");
        return 1;
    }
    if (ro_port == 0) {
        /* Step DOWN from the top of the range rather than wrapping: port + 1
           at 65535 is 0, and axl_9p_server_listen reads 0 as "use the default
           port 564", which would silently publish the read-only export
           somewhere the harness is not looking. */
        ro_port = (port < 65535u) ? (uint16_t)(port + 1u)
                                  : (uint16_t)(port - 1u);
    }

    char        root[128];
    const char *backing = "";
    pick_root(root, sizeof(root), &backing);
    if (!seed_tree(root)) {
        axl_printf("SERVER: FAIL seed\n");
        return 1;
    }
    axl_printf("SERVER: ROOT=%s backing=%s\n", root, backing);

    /* The startup script's `ifconfig -s eth0 dhcp` is enough to give the guest
       an address on x64/OVMF but NOT on aa64/AAVMF, where axl_tcp_listen finds
       no TCP4 service binding without this -- the same call every AxlTestNet
       serve mode that runs on both arches makes. Verified: without it, aa64
       reaches `SERVER: FAIL listen` with "tcp: no TCP4 service binding". */
    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        axl_printf("SERVER: FAIL loop\n");
        return 1;
    }

    /* One `stage` chain rather than four copies of the same teardown: the
       read-only export doubles the number of ways setup can fail, and
       axl_9p_server_free is a documented no-op on NULL, so the single exit
       below is correct however far the chain got. */
    Axl9pServer *srv   = NULL;
    Axl9pServer *ro    = NULL;
    const char  *stage = NULL;

    if (axl_9p_server_new(loop, root, false, &srv) != AXL_OK) {
        stage = "server-new";
    } else if (axl_9p_server_listen(srv, port) != AXL_OK) {
        stage = "listen";
    } else if (axl_9p_server_new(loop, root, true, &ro) != AXL_OK) {
        stage = "server-new-ro";
    } else if (axl_9p_server_listen(ro, ro_port) != AXL_OK) {
        stage = "listen-ro";
    } else if (axl_loop_add_timeout(loop, deadline_ms, on_deadline, loop) == 0) {
        stage = "deadline";
    }
    if (stage != NULL) {
        axl_printf("SERVER: FAIL %s\n", stage);
        axl_9p_server_free(ro);
        axl_9p_server_free(srv);
        axl_loop_free(loop);
        return 1;
    }

    axl_printf("SERVER: LISTENING port=%u ro=%u\n", (unsigned)port,
               (unsigned)ro_port);
    axl_loop_run(loop);

    /* Bracket the free so a hang or fault inside it is distinguishable from
       a deadline that never fired. */
    axl_printf("SERVER: REAPING\n");
    axl_9p_server_free(ro);
    axl_9p_server_free(srv);
    axl_loop_free(loop);
    axl_printf("SERVER: DONE\n");
    return 0;
}
