NVMe device identity + health (SMART) via `EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL`.

Header: `<axl/axl-nvme.h>`. A Platform Access module (like AxlPci /
AxlUsb / AxlBlock): it enumerates the NVMe controllers the firmware
exposes and reports each one's Identify data, namespaces, and
SMART/Health log — the device view that complements AxlBlock's logical
block geometry. It is the NVMe arm of the storage-access family
(AxlNvme / AxlAta / AxlScsi) described in `docs/AXL-Storage-Design.md`.

Scope is read-and-health: Identify (Controller + Namespace), the
SMART/Health log (Get Log Page 0x02), and Device Self-test (start +
poll) — the one active command, and non-destructive. Arbitrary admin
commands go through `axl_nvme_admin_passthru()`; the typed surface ships
no data-destroying command (Format NVM, Sanitize, firmware download).

A controller is the unit of enumeration because the pass-thru protocol,
the Identify Controller data, and the SMART log are all controller-wide
(the SMART log is read with NSID 0xFFFFFFFF). Namespaces — the
addressable capacities, usually one per SSD — are walked within a
controller with `axl_nvme_namespace_next`.

```c
AxlHandle ctrl = NULL;
while ((ctrl = axl_nvme_next(ctrl)) != NULL) {
    AxlNvmeController c;
    AxlNvmeSmart      s;
    if (axl_nvme_identify_controller(ctrl, &c) == AXL_OK
        && axl_nvme_smart(ctrl, &s) == AXL_OK) {
        axl_printf("%s %s: %s, %d C, %u%% used\n",
                   c.model, c.serial, s.healthy ? "OK" : "FAILING",
                   s.temperature_c, s.percent_used);
    }
}
```

The typed readers are each "pass-thru read the buffer" + a pure decode
function (`axl_nvme_decode_*`). The decoders are public in their own
right — a caller holding a raw Identify / SMART / self-test buffer it
obtained another way (a captured fixture blob, a buffer over Redfish)
can decode it without a device — and they are the hardware-free unit-test
seam. `tools/nvme` is the dogfood renderer; `tools/mkfixture` consumes
the same Identify path for hardware-fixture capture.

Internals: `axl-nvme.c` (enumeration + admin pass-thru over the EFI
protocol; bounces transfer buffers through an `IoAlign`-satisfying
allocation) and `axl-nvme-decode.c` (the pure, UEFI-free decoders).
Unit tests in `test/unit/axl-test-nvme.c` exercise the decoders against
spec-faithful buffers; `test/integration/test-nvme-qemu.sh` exercises
the device path against an emulated `-device nvme`.
