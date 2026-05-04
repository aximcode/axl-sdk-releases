/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-usb.c
    USB device enumeration via `EFI_USB_IO_PROTOCOL` handles.

    The UEFI USB stack installs one EFI_USB_IO_PROTOCOL handle per
    *interface* of every enumerated device. AxlUsb walks every such
    handle, derives a stable `(bus, addr, intf)` ordinal triple from
    the handle's device path, and exposes the result via a cursor-
    style enumeration that mirrors `axl_pci_next`.

    Bus / addr ordinals are synthesized so consumers don't have to
    care about UEFI's flatter "every interface is its own controller
    handle" model:

      - bus_key = device-path bytes BEFORE the first USB messaging node
                  (= "everything up to the host controller").
      - dev_key = device-path bytes through the LAST USB messaging node
                  with that node's `InterfaceNumber` field zeroed
                  (= "physical device, ignoring which interface this
                  handle refers to").

    Both keys point into the same `axl_memdup`'d copy of the device-
    path bytes that the entry owns; the firmware-owned original is
    only consulted during enumeration (the InterfaceNumber field
    needs zeroing before sort, so we copy regardless).

    Sort entries by (bus_key, dev_key, intf), then assign 1-based
    ordinals while walking the sorted array. Two interface handles of
    the same physical device share `(bus, addr)` and differ only in
    `intf`; two physical devices on the same controller share `bus`
    and differ in `addr`.

    Single-threaded; not reentrant. The handle cache and cursor are
    module-static, mirroring AxlPci. There is no teardown path —
    entries live for the app's lifetime, again mirroring AxlPci.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-array.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-usb.h>

AXL_LOG_DOMAIN("usb");

// ---------------------------------------------------------------------------
// Per-handle entry layout
// ---------------------------------------------------------------------------

typedef struct {
    EFI_USB_IO_PROTOCOL  *io;
    AxlUsbAddr            addr;        /* assigned post-sort */
    /* Keys are byte ranges into the firmware-owned device path; valid
       for the lifetime of the handle (which is the lifetime of the
       app for any device installed at boot — and we cache once). */
    const uint8_t        *bus_key;
    size_t                bus_key_len;
    const uint8_t        *dev_key;
    size_t                dev_key_len;
    /* Hub port chain — one byte per USB messaging-type node along
       the device path. Direct attachment to the host controller
       has n_ports == 1 (depth 0); each successive hub adds one
       entry. Bounded by AXL_USB_TREE_MAX_DEPTH. */
    uint8_t               ports[AXL_USB_TREE_MAX_DEPTH];
    uint8_t               n_ports;
    /* Lazily-probed string-descriptor language. 0 = unset; @p
       lang_probed flips to true the first time
       axl_usb_get_string is called against this entry. Devices
       that don't support any language ID stick at lang_id == 0
       with lang_probed == true so subsequent calls short-circuit. */
    uint16_t              lang_id;
    bool                  lang_probed;
} Entry;

/* Module state. AxlPci uses the same shape — lazy init on first call,
   sticky-failed flag so a missing USB stack doesn't re-probe on every
   axl_usb_next. */
static bool      init_done;
static bool      init_failed;
static AxlArray *entries;          /* AxlArray<Entry> */

/* Cursor returned to callers. Storage is reused across calls; passing
   any pointer other than &cursor restarts the walk (see header). */
static AxlUsbAddr  cursor;
static bool        cursor_valid;
static size_t      cursor_idx;

// ---------------------------------------------------------------------------
// Device-path slicing
// ---------------------------------------------------------------------------

/* Walk the device path, locating the first and last USB messaging
   nodes. Sets *out_bus_len to the byte count from the start to (but
   not including) the first USB node; *out_dev_len to bytes from start
   through the last USB node (inclusive); and *out_last_usb_off to the
   offset of the InterfaceNumber field of the last USB node so the
   caller can zero it for dedup-friendly comparison.

   Returns false if the path doesn't contain a USB messaging node, or
   the END node is missing within EFI_DP_MAX_BYTES — both signal "not
   a USB IO handle's path" and we drop the entry. */
#define EFI_DP_MAX_BYTES  (4u * 1024u)

static bool
slice_device_path(
    const EFI_DEVICE_PATH_PROTOCOL  *dp,
    size_t                          *out_bus_len,
    size_t                          *out_dev_len,
    size_t                          *out_last_intf_byte_off,
    uint8_t                         *out_intf_number
    )
{
    const uint8_t *base   = (const uint8_t *)dp;
    size_t         walked = 0;
    size_t         first_usb_off = 0;
    size_t         last_usb_off  = 0;
    size_t         last_usb_len  = 0;
    bool           any_usb       = false;
    bool           terminated    = false;

    /* Walk the path, refusing to read past EFI_DP_MAX_BYTES. Bound
       the header read up front so a malformed/unterminated path can't
       trigger an OOB read of node->Type past the firmware buffer. */
    while (walked + sizeof(EFI_DEVICE_PATH_PROTOCOL) <= EFI_DP_MAX_BYTES) {
        const EFI_DEVICE_PATH_PROTOCOL *node =
            (const EFI_DEVICE_PATH_PROTOCOL *)(base + walked);
        if (EFI_DP_IS_END(node)) {
            terminated = true;
            break;
        }
        size_t node_len = (size_t)EFI_DP_LENGTH(node);
        if (node_len < 4u || walked + node_len > EFI_DP_MAX_BYTES) {
            return false;  /* malformed: short header or runs past cap */
        }
        if (EFI_DP_TYPE(node) == MESSAGING_DEVICE_PATH
            && EFI_DP_SUBTYPE(node) == MSG_USB_DP
            && node_len >= sizeof(USB_DEVICE_PATH))
        {
            if (!any_usb) {
                first_usb_off = walked;
                any_usb = true;
            }
            last_usb_off = walked;
            last_usb_len = node_len;
        }
        walked += node_len;
    }
    if (!any_usb || !terminated) {
        return false;
    }

    *out_bus_len            = first_usb_off;
    *out_dev_len            = last_usb_off + last_usb_len;
    /* InterfaceNumber lives at offset (header_size + 1) within a USB
       device-path node — header is 4 bytes, ParentPortNumber is 1
       byte, then InterfaceNumber. */
    *out_last_intf_byte_off = last_usb_off + sizeof(EFI_DEVICE_PATH_PROTOCOL) + 1u;
    *out_intf_number        =
        ((const USB_DEVICE_PATH *)(base + last_usb_off))->InterfaceNumber;
    return true;
}

/* Extract the per-USB-node port chain (Phase F) — one entry per
   USB messaging-type node along the device path. The leaf node's
   ParentPortNumber lands at index n_ports-1; the path-depth
   (= USB hub depth) is n_ports - 1.

   Caps at AXL_USB_TREE_MAX_DEPTH so a malformed/long path can't
   overflow the caller's array. Reuses the bounded walk
   slice_device_path uses so OOB reads are impossible.

   Returns the number of ports written; 0 for a path with no USB
   nodes (slice_device_path would already reject such a path, so
   in practice we always see >= 1 here). */
static uint8_t
extract_port_chain(
    const EFI_DEVICE_PATH_PROTOCOL  *dp,
    uint8_t                         *ports,
    uint8_t                          max
    )
{
    const uint8_t  *base   = (const uint8_t *)dp;
    size_t          walked = 0;
    uint8_t         n      = 0;

    while (walked + sizeof(EFI_DEVICE_PATH_PROTOCOL) <= EFI_DP_MAX_BYTES) {
        const EFI_DEVICE_PATH_PROTOCOL *node =
            (const EFI_DEVICE_PATH_PROTOCOL *)(base + walked);
        if (EFI_DP_IS_END(node)) {
            break;
        }
        size_t node_len = (size_t)EFI_DP_LENGTH(node);
        if (node_len < 4u || walked + node_len > EFI_DP_MAX_BYTES) {
            break;
        }
        if (EFI_DP_TYPE(node) == MESSAGING_DEVICE_PATH
            && EFI_DP_SUBTYPE(node) == MSG_USB_DP
            && node_len >= sizeof(USB_DEVICE_PATH))
        {
            if (n < max) {
                ports[n++] =
                    ((const USB_DEVICE_PATH *)(base + walked))->ParentPortNumber;
            }
            /* If a path exceeds max depth, stop recording but keep
               walking to ensure the slice_device_path bounds
               assumptions still hold downstream. NOTE: the
               returned `n` then caps at `max`, so a consumer that
               does `depth = n - 1` will under-report depth for
               paths deeper than max. USB 2.0/3.x cap real-world
               depth at 5 hubs (so 6 USB nodes), well within the
               default AXL_USB_TREE_MAX_DEPTH = 8 — but a future
               consumer that needs an explicit "truncated" signal
               should pass a separate out-bool. */
        }
        walked += node_len;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Sort + ordinal assignment
// ---------------------------------------------------------------------------

/* Lexicographic compare of two byte ranges, with shorter ranges
   ordered before longer ones at common-prefix equality. Mirrors
   memcmp's "shorter < longer when prefix matches" convention. */
static int
range_cmp(
    const uint8_t  *a,
    size_t          alen,
    const uint8_t  *b,
    size_t          blen
    )
{
    size_t n = alen < blen ? alen : blen;
    int rc = axl_memcmp(a, b, n);
    if (rc != 0) {
        return rc;
    }
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

static int
entry_cmp(
    const void  *pa,
    const void  *pb
    )
{
    const Entry *a = pa;
    const Entry *b = pb;
    int rc = range_cmp(a->bus_key, a->bus_key_len,
                       b->bus_key, b->bus_key_len);
    if (rc != 0) {
        return rc;
    }
    rc = range_cmp(a->dev_key, a->dev_key_len,
                   b->dev_key, b->dev_key_len);
    if (rc != 0) {
        return rc;
    }
    return (int)a->addr.intf - (int)b->addr.intf;
}

static void
assign_ordinals(
    AxlArray  *arr
    )
{
    const uint8_t *prev_bus = NULL;
    size_t         prev_bus_len = 0;
    const uint8_t *prev_dev = NULL;
    size_t         prev_dev_len = 0;
    uint8_t        bus_ord = 0;
    uint8_t        addr_ord = 0;

    size_t n = axl_array_len(arr);
    for (size_t i = 0; i < n; i++) {
        Entry *e = (Entry *)axl_array_get(arr, i);
        bool bus_changed =
            prev_bus == NULL
            || range_cmp(prev_bus, prev_bus_len,
                         e->bus_key, e->bus_key_len) != 0;
        if (bus_changed) {
            if (bus_ord < 0xFF) {
                bus_ord++;
            }
            addr_ord     = 0;
            prev_dev     = NULL;
            prev_dev_len = 0;
            prev_bus     = e->bus_key;
            prev_bus_len = e->bus_key_len;
        }
        bool dev_changed =
            prev_dev == NULL
            || range_cmp(prev_dev, prev_dev_len,
                         e->dev_key, e->dev_key_len) != 0;
        if (dev_changed) {
            if (addr_ord < 0xFF) {
                addr_ord++;
            }
            prev_dev     = e->dev_key;
            prev_dev_len = e->dev_key_len;
        }
        e->addr.bus  = bus_ord;
        e->addr.addr = addr_ord;
        /* intf field already populated from the descriptor walk */
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

static void
ingest_handle(
    EFI_HANDLE  h
    )
{
    EFI_USB_IO_PROTOCOL *io = NULL;
    EFI_STATUS status = axl_efi_call(
        axl_bs()->HandleProtocol, 3,
        h, &gEfiUsbIoProtocolGuid, (void **)&io);
    if (EFI_ERROR(status) || io == NULL) {
        return;
    }
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    status = axl_efi_call(
        axl_bs()->HandleProtocol, 3,
        h, &gEfiDevicePathProtocolGuid, (void **)&dp);
    if (EFI_ERROR(status) || dp == NULL) {
        return;
    }

    size_t  bus_len = 0;
    size_t  dev_len = 0;
    size_t  intf_off = 0;
    uint8_t intf_num = 0;
    if (!slice_device_path(dp,
                           &bus_len, &dev_len, &intf_off, &intf_num))
    {
        return;
    }

    /* dev_key needs the InterfaceNumber field zeroed so two interfaces
       of the same device compare equal. Take an owned copy of the
       device-path bytes through the last USB node, then zero the
       offending byte. The bus_key shares the same backing buffer's
       prefix — both keys point into the same axl_memdup'd allocation. */
    uint8_t *owned = axl_memdup((const uint8_t *)dp, dev_len);
    if (owned == NULL) {
        return;
    }
    if (intf_off < dev_len) {
        owned[intf_off] = 0;
    }

    Entry entry = {
        .io           = io,
        .addr         = { .bus = 0, .addr = 0, .intf = intf_num },
        .bus_key      = owned,
        .bus_key_len  = bus_len,
        .dev_key      = owned,
        .dev_key_len  = dev_len,
    };
    /* Populate the per-node port chain so axl_usb_tree_for_each
       (Phase F) can emit USB hub depth without re-walking the
       device path on each call. */
    entry.n_ports = extract_port_chain(
        dp, entry.ports, AXL_USB_TREE_MAX_DEPTH);

    if (axl_array_append(entries, &entry) < 0) {
        axl_free(owned);
    }
}

static int
ensure_init(
    void
    )
{
    if (init_done) {
        return 0;
    }
    if (init_failed) {
        return -1;
    }

    entries = axl_array_new(sizeof(Entry));
    if (entries == NULL) {
        init_failed = true;
        return -1;
    }

    EFI_HANDLE *handles      = NULL;
    size_t      handle_count = 0;
    EFI_STATUS  status = axl_efi_call(
        axl_bs()->LocateHandleBuffer, 5,
        ByProtocol, &gEfiUsbIoProtocolGuid, NULL,
        &handle_count, &handles);
    if (EFI_ERROR(status) || handle_count == 0) {
        axl_debug("no EFI_USB_IO_PROTOCOL handles installed");
        init_done = true;  /* successful "no devices" — not failed */
        return 0;
    }

    for (size_t i = 0; i < handle_count; i++) {
        ingest_handle(handles[i]);
    }
    axl_backend_free(handles);

    /* Sort by (bus_key, dev_key, intf) and assign ordinals. AxlArray
       sort is a stable insertion sort — fine for the typical USB
       enumeration size (≤ a few dozen entries). */
    axl_array_sort(entries, entry_cmp);
    assign_ordinals(entries);

    axl_debug("USB: %zu interface handle(s) enumerated",
              axl_array_len(entries));
    init_done = true;
    return 0;
}

// ---------------------------------------------------------------------------
// Cursor + lookups
// ---------------------------------------------------------------------------

AxlUsbAddr *
axl_usb_next(
    AxlUsbAddr  *prev
    )
{
    if (ensure_init() != 0) {
        return NULL;
    }
    size_t n = axl_array_len(entries);

    /* prev is treated as a one-bit "continue or restart" signal —
       only &cursor is a valid resume token; any other pointer or
       NULL restarts. */
    if (prev == NULL || prev != &cursor || !cursor_valid) {
        cursor_idx = 0;
    } else {
        cursor_idx++;
    }

    if (cursor_idx >= n) {
        cursor_valid = false;
        return NULL;
    }

    Entry *e = (Entry *)axl_array_get(entries, cursor_idx);
    cursor       = e->addr;
    cursor_valid = true;
    return &cursor;
}

/* Linear scan over the entry array — typical USB enumeration is
   a few dozen entries at most, so a hash table would be overkill. */
static Entry *
find_entry(
    AxlUsbAddr  a
    )
{
    if (entries == NULL) {
        return NULL;
    }
    size_t n = axl_array_len(entries);
    for (size_t i = 0; i < n; i++) {
        Entry *e = (Entry *)axl_array_get(entries, i);
        if (e->addr.bus == a.bus
            && e->addr.addr == a.addr
            && e->addr.intf == a.intf)
        {
            return e;
        }
    }
    return NULL;
}

int
axl_usb_get_vid_pid(
    AxlUsbAddr   addr,
    uint16_t    *vid,
    uint16_t    *pid
    )
{
    if (vid == NULL || pid == NULL) {
        return AXL_ERR;
    }
    if (ensure_init() != 0) {
        return AXL_ERR;
    }
    Entry *e = find_entry(addr);
    if (e == NULL) {
        return AXL_ERR;
    }
    EFI_USB_DEVICE_DESCRIPTOR  desc = { 0 };
    EFI_STATUS status = axl_efi_call(
        e->io->UsbGetDeviceDescriptor, 2, e->io, &desc);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    *vid = desc.IdVendor;
    *pid = desc.IdProduct;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// String descriptor reads (Phase C)
// ---------------------------------------------------------------------------

/* Lazily probe the device's supported-language table and cache the
   first ID. UsbGetSupportedLanguages returns a pointer into firmware-
   managed memory (do NOT free — see EDK2 UsbBus.c) along with the
   table size in bytes; we want the first entry. Returns the cached
   lang_id (0 means "no strings supported"). */
static uint16_t
ensure_lang_id(
    Entry  *e
    )
{
    if (e->lang_probed) {
        return e->lang_id;
    }
    UINT16     *table     = NULL;
    UINT16      table_len = 0;  /* size in bytes per the spec */
    EFI_STATUS  status    = axl_efi_call(
        e->io->UsbGetSupportedLanguages, 3, e->io, &table, &table_len);
    if (!EFI_ERROR(status)
        && table != NULL
        && table_len >= sizeof(UINT16))
    {
        e->lang_id = (uint16_t)table[0];
    }
    /* Mark probed regardless — devices that advertise no languages
       (return EFI_NOT_FOUND or zero-length table) will never have
       strings; subsequent calls short-circuit through the cached
       lang_id == 0. */
    e->lang_probed = true;
    return e->lang_id;
}

int
axl_usb_get_string(
    AxlUsbAddr   addr,
    uint8_t      string_index,
    char        *buf,
    size_t       buflen
    )
{
    if (buf == NULL || buflen == 0 || string_index == 0) {
        return -1;
    }
    if (ensure_init() != 0) {
        return -1;
    }
    Entry *e = find_entry(addr);
    if (e == NULL) {
        return -1;
    }
    uint16_t lang = ensure_lang_id(e);
    if (lang == 0) {
        return -1;  /* device advertises no string-descriptor languages */
    }

    CHAR16     *str    = NULL;
    EFI_STATUS  status = axl_efi_call(
        e->io->UsbGetStringDescriptor, 4,
        e->io, lang, string_index, &str);
    if (EFI_ERROR(status) || str == NULL) {
        return -1;
    }
    /* axl_ucs2_to_utf8_buf takes (const unsigned short *, dst, dst_size)
       and NUL-terminates within dst_size; never writes a partial
       multi-byte sequence. Returns the byte count excluding NUL. */
    size_t n = axl_ucs2_to_utf8_buf((const unsigned short *)str,
                                    buf, buflen);
    /* UsbGetStringDescriptor's CHAR16 buffer is firmware-allocated
       via gBS->AllocatePool; reclaim it via axl_backend_free
       (which wraps gBS->FreePool). */
    axl_backend_free(str);
    return (int)n;
}

/* Standard device-descriptor slots for the three convenience
   helpers. Mirrors EFI_USB_DEVICE_DESCRIPTOR's field naming. */
typedef enum {
    DESC_STR_MANUFACTURER = 1,
    DESC_STR_PRODUCT      = 2,
    DESC_STR_SERIAL       = 3,
} DescStrSlot;

/* Read the DeviceDescriptor's string-index byte at @p slot and
   delegate to axl_usb_get_string with that index. Returns -1 if
   the device declares no string at that slot (index == 0) or any
   underlying call fails. */
static int
get_string_for_slot(
    AxlUsbAddr   addr,
    DescStrSlot  slot,
    char        *buf,
    size_t       buflen
    )
{
    if (buf == NULL || buflen == 0) {
        return -1;
    }
    if (ensure_init() != 0) {
        return -1;
    }
    Entry *e = find_entry(addr);
    if (e == NULL) {
        return -1;
    }
    EFI_USB_DEVICE_DESCRIPTOR  desc = { 0 };
    EFI_STATUS status = axl_efi_call(
        e->io->UsbGetDeviceDescriptor, 2, e->io, &desc);
    if (EFI_ERROR(status)) {
        return -1;
    }
    uint8_t string_index;
    switch (slot) {
    case DESC_STR_MANUFACTURER:  string_index = desc.StrManufacturer;  break;
    case DESC_STR_PRODUCT:       string_index = desc.StrProduct;       break;
    case DESC_STR_SERIAL:        string_index = desc.StrSerialNumber;  break;
    default:                     return -1;
    }
    if (string_index == 0) {
        return -1;
    }
    return axl_usb_get_string(addr, string_index, buf, buflen);
}

int
axl_usb_get_manufacturer(AxlUsbAddr addr, char *buf, size_t buflen)
{
    return get_string_for_slot(addr, DESC_STR_MANUFACTURER, buf, buflen);
}

int
axl_usb_get_product(AxlUsbAddr addr, char *buf, size_t buflen)
{
    return get_string_for_slot(addr, DESC_STR_PRODUCT, buf, buflen);
}

int
axl_usb_get_serial(AxlUsbAddr addr, char *buf, size_t buflen)
{
    return get_string_for_slot(addr, DESC_STR_SERIAL, buf, buflen);
}

// ---------------------------------------------------------------------------
// Topology walk (Phase F)
// ---------------------------------------------------------------------------

/* The entry array is already sorted by (bus_key, dev_key, intf) at
   ingest time. dev_key is the device-path bytes through the last
   USB node (with InterfaceNumber zeroed for dedup), so consecutive
   USB nodes' bytes appear in port-chain order. Lex-sort over
   dev_key therefore lex-sorts over port chains: parents (shorter
   chains) sort before children (longer chains that share the
   parent's prefix), and siblings sort by port number.

   The walker just iterates the sorted array and emits each entry
   with depth = n_ports - 1. Direct attachment to the root hub
   yields n_ports == 1 → depth 0; one hub in between yields
   n_ports == 2 → depth 1; etc. */
int
axl_usb_tree_for_each(
    AxlUsbTreeFn  fn,
    void         *ctx
    )
{
    if (fn == NULL) {
        return AXL_ERR;
    }
    if (ensure_init() != 0) {
        return AXL_ERR;
    }
    size_t n = axl_array_len(entries);
    for (size_t i = 0; i < n; i++) {
        Entry   *e = (Entry *)axl_array_get(entries, i);
        /* n_ports >= 1 by construction: slice_device_path rejects
           paths without a USB node, and extract_port_chain always
           writes the leaf port for any accepted path. */
        unsigned depth = (unsigned)(e->n_ports - 1);
        int rc = fn(e->addr, depth, ctx);
        if (rc != 0) {
            return rc;
        }
    }
    return AXL_OK;
}

int
axl_usb_get_class(
    AxlUsbAddr   addr,
    uint8_t     *class_,
    uint8_t     *sub,
    uint8_t     *prot
    )
{
    if (ensure_init() != 0) {
        return AXL_ERR;
    }
    Entry *e = find_entry(addr);
    if (e == NULL) {
        return AXL_ERR;
    }
    EFI_USB_INTERFACE_DESCRIPTOR  desc = { 0 };
    EFI_STATUS status = axl_efi_call(
        e->io->UsbGetInterfaceDescriptor, 2, e->io, &desc);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    /* Each out parameter is independently optional — composite
       devices that only need the base class for routing can pass
       NULL for the others. */
    if (class_ != NULL) {
        *class_ = desc.InterfaceClass;
    }
    if (sub != NULL) {
        *sub = desc.InterfaceSubClass;
    }
    if (prot != NULL) {
        *prot = desc.InterfaceProtocol;
    }
    return AXL_OK;
}
