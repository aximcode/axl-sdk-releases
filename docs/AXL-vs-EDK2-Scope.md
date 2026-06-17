# AXL vs EDK2 — scope, gaps, and what we deliberately omit

This is a strategic scoping note, not a roadmap. It records what axl-sdk
**deliberately does not** try to be (so we never apologize for those
omissions), and the gaps that a real user of the stated audience —
**Linux C/C++ developers writing UEFI apps and drivers** — would
actually hit. Tactical backlog items live in
[ROADMAP.md](ROADMAP.md); per-subsystem plans live in their design docs.

## The reframe: two EDK2s, we replace one

EDK2 is two things welded together:

1. A **firmware-construction platform** — build the whole BIOS: the
   DSC/FDF/INF build meta-language, flash-image (FD/FV/FFS) assembly,
   the PEI and DXE cores, the dispatcher and depex resolution, PCD, SMM
   / Standalone MM, EBC.
2. An **application / driver runtime API** — the protocols, the UEFI
   Driver Model, services an image uses once firmware is already up.

axl-sdk competes with **(2) only**. It is an SDK for images that run on
top of existing firmware, not a platform for building firmware. That
means roughly 70% of EDK2's surface is *correctly* absent, and leaving
it out is the product, not a deficiency:

- DSC/FDF/INF + FV/FD/FFS/flash-image assembly, the build meta-language
- PCD (Platform Configuration Database)
- PEI / PEIM, the DXE core + dispatcher, depex
- SMM / MM / Standalone MM, SMI handlers
- EBC (EFI Byte Code)

A Linux developer writing a UEFI app or driver should never touch any of
that. If a user needs to author system-firmware internals, EDK2 (or
coreboot, or a vendor BSP) is the right tool and we should say so.

## What axl already covers (so it isn't on the gap list)

A feature-by-feature checklist understates how complete the runtime
surface already is. axl ships, among much else:

- **Device paths** (`axl-device-path.h`), **UEFI variables**
  (`axl-nvstore.h`: `GetVariable`/`SetVariable`, `BootOrder`,
  `SecureBoot`, OEM vendor namespaces), **boot management**
  (`axl-boot.h`: `Boot####` option get/set/delete, `BootOrder`,
  `BootNext`, `BootCurrent`).
- **Image loading + Authenticode / Secure-Boot signature inspection**
  (`axl-image.h`, `axl-image-verify.h`).
- Device readers: Block IO, Serial IO, FV2, TCG2/TPM, SMBIOS, ACPI, PCI,
  USB, SMBus, IPMI, SPD; a graphics/compositor/TrueType/EDID/cursor
  stack; an HTTP/WebDAV/TLS app stack; RAM disks; MP services.
- The GLib-parity layer — containers, an event loop, async, pubsub, task
  pool — plus a coherent C++ RAII story and a QEMU-in-the-loop test
  harness. **These last two are things EDK2 largely lacks**; they are
  the differentiator, not a gap.

## The gaps that matter for the audience

Ranked by how likely a real user is to hit a wall.

### 1. The full UEFI Driver Model (the real one)
We have Type-A (protocol publisher) and Type-B (`AxlDriverBinding`, one
binding per image). Missing is the part that makes EDK2 indispensable to
driver authors: **demand-driven bus drivers** (creating and managing
child handles), plus Driver Health, Driver Diagnostics, Driver
Configuration, and richer Component Name. This is the single biggest
**functional** gap — anyone porting a NIC, storage controller, or
USB-class driver needs it. Tracked as driver-authoring Phase 4.

### 2. HII (Human Interface Infrastructure)
Forms, IFR, config routing, string/font packages. Nothing config-UI
shaped (BIOS setup pages, driver config forms) works without it.
In progress — see [AXL-HII-Design.md](AXL-HII-Design.md).

### 3. A general crypto / PKI toolkit (our `CryptoPkg` equivalent)
Today: digest/HMAC, TLS (vendored mbedTLS), and Authenticode
*inspection*. Missing is the *toolkit*: verify/sign arbitrary PKCS#7,
build and validate X.509 chains, RSA/ECC key operations, ASN.1. The
mbedTLS primitives are already in-tree for TLS, so this is about
*exposing* a clean crypto API, not vendoring a new dependency. Needed
for any secure-boot / capsule / signing tooling beyond "is this signed?"

### 4. Networking and storage breadth
- **Networking**: IPv6 is explicitly v1-deferred (`axl-net-opts.h`);
  netboot (PXE / TFTP / HTTP-Boot / iSCSI) and a first-class DHCP client
  are absent. We *consume* the firmware's network protocols rather than
  reimplementing a stack, so this is about which protocols we wrap.
- **Storage**: we consume Block IO well but do not *produce* — no
  GPT/MBR partition parsing, no DiskIo / pass-through, no storage/bus
  driver authoring. This loops back to gap (1).
- **Capsule update / FMP** authoring is absent (read-side ESRT is on the
  fixture roadmap). Probably correctly low priority for the audience.

### 5. The host / core backend — the strategic one
This is the gap that most advances the *stated goal*, and it is
positioning, not a single feature. Today "an alternative to EDK2 for
Linux C/C++ developers" really means *"for Linux devs who are targeting
UEFI."* The GLib-parity layer — containers, loop, async, pubsub, task
pool, even much of net — has no intrinsic reason to be UEFI-bound. If
that subset built and ran natively on a Linux host (and tested without
QEMU), the pitch changes from "a nicer UEFI SDK" to **"write the logic
once, run it on host *and* UEFI"** — which is the actual GLib value
proposition and a thing no EDK2 competitor offers. The backend
abstraction (`src/backend/axl-backend.h`, single native impl today)
already makes this a *finishable* job rather than a rewrite. See
[AXL-EFI-Encapsulation-Plan.md](AXL-EFI-Encapsulation-Plan.md) for the
core-platform split (`libaxl-core.a`, optional coreboot/Linux backend).

### 6. Multi-arch breadth (minor)
axl targets X64 + AArch64. EDK2 also covers IA32 (32-bit), ARM (32),
RISC-V 64, and LoongArch. RISC-V is the one with real momentum; the rest
matter to few in the target audience. A one-liner, not a priority.

## How to read this list

The consumer-pull loop (the SoftBMC port) is the real signal for which
gap bites first — so far it has pulled toward HII and dashboard
substrate, which lines up with (1)/(2). This note is assessed from the
public headers, the roadmap, and that consumer signal; it is not a
claim that every shipped module has been stress-tested against a real
driver port. Update it as the consumer pulls reorder the priorities.
