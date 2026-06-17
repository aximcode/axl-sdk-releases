Firmware-volume enumeration via `EFI_FIRMWARE_VOLUME2_PROTOCOL`.

Header: `<axl/axl-fv.h>`. A read-only inventory probe over the
firmware's Firmware Volume 2 handles — the FFS containers the
platform's DXE drivers and other firmware files live in. It reports
each volume's access attributes and file count; it does not read file
contents or sections.

Lazy on first call: AxlFv locates the FV2 handles once with
`LocateHandleBuffer` and caches the set for the image lifetime. On
platforms publishing no firmware volumes every call returns NULL /
`AXL_ERR` cleanly.

Cursor-style enumeration matches `axl_block_next` / `axl_serial_next`
and returns the firmware `AxlHandle` directly, so position is recovered
from the handle you pass back — no hidden shared cursor:

```c
AxlHandle h = NULL;
while ((h = axl_fv_next(h)) != NULL) {
    AxlFvAttributes a;
    size_t files = 0;
    if (axl_fv_get_attributes(h, &a) == AXL_OK
        && axl_fv_count_files(h, &files) == AXL_OK) {
        axl_printf("FV: %s%s%s, %zu files\n",
                   a.readable ? "r" : "-",
                   a.writable ? "w" : "-",
                   a.locked   ? "L" : "-", files);
    }
}
```

Device-path text needs no extra API: the same `AxlHandle` resolves
through the existing `axl_handle_get_protocol(h, "device-path", ...)`
+ `axl_device_path_to_text()` (both in `<axl/axl-sys.h>`).

`AxlFvAttributes` reports the *current effective* read/write/lock
state (not the volume's capabilities). `axl_fv_count_files` is an
O(files) walk of the volume's `GetNextFile` enumeration, not a cached
field — cache it if you serve it repeatedly.
