/** @file smbus-hc-shim.c
    DXE driver that publishes EFI_I2C_MASTER_PROTOCOL on top of the
    QEMU q35 ICH9 SMBus host controller (PCI 8086:2930).

    Built for test-ipmi-ssif-qemu.sh: stock OVMF ships no producer of
    EFI_SMBUS_HC_PROTOCOL or EFI_I2C_MASTER_PROTOCOL for the emulated
    SMBus controller, so AxlSmbus's backend can't reach QEMU's
    smbus-ipmi device. This shim plugs that hole from the guest side
    without rebuilding OVMF.

    Intentionally publishes only EFI_I2C_MASTER_PROTOCOL (not SMBus
    HC): AxlSmbus probes HC first, and if we published both the SMBus
    HC path would win — but the B1-fixed framing (and the real
    Dell/Grace firmware path) is the I2C Master fallback. So this
    shim forces the fallback so the test exercises the code path that
    actually matters.

    x86-only. ICH9 is an Intel chipset — on aa64/arm there is no
    equivalent, and the PCI config ports (0xCF8/0xCFC) don't exist.
    DriverEntry declines with EFI_UNSUPPORTED on non-x86.

    Register model follows QEMU's hw/i2c/pm_smbus.c (and the
    Intel 82801I datasheet). Block transfer uses PROT_BLOCK_DATA + the
    AUX_BLK bit so the hardware buffers all payload bytes and delivers
    a single completion.

    Build:  (automatic via Makefile SmbusHcShim target)
    Load:   load SmbusHcShim.efi
    Use:    any AxlSmbus consumer will now find I2C Master
**/

#include <axl.h>
#include <uefi/axl-uefi.h>

AXL_LOG_DOMAIN("smbus-hc-shim");

// ---------------------------------------------------------------------------
// x86 port I/O (the shim is x86-only; arm builds stub out DriverEntry)
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(__i386__)

static inline uint8_t
inb(uint16_t port)
{
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint16_t
inw(uint16_t port)
{
    uint16_t v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t
inl(uint16_t port)
{
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void
outb(uint16_t port, uint8_t v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline void
outw(uint16_t port, uint16_t v)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline void
outl(uint16_t port, uint32_t v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

// ---------------------------------------------------------------------------
// PCI config-space access via CF8/CFC (Type-1 mechanism, standard on x86)
// ---------------------------------------------------------------------------

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static uint32_t
pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)func <<  8)
         | ((uint32_t)reg  &  0xFC);
}

static uint32_t
pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, reg));
    return inl(PCI_CONFIG_DATA);
}

static uint16_t
pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, reg));
    return inw(PCI_CONFIG_DATA + (reg & 2));
}

static uint8_t
pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, reg));
    return inb(PCI_CONFIG_DATA + (reg & 3));
}

static void
pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg,
                uint16_t val)
{
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, reg));
    outw(PCI_CONFIG_DATA + (reg & 2), val);
}

static void
pci_cfg_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg,
               uint8_t val)
{
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, reg));
    outb(PCI_CONFIG_DATA + (reg & 3), val);
}

// ---------------------------------------------------------------------------
// ICH9 SMBus PCI and register layout
// ---------------------------------------------------------------------------

#define ICH9_VENDOR  0x8086
#define ICH9_DEVICE  0x2930   // ICH9 SMBus controller

// PCI config offsets
#define PCI_CMD      0x04     // Command register (16-bit)
#define PCI_BAR4     0x20     // BAR4 holds the SMB I/O base
#define ICH9_HOSTC   0x40     // Host Configuration (8-bit)

#define CMD_IOSE     (1 << 0) // I/O Space Enable
#define HOSTC_HST_EN (1 << 0) // Host interface enable

// SMB I/O register offsets (from SMB base)
#define SMBHSTSTS    0x00
#define SMBHSTCNT    0x02
#define SMBHSTCMD    0x03
#define SMBHSTADD    0x04
#define SMBHSTDAT0   0x05
#define SMBHSTDAT1   0x06
#define SMBBLKDAT    0x07
#define SMBAUXCTL    0x0D

// SMBHSTSTS bits
#define STS_HOST_BUSY  (1 << 0)
#define STS_INTR       (1 << 1)
#define STS_DEV_ERR    (1 << 2)
#define STS_BUS_ERR    (1 << 3)
#define STS_FAILED     (1 << 4)
#define STS_BYTE_DONE  (1 << 7)
#define STS_ERR_MASK   (STS_DEV_ERR | STS_BUS_ERR | STS_FAILED)
#define STS_ALL        0xFF    // write-1-to-clear

// SMBHSTCNT bits
#define CTL_START      (1 << 6)

// Protocols (in bits 4:2 of SMBHSTCNT)
#define PROT_BLOCK_DATA  5

// SMBAUXCTL bits
#define AUX_BLK        (1 << 1)

#define SMBUS_BLOCK_MAX  32

// ---------------------------------------------------------------------------
// Shim state
// ---------------------------------------------------------------------------

static uint16_t                 mSmbBase;
static EFI_HANDLE               mHandle;
static EFI_I2C_MASTER_PROTOCOL  mProtocol;

// ---------------------------------------------------------------------------
// PCI scan for ICH9 SMBus
// ---------------------------------------------------------------------------

static int
find_ich9_smbus(void)
{
    //
    // On q35 the SMBus function lives at 00:1F.3, but scan defensively
    // — newer QEMU machines or real Intel boards may place it
    // elsewhere.
    //
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t vid_did = pci_cfg_read32(0, dev, func, 0x00);
            if ((vid_did & 0xFFFF) != ICH9_VENDOR) continue;
            if ((vid_did >> 16)    != ICH9_DEVICE) continue;

            uint32_t bar4 = pci_cfg_read32(0, dev, func, PCI_BAR4);
            if ((bar4 & 0x1) == 0) {
                axl_warning("ICH9 SMBus BAR4 is not I/O space (0x%x)",
                            (unsigned)bar4);
                return -1;
            }
            mSmbBase = (uint16_t)(bar4 & 0xFFFE);

            //
            // Ensure the controller is enabled:
            //   - I/O space decode in the command register
            //   - host interface bit in HOSTC. OVMF leaves this off
            //     on the q35 profile; setting it makes the SMBus
            //     registers usable.
            //
            uint16_t cmd = pci_cfg_read16(0, dev, func, PCI_CMD);
            if ((cmd & CMD_IOSE) == 0) {
                pci_cfg_write16(0, dev, func, PCI_CMD,
                                (uint16_t)(cmd | CMD_IOSE));
            }
            uint8_t hostc = pci_cfg_read8(0, dev, func, ICH9_HOSTC);
            if ((hostc & HOSTC_HST_EN) == 0) {
                pci_cfg_write8(0, dev, func, ICH9_HOSTC,
                               (uint8_t)(hostc | HOSTC_HST_EN));
            }

            axl_info("ICH9 SMBus found at %02u:%02u.%u, SMB I/O base=0x%04x",
                     (unsigned)0, (unsigned)dev, (unsigned)func,
                     (unsigned)mSmbBase);
            return 0;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// SMBus block transfer primitives
// ---------------------------------------------------------------------------

static EFI_STATUS
smb_wait_ready(void)
{
    //
    // Cap the wait at ~1s (1000 × 1ms). QEMU responds in microseconds;
    // real hardware within a few ms at most.
    //
    for (size_t i = 0; i < 1000; i++) {
        uint8_t sts = inb(mSmbBase + SMBHSTSTS);
        if ((sts & STS_HOST_BUSY) == 0) {
            //
            // Clear any stale status bits (write-1-to-clear) so a
            // previous abort doesn't bleed into the next op.
            //
            outb(mSmbBase + SMBHSTSTS, STS_ALL);
            return EFI_SUCCESS;
        }
        gBS->Stall(1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS
smb_run_and_wait(uint8_t ctl)
{
    outb(mSmbBase + SMBHSTCNT, ctl);
    for (size_t i = 0; i < 1000; i++) {
        uint8_t sts = inb(mSmbBase + SMBHSTSTS);
        if (sts & STS_INTR) {
            outb(mSmbBase + SMBHSTSTS, STS_INTR);
            return EFI_SUCCESS;
        }
        if (sts & STS_ERR_MASK) {
            outb(mSmbBase + SMBHSTSTS, STS_ERR_MASK);
            axl_debug("SMBus op error status=0x%02x", (unsigned)sts);
            return EFI_DEVICE_ERROR;
        }
        gBS->Stall(1000);
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS
smb_block_write(uint8_t slave, uint8_t cmd, const uint8_t *buf, size_t len)
{
    if (len == 0 || len > SMBUS_BLOCK_MAX) {
        return EFI_INVALID_PARAMETER;
    }
    EFI_STATUS s = smb_wait_ready();
    if (EFI_ERROR(s)) return s;

    outb(mSmbBase + SMBHSTADD,  (uint8_t)(slave << 1));        // R/W=0
    outb(mSmbBase + SMBHSTCMD,  cmd);
    outb(mSmbBase + SMBHSTDAT0, (uint8_t)len);
    outb(mSmbBase + SMBAUXCTL,  AUX_BLK);
    for (size_t i = 0; i < len; i++) {
        outb(mSmbBase + SMBBLKDAT, buf[i]);
    }
    return smb_run_and_wait((PROT_BLOCK_DATA << 2) | CTL_START);
}

static EFI_STATUS
smb_block_read(uint8_t slave, uint8_t cmd, uint8_t *buf, size_t cap,
               size_t *out_len)
{
    if (cap == 0) return EFI_INVALID_PARAMETER;

    EFI_STATUS s = smb_wait_ready();
    if (EFI_ERROR(s)) return s;

    outb(mSmbBase + SMBHSTADD, (uint8_t)((slave << 1) | 1));   // R/W=1
    outb(mSmbBase + SMBHSTCMD, cmd);
    outb(mSmbBase + SMBAUXCTL, AUX_BLK);

    s = smb_run_and_wait((PROT_BLOCK_DATA << 2) | CTL_START);
    if (EFI_ERROR(s)) return s;

    uint8_t n = inb(mSmbBase + SMBHSTDAT0);
    if (n > SMBUS_BLOCK_MAX) n = SMBUS_BLOCK_MAX;
    if (n > cap)             n = (uint8_t)cap;
    for (size_t i = 0; i < n; i++) {
        buf[i] = inb(mSmbBase + SMBBLKDAT);
    }
    *out_len = n;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_I2C_MASTER_PROTOCOL implementation
//
// AxlSmbus's I2C path forms exactly two packet shapes
// (see src/smbus/axl-smbus-i2c.c):
//   - block write: 1 op, write-only, buffer = [cmd][count][data...]
//   - block read:  2 ops, op[0] writes the cmd byte, op[1] reads
//                  into a buffer whose first byte receives the count
//                  and whose remainder receives the payload.
// Anything else → EFI_UNSUPPORTED.
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
i2c_set_bus_frequency(const EFI_I2C_MASTER_PROTOCOL *This,
                      UINTN                         *BusClockHertz)
{
    (void)This;
    (void)BusClockHertz;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
i2c_reset(const EFI_I2C_MASTER_PROTOCOL *This)
{
    (void)This;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
i2c_start_request(const EFI_I2C_MASTER_PROTOCOL *This,
                  UINTN                          SlaveAddress,
                  EFI_I2C_REQUEST_PACKET        *RequestPacket,
                  EFI_EVENT                      Event,
                  EFI_STATUS                    *I2cStatus)
{
    (void)This;
    (void)Event;
    (void)I2cStatus;

    if (RequestPacket == NULL) return EFI_INVALID_PARAMETER;
    uint8_t slave = (uint8_t)(SlaveAddress & 0x7F);

    if (RequestPacket->OperationCount == 1) {
        EFI_I2C_OPERATION *op = &RequestPacket->Operation[0];
        if (op->Flags & I2C_FLAG_READ)   return EFI_UNSUPPORTED;
        if (op->LengthInBytes < 2)       return EFI_INVALID_PARAMETER;
        uint8_t cmd   = op->Buffer[0];
        uint8_t count = op->Buffer[1];
        if (count + 2u != op->LengthInBytes) return EFI_INVALID_PARAMETER;
        return smb_block_write(slave, cmd, &op->Buffer[2], count);
    }

    if (RequestPacket->OperationCount == 2) {
        EFI_I2C_OPERATION *op0 = &RequestPacket->Operation[0];
        EFI_I2C_OPERATION *op1 = &RequestPacket->Operation[1];
        if (op0->Flags & I2C_FLAG_READ)        return EFI_UNSUPPORTED;
        if (!(op1->Flags & I2C_FLAG_READ))     return EFI_UNSUPPORTED;
        if (op0->LengthInBytes != 1)           return EFI_INVALID_PARAMETER;
        if (op1->LengthInBytes < 1)            return EFI_INVALID_PARAMETER;

        uint8_t cmd = op0->Buffer[0];
        size_t  got = 0;
        //
        // op1's buffer is the [count][payload...] landing zone per
        // AxlSmbus's I2C read framing. We stash count at Buffer[0]
        // and payload starting at Buffer[1], matching what the
        // AxlSmbus code expects.
        //
        EFI_STATUS s = smb_block_read(slave, cmd,
                                      &op1->Buffer[1],
                                      (size_t)op1->LengthInBytes - 1,
                                      &got);
        if (EFI_ERROR(s)) return s;
        op1->Buffer[0] = (uint8_t)got;
        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// SMBIOS Type 38 sanity dump (boot-time aid for slave-address mismatches)
// ---------------------------------------------------------------------------

static void
dump_smbios_type38(void)
{
    AxlSmbiosHeader *hdr = axl_smbios_find(38);
    if (hdr == NULL) {
        axl_warning("SMBIOS Type 38 not present — SSIF auto-detect will fail");
        return;
    }
    const uint8_t *raw = (const uint8_t *)hdr;
    if (hdr->Length < 0x10) {
        axl_warning("SMBIOS Type 38 too short (%u bytes)",
                    (unsigned)hdr->Length);
        return;
    }
    axl_info("SMBIOS Type 38: iface=%u rev=0x%02x i2c_slave=0x%02x "
             "(AxlSmbus will decode as 7-bit 0x%02x)",
             (unsigned)raw[4], (unsigned)raw[5], (unsigned)raw[6],
             (unsigned)(raw[6] >> 1));
}

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------

EFI_STATUS EFIAPI
DriverEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    axl_driver_init(ImageHandle, SystemTable);

    if (find_ich9_smbus() != 0) {
        axl_error("ICH9 SMBus controller not found — shim cannot attach");
        return EFI_UNSUPPORTED;
    }

    dump_smbios_type38();

    mProtocol.SetBusFrequency           = i2c_set_bus_frequency;
    mProtocol.Reset                     = i2c_reset;
    mProtocol.StartRequest              = i2c_start_request;
    mProtocol.I2cControllerCapabilities = NULL;

    EFI_GUID guid = EFI_I2C_MASTER_PROTOCOL_GUID;
    mHandle = NULL;
    EFI_STATUS s = gBS->InstallProtocolInterface(
        &mHandle, &guid, EFI_NATIVE_INTERFACE, &mProtocol);
    if (EFI_ERROR(s)) {
        axl_error("InstallProtocolInterface failed: 0x%lx",
                  (unsigned long)s);
        return s;
    }

    axl_info("SmbusHcShim: published EFI_I2C_MASTER_PROTOCOL "
             "(ICH9 SMB I/O 0x%04x)", (unsigned)mSmbBase);
    return EFI_SUCCESS;
}

#else  /* !x86 */

EFI_STATUS EFIAPI
DriverEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    axl_driver_init(ImageHandle, SystemTable);
    axl_warning("SmbusHcShim is x86-only (ICH9 SMBus needs Intel chipset + I/O ports)");
    return EFI_UNSUPPORTED;
}

#endif  /* x86 */
