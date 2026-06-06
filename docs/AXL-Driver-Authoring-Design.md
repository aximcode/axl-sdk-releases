# AXL Driver Authoring — Design

**Status:** Phase 1 DONE (`axl_protocol_install`/`_uninstall`, the
`tables.h` typedef fix, the protocol-publish dogfood, and the Type-A guide
— see [AXL-Driver-Authoring-Guide.md](AXL-Driver-Authoring-Guide.md)).
**Phase 2 DONE** (the `AxlDriverBinding` Type-B layer — contract, EFI
prerequisites, the managed thunks, and in-QEMU validation; §5).
**Phase 3 DONE** (the canonical `sdk/examples/binding-driver.c` example,
the `axl_driver_disconnect_handle` symmetric wrapper it surfaced, the §3
Type-B guide section, and a `load`+lifecycle integration test;
`test/integration/test-binding-driver.sh`).
**Goal:** make AXL a first-class way to *author* UEFI drivers — both
resident service/protocol-publisher drivers and full UEFI Driver Model
(controller-binding) drivers — in clean C, with no EDK2 source tree and
(as far as the Driver Model allows) no raw EFI types leaking into
consumer code.

This is a public-SDK capability: the value is enabling driver authors who
don't exist yet. See the design-rigor note in §3 for how we validate an
API with no shipped downstream.

---

## 1. Current state (corrected)

AXL can already author DXE drivers. The earlier roadmap "open items"
(type the driver Boot-Services slots; add `EFI_DRIVER_BINDING_PROTOCOL`;
shell-parameter parsing) are all **done** — the checkboxes lagged:

- `InstallProtocolInterface` / `Reinstall` / `Uninstall` / `OpenProtocol`
  / `InstallMultipleProtocolInterfaces` are typed funcptrs in
  `include/uefi/generated/tables.h` (not `void *`).
- `EFI_DRIVER_BINDING_PROTOCOL` (`Supported`/`Start`/`Stop`) is in
  `include/uefi/generated/driver-model.h`.
- `EFI_SHELL_PARAMETERS_PROTOCOL` argv extraction is wired in
  `src/posix/axl-app.c`.

What exists and works:

| Capability | Surface |
|---|---|
| Driver entry/unload wiring (AXL-typed) | `AXL_DRIVER(entry, unload)`, `axl_driver_init`, `axl_driver_set_unload` |
| Load/start/connect/unload *other* drivers | `axl_driver_load` / `_start` / `_connect` / `_disconnect` / `_unload` |
| Resident "thin launcher + driver" pattern | `<axl/axl-shared-driver.h>` + `AXL-Shared-Driver-Recipe.md` |
| Structured lifecycle service | `AxlService`, `AXL_SERVICE_DRIVER` |
| Real driver examples | `sdk/examples/{driver,smbus-hc-shim,http-server-driver,shared-driver-demo}` |
| Real consumers | AxlServiceDxe (delldiags), axl-webfs-dxe, CmdServe |

### Relationship to prior design work

This design is the unfinished portion of an existing plan, not a new
direction:

- **`AXL-EFI-Encapsulation-Plan.md`** sets the same goal — consumers
  write zero `EFI_*` identifiers "even when authoring drivers, publishing
  protocols, or implementing UEFI spec interfaces." Its **Phase C
  shipped** `<axl/axl-fs-provider.h>`: a consumer-vtable → SDK-emitted
  EFIAPI-thunk pattern that publishes `EFI_FILE_PROTOCOL` without the
  consumer touching EFI types. **`AxlDriverBinding` (§4.2) reuses exactly
  that pattern** for `EFI_DRIVER_BINDING_PROTOCOL`. The encapsulation
  plan stopped at filesystem publishing; the UEFI **Driver Model** is the
  part of its own "authoring drivers" goal it never built — this design
  completes it.
- **ROADMAP Phase P2** (backend abstraction) already names
  `axl_backend_install_protocol`. `axl_protocol_install` (§4.1) is the
  *public* surface over that *backend* seam — see §4.1.

## 2. The two driver kinds

**Type A — resident service / protocol publisher.** Loads, installs one
or more protocols (or just runs a resident loop / RPC service), stays
resident, unloads cleanly. Not driven by `ConnectController`. This is
every AXL driver today and ~90% of pre-boot service/diagnostic needs.
**Fully supported and ergonomic** via `AXL_DRIVER` + (after Phase 1)
`axl_protocol_install`.

**Type B — UEFI Driver Model controller-binding driver.** Publishes
`EFI_DRIVER_BINDING_PROTOCOL`; the firmware calls `Supported`/`Start`/
`Stop` to bind it to controllers (a PCI device, a USB interface, a custom
bus child). This is what a NIC, storage, or bus driver is. **The protocol
type exists, but there is no AXL ergonomic layer** — today an author
hand-rolls EFIAPI `Supported`/`Start`/`Stop` with raw `EFI_HANDLE` /
`EFI_DEVICE_PATH_PROTOCOL`, installs the binding by hand, and performs the
`OpenProtocol(BY_DRIVER)` / `CloseProtocol` bookkeeping themselves. That
leaks EFI types and the Driver Model's fiddliest mechanic into consumer
code — exactly what AXL exists to hide. **Closing this is the point of
this design.**

## 3. Design principle for an unvalidated public API

There is no shipped Type-B consumer yet, and for a public SDK that's
fine — enabling one is the product. But a public API is the most
expensive thing to get wrong (downstreams pin to it). So the rigor that
*substitutes* for a real consumer:

1. **Design-first** — this doc, then a header contract with a
   contract-first review.
2. **Spike the hard parts** — a throwaway Driver Model driver proving the
   `OpenProtocol`/thunk mechanics before the API is fixed.
3. **A real example-as-consumer** — a genuine binding driver that
   exercises the API end to end. If the abstraction makes it read clean,
   the contract is right; the example ships as the canonical reference.
4. **An integration test** — QEMU `load` + `connect` that drives
   `Supported`→`Start` against an emulated controller.

## 4. Unified design

One surface (`<axl/axl-driver.h>`) over a shared primitive.

### 4.1 The primitive (serves both types)

```c
int axl_protocol_install(AxlHandle handle, const AxlGuid *guid, void *iface);   /* 0 = ok */
int axl_protocol_uninstall(AxlHandle handle, const AxlGuid *guid, void *iface);
```

AXL-typed wrappers over `InstallProtocolInterface` /
`UninstallProtocolInterface`. A `NULL` handle installs on a fresh handle
(out-param variant TBD). Type-A drivers call these directly; the Type-B
layer uses them internally. Removes the last `gBS->` drop-down from
`smbus-hc-shim` et al.

**Layering (reconciles ROADMAP Phase P2).** P2 already names the
*backend* function `axl_backend_install_protocol(handle, guid, interface)`
(the internal portability seam, for a future non-UEFI backend).
`axl_protocol_install` is the **public** driver-author surface; it is
implemented *over* `axl_backend_install_protocol`, exactly as the rest of
the public API sits on `axl_backend_*`. They are two layers, not two
competing names — building `axl_protocol_install` is what finally
fulfills P2's `axl_backend_install_protocol` line. Note `axl-fs-provider`
already performs this install internally today, so the underlying
plumbing partly exists and can be factored out rather than written fresh.

### 4.2 The Driver Model layer (Type B)

```c
typedef struct {
    const char    *name;            /* → Component Name 2 */
    const AxlGuid *binds;           /* protocol on controllers this driver manages */
    bool (*supported)(AxlHandle controller, void *ctx);            /* optional extra gate */
    int  (*start)(AxlHandle controller, void *iface, void *ctx);   /* 0 = ok */
    int  (*stop)(AxlHandle controller, void *ctx);
    void *ctx;
} AxlDriverBinding;

int axl_driver_binding_install(const AxlDriverBinding *db);   /* call from AXL_DRIVER entry */
```

**What AXL manages — the actual value, the part nobody wants to hand-roll:**

- The **EFIAPI `Supported`/`Start`/`Stop` thunks**: marshal
  `EFI_HANDLE → AxlHandle`, `EFI_STATUS ↔ int`, hide the calling convention.
- The **`OpenProtocol(BY_DRIVER)` / `CloseProtocol` bookkeeping** around
  `binds` — the load-bearing Driver Model mechanic that tags controller
  ownership and prevents double-binding:
  - `Supported` → `OpenProtocol(..BY_DRIVER..)` test on `binds`
    (handling `EFI_ALREADY_STARTED`), close, then the optional
    `supported` gate; reports manageability.
  - `Start` → `OpenProtocol(..BY_DRIVER..)` on `binds` (tagging
    ownership, handing the interface to the callback), call `start`,
    roll back (`CloseProtocol`) if it returns non-zero.
  - `Stop` → call `stop`, then `CloseProtocol`.
- Building the `EFI_DRIVER_BINDING_PROTOCOL` struct (Version, ImageHandle,
  DriverBindingHandle) and installing it + Component Name 2 via §4.1.

The consumer writes pure AXL C: `AxlHandle`, no `EFI_HANDLE`, no EFIAPI,
no OpenProtocol dance.

### 4.3 Deliberate v1 scope decisions

- **Opinionated around "device driver binds to protocol X."** `binds`
  drives the managed open/close. Covers the 90% case (drivers attaching
  to controllers exposing PCI I/O, USB I/O, a custom bus protocol).
- **The bound `iface` stays raw.** `start` receives the actual bound
  protocol (e.g. `EFI_PCI_IO_PROTOCOL *`) — AXL can't hide it, because
  operating that protocol *is* the driver's job. AXL hides the handle and
  the bookkeeping; use AXL's typed module for the bound protocol where
  one exists. This is the one honest, unavoidable EFI-type touch.
- **Bus-driver territory deferred to v2** — `RemainingDevicePath`,
  child-handle creation, `Stop`'s child buffer. The genuinely hard, less
  common case. A v1 that faked it would be the "half-baked abstraction
  worse than raw" failure. The raw `EFI_DRIVER_BINDING_PROTOCOL` stays
  available as the escape hatch; see §6.

## 5. Phases

### Phase 1 — cheap, non-speculative — DONE

- [x] `axl_protocol_install` / `_uninstall` (+ `AxlTestDriver`; both
  arches) over the new `axl_backend_install_protocol` seam.
- [x] Fix the `tables.h` typedef typo: `InstallMultipleProtocolInterfaces`
  was typed `EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES` (harmless —
  identical signature — but wrong). Fixed at the generator (a targeted
  spec-typo rewrite), not by editing the generated header.
- [x] Migrate `smbus-hc-shim` onto `axl_protocol_install` (dogfood).
- [x] Write the **Type-A section** of the guide
  ([AXL-Driver-Authoring-Guide.md](AXL-Driver-Authoring-Guide.md)).
- [x] **Consolidate the protocol-publish overlap (dogfood, `9182922d`).**
  `axl-protocol.c` had a parallel `axl_protocol_register_guid` /
  `_unregister_guid` that hand-rolled raw `gBS->InstallProtocolInterface` —
  duplicating the new primitive. Removed them; migrated every consumer
  (`axl-shm`, `axl-service`, `axl-shared-driver`, tests) onto
  `axl_protocol_install` / `_uninstall`; reimplemented the name-based family
  (`register` / `register_multiple` / `unregister`, which add the name→GUID
  lookup) over the primitive. `axl-protocol.c` now makes **zero** raw `gBS`
  protocol calls — the backend seam (§4.1) is the single place that talks to
  `gBS`, proving the layering. Also reordered `axl_protocol_install` so the
  in/out handle is **last** (style guide "output args last"). *(Remaining raw
  `gBS->InstallProtocolInterface` is the backend seam itself plus one
  backend-internal TSC-freq publish — both legitimately at the gBS layer.)*

### Phase 2 — design + spike (the real work)

- [x] **`AxlDriverBinding` header contract** (`<axl/axl-driver.h>`,
  `d3a4469a`) — `{name, binds, supported, start, stop, ctx}` +
  `axl_driver_binding_install`. Contract-first reviewed *with the user*:
  Version omitted (AXL default; append-compatible later); per-controller
  state via shared `ctx` keyed by the controller handle; `name` required
  (Component Name 2 always installed); `start`'s `iface` stays raw.
- [x] **EFI prerequisites** (`7c97f769`) — `EFI_OPEN_PROTOCOL_BY_DRIVER`
  (+ `BY_CHILD_CONTROLLER`/`EXCLUSIVE`) in `axl-uefi-extra.h`;
  `EFI_COMPONENT_NAME2_PROTOCOL` (+ the shared `GET_*_NAME` funcptrs) added
  to the manifest and regenerated. `EFI_DRIVER_BINDING_PROTOCOL`,
  `gBS->OpenProtocol/CloseProtocol/ConnectController`, `EFI_ALREADY_STARTED`
  already existed.
- [x] **Implemented the managed thunks** (`axl-driver.c`). The EFIAPI
  `Supported`/`Start`/`Stop` trampolines recover the `AxlBindingRec` from
  `This` (`EFI_DRIVER_BINDING_PROTOCOL` is the record's first member; Component
  Name 2 by `offsetof`); the `OpenProtocol(BY_DRIVER)`
  test/claim/`CloseProtocol` bookkeeping around `binds` (handling
  `EFI_ALREADY_STARTED`, rolling back the open if `start` fails); Component
  Name 2's `GetDriverName`; `axl_atexit`-driven uninstall+free at image
  teardown — freeing **only** when the binding uninstall succeeds, so a
  firmware-referenced thunk is never left dangling into freed memory (a
  free-and-dangle bug caught + fixed; 4× aa64 stability runs after).
  **The spike IS the test** (`test_driver_binding`): it installs a synthetic
  protocol on a fresh handle (a controller), installs the binding, and drives
  the **real OVMF** `gBS->ConnectController` / `DisconnectController` in the
  unit harness — asserting `Supported`→`Start`(controller+iface) and `Stop`
  fire. Independent-reviewed; X64 + AARCH64 green.
  - **v1 scope decision — one binding per driver image.** Both protocols
    install on `gImageHandle` (`DriverBindingHandle == ImageHandle`, the
    standard single-binding EDK2 shape); a second `axl_driver_binding_install`
    returns `AXL_ERR` (duplicate `EFI_DRIVER_BINDING_PROTOCOL` rejected) —
    pinned by a test. Matches §4.3's "device driver binds to protocol X".
    Multi-binding (one binding per fresh handle, with the `DisconnectController`
    agent-handle semantics that implies) is a demand-driven follow-on; the raw
    protocol is the escape hatch meanwhile.

Phase 3 (productionize) then added the canonical `binding-driver.c`
example, the Type-B guide section (§3 of the guide), and a `load`+lifecycle
integration test — the core impl + in-QEMU validation already landed in
Phase 2 here.

### Phase 3 — productionize — DONE

- [x] `axl_driver_binding_install` implemented test-first, both arches
  (landed in Phase 2 — the spike WAS the impl).
- [x] **Canonical Type-B example** (`sdk/examples/binding-driver.c`) — a
  self-contained driver that publishes a synthetic "widget" controller and
  manages it via `AxlDriverBinding`, with **zero `<uefi/...>` includes**.
  Its entry self-drives connect/disconnect so a bare `load` walks the whole
  `Supported → Start → Stop` lifecycle. Built both arches, run-QEMU-verified.
- [x] **`axl_driver_disconnect_handle`** — the example surfaced a missing
  symmetric counterpart to `axl_driver_connect_handle` (there was no
  controller-handle disconnect in pure AXL). Added test-first, both arches;
  lets the example fire `Stop` without dropping to `<uefi/...>`.
- [x] **Integration test** (`test/integration/test-binding-driver.sh`):
  `load`s the driver and asserts each lifecycle stage fired in the bound
  interface's own data (`model=AXL-Widget-9000 rev=2`), both arches. Run
  locally (like `test-driver.sh`, not wired into ci.yml).
- [x] **Type-B section of the guide** (§3 of
  [AXL-Driver-Authoring-Guide.md](AXL-Driver-Authoring-Guide.md)): what AXL
  manages vs. what you write, the raw-bound-`iface` rule, install + how the
  firmware drives it, testing under QEMU `connect`, v1 scope + escape hatch.

### Phase 4 — v2 / future (consumer- or demand-driven)

- Bus drivers: `RemainingDevicePath`, child handle creation, `Stop`
  child buffer.
- `EFI_DRIVER_DIAGNOSTICS2` / localized Component Name.
- Runtime-driver specifics (`axl-cc --type runtime` exists; document the
  SetVirtualAddressMap constraints).

## 6. Escape hatch

The raw `EFI_DRIVER_BINDING_PROTOCOL` and `gBS->` slots remain public and
usable. Anything the ergonomic layer doesn't cover (bus drivers in v1,
exotic binding logic) can still be hand-written against the typed
headers — the abstraction is additive, never a wall.

## 7. Deliverable: "Writing a DXE Driver with AXL" (both types)

A user-facing guide (`docs/` + a Sphinx guide page), structured:

1. **What a UEFI driver is** vs. an app; the `.efi` subsystem; load vs.
   ConnectController.
2. **Type A — a resident service / protocol publisher.** `AXL_DRIVER`
   entry/unload, `axl_protocol_install`, staying resident, clean unload.
   Worked example from `smbus-hc-shim`. *(Writable in Phase 1 — it's
   real today.)*
3. **Type B — a UEFI Driver Model driver.** `AxlDriverBinding`,
   `supported`/`start`/`stop`, what AXL manages vs. what you write, the
   raw-bound-interface rule, testing under QEMU `connect`. Worked example
   from `sdk/examples/binding-driver.c`. *(Done in Phase 3.)*
4. **Choosing A vs. B**, the escape hatch, packaging (`axl-cc --type
   driver`), and load/unload in the shell.

## 8. Open decision

**How opinionated is v1 Driver Binding?** Recommended: the managed
`OpenProtocol(BY_DRIVER)`-around-a-single-`binds`-protocol model (§4.2) —
the bookkeeping *is* the value. The thinner alternative ("AXL marshals
the thunks, you do the opens") is more flexible but hands the fiddly part
back and is barely better than raw. Leaning strongly opinionated.
