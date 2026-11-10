/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * pack-fixture.c — reports everything a wrapped (packed) image must see
 * exactly as its raw original does.
 *
 * Built twice by the build: raw as pack-fixture.efi and force-wrapped by
 * the axl-pack stub as pack-fixture-w.efi. test-efi-pack-qemu.sh runs both and
 * compares the lines, so each line below is one channel the stub has to carry
 * across to the inner image:
 *
 *   ARGC= / ARGV[i]=   LoadedImage->LoadOptions (AXL tokenizes argv from it)
 *   SELF=              DeviceHandle + FilePath (axl_app_image_path)
 *   BOOT=              DeviceHandle re-resolved (axl_app_boot_path)
 *   LIDP=              the LoadedImageDevicePath the firmware recorded from
 *                      LoadImage's device path argument (NULL when it was
 *                      given none -- what the aarch64 Shell's `dh` faults on)
 *   --write            prints `hello`, so a shell `>` or `|` is observable
 *   --read             prints `READ=<first stdin line>` (`<EOF>` when none);
 *                      a `|` pipe reaches an image only through
 *                      EFI_SHELL_PARAMETERS_PROTOCOL, in both directions
 *   --exit N           the image's exit status, read back as %lasterror%
 *   --exitdata         Exit(EFI_ABORTED) with ExitData "XDATA"
 *   --launch \FILE     LoadImage + StartImage FILE (on this volume) with
 *                      `x --exitdata` as its LoadOptions, and print what
 *                      StartImage returned: `EXIT=0x... DATA=<text|NULL>`.
 *                      The Shell discards ExitData, so this is the only way
 *                      to observe that a wrapped image forwards it.
 *   --launch-buf PATH  the same, but LoadImage from the file's bytes in a
 *                      buffer with NO device path: the started image has no
 *                      DeviceHandle or FilePath, so it cannot re-read its own
 *                      file (the stub's retry must give up cleanly).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>   /* gBS, gImageHandle -- fixture opts in explicitly */

#define PACK_FIXTURE_EXITDATA u"XDATA"

static int  pack_fixture_launch(const char *file);
static int  pack_fixture_launch_buf(const char *path);
static int  pack_fixture_start(EFI_HANDLE child);
static void pack_fixture_exitdata(void);

/* Exit through the firmware with ExitData; the AXL teardown is skipped on
   purpose (there is no AXL route that carries ExitData). */
static void
pack_fixture_exitdata(void)
{
    static const CHAR16 text[] = PACK_FIXTURE_EXITDATA;
    CHAR16             *data   = NULL;

    if (gBS->AllocatePool(EfiBootServicesData, sizeof(text),
                          (void **)&data) != EFI_SUCCESS) {
        return;
    }
    axl_memcpy(data, text, sizeof(text));
    gBS->Exit(gImageHandle, EFI_ABORTED, sizeof(text), data);
}

/* Start FILE (a path on this image's own volume, e.g. "\pack-fixture-w.efi")
   with `x --exitdata` and report StartImage's status and ExitData. */
static int
pack_fixture_launch(
    const char *file)
{
    EFI_LOADED_IMAGE_PROTOCOL *li  = NULL;
    EFI_DEVICE_PATH_PROTOCOL  *dev = NULL;

    if (gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid,
                            (void **)&li) != EFI_SUCCESS
        || gBS->HandleProtocol(li->DeviceHandle, &gEfiDevicePathProtocolGuid,
                               (void **)&dev) != EFI_SUCCESS) {
        axl_printf("LAUNCH=no device path\n");
        return 1;
    }

    /* <device path without its end node> + FILEPATH(file) + end node. */
    size_t   dev_len  = axl_device_path_size(dev) - 4;
    size_t   name_len = axl_strlen(file);
    size_t   node_len = 4 + 2 * (name_len + 1);
    uint8_t *dp       = axl_malloc(dev_len + node_len + 4);
    if (dp == NULL) {
        axl_printf("LAUNCH=no memory\n");
        return 1;
    }
    axl_memcpy(dp, dev, dev_len);
    uint8_t *node = dp + dev_len;
    node[0] = 0x04;                         /* MEDIA_DEVICE_PATH */
    node[1] = 0x04;                         /* MEDIA_FILEPATH_DP */
    node[2] = (uint8_t)(node_len & 0xFF);
    node[3] = (uint8_t)(node_len >> 8);
    for (size_t i = 0; i <= name_len; i++) {
        node[4 + 2 * i]     = (uint8_t)file[i];
        node[4 + 2 * i + 1] = 0;
    }
    uint8_t *end = node + node_len;
    end[0] = 0x7F;
    end[1] = 0xFF;
    end[2] = 4;
    end[3] = 0;

    EFI_HANDLE child = NULL;
    EFI_STATUS st    = gBS->LoadImage(FALSE, gImageHandle,
                                      (EFI_DEVICE_PATH_PROTOCOL *)dp, NULL, 0,
                                      &child);
    axl_free(dp);
    if (st != EFI_SUCCESS) {
        axl_printf("LAUNCH=LoadImage 0x%llx\n", (unsigned long long)st);
        return 1;
    }

    return pack_fixture_start(child);
}

/* Load PATH from a buffer, with no device path, and start it. */
static int
pack_fixture_launch_buf(
    const char *path)
{
    void  *buf = NULL;
    size_t len = 0;

    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        axl_printf("LAUNCH=cannot read %s\n", path);
        return 1;
    }
    EFI_HANDLE child = NULL;
    EFI_STATUS st    = gBS->LoadImage(FALSE, gImageHandle, NULL, buf, len,
                                      &child);
    axl_free(buf);
    if (st != EFI_SUCCESS) {
        axl_printf("LAUNCH=LoadImage 0x%llx\n", (unsigned long long)st);
        return 1;
    }
    return pack_fixture_start(child);
}

/* StartImage CHILD with `x --exitdata` and print its status and ExitData. */
static int
pack_fixture_start(
    EFI_HANDLE child)
{
    static CHAR16              opts[] = u"x --exitdata";
    EFI_LOADED_IMAGE_PROTOCOL *cli    = NULL;
    if (gBS->HandleProtocol(child, &gEfiLoadedImageProtocolGuid,
                            (void **)&cli) == EFI_SUCCESS) {
        cli->LoadOptions     = opts;
        cli->LoadOptionsSize = (UINT32)sizeof(opts);
    }

    UINTN      size = 0;
    CHAR16    *data = NULL;
    EFI_STATUS st   = gBS->StartImage(child, &size, &data);

    char text[32];
    size_t n = 0;
    for (size_t i = 0; data != NULL && i < size / 2 && n < sizeof(text) - 1; i++) {
        if (data[i] == 0) {
            break;
        }
        text[n++] = (char)data[i];
    }
    text[n] = '\0';
    axl_printf("EXIT=0x%llx DATA=%s\n", (unsigned long long)st,
               (data != NULL) ? text : "NULL");
    if (data != NULL) {
        gBS->FreePool(data);
    }
    return 0;
}

int
main(
    int    argc,
    char **argv)
{
    if (argc >= 2 && axl_strcmp(argv[1], "--write") == 0) {
        axl_printf("hello\n");
        return 0;
    }
    if (argc >= 2 && axl_strcmp(argv[1], "--exitdata") == 0) {
        pack_fixture_exitdata();
        return 1;
    }
    if (argc >= 3 && axl_strcmp(argv[1], "--launch") == 0) {
        return pack_fixture_launch(argv[2]);
    }
    if (argc >= 3 && axl_strcmp(argv[1], "--launch-buf") == 0) {
        return pack_fixture_launch_buf(argv[2]);
    }
    if (argc >= 2 && axl_strcmp(argv[1], "--read") == 0) {
        AxlStream *t    = axl_stdin_text();
        char      *line = (t != NULL) ? axl_readline(t) : NULL;
        if (line != NULL) {
            size_t n = axl_strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                line[--n] = '\0';
            }
        }
        axl_printf("READ=%s\n", (line != NULL) ? line : "<EOF>");
        axl_free(line);
        if (t != NULL) {
            axl_fclose(t);
        }
        return 0;
    }

    axl_printf("ARGC=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        axl_printf("ARGV[%d]=%s\n", i, argv[i]);
    }

    const char *self = axl_app_image_path();
    axl_printf("SELF=%s\n", (self != NULL) ? self : "NULL");

    char boot[256];
    if (axl_app_boot_path("x", boot, sizeof(boot)) == AXL_OK) {
        axl_printf("BOOT=%s\n", boot);
    } else {
        axl_printf("BOOT=ERR\n");
    }

    void *lidp = NULL;
    if (gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageDevicePathProtocolGuid,
                            &lidp) != EFI_SUCCESS) {
        lidp = NULL;
    }
    char *lidp_text = (lidp != NULL) ? axl_device_path_to_text(lidp) : NULL;
    axl_printf("LIDP=%s\n", (lidp_text != NULL) ? lidp_text : "NULL");
    axl_free(lidp_text);

    if (argc >= 3 && axl_strcmp(argv[1], "--exit") == 0) {
        axl_set_exit_status((AxlEfiStatus)axl_strtou64(argv[2]));
    }
    return 0;
}
