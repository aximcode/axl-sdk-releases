/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-pack-stub.c -- the self-extracting front of a packed (compressed) .efi.
 *
 * A wrapped image is this stub linked with the packed payload as
 * `axl_embedded_pack` (an `.incbin` blob; see cmake/AxlWrapEfi.cmake). The
 * stub decodes the payload into a pool buffer and has the FIRMWARE load and
 * start the original from it. It never executes decoded bytes itself, so
 * relocation, section protection, I-cache maintenance and Secure Boot policy
 * are all the firmware's, exactly as for the raw image.
 *
 * Raw UEFI, NOT libaxl, on purpose: the AXL runtime is ~30 KB before it does
 * anything (docs/AXL-Minimal-Image-Notes.md), and every byte here is a
 * byte every wrapped tool pays. Only the freestanding format module and the
 * vendored LZMA decoder + branch filters are linked beside it.
 *
 * What the inner image must see exactly as it would have raw -- each of these
 * is a separate channel, and test-efi-pack-qemu.sh has a case for each:
 *
 *   argv         AXL tokenizes argv from LoadedImage->LoadOptions, so the
 *                stub's LoadOptions are copied to the inner image.
 *   stdio        A Shell redirect is EFI_SHELL_PARAMETERS_PROTOCOL on the
 *                image handle; the stub installs the SAME interface on the
 *                inner handle for the duration of StartImage, as the Shell
 *                itself does for the stub.
 *   identity     LoadImage gets the stub's own file device path, so the
 *                firmware fills the inner DeviceHandle/FilePath (the image's
 *                own path, its boot volume, sidecar lookup). A NULL path is
 *                never passed where a real one exists: the aarch64 Shell's
 *                `dh` faults on a NULL device path.
 *   exit status  StartImage's status (and any ExitData) is the stub's.
 *
 * A payload that fails its checks (AXL_PACK_E_CORRUPT, or AXL_PACK_E_HEADER,
 * which the same glitch can hit) is re-read from the image's own FILE, up to
 * PACK_STUB_REREADS times, before the stub gives up: on BMC virtual media
 * the firmware's one large read of the file has been measured coming back
 * corrupt 7 times in 10 while 64 KB reads of it were exact, so the stub's
 * re-read is made in 64 KB pieces (src/pack/README.md, "Re-reading a corrupt
 * payload"). Each re-read prints one line first. An image with no file to
 * re-read (loaded from a buffer) ends the retries at once.
 *
 * One object serves both the app and the driver variant; they differ only in
 * the PE subsystem written at link time. A driver's inner image stays
 * resident after StartImage, and so does the stub, which costs its own few KB.
 *
 * Every failure prints ONE final line on standard error, `axl-pack: <reason>`
 * (after the re-read lines, for a bad payload), and returns EFI_LOAD_ERROR for
 * a bad payload or the firmware's own status otherwise.
 */

#include <uefi/axl-uefi.h>

#include "axl-pack-format.h"
#include "../backend/native/axl-backend-native-lines.h"  /* axl_text_out_lines */
#include "../fs/axl-fs-read-chunk.h"  /* AXL_FS_READ_CHUNK */

#if !defined(AXL_PACK_MACHINE_NATIVE)
#  error "axl-pack-stub: unsupported architecture"
#endif

#define PACK_DP_END_TYPE        0x7Fu
#define PACK_DP_END_ENTIRE      0xFFu
#define PACK_DP_END_SIZE        4u
#define PACK_MSG_MAX            128u
#define PACK_DP_MEDIA           0x04u
#define PACK_DP_MEDIA_FILEPATH  0x04u
#define PACK_STUB_REREADS       2
#define PACK_STR_(x)            #x
#define PACK_STR(x)             PACK_STR_(x)
/* What follows the attempt digit in the re-read line. */
#define PACK_REREAD_OF          " of " PACK_STR(PACK_STUB_REREADS) ")"
#define PACK_REREAD_UNREADABLE  (-1)
/* EFI_FILE_INFO with room for a 255-character name (the FAT limit), in
   UINT64s for its alignment: on the stack, so one GetInfo call does. */
#define PACK_FILE_INFO_WORDS    ((sizeof(EFI_FILE_INFO) + 256u * sizeof(CHAR16) + 7u) / 8u)

/** Pool allocator context: the memory type the firmware gave this image. */
typedef struct {
    EFI_BOOT_SERVICES *bs;
    EFI_MEMORY_TYPE    type;
} PackStubPool;

/* The packed payload, linked in by WRAP_EFI (zero bytes in the -empty images
   used only to measure the stub). */
extern const unsigned char axl_embedded_pack[];
extern const unsigned char axl_embedded_pack_end[];

/* -fstack-protector-strong is on for every object in this tree, and its two
   hooks live in libaxl, which this image does not link. Supplied here rather
   than compiled out, so the decoder keeps the protection. */
unsigned long __stack_chk_guard = 0x00000aff0d0a0000UL;

/* This image's handle, for pack_stub_say's look at its Shell parameters. */
static EFI_HANDLE pack_stub_image;

void           __stack_chk_fail(void);
void          *memcpy(void *dst, const void *src, size_t n);
void          *memset(void *dst, int c, size_t n);
EFI_STATUS EFIAPI axl_pack_stub_entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

static void    pack_stub_copy(unsigned char *dst, const void *src, size_t n);
static void   *pack_stub_alloc(void *ctx, size_t n);
static void    pack_stub_free(void *ctx, void *p);
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *pack_stub_text_out(
    EFI_BOOT_SERVICES *bs, EFI_GUID *on, EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mode_of);
static void    pack_stub_say(EFI_SYSTEM_TABLE *st, const char *what,
                             const char *tail, EFI_STATUS status,
                             int with_status);
static size_t  pack_stub_dp_size(const EFI_DEVICE_PATH_PROTOCOL *dp);
static EFI_DEVICE_PATH_PROTOCOL *pack_stub_dp_dup(EFI_BOOT_SERVICES *bs,
                                                  const EFI_DEVICE_PATH_PROTOCOL *a,
                                                  const EFI_DEVICE_PATH_PROTOCOL *b);
static EFI_STATUS pack_stub_self_dp(EFI_BOOT_SERVICES *bs, EFI_HANDLE image,
                                    EFI_LOADED_IMAGE_PROTOCOL *li,
                                    EFI_DEVICE_PATH_PROTOCOL **out, int *owned);
static int     pack_stub_reread(EFI_BOOT_SERVICES *bs,
                                EFI_LOADED_IMAGE_PROTOCOL *li,
                                const AxlPackAlloc *alloc, void **raw,
                                size_t *raw_len);

void
__stack_chk_fail(void)
{
    /* No safe way back into a smashed frame, and no runtime to report it. */
    for (;;) {
    }
}

/* For the decoder and the compiler only: LzmaDec.c references memcpy, and GCC
   may emit memcpy/memset for aggregate copies and initialisers. The stub's
   own copies use pack_stub_copy. Byte loops through volatile pointers, so the
   loop-idiom pass cannot turn them back into calls to themselves. */
void *
memcpy(
    void       *dst,
    const void *src,
    size_t      n)
{
    volatile unsigned char *d = dst;
    const unsigned char    *s = src;
    while (n-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

void *
memset(
    void   *dst,
    int     c,
    size_t  n)
{
    volatile unsigned char *d = dst;
    while (n-- > 0) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

static void
pack_stub_copy(
    unsigned char *dst,
    const void    *src,
    size_t         n)
{
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s[i];
    }
}

static void *
pack_stub_alloc(
    void   *ctx,
    size_t  n)
{
    PackStubPool *pool = ctx;
    void         *p    = NULL;
    if (pool->bs->AllocatePool(pool->type, n, &p) != EFI_SUCCESS) {
        return NULL;
    }
    return p;
}

static void
pack_stub_free(
    void *ctx,
    void *p)
{
    PackStubPool *pool = ctx;
    if (p != NULL) {
        pool->bs->FreePool(p);
    }
}

/* The first SimpleTextOut on a handle carrying @p on: with @p mode_of NULL,
   any; else the one other than @p mode_of that shares its Mode. */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *
pack_stub_text_out(
    EFI_BOOT_SERVICES               *bs,
    EFI_GUID                        *on,
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mode_of)
{
    UINTN                            count   = 0;
    EFI_HANDLE                      *handles = NULL;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *found   = NULL;
    if (bs->LocateHandleBuffer(ByProtocol, on, NULL, &count, &handles) != EFI_SUCCESS) {
        return NULL;
    }
    for (UINTN i = 0; i < count && found == NULL; i++) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t = NULL;
        if (bs->HandleProtocol(handles[i], &gEfiSimpleTextOutputProtocolGuid,
                               (void **)&t) == EFI_SUCCESS
            && t != NULL && t != mode_of
            && (mode_of == NULL || t->Mode == mode_of->Mode)) {
            found = t;
        }
    }
    bs->FreePool(handles);
    return found;
}

/* One line, `axl-pack: <what><tail>` or `axl-pack: <what>: EFI status 0x...`,
   on standard error (below). */
static void
pack_stub_say(
    EFI_SYSTEM_TABLE *st,
    const char       *what,
    const char       *tail,
    EFI_STATUS        status,
    int               with_status)
{
    /* Not const, so in .data: the stub's .rodata sits exactly on a
       FileAlignment step, and one byte more there is 512 more in every
       wrapped image (src/pack/README.md, "Re-reading a corrupt payload"). */
    static char  prefix[] = "axl-pack: ";
    static char  sep[]    = ": EFI status 0x";
    const char  *parts[3] = {
        prefix, what, (tail != NULL) ? tail : (with_status ? sep : "")
    };
    CHAR16       msg[PACK_MSG_MAX];
    size_t       n = 0;

    for (size_t i = 0; i < 3; i++) {
        for (const char *s = parts[i]; *s != '\0' && n < PACK_MSG_MAX - 21; s++) {
            msg[n++] = (CHAR16)*s;
        }
    }
    if (with_status) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            unsigned d = (unsigned)((status >> shift) & 0xFu);
            msg[n++] = (CHAR16)(d < 10 ? '0' + d : 'A' + d - 10);
        }
    }
    msg[n++] = '\r';
    msg[n++] = '\n';
    msg[n]   = 0;

    /* Standard error on the console is stdout's stream, as libaxl writes it
       (axl_backend_console_write_err): through the console's ConOut
       splitter, whose cursor record the Shell draws its next prompt from --
       a StdErr line never moves it, so the prompt landed on this line --
       plus every StdErr device ConOut does not reach. "On the console" is
       by identity: the Shell handle IS the Shell's console file
       (OpenFileByName ">e"), which `NUL` and `2>v name` are not. The
       splitter is the SimpleTextOut sharing the Mode of the Shell's console
       logger (on the handle carrying EFI_SHELL_PROTOCOL); not the logger
       itself, which can stop and wait for a key. Anything else -- `2>`, no
       Shell parameters (the EFI 1.x shell), no Shell protocol, no splitter
       -- StdErr itself; ConOut when there is no StdErr. A line exactly the
       console's width is written as libaxl writes it: the same object
       (axl_text_out_lines), told which devices each output reaches. */
    static CHAR16                    name[] = L">e";   /* .data, as above */
    EFI_BOOT_SERVICES               *bs  = st->BootServices;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *err = st->StdErr;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = st->ConOut;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *lg  = NULL;
    EFI_SHELL_PARAMETERS_PROTOCOL   *sp  = NULL;
    EFI_SHELL_PROTOCOL              *sh  = NULL;
    SHELL_FILE_HANDLE                id  = NULL;
    EFI_GUID                        *fan = &gEfiConsoleOutDeviceGuid;
    if (err == NULL || err == out) {
        err = NULL;
    } else {
        out = err;
        fan = &gEfiStandardErrorDeviceGuid;
        if (bs->HandleProtocol(pack_stub_image, &gEfiShellParametersProtocolGuid,
                               (void **)&sp) == EFI_SUCCESS
            && bs->LocateProtocol(&gEfiShellProtocolGuid, NULL, (void **)&sh) == EFI_SUCCESS
            && sh->OpenFileByName(name, &id, EFI_FILE_MODE_WRITE) == EFI_SUCCESS
            && id == sp->StdErr) {
            lg = pack_stub_text_out(bs, &gEfiShellProtocolGuid, NULL);
        }
        if (lg != NULL) {
            lg = pack_stub_text_out(bs, &gEfiSimpleTextOutputProtocolGuid, lg);
        }
        if (lg != NULL) {
            out = lg;
            fan = &gEfiConsoleOutDeviceGuid;
        } else {
            err = NULL;
        }
    }
    if (out != NULL) {
        axl_text_out_lines(st, out, fan, NULL, msg);
    }
    UINTN       count   = 0;
    EFI_HANDLE *handles = NULL;
    if (err != NULL
        && bs->LocateHandleBuffer(ByProtocol, &gEfiStandardErrorDeviceGuid, NULL,
                                  &count, &handles) == EFI_SUCCESS) {
        for (UINTN i = 0; i < count; i++) {
            void *tag = NULL;
            if (bs->HandleProtocol(handles[i], &gEfiConsoleOutDeviceGuid,
                                   &tag) != EFI_SUCCESS
                && bs->HandleProtocol(handles[i], &gEfiSimpleTextOutputProtocolGuid,
                                      (void **)&out) == EFI_SUCCESS) {
                axl_text_out_lines(st, out, NULL, handles[i], msg);
            }
        }
        bs->FreePool(handles);
    }
}

/* Bytes in a device path up to, NOT including, its end node. */
static size_t
pack_stub_dp_size(
    const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    const unsigned char *p = (const unsigned char *)dp;
    size_t               n = 0;
    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *node = (const EFI_DEVICE_PATH_PROTOCOL *)(p + n);
        size_t len = (size_t)node->Length[0] | ((size_t)node->Length[1] << 8);
        if (node->Type == PACK_DP_END_TYPE || len < PACK_DP_END_SIZE) {
            return n;
        }
        n += len;
    }
}

/* A pool copy of a, then b (either may be NULL), with one end node. */
static EFI_DEVICE_PATH_PROTOCOL *
pack_stub_dp_dup(
    EFI_BOOT_SERVICES              *bs,
    const EFI_DEVICE_PATH_PROTOCOL *a,
    const EFI_DEVICE_PATH_PROTOCOL *b)
{
    size_t         na  = (a != NULL) ? pack_stub_dp_size(a) : 0;
    size_t         nb  = (b != NULL) ? pack_stub_dp_size(b) : 0;
    unsigned char *out = NULL;

    if (bs->AllocatePool(EfiBootServicesData, na + nb + PACK_DP_END_SIZE,
                         (void **)&out) != EFI_SUCCESS) {
        return NULL;
    }
    pack_stub_copy(out, a, na);
    pack_stub_copy(out + na, b, nb);
    out[na + nb + 0] = PACK_DP_END_TYPE;
    out[na + nb + 1] = PACK_DP_END_ENTIRE;
    out[na + nb + 2] = PACK_DP_END_SIZE;
    out[na + nb + 3] = 0;
    return (EFI_DEVICE_PATH_PROTOCOL *)out;
}

/* The device path this image was loaded from: the firmware's own record
   (LoadedImageDevicePath), else DevicePath(DeviceHandle) + FilePath as a pool
   copy (*owned set; the caller frees it). *out is NULL with EFI_SUCCESS only
   when the stub itself has no path at all, in which case the raw image would
   have had none either. A path that exists but cannot be copied is
   EFI_OUT_OF_RESOURCES, never a silent NULL: LoadImage is always handed the
   real path when there is one. */
static EFI_STATUS
pack_stub_self_dp(
    EFI_BOOT_SERVICES          *bs,
    EFI_HANDLE                  image,
    EFI_LOADED_IMAGE_PROTOCOL  *li,
    EFI_DEVICE_PATH_PROTOCOL  **out,
    int                        *owned)
{
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;

    *out   = NULL;
    *owned = 0;
    if (bs->HandleProtocol(image, &gEfiLoadedImageDevicePathProtocolGuid,
                           (void **)&dp) == EFI_SUCCESS && dp != NULL) {
        *out = dp;
        return EFI_SUCCESS;
    }

    EFI_DEVICE_PATH_PROTOCOL *dev = NULL;
    if (li->DeviceHandle != NULL) {
        if (bs->HandleProtocol(li->DeviceHandle, &gEfiDevicePathProtocolGuid,
                               (void **)&dev) != EFI_SUCCESS) {
            dev = NULL;
        }
    }
    if (dev == NULL && li->FilePath == NULL) {
        return EFI_SUCCESS;
    }
    dp = pack_stub_dp_dup(bs, dev, li->FilePath);
    if (dp == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    *out   = dp;
    *owned = 1;
    return EFI_SUCCESS;
}

/* Read this image's own file from the volume it was loaded from, find the
   payload by the section table and decode it. PACK_REREAD_UNREADABLE when
   there is no file to read: no DeviceHandle or FilePath (loaded from a
   buffer) or no file system on the device. Otherwise an AxlPackStatus: the
   decode's, or AXL_PACK_E_TRUNCATED when the file could not be opened or
   read whole this time (the media that made the first read bad can fail a
   later one; that is one failed attempt, not the end of the retries). The
   file is untrusted input: axl_pack_find_payload bounds every offset in it.
   Frees everything it allocated but *raw on AXL_PACK_OK. */
static int
pack_stub_reread(
    EFI_BOOT_SERVICES         *bs,
    EFI_LOADED_IMAGE_PROTOCOL *li,
    const AxlPackAlloc        *alloc,
    void                     **raw,
    size_t                    *raw_len)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs  = NULL;
    EFI_FILE_PROTOCOL               *dir  = NULL;
    EFI_FILE_PROTOCOL               *file = NULL;
    unsigned char                   *buf  = NULL;
    UINT64                           info[PACK_FILE_INFO_WORDS];
    UINTN                            isz  = sizeof(info);
    UINT64                           size = 0;
    UINT64                           got  = 0;
    int                              rc   = AXL_PACK_E_TRUNCATED;

    if (li->DeviceHandle == NULL || li->FilePath == NULL
        || bs->HandleProtocol(li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid,
                              (void **)&sfs) != EFI_SUCCESS) {
        return PACK_REREAD_UNREADABLE;
    }

    /* Open the FilePath one node at a time, each relative to the last, as
       EDK2's EfiOpenFileByDevicePath does: a FilePath may split the path over
       several MEDIA_FILEPATH nodes. Each node's PathName is opened in place:
       the firmware's pool copy of the path is aligned and every node of it
       an even length (checked), so the CHAR16s are aligned. Any other node
       type ends the walk with no file. */
    if (sfs->OpenVolume(sfs, &file) != EFI_SUCCESS) {
        return rc;
    }
    for (EFI_DEVICE_PATH_PROTOCOL *node = li->FilePath;
         node->Type != PACK_DP_END_TYPE;) {
        size_t len = (size_t)node->Length[0] | ((size_t)node->Length[1] << 8);
        dir  = file;
        file = NULL;
        if (node->Type != PACK_DP_MEDIA || node->SubType != PACK_DP_MEDIA_FILEPATH
            || len <= PACK_DP_END_SIZE || (len & 1u) != 0
            || dir->Open(dir, &file, (CHAR16 *)(node + 1), EFI_FILE_MODE_READ,
                         0) != EFI_SUCCESS) {
            file = NULL;
        }
        dir->Close(dir);
        if (file == NULL) {
            return rc;
        }
        node = (EFI_DEVICE_PATH_PROTOCOL *)((unsigned char *)node + len);
    }

    /* The size from EFI_FILE_INFO (one call, into the stack buffer above),
       as the firmware's own file loader gets it: every file system
       implements GetInfo, where GetPosition is one a file system can leave
       unsupported (AXL's own AxlFsProvider volumes do). A wrapped file is smaller than the raw image it carries, so the
       format's cap bounds it too. Read in AXL_FS_READ_CHUNK pieces, never in
       one: the firmware's single read of the whole file is the read that
       came back corrupt, and on BMC virtual media large single reads have
       been measured failing where 64 KB ones do not (axl-fs-read-chunk.h).
       A 0-byte read is end of file. */
    if (file->GetInfo(file, &gEfiFileInfoGuid, &isz, info) == EFI_SUCCESS
        && ((EFI_FILE_INFO *)info)->FileSize <= AXL_PACK_MAX_RAW) {
        size = ((EFI_FILE_INFO *)info)->FileSize;
        buf  = alloc->alloc(alloc->ctx, (size_t)size);
    }
    for (UINTN n = 1; buf != NULL && got < size && n != 0; got += n) {
        n = (size - got < AXL_FS_READ_CHUNK) ? (UINTN)(size - got)
                                             : AXL_FS_READ_CHUNK;
        if (file->Read(file, &n, buf + got) != EFI_SUCCESS) {
            break;
        }
    }
    if (buf != NULL && got == size) {
        size_t        off  = 0;
        size_t        plen = 0;
        AxlPackStatus ps   = axl_pack_find_payload(buf, got, &off, &plen, NULL);
        if (ps == AXL_PACK_OK) {
            ps = axl_pack_unpack(buf + off, plen, AXL_PACK_MACHINE_NATIVE,
                                 alloc, raw, raw_len);
        }
        rc = (int)ps;
    }
    alloc->free(alloc->ctx, buf);
    file->Close(file);
    return rc;
}

/* The entry point (the asm CRT0 tail-calls _AxlEntry, defsym'd to this). */
EFI_STATUS EFIAPI
axl_pack_stub_entry(
    EFI_HANDLE        image,
    EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES         *bs = st->BootServices;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS                 status;

    pack_stub_image = image;
    status = bs->HandleProtocol(image, &gEfiLoadedImageProtocolGuid, (void **)&li);
    if (status != EFI_SUCCESS || li == NULL) {
        pack_stub_say(st, "no loaded-image protocol", NULL, status, 1);
        return (status != EFI_SUCCESS) ? status : EFI_LOAD_ERROR;
    }

    /* Decode. The 64 MiB cap and every bound live in the format module. */
    PackStubPool       pool  = { bs, li->ImageDataType };
    const AxlPackAlloc alloc = { pack_stub_alloc, pack_stub_free, &pool };
    void              *raw     = NULL;
    size_t             raw_len = 0;
    const unsigned char *payload     = axl_embedded_pack;
    size_t               payload_len =
        (size_t)((uintptr_t)axl_embedded_pack_end - (uintptr_t)axl_embedded_pack);
#if defined(AXL_PACK_STUB_TEST_CORRUPT_FIRST)
    /* TEST stub only (pack-fixture-rt): the first decode reads a copy with one
       byte in the middle of the LZMA stream flipped, as a bad first read of
       the file would leave it. The shipped stub has no such seam. */
    unsigned char *damaged = pack_stub_alloc(&pool, payload_len);
    if (damaged != NULL && payload_len > AXL_PACK_HDR_SIZE) {
        pack_stub_copy(damaged, payload, payload_len);
        damaged[AXL_PACK_HDR_SIZE + (payload_len - AXL_PACK_HDR_SIZE) / 2] ^= 0xFFu;
        payload = damaged;
    }
#endif
    AxlPackStatus ps = axl_pack_unpack(payload, payload_len,
                                       AXL_PACK_MACHINE_NATIVE, &alloc,
                                       &raw, &raw_len);
#if defined(AXL_PACK_STUB_TEST_CORRUPT_FIRST)
    pack_stub_free(&pool, damaged);
#endif
    /* A bad payload in memory may be a bad READ of a good file: decode the
       file again. The final message stays the in-memory failure's. */
    for (int attempt = 1;
         (ps == AXL_PACK_E_CORRUPT || ps == AXL_PACK_E_HEADER)
         && attempt <= PACK_STUB_REREADS;
         attempt++) {
        /* Writable, so the digit is patched in place -- and in .data, not
           .rodata, like pack_stub_say's own strings. */
        static char tail[] = " -- re-reading from the volume (attempt 0" PACK_REREAD_OF;
        tail[sizeof(tail) - sizeof(PACK_REREAD_OF) - 1] = (char)('0' + attempt);
        pack_stub_say(st, axl_pack_status_str(ps), tail, 0, 0);
        int rc = pack_stub_reread(bs, li, &alloc, &raw, &raw_len);
        if (rc == AXL_PACK_OK) {
            ps = AXL_PACK_OK;
        } else if (rc == PACK_REREAD_UNREADABLE) {
            break;
        }
    }
    if (ps != AXL_PACK_OK) {
        pack_stub_say(st, axl_pack_status_str(ps), NULL, 0, 0);
        return (ps == AXL_PACK_E_NO_MEMORY) ? EFI_OUT_OF_RESOURCES
                                            : EFI_LOAD_ERROR;
    }

    /* Load under the stub's own file identity. LoadImage copies the buffer. */
    int                       dp_owned = 0;
    EFI_DEVICE_PATH_PROTOCOL *dp       = NULL;
    EFI_HANDLE                inner    = NULL;
    status = pack_stub_self_dp(bs, image, li, &dp, &dp_owned);
    if (status != EFI_SUCCESS) {
        bs->FreePool(raw);
        pack_stub_say(st, "cannot copy a device path", NULL, status, 1);
        return status;
    }
    status = bs->LoadImage(FALSE, image, dp, raw, raw_len, &inner);
    bs->FreePool(raw);
    if (dp_owned) {
        bs->FreePool(dp);
    }
    if (status != EFI_SUCCESS) {
        /* UEFI 2.11 LoadImage: EFI_SECURITY_VIOLATION still creates the
           image handle (the image failed authentication but is loaded), and
           the caller must UnloadImage it. Every other failure creates none. */
        if (status == EFI_SECURITY_VIOLATION && inner != NULL) {
            bs->UnloadImage(inner);
        }
        pack_stub_say(st, "LoadImage failed", NULL, status, 1);
        return status;
    }

    EFI_LOADED_IMAGE_PROTOCOL *inner_li = NULL;
    status = bs->HandleProtocol(inner, &gEfiLoadedImageProtocolGuid,
                                (void **)&inner_li);
    if (status != EFI_SUCCESS || inner_li == NULL) {
        pack_stub_say(st, "no loaded-image protocol", NULL, status, 1);
        bs->UnloadImage(inner);
        return (status != EFI_SUCCESS) ? status : EFI_LOAD_ERROR;
    }

    /* A firmware that did not resolve the path leaves these NULL: give the
       inner image the stub's. FilePath is a COPY -- the firmware frees the
       inner image's FilePath when it unloads it. */
    if (inner_li->DeviceHandle == NULL) {
        inner_li->DeviceHandle = li->DeviceHandle;
    }
    if (inner_li->FilePath == NULL && li->FilePath != NULL) {
        inner_li->FilePath = pack_stub_dp_dup(bs, NULL, li->FilePath);
        if (inner_li->FilePath == NULL) {
            pack_stub_say(st, "cannot copy a device path", NULL,
                          EFI_OUT_OF_RESOURCES, 1);
            bs->UnloadImage(inner);
            return EFI_OUT_OF_RESOURCES;
        }
    }
    inner_li->LoadOptions     = li->LoadOptions;
    inner_li->LoadOptionsSize = li->LoadOptionsSize;

    /* The Shell's redirection and argv, carried to the inner image for the
       length of StartImage -- the same install/uninstall the Shell itself
       performs around the stub. */
    void *params = NULL;
    if (bs->HandleProtocol(image, &gEfiShellParametersProtocolGuid,
                           &params) != EFI_SUCCESS) {
        params = NULL;
    }
    if (params != NULL) {
        status = bs->InstallProtocolInterface(&inner, &gEfiShellParametersProtocolGuid,
                                              EFI_NATIVE_INTERFACE, params);
        if (status != EFI_SUCCESS) {
            pack_stub_say(st, "cannot pass shell parameters", NULL, status, 1);
            bs->UnloadImage(inner);
            return status;
        }
    }

    UINTN   exit_size = 0;
    CHAR16 *exit_data = NULL;
    status = bs->StartImage(inner, &exit_size, &exit_data);

    if (params != NULL) {
        bs->UninstallProtocolInterface(inner, &gEfiShellParametersProtocolGuid,
                                       params);
    }

    if (exit_data != NULL) {
        /* Exit hands the inner image's ExitData on to whoever started us. */
        bs->Exit(image, status, exit_size, exit_data);
    }
    return status;
}
