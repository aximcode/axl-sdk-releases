# AXL SoftBMC Port — Scoping & Migration Design

**Status:** DRAFT — living doc, for iteration. Opened 2026-06-11.
**Scope:** re-base **SoftBMC** (the BMC firmware application) off EDK2 onto
**axl-sdk + AGT**, end to end. This is the *scoping pass*: the migration
strategy, an inventory template to fill against the SoftBMC source, the
known module → axl-sdk mapping, and — the real payoff — the concrete list
of **substrate gaps the port will pull**, which turns the speculative
dashboard / HII wishlists into a consumer-validated requirements list.

Companion docs:
[AXL-Dashboard-Server-Design.md](AXL-Dashboard-Server-Design.md),
[AXL-HII-Design.md](AXL-HII-Design.md). SoftBMC is the flagship consumer
that drives both.

> **Inventory done 2026-06-11** against `../softbmc` (the EDK2 SoftBMC:
> `SoftBmcPkg` + `AppRuntimePkg`, ~99 .c / 154 .h, Phases 1–4c shipped).
> §3 below is the real module map, not a template.

---

## 0. Why this is the flagship, and why scope-first

SoftBMC is the **consumer that validates the substrate**. axl-sdk's
networking, BMC access (IPMI/SMBus/SPD/PCI/USB), graphics, async, and
fixtures already exist largely *because* downstream consumers pulled them
(the roadmap is full of "consumer-driven" additions). The remaining big
substrate pieces — the dashboard server and the HII forms engine — are
best built **as SoftBMC pulls them**, not speculatively: the §0 line of
both companion docs (mechanism in axl-sdk, policy/rendering in the
consumer) is only verifiable against a real consumer's load and threat
model.

So the order is **scope → thin vertical slice → build substrate
just-in-time**, not "build the substrate, then port." This doc is the
scope step.

---

## 1. Migration principles

1. **The existing SoftBMC code is the reference — port behavior, don't
   reinvent it.** The EDK2 SoftBMC in `../softbmc` is the behavioral spec:
   its routes, JSON shapes, WebSocket/RFB protocols, config keys, auth/session
   semantics, dashboard UX, and the hand-rolled implementations
   (`Core/Auth.c` + `Session.c`, `UefiSetup/HiiParse.c`, the RFB encoder, the
   alert transports, the EC chip maps) define what the ported version must
   reproduce. Read the original module before porting it; preserve parity
   (same input → same output / same wire protocol) unless we deliberately
   change it. Where axl-sdk replaces in-tree infra (§3.1), diff the behavior,
   don't just delete blindly — the TelCon `Core/` is the contract the
   axl-sdk calls have to meet.
2. **Keep it bootable at every step.** Never a months-long "big bang"
   branch. Each merge leaves SoftBMC building and running, even if a module
   is still EDK2-backed behind a seam.
3. **Thin vertical slice first.** Get the *simplest* end-to-end path live on
   axl-sdk early (build re-based onto `axl-cc` + one HTTP endpoint serving a
   static dashboard shell). Surfaces real toolchain/integration pain when
   it's cheap to fix.
4. **Substrate is pulled, not pushed.** When the port hits a gap (SSE for
   telemetry, HII for BIOS attributes, session auth), build that substrate
   *then*, against the concrete need — and upstream it into axl-sdk with
   the mechanism/policy line drawn per the companion docs.
5. **App vs driver shape.** SoftBMC is (mostly) a long-running application;
   parts may be drivers (e.g. an EC/sensor provider). axl-sdk supports both
   (`axl-cc --type app|driver`, the shared-driver recipe). Decide per module.
6. **No EDK2 leakage in the end state.** The port is done when SoftBMC
   builds with `axl-cc` against no EDK2 tree (the same bar axl-sdk holds).

---

## 2. End-state architecture (target)

```
              SoftBMC.efi  (axl-cc app, no EDK2)
  ┌───────────────────────────────────────────────────────────┐
  │  Web dashboard  ──  Redfish/REST  ──  (local AGT UI, opt.)  │  front-ends
  ├───────────────────────────────────────────────────────────┤
  │  Dashboard server (SSE/static/auth)   axl-hii (BIOS attrs)  │  app logic
  │  firmware-update  VNC/remote-console  sensors/EC/SEL         │
  ├───────────────────────────────────────────────────────────┤
  │  axl-sdk: HTTP/TLS · AxlIpmi/Smbus/Spd/Pci/Usb · AxlGfx +    │  substrate
  │  pointer seat · AxlAsync/BufPool/Pubsub · AxlFs/Nvstore/Shm  │
  │  · AxlLog · native CRT0 + backend                           │
  └───────────────────────────────────────────────────────────┘
```

---

## 3. Inventory (real, 2026-06-11)

### 3.0 Headline finding — this is mostly "delete `Core/`, call axl-sdk"

SoftBMC's `Core/` is largely **hand-rolled infrastructure inherited from a
predecessor project ("TelCon")** — and axl-sdk now provides essentially all
of it as tested, released substrate. Plus `AppRuntimePkg` (ArenaLib /
RuntimeTimerLib / TaskPoolLib) is a pre-axl-sdk runtime that
**AxlArena / AxlLoop / AxlTask supersede outright** (delete the whole
package). So the bulk of the port is *replacing in-tree infra with library
calls*, not rewriting features — a far more tractable shape than feared.

The **genuine new substrate** the port pulls is small and specific: it's the
two companion sub-projects, and the SoftBMC tree already contains the
hand-rolled proof of each:
- `Modules/Feature/UefiSetup/HiiParse.c` — a hand-rolled HII parser →
  validates **`axl-hii`**.
- `Core/Auth.c` + `Core/Session.c` — hand-rolled auth/session →
  validates the dashboard **session/RBAC** mechanism.

**Size constraint (don't lose it):** SoftBMC targets SPI flash — its design
doc tracks 250 KB–800 KB build profiles. axl-sdk's selective linking should
hold parity, but *measure* the ported binary against the EDK2 one; a
regression here matters for embedded targets.

### 3.1 `Core/` — replace with axl-sdk (delete most of it)

| SoftBMC `Core/` | axl-sdk replacement |
|---|---|
| `Network.c`, `TcpClient/TcpUtil/UdpUtil.c` | AxlNet (`axl_tcp_*`, `axl_udp_*`, DHCP/iface) |
| `HttpServer.c`, `HttpWsHandler.c`, `HttpHelpers.c`, `SoftBmcRoutes.c` | `axl_http_server_*` (routes + middleware) |
| `WebSocket.c` | `axl-http-ws` / `axl-websocket` |
| `WebDav.c` | `axl-http-webdav` |
| `JsonBuilder/JsonParser/JsonPrint.c`, `jsmn.h` | `axl_json` |
| `TlsShim.c` | `axl-tls` (mbedTLS) + `axl_tls_generate_self_signed` |
| `Sha256.c` | AxlDigest (`axl_sha256`) |
| `Config.c` | `axl-config` |
| `Log.c` | `axl-log` (+ ring/file sinks) |
| `RamDisk.c` | AXL RAM-disk (`mkrd` / RamDisk) |
| `MpAccel.c` | AxlTask (MP services) / AxlAsync |
| `StringUtils/TimeUtils/TimeManager/DevicePathUtils.c` | `axl-str` / `axl-time` / `axl-path` |
| `SmbiosUtil.c` | AxlSmbios |
| `ConsoleWrapper.c`, `Tui.c`, `Splash.c` | AxlGfx + `axl-console` |
| `DashboardAssets.c` (embedded web bundle) | `axl-embed` (bundler stays a build step — esbuild) |
| `ShellLauncher/ShellCommands/ShellSetup.c` | AxlImage/`axl_driver_load` + shell wrappers |

### 3.2 `Core/` — keep (app logic), re-host on axl APIs

| SoftBMC `Core/` | Notes |
|---|---|
| `ModuleManager.c` | SoftBMC's plugin/module system — app-level, keep |
| `AlertEngine.c` + `AlertSmtp/Snmp/Syslog/Webhook.c` | app-level event/alert bus over AxlNet/UDP — keep |
| `Auth.c`, `Session.c` | **port onto the dashboard session/RBAC mechanism** (GAP) |
| `EcAccess.c`, `EcRegMap.c` | rehost on AXL I/O-port (`axl-port`) + AxlSmbus |
| `BuildInfo.c` | keep |

### 3.3 `Modules/Feature/*` — keep features; rehost hardware reads

| Module | axl-sdk substrate / disposition |
|---|---|
| `HwInfo/` (Acpi, CpuInfo, Pci, Smbios, Usb, Storage, Tpm, Display, MemMap, FirmwareVol, Serial, UefiVars, Diagnostics) | AxlAcpi / AxlPci / AxlSmbios / AxlUsb / AxlGfx / AxlNvstore — **substrate already ships**; decode/diagnostics stay app-level |
| `RemoteShell/` (`Vt100Parser`) | app + `axl-console`/shell |
| `RemoteKvm/` (`RfbServer`, `RfbEncoding`, `RfbEncHextile`, `KeysymMap`) | **app-side RFB/VNC encoder** rides AxlGfx (capture) + the absolute-pointer/key seat (input inject) |
| `Ec/EcModule` + `Hardware/Ec{Framework,Generic,Ite,Nuvoton}` | app EC chip drivers over `axl-port` + AxlSmbus |
| `BootConfig/`, `OsBoot/`, `PowerControl/` | AxlBoot, AxlImage (OS load), `axl_reset` (axl-sys) |
| `Redfish/` (stub) | `axl_http` + `axl_json`; extract `axl_redfish_*` session helper only if depth warrants |
| `UefiSetup/` (`HiiParse.c`) | **`axl-hii`** engine + AGT `AgtFormBrowser` (or keep web-rendered over the model) |
| `VirtualMedia/` | AXL RAM-disk + `LoadFile2` / virtual-disk GUIDs |

### 3.4 `AppRuntimePkg/` — delete (superseded)

| EDK2 lib | axl-sdk |
|---|---|
| `ArenaLib` | AxlArena |
| `RuntimeTimerLib` | AxlLoop timers / `axl-time` |
| `TaskPoolLib` | AxlTask |

### 3.5 Entry / build

EDK2 `UEFI_APPLICATION` + `.dsc`/`.inf` + `UefiApplicationEntryPoint` →
`axl-cc` app build, native CRT0, `AXL_APP` (or AxlService for the lifecycle
loop). Node/esbuild web-bundle step stays (output embedded via `axl-embed`).
The `.inf` `[Protocols]` list (TCP4/DHCP4/GOP/PciIo/USB/HII*/SMBIOS/Tcg2/
RamDisk/…) maps onto axl-sdk's generated GUIDs + modules; a few (Tcg2,
LoadFile2, RamDisk) may surface new thin `axl_backend_*`/module needs.

### 3.6 Substrate readiness register (pre-migration audit, 2026-06-11)

Cross-referenced SoftBMC's `Core/` infra + the `.inf` `[Protocols]` against
axl-sdk's public surface. **Verdict: the foundation is GO — the MVP slice is
unblocked.** The remaining gaps are *feature-level*; per the "substrate is
pulled, not pushed" principle, **do not pre-build them — pull each as its
module is ported.**

**Foundation — READY (the slice's needs all ship):**
HTTP server (routes/middleware/WebSocket/upload/static/range/auth-hook/
server-side cache), WebDAV, JSON (build/parse/navigate), TLS + self-signed
cert, TCP/UDP/DHCP + NIC-driver-load (`axl_net_ensure_drivers`/`auto_init`) +
**interface enumeration** (`axl_net_list_interfaces`), crypto (SHA-1/SHA-256/
HMAC), Config, Log, embedded assets, MP/Task (→ replaces `MpAccel`),
Arena/Loop/Task (→ deletes `AppRuntimePkg`), GFX + capture (VNC framebuffer),
console + input seat, I/O ports, watchdog, reset, boot options, nvstore,
SMBIOS/ACPI/PCI/USB, and **device-path → text** (`axl_device_path_to_text`).

**Gap register (build per phase, not up front):**

| Capability | SoftBMC consumer | axl-sdk status | Action |
|---|---|---|---|
| HII forms (IFR + expr VM + config routing) | `UefiSetup/HiiParse.c` | **missing** | `axl-hii` sub-project (phase 3) |
| Session / RBAC + cert lifecycle | `Core/Auth.c` + `Session.c` | hook only | dashboard mechanism (phase 2) |
| Static pipeline (encoding/ETag/304/SPA) | `Core/DashboardAssets` + routes | partial (`add_static`) | dashboard (phase 2) |
| Multipart upload | firmware / VirtualMedia | **missing** | dashboard (phase 2) |
| RAM-disk **publish** API | `Core/RamDisk.c`, `VirtualMedia` | tool-only (`mkrd` uses the protocol; no lib API) | extract a small module when porting VirtualMedia |
| Serial-IO read | `HwInfo/Serial.c` | **missing** (console only) | thin reader when porting HwInfo Serial |
| Firmware-Volume access | `HwInfo/FirmwareVol.c` | **missing** | reader when porting that HwInfo readout |
| TPM / `Tcg2` (PCRs, event log) | `HwInfo/Tpm.c` | **missing** (HF TPM track is related/future) | reader when porting (or fold into HF) |
| Block-IO **enumeration** | `HwInfo/Storage.c` | fs/volume only, no raw block enum | enumerator when porting Storage |
| PXE Base Code | `Core/Network.c` (option) | **missing** | only if SoftBMC's net init truly needs PXE vs DHCP/SNP |
| Shell **dynamic-command** publish | `ShellSetup/ShellCommands` | **missing** | decide if needed — a plain app may not register a shell verb |

None of the gap-register rows block the **MVP slice** (loop + HTTP + one
HwInfo-JSON route + static shell), which uses only Foundation items. They gate
specific later modules and get built just-in-time in that module's phase.

---

## 4. Substrate gaps the port pulls (the payoff)

Now consumer-validated against the real tree — a **short** list, because most
infra already ships (§3.1/3.4):

- **`axl-hii`** — concrete: replaces `UefiSetup/HiiParse.c`. Parse + model +
  string resolve, then the expression VM, then config read; write last/gated.
- **Dashboard session/RBAC mechanism** — concrete: replaces `Core/Auth.c` +
  `Core/Session.c` (password auth, timeout, rate-limit, sessions). Plus the
  **TLS cert lifecycle** around the already-shipped `axl_tls_generate_self_signed`.
- **Dashboard static pipeline** — `Core/DashboardAssets.c` is the embedded
  bundle today; the port wants content-type/encoding/ETag/SPA-fallback on
  `add_static` (the bundler stays a build step — matches the non-goal).
- **SSE** — *a choice, not a must*: SoftBMC pushes telemetry over **WebSocket**
  today (which axl-sdk already provides), so SSE is an option (simpler,
  proxy-friendly) the port can adopt or skip. Build it if/when a one-way
  stream wants it; don't block the port on it.
- **Multipart upload** — for firmware update + VirtualMedia (browser → RAM-disk).
- **Thin substrate adds surfaced by the `.inf`** — possibly `Tcg2`
  (TPM/measurements), `LoadFile2`, and a first-class RamDisk module if the
  existing AXL RAM-disk tooling doesn't cover SoftBMC's use.

App-side, **not** substrate (stay in SoftBMC, ride axl-sdk): the RFB/VNC
encoder, the alert transports (SMTP/SNMP/Syslog/Webhook), the EC chip drivers,
the module system, and the web asset bundler.

This is *why* scoping the port first was the right call: it shrank the
speculative dashboard/HII wishlists to a concrete, ordered, consumer-proven
list (HII + session/RBAC + static pipeline first; SSE optional; O*Data later).

---

## 5. Phasing

1. **Slice** — re-base the build onto `axl-cc`; boot a minimal SoftBMC that
   serves a static dashboard shell over `axl_http_server`. Proves toolchain +
   integration. (Pulls: nothing new — uses what ships.)
2. **Core services** — port REST/Redfish, IPMI/sensors/inventory, pubsub
   decoupling, storage, logging, TLS+certs, auth. (Pulls: dashboard SSE +
   static pipeline + multipart + session/RBAC + cert lifecycle.)
3. **Forms + console** — BIOS attributes via `axl-hii` + AGT form browser;
   firmware update on AxlAsync; VNC/remote console on AxlGfx + pointer seat.
   (Pulls: the HII engine; an RFB encoder.)
4. **De-EDK2 + harden** — remove the last EDK2-backed seams; security review
   of auth/TLS/HII-write; real-hardware validation (iDRAC-class).

AGT's `AgtFormBrowser` and any local-GUI front-end ride phase 3.

---

## 6. Risks

- **Scale.** Full firmware-app port is a marathon — the keep-it-bootable +
  thin-slice discipline is what keeps it from stalling.
- **Hidden EDK2 coupling.** Modules may lean on EDK2 protocols/libs not yet
  abstracted; some will surface new `axl_backend_*` needs (see the
  EFI-encapsulation plan). Budget for substrate gaps mid-port.
- **Security surface.** Auth, TLS/cert handling, and HII *write* are the
  high-risk areas — build smallest-mechanism, gate writes, review hard.
- **Two moving targets.** axl-sdk + AGT both evolve under the port; pin
  SoftBMC to released axl-sdk versions (it already vendors release pins) so
  the port isn't chasing `main`.
- **Real-hardware-only paths.** VNC/pointer over a real BMC console, HII
  forms from real firmware — QEMU coverage is thin; lean on the HF fixture
  track to replay captured identity.

---

## 7. Open questions / next actions

- ~~Source-inventory pass~~ — **done** (§3). Next concrete step is the **MVP
  thin slice** below.
- **MVP slice:** smallest bootable SoftBMC-on-axl-sdk worth merging — likely
  *delete `AppRuntimePkg` + the TelCon `Core/` infra and bring up the event
  loop + HTTP server + one route (e.g. HwInfo JSON) on axl-sdk*, dashboard
  shell served via `add_static`. Proves the "delete-and-replace" thesis end
  to end.
- **Build coexistence:** can the EDK2 and `axl-cc` builds coexist per-module
  during the transition, or is it a hard entry-point cutover? (Leaning: hard
  cutover of entry + `Core/`, since axl-sdk replaces the runtime wholesale;
  features port incrementally behind the module manager.)
- **Telemetry transport:** keep WebSocket (ships today) or adopt SSE for
  one-way streams? (Not a blocker — decide per dashboard tab.)
- **Driver vs app split:** which modules (EC/sensor providers?) want to be
  drivers vs in-app?
- **Redfish depth:** does SoftBMC's Redfish justify `axl_redfish_*` helpers,
  or stay at `axl_http` + `axl_json` (the B2 decision)?
- **Size parity:** measure the ported binary vs the EDK2 one against the
  design doc's SPI-flash profiles.
- **Scope re-confirm:** re-check the substrate-vs-app line per pulled
  primitive (same discipline as the companion docs).
