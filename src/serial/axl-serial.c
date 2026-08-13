/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-serial.c
    Serial-port enumeration and line-setting readout.

    Enumerates the handles publishing EFI_SERIAL_IO_PROTOCOL and reads
    each port's SERIAL_IO_MODE (current line settings) and GetControl
    modem-status bits as typed structs. Read-only descriptor probe — no
    port is opened and no byte I/O is performed.

    The handle set is located once and cached for the image lifetime
    (the AxlBlock / AxlUsb model); `axl_serial_next` recovers its
    position from the handle the caller passes back, so there is no
    shared mutable cursor. The cached buffer is freed at exit.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* EFI_SERIAL_IO_PROTOCOL + SERIAL_IO_MODE (generated) */
#include "../util/axl-handle-iter.h"
#include <axl/axl-serial.h>
#include <axl/axl-loop.h>    /* axl_serial_read_async timer source */
#include <axl/axl-mem.h>     /* axl_malloc / axl_free */

/* GetControl modem-status bits (UEFI 2.11 §12.8 "Control Bits"). The
   generated headers carry the protocol struct but not these masks. */
#define SERIAL_CTS       0x0010u   /* EFI_SERIAL_CLEAR_TO_SEND */
#define SERIAL_DSR       0x0020u   /* EFI_SERIAL_DATA_SET_READY */
#define SERIAL_RI        0x0040u   /* EFI_SERIAL_RING_INDICATE */
#define SERIAL_DCD       0x0080u   /* EFI_SERIAL_CARRIER_DETECT */
#define SERIAL_HW_FLOW   0x4000u   /* EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE */

/* GetControl is exposed as void * in the generated protocol struct (its
   funcptr typedef is not generated); cast to call it. */
typedef EFI_STATUS (EFIAPI *SerialGetControlFn)(
    EFI_SERIAL_IO_PROTOCOL *This,
    UINT32                 *Control);

/* Enumeration cursor shared with the other platform readers; the handle
   set is located once and cached for the image lifetime. */
static AxlHandleIter serial_iter = {
    .guid = &gEfiSerialIoProtocolGuid,
    .what = "serial port"
};

static EFI_SERIAL_IO_PROTOCOL *
serial_proto(
    AxlHandle handle
    )
{
    if (handle == NULL) {
        return NULL;
    }
    EFI_SERIAL_IO_PROTOCOL *sio    = NULL;
    EFI_STATUS              status = axl_efi_call(
        axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handle, &gEfiSerialIoProtocolGuid, (void **)&sio);
    if (EFI_ERROR(status)) {
        return NULL;
    }
    return sio;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlHandle
axl_serial_next(
    AxlHandle prev
    )
{
    return axl_handle_iter_next(&serial_iter, prev);
}

int
axl_serial_get_mode(
    AxlHandle      handle,
    AxlSerialMode *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    EFI_SERIAL_IO_PROTOCOL *sio = serial_proto(handle);
    if (sio == NULL || sio->Mode == NULL) {
        return AXL_ERR;
    }

    SERIAL_IO_MODE *mode    = sio->Mode;
    out->baud_rate          = (uint32_t)mode->BaudRate;
    out->data_bits          = mode->DataBits;
    out->parity             = (uint8_t)mode->Parity;
    out->stop_bits          = (uint8_t)mode->StopBits;
    out->timeout            = mode->Timeout;
    out->receive_fifo_depth = mode->ReceiveFifoDepth;
    return AXL_OK;
}

int
axl_serial_get_control(
    AxlHandle         handle,
    AxlSerialControl *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    EFI_SERIAL_IO_PROTOCOL *sio = serial_proto(handle);
    if (sio == NULL || sio->GetControl == NULL) {
        return AXL_ERR;
    }

    SerialGetControlFn get_control = (SerialGetControlFn)sio->GetControl;
    UINT32             bits        = 0;
    EFI_STATUS         status = axl_efi_call(get_control, 2, sio, &bits);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    out->cts             = (bits & SERIAL_CTS) != 0;
    out->dsr             = (bits & SERIAL_DSR) != 0;
    out->ri              = (bits & SERIAL_RI) != 0;
    out->dcd             = (bits & SERIAL_DCD) != 0;
    out->hw_flow_control = (bits & SERIAL_HW_FLOW) != 0;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Byte I/O — open / set-mode / write / read / async receive
// ---------------------------------------------------------------------------

/* The protocol's Write / Read / SetAttributes are void * in the generated
   struct (their funcptr typedefs are not generated); cast to call. The enum
   args (Parity / StopBits) are passed as UINT32 — enum ABI is int. */
typedef EFI_STATUS (EFIAPI *SerialSetAttrFn)(
    EFI_SERIAL_IO_PROTOCOL *This, UINT64 BaudRate, UINT32 ReceiveFifoDepth,
    UINT32 Timeout, UINT32 Parity, UINT8 DataBits, UINT32 StopBits);
typedef EFI_STATUS (EFIAPI *SerialRwFn)(
    EFI_SERIAL_IO_PROTOCOL *This, UINTN *BufferSize, void *Buffer);

struct AxlSerial {
    EFI_SERIAL_IO_PROTOCOL *sio;
    AxlHandle               handle;      ///< handle it was opened from (axl_serial_handle)
    bool                    shared;      ///< opened via axl_serial_open_shared
    AxlSerial              *next_open;   ///< intrusive open-port list (see mOpenPorts)
    AxlLoop                *loop;        ///< async-read loop (NULL = none)
    AxlSourceId             timer_src;   ///< async-read timer source id (0 = none)
    AxlSerialReadFn         cb;          ///< async-read callback
    void                   *user;        ///< async-read context
};

/* Every open port in THIS image, threaded through the wrappers themselves.
   Intrusive rather than a side table so there is no fixed cap to overflow
   and no second allocation to fail: a port that could be opened can always
   be tracked, which matters because the whole point is that an untracked
   open is the corruption case. UEFI is single-threaded, so no locking. */
static AxlSerial *mOpenPorts = NULL;

/* The open claim on @handle, or NULL when free. */
static AxlSerial *
find_open(AxlHandle handle)
{
    for (AxlSerial *p = mOpenPorts; p != NULL; p = p->next_open) {
        if (p->handle == handle) {
            return p;
        }
    }
    return NULL;
}

/* Shared by axl_serial_open and axl_serial_open_shared. */
static int
serial_open_common(AxlHandle handle, AxlSerial **out, bool shared)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    *out = NULL;
    EFI_SERIAL_IO_PROTOCOL *sio = serial_proto(handle);
    if (sio == NULL) {
        return AXL_ERR;
    }
    /* Exclusivity is decided by the FIRST open: an exclusive holder refuses
       everyone, and a shared holder refuses only an exclusive claim. */
    AxlSerial *held = find_open(handle);
    if (held != NULL && (!shared || !held->shared)) {
        return AXL_BUSY;
    }
    AxlSerial *s = axl_malloc(sizeof(*s));
    if (s == NULL) {
        return AXL_ERR;
    }
    s->sio       = sio;
    s->handle    = handle;
    s->shared    = shared;
    s->loop      = NULL;
    s->timer_src = 0;
    s->cb        = NULL;
    s->user      = NULL;
    s->next_open = mOpenPorts;
    mOpenPorts   = s;
    *out = s;
    return AXL_OK;
}

int
axl_serial_open(AxlHandle handle, AxlSerial **out)
{
    return serial_open_common(handle, out, false);
}

int
axl_serial_open_shared(AxlHandle handle, AxlSerial **out)
{
    return serial_open_common(handle, out, true);
}

bool
axl_serial_is_open(AxlHandle handle)
{
    return handle != NULL && find_open(handle) != NULL;
}

AxlHandle
axl_serial_handle(const AxlSerial *s)
{
    return (s != NULL) ? s->handle : NULL;
}

void
axl_serial_close(AxlSerial *s)
{
    if (s == NULL) {
        return;
    }
    /* Poison check: a closed port has its borrowed protocol pointer cleared
       below, so a second close on the same pointer is a no-op instead of a
       double free -- PROVIDED the allocator has not recycled the memory. See
       the residual-hazard note on axl_serial_close in the header. */
    if (s->sio == NULL) {
        return;
    }
    if (s->loop != NULL && s->timer_src != 0) {
        axl_loop_remove_source(s->loop, s->timer_src);
    }
    /* Drop the claim before freeing, or the next open finds a dangling
       entry and reports the port busy forever.
       Unlink only if this really is a live entry: if it is absent, the
       pointer is not a port we currently own and must not be freed. */
    bool linked = false;
    for (AxlSerial **pp = &mOpenPorts; *pp != NULL; pp = &(*pp)->next_open) {
        if (*pp == s) {
            *pp = s->next_open;
            linked = true;
            break;
        }
    }
    if (!linked) {
        return;
    }
    s->sio      = NULL;
    s->next_open = NULL;
    axl_free(s);
}

int
axl_serial_set_mode(AxlSerial *s, const AxlSerialMode *mode)
{
    if (s == NULL || mode == NULL || s->sio->SetAttributes == NULL) {
        return AXL_ERR;
    }
    SerialSetAttrFn set_attr = (SerialSetAttrFn)s->sio->SetAttributes;
    EFI_STATUS st = axl_efi_call(set_attr, 7, s->sio,
        (UINT64)mode->baud_rate, (UINT32)mode->receive_fifo_depth,
        (UINT32)mode->timeout, (UINT32)mode->parity,
        (UINT8)mode->data_bits, (UINT32)mode->stop_bits);
    return EFI_ERROR(st) ? AXL_ERR : AXL_OK;
}

int
axl_serial_write(AxlSerial *s, const void *buf, size_t len, size_t *out_written)
{
    if (out_written != NULL) {
        *out_written = 0;
    }
    if (s == NULL || buf == NULL || s->sio->Write == NULL) {
        return AXL_ERR;
    }
    if (len == 0) {
        return AXL_OK;
    }
    SerialRwFn write_fn = (SerialRwFn)s->sio->Write;
    UINTN      n = (UINTN)len;
    /* Write updates n to the count actually transmitted; EFI_TIMEOUT means a
       short write (the port's timeout elapsed) — not a hard error, the caller
       retries the remainder via out_written. */
    EFI_STATUS st = axl_efi_call(write_fn, 3, s->sio, &n, (void *)buf);
    if (EFI_ERROR(st) && st != EFI_TIMEOUT) {
        return AXL_ERR;
    }
    if (out_written != NULL) {
        *out_written = (size_t)n;
    }
    return AXL_OK;
}

int
axl_serial_read(AxlSerial *s, void *buf, size_t cap, size_t *out_read)
{
    if (out_read != NULL) {
        *out_read = 0;
    }
    if (s == NULL || buf == NULL || out_read == NULL || cap == 0
        || s->sio->Read == NULL) {
        return AXL_ERR;
    }
    SerialRwFn read_fn = (SerialRwFn)s->sio->Read;
    UINTN      n = (UINTN)cap;
    /* Read returns immediately with whatever is buffered; EFI_TIMEOUT means
       fewer than requested were available (n holds the count actually read) —
       a normal "nothing/partial" result, not an error. */
    EFI_STATUS st = axl_efi_call(read_fn, 3, s->sio, &n, buf);
    if (EFI_ERROR(st) && st != EFI_TIMEOUT) {
        return AXL_ERR;
    }
    *out_read = (size_t)n;
    return AXL_OK;
}

/* Async-receive poll tick: drain available bytes, deliver to the callback. */
static bool
serial_poll_tick(void *data)
{
    AxlSerial *s = (AxlSerial *)data;
    uint8_t    rx[256];
    size_t     n = 0;
    if (axl_serial_read(s, rx, sizeof(rx), &n) == AXL_OK && n > 0) {
        s->cb(rx, n, s->user);
    }
    return AXL_SOURCE_CONTINUE;
}

int
axl_serial_read_async(AxlSerial *s, AxlLoop *loop, size_t poll_ms,
                      AxlSerialReadFn cb, void *user)
{
    if (s == NULL || loop == NULL || cb == NULL || poll_ms == 0) {
        return AXL_ERR;
    }
    /* Only one async receive per port — replace any prior one. */
    if (s->loop != NULL && s->timer_src != 0) {
        axl_loop_remove_source(s->loop, s->timer_src);
        s->timer_src = 0;
    }
    s->cb   = cb;
    s->user = user;
    s->loop = loop;
    s->timer_src = axl_loop_add_timer(loop, (uint32_t)poll_ms,
                                      serial_poll_tick, s);
    if (s->timer_src == 0) {
        s->loop = NULL;
        return AXL_ERR;
    }
    return AXL_OK;
}
