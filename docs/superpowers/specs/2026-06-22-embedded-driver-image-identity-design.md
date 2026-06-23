# Design: embedded-driver loaded-image identity + app-dir-restricted driver load

**Date:** 2026-06-22
**Status:** approved (brainstorming) — pending spec review → implementation plan
**Consumer:** delldiags / axl-utils (`do.efi`, the only consumer of the
shared-driver embedded-load path)

## Problem

`axl_driver_load_buffer()` loads an embedded driver from a memory buffer with
`gBS->LoadImage(BootPolicy=FALSE, parent, DevicePath=NULL, SourceBuffer=buf,
len, &handle)`. With `DevicePath == NULL` the firmware leaves:

- `EFI_LOADED_IMAGE_PROTOCOL.FilePath = NULL`, and
- the `gEfiLoadedImageDevicePathProtocol` interface installed on the handle
  with a **NULL** pointer.

The x64 UEFI shell tolerates these (prints `<null string>`); the **aarch64**
shell dereferences/renders the device-path pointer and faults with a
`Synchronous Exception` when inspecting the handle (e.g. `dh -p <handle>` /
`dh -v -p <guid>`). The on-disk load path (`axl_driver_load(path)`) already
builds a real `MEDIA_FILEPATH` device path (`driver_build_file_dp`,
`src/util/axl-driver.c:92`), so only the embedded/buffer path is affected.

Two features address this:

- **A (axl-utils request):** give buffer/embedded loads a real, caller-settable
  loaded-image identity so no NULL device-path/string fields remain, plus
  richer verbose logging.
- **B (axl-sdk owner request):** a new explicit API to load a driver **only**
  from the running app's own directory, with device-path values derived from
  the app's path (never NULL).

## Goals / non-goals

**Goals**
- After loading an embedded driver, the handle has a **non-NULL, renderable**
  `LoadedImage->FilePath` *and* `gEfiLoadedImageDevicePathProtocol` interface,
  so the aa64 shell's `dh -p` / `dh -v` never faults on it.
- Caller can fully populate the loaded-image identity (file name, device
  handle, optional vendor GUID prefix).
- Existing callers (incl. `do.efi`'s current `axl_shared_driver_locate(...,
  "doDriver.efi", ...)`) get the non-NULL fix with **no source change**.
- New `axl_driver_load_sibling()` loads a driver restricted to the app's
  directory, refusing any name that could escape it.

**Non-goals (YAGNI)**
- No change to the on-disk `axl_driver_load()` contract.
- No new *public* device-path helpers — the `MemoryMapped` node builder stays
  internal.
- No attempt to set firmware-owned fields (`ImageBase`/`ImageSize` are read,
  not written).

## API contract

### New public type — `include/axl/axl-driver.h`
```c
typedef struct {
    const char    *file_name;     ///< MEDIA_FILEPATH leaf, e.g. "doDriver.efi".
                                  ///<   NULL -> derived: driver_filename, else the app basename.
    AxlHandle      device_handle; ///< optional; sets LoadedImage->DeviceHandle. NULL -> unchanged.
    const AxlGuid *vendor_guid;   ///< optional Vendor() node prepended to the path. NULL -> omitted.
} AxlEmbeddedImageInfo;
```

### Feature A — non-NULL identity for buffer/embedded loads
New symbols (additive — house `_with_<thing>` style, matching
`axl_shared_driver_locate_with_load_options`):

```c
int axl_driver_load_buffer_with_image_info(
    const unsigned char        *buf,
    size_t                      len,
    const AxlEmbeddedImageInfo *info,        // NULL -> defaults (never NULL path)
    AxlDriverHandle            *out_handle);

int axl_shared_driver_locate_with_image_info(
    const char                 *name,
    const char                 *driver_filename,
    const unsigned char        *embed_blob,
    size_t                      embed_len,
    const void                 *load_options,      // may be NULL (mirrors _with_load_options)
    size_t                      load_options_size,
    const AxlEmbeddedImageInfo *info,              // may be NULL
    void                      **out_iface);
```

Upgraded (no signature change) — these route through the new synthesis with
`info = NULL`, so they stop producing a NULL device path:
- `axl_driver_load_buffer(buf, len, &h)`
- `axl_shared_driver_locate(name, driver_filename, ...)`
- `axl_shared_driver_locate_with_load_options(...)`

Default `file_name` resolution when `info == NULL` or `info->file_name == NULL`:
`driver_filename` (already threaded to the shared-driver layer) → else the app
basename from `axl_app_image_path()` → else the literal `"driver.efi"`.

### Feature B — `include/axl/axl-driver.h`
```c
int axl_driver_load_sibling(
    const char      *file_name,   ///< bare basename only; no '/', '\\', or ':'
    AxlDriverHandle *out_handle);
```
- Rejects `file_name` containing `/`, `\`, or `:` -> `AXL_INVALID` (cannot
  escape the app directory).
- Derives the app directory from `axl_app_image_path()` (strip the basename),
  builds `<app-dir>\<file_name>`, and calls the existing on-disk
  `axl_driver_load()` -> real `MEDIA_FILEPATH` device path (non-NULL identity
  for free).
- `AXL_ERR` if the app has no filesystem image path (network / RAM-disk boot);
  `AXL_NOT_FOUND` if the file is not present in the app directory.

## Identity synthesis (internal)

In `driver_load_embedded` (`src/util/axl-driver.c:997`), after a successful
`LoadImage`:

1. `HandleProtocol(drv, &EFI_LOADED_IMAGE_PROTOCOL_GUID, &li)` -> read
   `li->ImageBase`, `li->ImageSize`.
2. Build the device path:
   `[Vendor(info->vendor_guid)?] + MemoryMapped(EfiBootServicesCode, ImageBase,
   ImageBase + ImageSize) + MEDIA_FILEPATH("\<file_name>") + END`.
   - `MemoryMapped` node: HARDWARE_DEVICE_PATH (0x01) / HW_MEMMAP_DP (0x03),
     length 24: `MemoryType(u32) StartingAddress(u64) EndingAddress(u64)`.
   - `MEDIA_FILEPATH` node reuses the builder pattern at `axl-driver.c:106`.
3. `Reinstall`/`InstallProtocolInterface(drv,
   &gEfiLoadedImageDevicePathProtocolGuid, full_path)` — reinstall over the
   firmware's NULL interface; fall back to install if no interface present.
4. Set `li->FilePath` = the `MEDIA_FILEPATH` portion (file node(s) + END);
   set `li->DeviceHandle = info->device_handle` if the caller supplied one.
5. **Non-fatal:** any allocation / protocol failure in steps 1-4 logs a warning
   and leaves the driver loaded with whatever identity it had — the driver
   still works; only `dh -v` cosmetics degrade. (Mirrors the existing
   best-effort posture of `driver_build_file_dp`.)

Threading: identity flows
`axl_shared_driver_locate_with_image_info` -> `axl_driver_ensure_with_embedded`
-> `driver_load_embedded` -> synthesis (internal signatures change freely).

## Logging

Replace `[INFO] driver ensure: loaded '<embedded>'` with a single INFO line:

```
[INFO] driver ensure: loaded 'doDriver.efi'
       handle=0x1DFAC318 guid=7600E4D1-BA50-0D53-B176-D6385FDA84D4
       base=0x1DDD1000 size=0x66000
       path=MemoryMapped(0xB,0x1DDD1000,0x1DE37000)/\doDriver.efi
```

Path text via `axl_device_path_to_text`. If synthesis was skipped (failure
path), the line still prints name/handle/guid/base/size with `path=<none>`.

## Testing (test-first; bug-fix bucket D + new-API bucket A)

**Unit (`test/unit/axl-test-driver.c`), safe negatives + exact strings:**
- `axl_driver_load_buffer*` NULL/zero arg validation -> `AXL_ERR`.
- `axl_driver_load_sibling` rejects names containing `/`, `\`, `:` ->
  `AXL_INVALID`; NULL args -> `AXL_ERR`.
- Device-path builder: feed known `ImageBase`/`ImageSize`/`file_name`
  (+ optional `vendor_guid`) and assert the **exact**
  `axl_device_path_to_text` output, e.g.
  `MemoryMapped(0xB,0x1DDD1000,0x1DE37000)/\doDriver.efi`
  (`axl_strcmp(... ) == 0`, never substring). This is the fast regression
  guard on the synthesized path; hazard-free (pure construction).

**Integration (`test/integration/test-*-qemu.sh`), both arches, RED-first:**
- Boot an app that embeds a benign driver, load it via the shared-driver path
  (publishing a protocol with a known GUID), then from `startup.nsh` run the
  handle/protocol inspection the consumer uses — both `dh -p <guid>` and
  `dh -v -p <guid>` — against the loaded driver.
- Assert the serial log: **does NOT** contain `Synchronous Exception`, and
  **does** show a non-NULL `FilePath` / `ImageDevicePath` with the expected
  text.
- **RED-first:** confirm current code reproduces the aa64 `dh -v` fault before
  implementing; then GREEN after the fix.
- Hazard note (`feedback_uefi_firmware_test_hazards`): this is the positive
  round-trip with a self-contained driver we control (the safe direction);
  run the single binary first to confirm it prints its Results footer before
  the full suite.

**Acceptance (the consumer's verify):** on aa64 host-tools run-qemu.sh, after
loading an embedded driver, `dh -p <guid>` / `dh -v -p <guid>` must not raise a
`Synchronous Exception` and must show the FilePath + non-NULL ImageDevicePath;
x64 still works, now showing the real path instead of `<null>`. The new verbose
log line shows handle + GUID + base/size + path.

## Files touched

- `include/axl/axl-driver.h` — `AxlEmbeddedImageInfo`,
  `axl_driver_load_buffer_with_image_info`, `axl_driver_load_sibling` decls +
  docstrings.
- `include/axl/axl-shared-driver.h` —
  `axl_shared_driver_locate_with_image_info` decl + docstring.
- `src/util/axl-driver.c` — synthesis, MemoryMapped node builder, threading,
  logging, sibling loader.
- `src/util/axl-shared-driver.c` — image-info threading.
- `test/unit/axl-test-driver.c` — unit assertions.
- `test/integration/test-driver-dh-qemu.sh` (new) — aa64/x64 `dh` guard.
- `src/util/README.md` — document the new APIs (doc-sync per CLAUDE.md).
- `CHANGELOG.md` — Unreleased "Added" (the two APIs) + "Fixed" (aa64 `dh`
  fault on embedded drivers).

## Open questions

None blocking. Default `file_name` precedence (driver_filename -> app basename
-> "driver.efi") and non-fatal synthesis failure were confirmed in
brainstorming.
