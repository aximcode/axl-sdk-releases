Block-device enumeration via `EFI_BLOCK_IO_PROTOCOL`.

Header: `<axl/axl-block.h>`. This is the low-level counterpart to
`<axl/axl-fs.h>`: AxlFs gives path-based file access over mounted
volumes, while AxlBlock enumerates the underlying raw block devices
(disks, partitions, CD-ROMs, RAM disks) and reports each one's media
descriptor — geometry and media state. Scope is enumeration + the
media readout; block read/write is out of scope (use AxlFs for data).

Lazy on first call: AxlBlock locates the firmware-installed block-I/O
handles once with `LocateHandleBuffer` and caches the set for the
image lifetime. On platforms with no block devices every call returns
NULL / `AXL_ERR` cleanly.

Cursor-style enumeration matches `axl_usb_next` / `axl_acpi_find_next`,
but returns the firmware `AxlHandle` directly so position is recovered
from the handle you pass back — there is no hidden shared cursor, so
independent (even nested) walks over the cached set don't interfere:

```c
AxlHandle h = NULL;
while ((h = axl_block_next(h)) != NULL) {
    AxlBlockMedia m;
    if (axl_block_get_media(h, &m) == AXL_OK && m.media_present) {
        uint64_t capacity = (m.last_block + 1) * m.block_size;
        axl_printf("block dev: %u-byte sectors, %llu bytes%s\n",
                   m.block_size, (unsigned long long)capacity,
                   m.read_only ? " (read-only)" : "");
    }
}
```

Device-path text needs no extra API: the same `AxlHandle` resolves
through the existing `axl_handle_get_protocol(h, "device-path", ...)`
+ `axl_device_path_to_text()` (both in `<axl/axl-sys.h>`).

Fields in `AxlBlockMedia` are raw readouts of `EFI_BLOCK_IO_MEDIA`;
the caller derives presentation values (capacity, device type). The
geometry fields (`block_size`, `last_block`) are meaningful only when
`media_present` is true — an empty removable slot may report stale or
zero values.
