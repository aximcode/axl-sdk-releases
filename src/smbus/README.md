# AxlSmbus — SMBus / I2C block access

SMBus session abstraction that gives UEFI apps block read/write over
whichever controller the platform publishes. Dispatches on the first
available firmware-provided transport:

| Kind | Mechanism | Who owns framing |
|---|---|---|
| `AXL_SMBUS_TRANSPORT_HC`  | `EFI_SMBUS_HC_PROTOCOL`     | firmware |
| `AXL_SMBUS_TRANSPORT_I2C` | `EFI_I2C_MASTER_PROTOCOL`   | this module |

Auto-detect preference: SMBus HC → I2C Master. The I2C fallback
constructs the SMBus block-transfer wire format (`[cmd][count][data]`
on writes, `[count][data]` on reads) on top of raw I2C operations.

## Consumers

- **AxlIpmi SSIF transport** — every IPMI round-trip over SSIF goes
  through an `AxlSmbus` session.
- **AxlSpd** *(planned, Phase B3)* — DDR4/5 SPD readers at the
  canonical 0xA0–0xAE addresses.

## Usage

```c
#include <axl/axl-smbus.h>

AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();
if (s == NULL) {
    axl_error("No SMBus controller available");
    return -1;
}

uint8_t  buf[32];
size_t   len = sizeof(buf);
if (axl_smbus_read_block(s, /*slave=*/0x10, /*cmd=*/0x03,
                         buf, &len) != 0) {
    axl_error("SMBus read failed");
    return -1;
}
// buf[0..len-1] carries the payload (count byte already stripped).
```

`AXL_AUTOPTR` frees the session at scope exit; firmware owns the
underlying protocol instance so nothing in the shim is leaked.

## Layout

```
src/smbus/
├── axl-smbus.c              session lifecycle + auto-detect
├── axl-smbus-internal.h     transport vtable + session layout
├── axl-smbus-hc.c           EFI_SMBUS_HC_PROTOCOL pass-through
├── axl-smbus-i2c.c          EFI_I2C_MASTER_PROTOCOL framing (B1 code path)
└── axl-smbus-format.c       enum-to-string helpers
```

## Wire format (I2C path)

SMBus 2.0 §5.5.7/5.5.8. The SMBus HC protocol inserts and strips the
byte-count for us; the I2C Master protocol does not, so this module
builds it manually:

```
Block write:  [cmd] [count] [payload_0 ... payload_{count-1}]
Block read:   (write cmd) → (read) [count] [payload_0 ... payload_{count-1}]
```

On reads, the count byte is stripped before the payload is copied to
the caller's buffer. On writes, it is inserted at `tx[1]`. Both sides
are asserted on by `test/unit/axl-test-smbus.c` via a capturing mock
protocol — any future regression of this framing fails CI without a
BMC in the loop.

## Limits

- Max block payload: 32 bytes (spec limit; `AXL_SMBUS_BLOCK_MAX`).
- Higher-level SMBus commands (quick, byte, byte_data, word_data,
  process_call, bus enumeration) are not implemented yet — they land
  when AxlSpd's scope demands them, not speculatively.
