TPM 2.0 access via `EFI_TCG2_PROTOCOL`: presence + capability readout, the
Endorsement Key public part, and PCR-bound seal/unseal.

Header: `<axl/axl-tpm.h>`. The TCG2 protocol is a singleton, so there is
nothing to enumerate. The read-only surface is a presence check plus one
typed capability struct; beyond that the module drives raw TPM2 commands
over `SubmitCommand` for the Endorsement Key and for sealing a secret to
the platform's measured-boot state (see below). Measurement (PCR extend)
and the event log remain out of scope — sealing *reads* PCRs into a
policy, it does not extend them.

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

## Endorsement Key + PCR-bound seal/unseal

Beyond the read-only capability surface, the module also drives raw TPM2
commands over `EFI_TCG2_PROTOCOL.SubmitCommand`:

- `axl_tpm_read_ek_pub` derives the Endorsement Key public part (a stable
  per-device identity) via `TPM2_CreatePrimary` in the endorsement
  hierarchy.
- `axl_tpm_seal` / `axl_tpm_unseal` seal a small secret (e.g. a TLS
  private key) to the chosen SHA-256 PCRs and recover it only when the
  live PCRs reproduce the seal-time measured state. The chain is
  `CreatePrimary` (a deterministic ECC SRK) -> `PCR_Read` (the PolicyPCR
  digest is computed in software) -> `Create` (a keyedhash sealed object
  whose authPolicy is that digest); unseal runs `Load` ->
  `StartAuthSession` -> `PolicyPCR` -> `Unseal`. The blob `axl_tpm_seal`
  returns is opaque ciphertext the caller persists; `axl_tpm_unseal`
  returns `AXL_DENIED` if the measured state changed. Uses empty
  owner/parent authorization (the common firmware default); cross-boot
  unseal also needs a stable owner primary seed (no `TPM2_Clear` between).

The raw-command paths are validated against `swtpm` in QEMU
(`test/integration/test-tpm-qemu.sh`, `test-tpm-seal-qemu.sh`); the
absent path is covered on both arches by the unit suite.
