Serial-port enumeration via `EFI_SERIAL_IO_PROTOCOL`.

Header: `<axl/axl-serial.h>`. Two layers over `EFI_SERIAL_IO_PROTOCOL`:
a read-only descriptor probe (enumerate handles, read each port's line
settings — baud, framing, timeout, FIFO depth — and modem
control/status lines), and **byte I/O** for moving raw bytes over a
chosen port. For *console* byte streams use `<axl/axl-stream.h>`; this
is the lower-level per-port path (e.g. talking to a BMC's SOL / a
device on a specific UART).

Lazy on first call: AxlSerial locates the serial-I/O handles once with
`LocateHandleBuffer` and caches the set for the image lifetime. On
platforms with no serial ports every call returns NULL / `AXL_ERR`
cleanly.

Cursor-style enumeration matches `axl_block_next` / `axl_usb_next` and
returns the firmware `AxlHandle` directly, so position is recovered
from the handle you pass back — no hidden shared cursor:

```c
AxlHandle h = NULL;
while ((h = axl_serial_next(h)) != NULL) {
    AxlSerialMode m;
    AxlSerialControl c;
    if (axl_serial_get_mode(h, &m) == AXL_OK) {
        axl_printf("Uart(%u,%u,parity=%u,stop=%u)\n",
                   m.baud_rate, m.data_bits, m.parity, m.stop_bits);
    }
    if (axl_serial_get_control(h, &c) == AXL_OK && c.cts) {
        axl_printf("  CTS asserted\n");
    }
}
```

## Byte I/O

To move bytes over a port, open an `AxlSerial` on one of the enumerated
handles, optionally set the line mode, then read/write:

```c
AxlSerial *s = NULL;
axl_serial_open(h, &s);                      // h from axl_serial_next
axl_serial_set_mode(s, &(AxlSerialMode){ .baud_rate = 115200,
                                         .data_bits = 8 });
size_t written = 0, got = 0;
axl_serial_write(s, "AT\r\n", 4, &written);
axl_serial_read(s, buf, sizeof buf, &got);   // non-blocking; got may be 0
axl_serial_close(s);
```

### One UART, one owner

`axl_serial_open` is **exclusive**: while this image holds a port, a second
open of the same handle returns `AXL_BUSY` and leaves the out-param NULL.

That is not fussiness about interleaved output. The wrapper is a thin skin
over one `EFI_SERIAL_IO_PROTOCOL*`, so two subsystems each holding "a port"
that is the same UART share firmware state — and whichever one calls
`axl_driver_disconnect_handle` first detaches the port's drivers underneath a
protocol pointer the other is still writing through. That is a
use-after-detach, and before this it happened silently.

Sharing is available, but has to be asked for by name:

```c
if (axl_serial_open(h, &s) == AXL_BUSY) { /* someone else owns it */ }

axl_serial_open_shared(h, &s);   // coexists with other SHARED opens only
bool taken = axl_serial_is_open(h);
```

Sharing is all-or-nothing per port, decided by the first open: an exclusive
holder refuses everyone, and a shared holder refuses an exclusive claim.
Prefer acting on `AXL_BUSY` over asking `axl_serial_is_open` first — a check
followed by an open is two steps, and the answer can go stale between them.

**Scope is this image.** The claim lives in the library's static state, so it
cannot see a port another loaded image has open. It resolves collisions
between subsystems of one program, which is where they are both most likely
and least visible.

`axl_serial_handle(s)` returns the handle a port was opened from, so the
handle-taking queries (`axl_serial_get_mode`, `axl_serial_get_control`) are
reachable from an open port alone — which a read-modify-write of one line
setting needs, since `axl_serial_set_mode` takes the whole struct and reads a
zero field as "device default".

`EFI_SERIAL_IO` exposes no receive event, so for loop-driven input
`axl_serial_read_async(s, loop, poll_ms, cb, user)` registers a timer
that drains the port each tick and calls `cb` with whatever arrived
(pick `poll_ms` to suit the UART rate; 5-10 ms for an interactive
console). One async receive per port; `axl_serial_close` removes the
source.

Device-path text needs no extra API: the same `AxlHandle` resolves
through the existing `axl_handle_get_protocol(h, "device-path", ...)`
+ `axl_device_path_to_text()` (both in `<axl/axl-sys.h>`).

`parity` and `stop_bits` are raw enum codes the consumer names (e.g.
`"N"`, `"8N1"`); `baud_rate` is the firmware's `UINT64` BaudRate
narrowed to 32 bits (every real UART rate fits).
