TPM 2.0 presence and capability via `EFI_TCG2_PROTOCOL`.

Header: `<axl/axl-tpm.h>`. Unlike the other platform readers there is
nothing to enumerate — the TCG2 protocol is a singleton — so this is a
presence check plus one typed capability struct. Scope is the
boot-service capability fields a diagnostic/inventory view reports;
measurement, the event log, and PCR extension are out of scope.

The protocol is located lazily and cached (like the CPU-arch /
MP-services helpers). `axl_tpm_present()` reports whether the TCG2
protocol is published; `axl_tpm_get_capability()` calls GetCapability
and projects `EFI_TCG2_BOOT_SERVICE_CAPABILITY` into `AxlTpmCapability`.

```c
if (axl_tpm_present()) {
    AxlTpmCapability cap;
    if (axl_tpm_get_capability(&cap) == AXL_OK && cap.present) {
        axl_printf("TPM mfr=0x%08x banks=%u active=0x%x\n",
                   cap.manufacturer_id, cap.number_of_pcr_banks,
                   cap.active_pcr_banks);
    }
}
```

Two presence concepts: `axl_tpm_present()` is "the TCG2 *protocol* is
published" (a stack is available to query); `AxlTpmCapability.present`
is the firmware's TPMPresentFlag ("a chip is installed and
responding"). When the protocol is absent `axl_tpm_get_capability`
returns `AXL_ERR` and the consumer reports the TPM as not present (the
QEMU-default golden, `{"tpm":{"present":false}}`).

`active_pcr_banks` is a hash-algorithm *bitmask* (`EFI_TCG2_BOOT_HASH_ALG_*`,
consumer decodes names); `number_of_pcr_banks` is a *count*. Both are
valid only when the capability structure version is >= 1.1.
