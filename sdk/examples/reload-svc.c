/**
 * reload-svc.c — the application half of the axl_service_reload example.
 *
 * Run from the Shell while reload-svc-dxe.efi is serving. A reload is an
 * application's call: UEFI allows LoadImage and StartImage only below
 * TPL_CALLBACK (2.11 Table 7.3), which a resident service's own callbacks
 * never are, and an image cannot unload itself. This launcher, at
 * TPL_APPLICATION, asks for the replacement; the library loads it, stops the
 * running service and starts the new one.
 *
 *   reload-svc            reload to reload-svc-dxe.efi beside it (gen + 1)
 *
 * With reload-svc-fail-dxe.efi staged beside it (a replacement whose setup
 * fails), it reloads to that instead and reports the start failure: the
 * service is then down. Driven by test-service-reload-qemu.sh and
 * test-service-reload-fail-qemu.sh.
 *
 * The replacements are named beside this image (axl_app_image_dir_path), not
 * at a volume's root, so the example works wherever its files were copied --
 * a disk's fs0:\, or a directory on a shared volume.
 */
#include <axl.h>

#define RSVC_RELOAD_NAME  "reload-svc-dxe.efi"
#define RSVC_POISON_NAME  "reload-svc-fail-dxe.efi"
#define RSVC_PATH_CAP     512

static const AxlGuid RSVC_NS_GUID =
    AXL_GUID(0x71c0de11, 0x4a22, 0x4e55,
             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01);

/* The running service is found by its name: the descriptor needs nothing
   else here. */
static const AxlService reload_svc = {
    .name = "reload-svc",
};

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (axl_nvstore_register_namespace("rsvc", &RSVC_NS_GUID) != AXL_OK) {
        axl_printf("RSVC: nvstore namespace 'rsvc' unavailable\n");
        return 1;
    }
    uint32_t gen = 0;
    size_t   sz  = sizeof(gen);
    if (axl_nvstore_get("rsvc", "gen", &gen, &sz) != 0 || sz != sizeof(gen)) {
        gen = 0;
    }

    char reload_path[RSVC_PATH_CAP];
    char poison_path[RSVC_PATH_CAP];
    if (axl_app_image_dir_path(RSVC_RELOAD_NAME, reload_path, sizeof(reload_path)) != AXL_OK
        || axl_app_image_dir_path(RSVC_POISON_NAME, poison_path, sizeof(poison_path)) != AXL_OK) {
        axl_printf("RSVC: cannot name the replacements beside this image\n");
        return 1;
    }

    /* A replacement that cannot be LOADED stops nothing: the service is
       still serving afterwards, and the code says so (AXL_NOT_FOUND, distinct
       from the start-failure AXL_ERR below). */
    int miss = axl_service_reload(&reload_svc, "fs0:\\no-such-image.efi");
    axl_printf("RSVC: gen%u load-miss rc=%d\n", (unsigned)gen, miss);

    /* Poison staged: the start-failure scenario. The replacement loads but
       its setup fails, so the service is DOWN afterwards (AXL_ERR). */
    AxlFsEntry poison;
    if (axl_file_info(poison_path, &poison) == AXL_OK && !axl_fs_entry_is_dir(&poison)) {
        int frc = axl_service_reload(&reload_svc, poison_path);
        axl_printf("RSVC: gen%u start-fail rc=%d\n", (unsigned)gen, frc);
        /* AXL_ERR means DOWN: the old image is gone too, so nothing serves.
           Recovery is ours -- start the old image again, or reset. */
        AxlServiceDeploy d = { .service = &reload_svc };
        axl_printf("RSVC: gen%u after start-fail running=%d\n", (unsigned)gen,
                   axl_service_is_running(&d) ? 1 : 0);
        return 0;
    }

    /* Bump the generation the replacement will read (test observability). */
    uint32_t next = gen + 1;
    axl_nvstore_set("rsvc", "gen", &next, sizeof(next), AXL_NV_VOLATILE);

    int rc = axl_service_reload(&reload_svc, reload_path);
    axl_printf("RSVC: gen%u reload rc=%d\n", (unsigned)gen, rc);
    return rc == AXL_OK ? 0 : 1;
}
