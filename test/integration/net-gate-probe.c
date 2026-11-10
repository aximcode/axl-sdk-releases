/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * net-gate-probe.c -- the bring-up engine (axl_net_auto_init_opts), driven
 * from the Shell and reported as exact lines for test-net-gate-qemu.sh.
 *
 *   net-gate-probe late
 *       A firmware-only bring-up with a 10 s link wait and a 3 s DHCP, for a
 *       NIC whose interface appears DURING the link wait: the probe prints
 *       how many NICs exist when it starts, and the host hot-plugs a USB NIC
 *       on seeing that line -- a USB dongle still enumerating. The gate must
 *       find it while it waits.
 *
 *   net-gate-probe sweep DIR
 *       A sweep of DIR alone (no firmware pass), with its companion sidecar
 *       and a 5 s link wait: what a sweep does with a candidate it cannot
 *       use, and with that candidate's companions, when the sweep ends.
 *
 *   net-gate-probe sweep-emb DIR NAME=FILE...
 *       The same sweep with each FILE read into memory and handed to the
 *       engine as an embedded driver named NAME, in the order given.
 *
 *   net-gate-probe race
 *       A firmware-only bring-up with a 5 s link wait and a 15 s DHCP: the
 *       DHCP race over every link-up NIC.
 *
 *   net-gate-probe race-reject
 *       The race with a verifier that REJECTS the first NIC it is asked
 *       about. As it rejects, it restarts every other NIC's DHCP (policy
 *       static, then DHCP), so the NIC that should win leases seconds after
 *       the rejected one -- whatever order the two leased in.
 *
 *   net-gate-probe race-reject-long
 *       race-reject with a 60 s DHCP (two tries) under a 40 s deadline: on a
 *       box with ONE NIC, whose lease the verifier rejects, the pass must end
 *       at the reject (NO_REACH), not wait out the DHCP budget, or retry, to
 *       the deadline.
 *
 *   net-gate-probe unload-refuse FILE
 *       Load FILE (a driver whose Unload refuses) from MEMORY, start it and
 *       unload it, then check the image's loaded-image identity survived the
 *       refused unload: its LoadedImageDevicePath protocol answers with the
 *       same bytes as before, and LoadedImage->FilePath is still set.
 *
 * Output (one line each, in order):
 *   GATE: nics=<n>                       NICs present when the bring-up starts
 *   GATE: ev <OUTCOME> <driver> <mac|->[ dep][ emb]  each progress event;
 *                                        " dep" marks a co-loaded companion,
 *                                        " emb" an embedded driver
 *   GATE: verify <mac> reject|accept     race-reject's verifier
 *   GATE: rc=<rc> online=<0|1> via=<via|-> ip=<a.b.c.d|->
 *   GATE: unload rc=<rc> dp=<intact|changed|gone> filepath=<set|null>
 *
 * UEFI is used directly for what no AXL API does: restarting a NIC's DHCP,
 * and reading an image's loaded-image protocols (AXL_ALLOW_UEFI, as every
 * test in this tree builds with).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

#define GATE_EMB_MAX  8

static bool g_rejected;   ///< race-reject: the first verify call happened

static const char *
outcome_name(
    AxlNetDriverOutcome o
    )
{
    switch (o) {
    case AXL_NET_DRV_EV_UP:            return "UP";
    case AXL_NET_DRV_EV_LINK_NO_LEASE: return "LINK_NO_LEASE";
    case AXL_NET_DRV_EV_NO_REACH:      return "NO_REACH";
    case AXL_NET_DRV_EV_NO_NIC:        return "NO_NIC";
    case AXL_NET_DRV_EV_LOAD_FAIL:     return "LOAD_FAIL";
    case AXL_NET_DRV_EV_SKIPPED_QUAR:  return "SKIPPED_QUAR";
    case AXL_NET_DRV_EV_TRYING:        return "TRYING";
    case AXL_NET_DRV_EV_LINK_DOWN:     return "LINK_DOWN";
    case AXL_NET_DRV_EV_CONFIGURING:   return "CONFIGURING";
    }
    return "?";
}

static void
on_event(
    const AxlNetDriverEvent *ev,   ///< the event (borrowed)
    void                    *ctx   ///< unused
    )
{
    char mac[18] = "-";

    (void)ctx;
    if (ev->have_nic) {
        axl_mac_format(ev->mac, mac, sizeof(mac));
    }
    axl_printf("GATE: ev %s %s %s%s%s\n", outcome_name(ev->outcome), ev->driver, mac,
               ev->is_dependency ? " dep" : "", ev->is_embedded ? " emb" : "");
}

/* Restart the DHCP of every NIC but @p mac: policy static, then DHCP. */
static void
restart_others(
    const uint8_t *mac
    )
{
    EFI_HANDLE *h = NULL;
    UINTN       n = 0;

    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
                                          NULL, &n, &h))) {
        return;
    }
    for (UINTN i = 0; i < n; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
        EFI_IP4_CONFIG2_PROTOCOL    *cfg = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(h[i], &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
                                          (void **)&snp))
            || snp->Mode == NULL
            || axl_memcmp(&snp->Mode->CurrentAddress, mac, 6) == 0
            || EFI_ERROR(gBS->HandleProtocol(h[i], &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                                             (void **)&cfg))) {
            continue;
        }
        uint32_t policy = 0;   /* static: drops the lease */
        cfg->SetData(cfg, Ip4Config2DataTypePolicy, sizeof(policy), &policy);
        policy = 1;            /* DHCP again */
        cfg->SetData(cfg, Ip4Config2DataTypePolicy, sizeof(policy), &policy);
    }
    gBS->FreePool(h);
}

/* race-reject's verifier: reject the first NIC asked about, accept the rest. */
static bool
reject_first(
    const struct AxlNetAutoOpts *opts,   ///< unused
    const uint8_t               *mac     ///< the NIC to verify
    )
{
    char m[18] = "-";

    (void)opts;
    if (mac != NULL) {
        axl_mac_format(mac, m, sizeof(m));
    }
    if (!g_rejected && mac != NULL) {
        g_rejected = true;
        axl_printf("GATE: verify %s reject\n", m);
        restart_others(mac);
        return false;
    }
    axl_printf("GATE: verify %s accept\n", m);
    return true;
}

/* Run the bring-up @p o describes and print its result line. */
static int
run(
    AxlNetAutoOpts *o   ///< the bring-up (on_driver is set here)
    )
{
    size_t nics = 0;

    if (axl_net_list_interfaces(NULL, &nics) != AXL_OK) {
        nics = 0;
    }
    axl_printf("GATE: nics=%zu\n", nics);

    o->on_driver = on_event;
    AxlNetBringUpResult res;
    int  rc = axl_net_auto_init_opts(o, &res);
    char ip[16] = "-";
    if (res.online) {
        axl_ipv4_format(res.ipv4, ip, sizeof(ip));
    }
    axl_printf("GATE: rc=%d online=%d via=%s ip=%s\n", rc, res.online ? 1 : 0,
               res.via[0] != '\0' ? res.via : "-", ip);
    return 0;
}

/* The bytes of @p dp, END node included. */
static size_t
dp_size(
    const EFI_DEVICE_PATH_PROTOCOL *dp
    )
{
    const uint8_t *p = (const uint8_t *)dp;
    size_t         n = 0;

    for (;;) {
        size_t len = (size_t)p[n + 2] | ((size_t)p[n + 3] << 8);
        if (len < 4) {
            return 0;
        }
        n += len;
        if (p[n - len] == END_DEVICE_PATH_TYPE
            && p[n - len + 1] == END_ENTIRE_DEVICE_PATH_SUBTYPE) {
            return n;
        }
    }
}

static int
run_unload_refuse(
    const char *file   ///< the refusing driver
    )
{
    void           *buf = NULL;
    size_t          len = 0;
    AxlDriverHandle drv = NULL;

    if (axl_file_get_contents(file, &buf, &len) != AXL_OK
        || axl_driver_load_buffer((const unsigned char *)buf, len, &drv) != AXL_OK
        || axl_driver_start(drv) != AXL_OK) {
        axl_printf("GATE: unload FAIL load %s\n", file);
        axl_free(buf);
        return 1;
    }
    axl_free(buf);

    EFI_DEVICE_PATH_PROTOCOL *dp     = NULL;
    uint8_t                  *before = NULL;
    size_t                    n      = 0;
    if (!EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)drv,
                                       &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
                                       (void **)&dp))
        && dp != NULL) {
        n = dp_size(dp);
    }
    if (n > 0) {
        before = axl_malloc(n);
        if (before != NULL) {
            axl_memcpy(before, dp, n);
        }
    }
    if (before == NULL) {
        axl_printf("GATE: unload FAIL no device path before the unload\n");
        return 1;
    }

    int rc = axl_driver_unload(drv);

    const char *dps = "gone";
    dp = NULL;
    if (!EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)drv,
                                       &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
                                       (void **)&dp)) && dp != NULL) {
        dps = (dp_size(dp) == n && axl_memcmp(dp, before, n) == 0) ? "intact" : "changed";
    }
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    bool fp = !EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)drv, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                             (void **)&li))
              && li != NULL && li->FilePath != NULL;
    axl_printf("GATE: unload rc=%d dp=%s filepath=%s\n", rc, dps, fp ? "set" : "null");
    axl_free(before);
    return 0;
}

static int
run_sweep_emb(
    int    argc,
    char **argv   ///< [2] DIR, [3..] NAME=FILE
    )
{
    AxlNetEmbeddedDriver emb[GATE_EMB_MAX];
    void                *bufs[GATE_EMB_MAX];
    size_t               ne = 0;
    int                  rc = 1;

    for (int i = 3; i < argc && ne < GATE_EMB_MAX; i++) {
        char *eq = axl_strchr(argv[i], '=');
        size_t len = 0;
        if (eq == NULL) {
            axl_printf("GATE: FAIL argument %s is not NAME=FILE\n", argv[i]);
            goto out;
        }
        *eq = '\0';
        bufs[ne] = NULL;
        if (axl_file_get_contents(eq + 1, &bufs[ne], &len) != AXL_OK) {
            axl_printf("GATE: FAIL cannot read %s\n", eq + 1);
            goto out;
        }
        emb[ne].name  = argv[i];
        emb[ne].image = bufs[ne];
        emb[ne].size  = len;
        ne++;
    }
    AxlNetAutoOpts o = {
        .driver_strategy     = AXL_NET_DRV_SWEEP_DIR,
        .sweep_dir           = argv[2],
        .deps                = axl_net_sweep_deps_sidecar(),
        .skip_firmware_first = true,
        .dhcp_timeout_sec    = 3,
        .timeout_sec         = 30,
        .link_wait_sec       = 5,
        .embedded            = emb,
        .embedded_count      = ne,
    };
    rc = run(&o);
out:
    for (size_t k = 0; k < ne; k++) {
        axl_free(bufs[k]);
    }
    return rc;
}

int
main(
    int    argc,
    char **argv
    )
{
    if (argc == 2 && axl_strcmp(argv[1], "late") == 0) {
        AxlNetAutoOpts o = {
            .driver_strategy  = AXL_NET_DRV_FIRMWARE_ONLY,
            .dhcp_timeout_sec = 3,
            .timeout_sec      = 30,
            .link_wait_sec    = 10,
        };
        return run(&o);
    }
    if (argc == 2 && (axl_strcmp(argv[1], "race") == 0
                      || axl_strcmp(argv[1], "race-reject") == 0)) {
        AxlNetAutoOpts o = {
            .driver_strategy  = AXL_NET_DRV_FIRMWARE_ONLY,
            .dhcp_timeout_sec = 15,
            .timeout_sec      = 40,
            .link_wait_sec    = 5,
        };
        if (axl_strcmp(argv[1], "race-reject") == 0) {
            o.verifier = reject_first;
        }
        return run(&o);
    }
    if (argc == 2 && axl_strcmp(argv[1], "race-reject-long") == 0) {
        AxlNetAutoOpts o = {
            .driver_strategy  = AXL_NET_DRV_FIRMWARE_ONLY,
            .dhcp_timeout_sec = 60,
            .dhcp_retries     = 2,    /* a retry must not wait either */
            .timeout_sec      = 40,
            .link_wait_sec    = 5,
            .verifier         = reject_first,
        };
        return run(&o);
    }
    if (argc == 3 && axl_strcmp(argv[1], "sweep") == 0) {
        AxlNetAutoOpts o = {
            .driver_strategy     = AXL_NET_DRV_SWEEP_DIR,
            .sweep_dir           = argv[2],
            .deps                = axl_net_sweep_deps_sidecar(),
            .skip_firmware_first = true,
            .dhcp_timeout_sec    = 3,
            .timeout_sec         = 30,
            .link_wait_sec       = 5,
        };
        return run(&o);
    }
    if (argc >= 3 && axl_strcmp(argv[1], "sweep-emb") == 0) {
        return run_sweep_emb(argc, argv);
    }
    if (argc == 3 && axl_strcmp(argv[1], "unload-refuse") == 0) {
        return run_unload_refuse(argv[2]);
    }
    axl_printf("usage: net-gate-probe late | race | race-reject | race-reject-long"
               " | sweep DIR"
               " | sweep-emb DIR NAME=FILE... | unload-refuse FILE\n");
    return 1;
}
