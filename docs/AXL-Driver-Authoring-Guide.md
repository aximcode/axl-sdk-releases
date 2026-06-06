# Writing a UEFI Driver with AXL

This guide shows how to author a UEFI driver in plain C with AXL — no
EDK2 source tree, no hand-rolled EFIAPI boilerplate. It covers the two
kinds of driver and which to reach for.

For the design rationale and the phased plan behind this surface, see
[AXL-Driver-Authoring-Design.md](AXL-Driver-Authoring-Design.md).

## 1. A driver is not an app

An AXL *app* has `int main(int argc, char *argv[])`, runs to completion,
and exits. A *driver* is a `.efi` with the EFI **driver** subsystem: the
firmware loads it, calls its entry point, and the driver typically stays
**resident** in memory after its entry point returns — publishing a
protocol or running a service that other images use.

Two firmware actions matter:

- **Load / Start** — the firmware (or the shell `load` command, or
  another AXL image via `axl_driver_load`/`axl_driver_start`) maps the
  image and calls its entry point. The entry point does its setup and
  returns; the image stays resident.
- **ConnectController** — the firmware matches a driver that publishes
  `EFI_DRIVER_BINDING_PROTOCOL` against a device and calls its
  `Supported`/`Start`. This is the UEFI *Driver Model* and is only
  relevant to **Type B** below.

Package a driver with:

```bash
axl-cc --type driver mydrv.c -o mydrv.efi    # SDK toolchain
# in-tree: add a Makefile target modeled on the `driver` example
```

`--type driver` selects the driver subsystem and the driver CRT0 wiring
(no `argv`, no `int main`).

## 2. Type A — a resident service / protocol publisher

This is the common case: load, publish one or more protocols (or run a
resident loop / RPC service), stay resident, unload cleanly. It is *not*
driven by `ConnectController`. Almost every AXL driver today is Type A.

### 2.1 Entry and unload

Use the `AXL_DRIVER` macro. It emits the firmware-side `DriverEntry` and
`Unload` stubs and wires `axl_driver_init` (sets up `gST`/`gBS`/`gRT` and
the AXL I/O streams) and `axl_driver_set_unload` for you, so you write
plain AXL C:

```c
#include <axl.h>

static int  my_main(AxlHandle image, AxlSystemTable *st);
static int  my_unload(AxlHandle image);

AXL_DRIVER(my_main, my_unload)

static int
my_main(AxlHandle image, AxlSystemTable *st)
{
    (void)image; (void)st;     /* axl_driver_init already wired by the macro */
    /* ... publish protocols / start a service ... */
    return 0;                  /* 0 = success; non-zero aborts the load */
}

static int
my_unload(AxlHandle image)
{
    (void)image;
    /* ... tear down everything my_main set up (see §2.3) ... */
    return 0;
}
```

### 2.2 Publishing a protocol

`axl_protocol_install` publishes an interface so other images can find it
via the locate/handle APIs. No `<uefi/...>` include or `gBS->` drop-down:
`AxlHandle` is `EFI_HANDLE` and `AxlGuid` is binary-compatible with
`EFI_GUID`.

```c
static MyServiceIface  iface = { .do_thing = my_do_thing };
static AxlHandle        my_handle = NULL;   /* fresh handle on first install */

/* The in/out handle is the LAST argument (guid, iface, handle). */
if (axl_protocol_install(&MY_SERVICE_GUID, &iface, &my_handle) != AXL_OK) {
    axl_error("failed to publish MyService");
    return 1;                  /* aborts the load */
}
```

- Pass a pointer to a **NULL** handle to publish on a **fresh** handle —
  the firmware allocates one and writes it back through `handle`. This is
  the usual "publish a new service" case.
- Pass an **existing** handle to add another protocol to it.
- The interface is **borrowed, not copied**: it must stay valid until you
  uninstall it. For a resident driver, that means it must outlive your
  image — so allocate it from a boot-services pool (or use a `static`, as
  above), **not** `axl_malloc` (which the AXL leak tracker reclaims at
  image exit).

### 2.3 Staying resident and unloading cleanly

A successful Type-A entry point returns `0` and the image stays resident.
The firmware does **not** clean up after you on unload — your unload
callback must reverse everything the entry point did:

- **Uninstall every protocol you published**, with the *same* interface
  pointer you installed:

  ```c
  axl_protocol_uninstall(my_handle, &MY_SERVICE_GUID, &iface);
  ```

  Uninstall fails (`AXL_ERR`) if another image still has the protocol
  open — unload only once dependents have released it. Leaving a protocol
  installed past unload leaves a dangling interface pointer into freed
  driver memory; the next consumer's `LocateProtocol` hands back a stale
  vtable and the following call faults.
- Free any boot-services-pool allocations, close events/timers, stop
  resident loops.

### 2.4 Worked example

[`sdk/examples/smbus-hc-shim.c`](../sdk/examples/smbus-hc-shim.c) is a
real Type-A driver: it finds the ICH9 SMBus controller and publishes
`EFI_I2C_MASTER_PROTOCOL` so `AxlSmbus` consumers can drive it. Its entry
point reduces to "probe hardware, fill the vtable, `axl_protocol_install`,
return."

### 2.5 Higher-level Type-A patterns

If your driver is more than a bare protocol publisher, two ready-made
layers sit on top of this primitive:

- **`<axl/axl-shared-driver.h>`** — the synchronous-RPC "thin launcher +
  resident driver" pattern (a launcher app talks to a resident driver
  over a protocol). See
  [AXL-Shared-Driver-Recipe.md](AXL-Shared-Driver-Recipe.md).
- **`AxlService` / `AXL_SERVICE_DRIVER`** — a structured lifecycle
  wrapper (start/stop/status, event loop, config) for service drivers.
  See `<axl/axl-service.h>`.

## 3. Type B — a UEFI Driver Model driver

A Type-B driver publishes `EFI_DRIVER_BINDING_PROTOCOL` and is bound to
controllers by the firmware's `ConnectController` (`Supported` → `Start`
→ `Stop`) — what a NIC, storage, or bus driver is.

### 3.1 What AXL manages vs. what you write

The Driver Model's fiddliest mechanics are exactly what `AxlDriverBinding`
(`<axl/axl-driver.h>`) manages — you never touch them:

- the **EFIAPI `Supported`/`Start`/`Stop` thunks** (marshalling
  `EFI_HANDLE` ↔ `AxlHandle`, `EFI_STATUS` ↔ `int`, hiding the calling
  convention);
- the **`OpenProtocol(BY_DRIVER)` / `CloseProtocol` ownership
  bookkeeping** around the protocol you bind — the load-bearing mechanic
  that tags controller ownership and prevents double-binding;
- **building and installing** `EFI_DRIVER_BINDING_PROTOCOL` (Version,
  ImageHandle, DriverBindingHandle) **and** `EFI_COMPONENT_NAME2_PROTOCOL`
  (so the shell `drivers` listing shows your driver's name).

You write a descriptor and three callbacks, in pure AXL C — `AxlHandle`,
no `EFI_HANDLE`, no `EFIAPI`, no `OpenProtocol` dance:

```c
typedef struct {
    const char    *name;     // → Component Name 2 (the `drivers` listing)
    const AxlGuid *binds;    // protocol a controller must expose to be managed
    bool (*supported)(AxlHandle controller, void *ctx);          // optional gate
    int  (*start)(AxlHandle controller, void *iface, void *ctx); // 0 = AXL_OK
    int  (*stop)(AxlHandle controller, void *ctx);
    void *ctx;
} AxlDriverBinding;

int axl_driver_binding_install(const AxlDriverBinding *db);  // from your entry
```

- **`supported`** is an optional extra gate, called *after* AXL has
  confirmed `binds` is present on the controller and openable
  `BY_DRIVER`. Return `true` to manage the controller. Keep it
  side-effect-free — the firmware runs it as a pure query against many
  controllers. (NULL = manage any controller exposing `binds`.)
- **`start`** is where you initialise the device. AXL has already claimed
  `binds` `BY_DRIVER` and hands you the bound interface as `iface`.
- **`stop`** tears down what `start` built. AXL closes `binds` afterward.

### 3.2 The one unavoidable raw-EFI touch — the bound interface

`start` receives the **actual bound protocol** as `void *iface`. AXL
cannot hide it, because *operating that protocol is the driver's job*:

```c
static int
my_start(AxlHandle controller, void *iface, void *ctx)
{
    EFI_PCI_IO_PROTOCOL *pci = iface;   // the one honest raw-EFI cast
    /* ...drive the device through `pci`... */
    return AXL_OK;
}
```

Where AXL has a typed module for the bound protocol, use it. AXL hides the
handle and the bookkeeping; the bound interface is the single place an EFI
type legitimately surfaces.

### 3.3 Installing and being driven

Call `axl_driver_binding_install` once from your `AXL_DRIVER` entry point.
AXL copies the descriptor (a stack value is fine; `name` and `binds` are
borrowed and must outlive the driver — literals/`static` are the norm). After
that, the firmware's `ConnectController` drives your callbacks against
matching controllers.

**Tear it down from your unload callback** with
`axl_driver_binding_uninstall` — firmware-driven driver unload does *not*
drain `axl_atexit`, so leaving the binding installed would dangle
`EFI_DRIVER_BINDING_PROTOCOL` on the freed image handle (a crash on the next
`connect`/`drivers`). Disconnect any controllers you manage first
(`axl_driver_disconnect_handle`) so the binding is unreferenced when you
uninstall it. (AXL also registers an `axl_atexit` hook, but it only covers an
*app* that installs a binding, not a driver being unloaded.)

To exercise it under QEMU, `load` the driver and `connect` a controller:

```
Shell> load binding-driver.efi
Shell> drivers                 # your name appears via Component Name 2
Shell> connect <handle>        # firmware runs Supported → Start
Shell> disconnect <handle>     # firmware runs Stop
```

`axl_driver_connect_handle` / `axl_driver_disconnect_handle` are the AXL
wrappers if you need to drive a specific controller handle from code.

### 3.4 Worked example

[`sdk/examples/binding-driver.c`](../sdk/examples/binding-driver.c) is the
canonical Type-B driver and **includes no `<uefi/...>` header at all**. To
stay self-contained and runnable on any machine, it plays both halves of
the relationship: it publishes a synthetic "widget" controller protocol
(standing in for a real firmware-enumerated device) *and* installs an
`AxlDriverBinding` that manages it. Its entry point then drives the
lifecycle the firmware normally would — connect, then disconnect — so
simply loading it prints the whole `Supported → Start → Stop` walk. In
production the two halves live in separate images and the firmware drives
`ConnectController`.

### 3.5 v1 scope and the escape hatch

v1 is the 90% case: *"this driver binds to controllers exposing protocol
X"* — a device / function driver, one binding per driver image. Two
honest limits:

- **Bus drivers are deferred to v2** — `RemainingDevicePath`, child-handle
  creation, and `Stop`'s child buffer aren't surfaced. For those, drop to
  the raw `EFI_DRIVER_BINDING_PROTOCOL` (see §4); the AXL layer is always
  additive, never a wall.
- **A second `axl_driver_binding_install` in one image returns `AXL_ERR`**
  (the firmware rejects a duplicate `EFI_DRIVER_BINDING_PROTOCOL` on the
  image handle). A driver that must bind several protocols from one image
  uses the raw protocol for the extras.

## 4. Choosing A vs B, and the escape hatch

- **Publishing a service / protocol, or running a resident loop, with no
  device binding?** → **Type A**. `AXL_DRIVER` + `axl_protocol_install`.
- **Binding to a controller the firmware enumerates (PCI / USB / a custom
  bus)?** → **Type B**. `AXL_DRIVER` + `AxlDriverBinding`.

The raw `gBS->` slots and the typed `<uefi/...>` headers
(`EFI_DRIVER_BINDING_PROTOCOL` included) remain public and usable. The
AXL ergonomic layer is always **additive** — anything it doesn't cover
can still be hand-written against the typed headers. There is no wall.

## 5. Reference

- `<axl/axl-driver.h>` — `axl_protocol_install` / `axl_protocol_uninstall`
  (Type A), `axl_driver_binding_install` / `axl_driver_binding_uninstall` +
  `AxlDriverBinding` (Type B),
  `axl_driver_init`, `axl_driver_set_unload`, and the
  load/start/connect/disconnect/unload APIs (including
  `axl_driver_connect_handle` / `axl_driver_disconnect_handle`) for driving
  *other* drivers and controllers.
- `AXL_DRIVER` macro — `<axl.h>`.
- Design + phase tracker —
  [AXL-Driver-Authoring-Design.md](AXL-Driver-Authoring-Design.md).
