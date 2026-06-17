# AXL Roadmap

The lean entry-point for AXL planning. **What shipped** lives in
[CHANGELOG.md](../CHANGELOG.md) (the authority, per version). **How a big
piece is designed** lives in its own design/plan doc (indexed below).
**Full historical phase detail** (every completed checkbox, hardware
findings, etc.) is preserved in
[ROADMAP-Archive.md](ROADMAP-Archive.md) — this file is the curated
front page that points into all of it.

Legend: `[x]` done · `[-]` in progress · `[ ]` pending.

---

## Design & plan docs (index)

Library / SDK foundations:
- [AXL-Design.md](AXL-Design.md) — library design (phases, API spec, style)
- [AXL-SDK-Design.md](AXL-SDK-Design.md) — SDK (toolchain, packaging)
- [AXL-Native-Backend-Design.md](AXL-Native-Backend-Design.md) — the native UEFI backend / CRT0
- [AXL-Coding-Style.md](AXL-Coding-Style.md) · [AXL-Lifecycle.md](AXL-Lifecycle.md) · [AXL-Concurrency.md](AXL-Concurrency.md)
- [AXLMM-Design.md](AXLMM-Design.md) — C++ (`libaxl-cxx`) bindings plan
- [AXL-EFI-Encapsulation-Plan.md](AXL-EFI-Encapsulation-Plan.md) — public-API UEFI-type hygiene / portability
- [AXL-Loop-Reentrancy-Plan.md](AXL-Loop-Reentrancy-Plan.md) — remediate blocking-on-a-running-loop (re-entrancy guard, deferred HTTP responses, async-first services, `axl_yield` split)
- [AXL-vs-EDK2-Scope.md](AXL-vs-EDK2-Scope.md) — what we replace vs deliberately omit; audience-facing gap list
- [AXL-Porting-Guide.md](AXL-Porting-Guide.md) · [RELEASING.md](RELEASING.md)

Subsystems:
- Drivers: [AXL-Driver-Authoring-Design.md](AXL-Driver-Authoring-Design.md) · [AXL-Driver-Authoring-Guide.md](AXL-Driver-Authoring-Guide.md) · [AXL-Shared-Driver-Recipe.md](AXL-Shared-Driver-Recipe.md) · [AXL-Network-Driver-Bundle-Design.md](AXL-Network-Driver-Bundle-Design.md)
- Graphics / UI: [AXL-Compositor-Design.md](AXL-Compositor-Design.md) · [AXL-Pointer-Cursor-Design.md](AXL-Pointer-Cursor-Design.md) · [AXL-Display-Design.md](AXL-Display-Design.md) · [AXL-Transform-Design.md](AXL-Transform-Design.md) · [AXL-Rich-UI-Plan.md](AXL-Rich-UI-Plan.md)
- Data: [AXL-PieceTree-Design.md](AXL-PieceTree-Design.md) · [AXL-RBTree-Design.md](AXL-RBTree-Design.md) · [AXL-Config-Design.md](AXL-Config-Design.md)
- Hardware fixtures / test: [AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md) · [HW-Testing-Workflow.md](HW-Testing-Workflow.md)

Active sub-projects (pre-code planning — see "Active sub-projects" below):
- [AXL-Dashboard-Server-Design.md](AXL-Dashboard-Server-Design.md) — native-SPA dashboard HTTP server
- [AXL-HII-Design.md](AXL-HII-Design.md) — headless HII forms engine (`axl-hii`)
- [AXL-SoftBMC-Port-Design.md](AXL-SoftBMC-Port-Design.md) — SoftBMC EDK2 → axl-sdk port (scoping)

---

## Shipped — milestone summary

[CHANGELOG.md](../CHANGELOG.md) is authoritative; this is the skim. Current:
**v1.7.1**, **6319 unit tests** both arches (X64 + AArch64), native backend
only (gcc + ld + objcopy). `scripts/cut-release.sh` automates the cut.

**Foundations (DONE):**
- **Library core** — AxlMem, AxlString/AxlStrBuf, AxlStream (console/file/buffer
  I-O), AXL_APP, the GLib-aligned data containers (HashTable/Array/List/SList/
  Queue/JSON), AxlLog, AxlLoop, AxlTask. Style + GLib-API-alignment passes done.
- **SDK** — `install.sh` packaging, `axl-cc` driver (app/driver/runtime, debug/
  release, `--run`, `--minimal-runtime`), CMake integration, .deb/.rpm + host-
  tools release artifacts, both arches.
- **Native UEFI backend** — own UEFI headers (manifest-generated from spec HTML),
  CRT0 (native + minimal), no EDK2 / gnu-efi. EDK2 & gnu-efi backends removed.

**Subsystems (DONE):**
- **Networking** — TCP/UDP/HTTP server+client/URL/WebSocket/WebDAV; TLS via
  mbedTLS (`AXL_TLS=1`) incl. `axl_tls_generate_self_signed`; HTTP server
  middleware / static / auth-hook / response-cache / upload-streaming / range.
- **BMC & platform access** — AxlIpmi (4 transports), AxlSmbus, AxlSpd
  (DDR4/DDR5), AxlAcpi, AxlPci, AxlUsb, AxlBoot, AxlNvstore, AxlMemPhys,
  AxlWatchdog, AxlRng, AxlImage; `tools/`: ipmi, lspci, lsusb, memspd, i2c,
  rfbrowse, dmidecode, mkfixture, fetch/grep/find/hexdump/sysinfo/netinfo.
- **Async** — AxlBufPool, AxlAsync (AP offload), AxlDefer, AxlPubsub, AxlEvent /
  AxlCancellable / AxlWait, AxlRuntime (signal/atexit), AxlService.
- **Graphics / UI** — AxlGfx (+ TTF, pixmap, EDID/HiDPI, multi-output),
  AxlCompositor (deferred surface compositor) + AxlCursor + AxlGfxRegion,
  `axl-input` (mouse/key + absolute-pointer/touch seat), AxlTransform. (AGT
  Phase-0 substrate shipped.)
- **Data / text** — AxlPieceTree editor substrate, AxlRegex (linear-time,
  ReDoS-free), AxlFind, AxlShm, cross-app AxlClipboard, AxlBytes, AxlHmac,
  AxlRand, AxlDigest, AxlCompress (gzip/zlib/DEFLATE), AxlTar (ustar), AxlSidecar,
  AxlNTree/AxlTree, AxlRadixTree.
- **Hardware fixtures** — `mkfixture` capture + `axl-emulate` replay (SMBIOS/
  ACPI/SPD/PCI/USB/net/video+EDID/NVMe manifests; HF2.3/2.4/HF4). See
  [AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md).

**Recent consumer-feedback releases (v1.2.0 → v1.7.1):** AxlCompress + AxlTar +
AxlEdid + AxlGfx display/HiDPI (1.2.0); AxlPci cap-walk fix + absolute-pointer
seat (1.3.1); regex `NOTBOL`/`NOTEOL` (1.4.0); AxlCursor absolute tracking +
ConsoleIn-only default + AxlArgs ASCII help (1.5.0) + console ASCII guard
(1.5.1); AxlArgs case-insensitive verbs (1.6.0); `axl_set_exit_status` +
AxlArgs compact DOS flags (1.7.0) + minimal-CRT0 exit-status fix (1.7.1).

---

## Active sub-projects (next up)

Forward-looking work, each with (or getting) its own design doc. Recommended
order is **SoftBMC-port-driven**: scope the port first, then build the
substrate it pulls (dashboard, HII) in consumer-validated order rather than
speculatively.

### SoftBMC — full EDK2 → axl-sdk port  *(flagship; drives the two below)* — [AXL-SoftBMC-Port-Design.md](AXL-SoftBMC-Port-Design.md)
Re-base SoftBMC (the BMC firmware app) off EDK2 onto axl-sdk + AGT, end to end.
- [-] Scoping doc drafted (strategy + mapping + gap list); **next: source-
      inventory pass over the SoftBMC repo** to fill the per-module table
- [ ] Build re-base onto the `axl-cc` native toolchain (no EDK2 tree)
- [ ] Networking on the AXL stack (HTTP/TLS); web dashboard on the native-SPA route
- [ ] BIOS attributes via the headless HII engine + AGT `AgtFormBrowser`
- [ ] Firmware update on AxlAsync + AxlBufPool; VNC on AxlGfx + pointer seat;
      sensors/EC/SEL on AxlIpmi + AxlSmbus + AxlPubsub

### Native-SPA dashboard server — [AXL-Dashboard-Server-Design.md](AXL-Dashboard-Server-Design.md)
Substrate polish on the existing HTTP server. Net-new (much already ships —
range, WebSocket, auth-hook, server-side cache, `add_static`, cert-gen).
Order reflects the SoftBMC inventory (SSE optional — consumer uses WebSocket):
- [ ] Static pipeline: `Accept-Encoding`/gzip negotiation, ETag/304, SPA fallback
- [ ] Session/RBAC **mechanism** + TLS cert **lifecycle** (replaces SoftBMC `Auth.c`/`Session.c`)
- [ ] `multipart/form-data` streaming parser (firmware / virtual-media upload)
- [ ] `axl-json` OData `$select`/`$expand` + `@odata` envelope (defer `$filter`)
- [ ] _(optional)_ SSE pubsub→push bridge — when a one-way stream wants it;
      design the **backpressure** policy first
- Non-goals: HTTP/2, an in-library asset bundler, baked-in role models (see doc)

### Headless HII forms engine `axl-hii` — [AXL-HII-Design.md](AXL-HII-Design.md)
Heavyweight module; phased. AGT renders via `AgtFormBrowser` (out of scope here).
- [ ] Parse + forms model + string resolve (buffer-in core, unit-tested)
- [ ] IFR **expression VM** (suppress/grayout/disable/inconsistentif) — centerpiece
- [ ] Config read / `ConfigResp` round-trip (covers Redfish *read*)
- [ ] Write + default stores (gated; the dangerous phase)
- [ ] Manifest: add HII IFR/package structs to `scripts/uefi-manifest.json5`

### Storage access (NVMe / ATA / SCSI) + SMART — [AXL-Storage-Design.md](AXL-Storage-Design.md)
Platform Access modules for device identity + health (the `smartctl` gap;
`storelib`/RAID is a non-goal). Per-transport, read-first, with a raw
pass-thru escape hatch and a normalized cross-transport health struct.
- [x] Design doc + `axl-nvme.h` contract (contract-first reviewed)
- [x] Phase 1 `AxlNvme` — Identify (Controller/Namespace) + SMART (Get Log Page 0x02) + Device Self-test + raw admin pass-thru; pure decoders unit-tested; `tools/nvme`; `mkfixture` refactored onto it; `test-nvme-qemu.sh` (`-device nvme`) in CI
- [x] Phase 2 `AxlAta` — IDENTIFY DEVICE + SMART (READ DATA + THRESHOLDS) + self-test; pure decoders unit-tested; `tools/ata`; AtaPassThru struct hand-written; `test-ata-qemu.sh` (`ich9-ahci` + SATA disk) in CI. Fixed the directly-attached-SATA device-walk (PortMultiplierPort 0xFFFF sentinel collision)
- [x] Phase 3 `AxlScsi` — INQUIRY (std + VPD 0x80 serial) + READ CAPACITY (16) + LOG SENSE health (IE page 0x2F + Temperature page 0x0D) + raw CDB pass-thru; pure decoders unit-tested; `tools/scsi`; ExtScsiPassThru struct hand-written; `test-scsi-qemu.sh` (`virtio-scsi` disk + CD) in CI. Walk filters phantom LUNs by INQUIRY peripheral qualifier. Self-test + VPD 0x83 deferred to the raw escape hatch
- [x] Phase 4 `AxlSmart` + `tools/smart` — normalized `AxlSmartHealth` rollup over the union device walk (`axl_storage_next` across NVMe/ATA/SCSI) + `axl_smart_health` dispatch + pure per-transport normalizers (`axl_smart_from_*`, unit-tested) + `axl_storage_get_location` (NVMe device-path / ATA port.pmp / SCSI target:lun). `test-smart-qemu.sh` (one device per transport) in CI; NVMe+ATA health end-to-end, SCSI health real-hardware-only
- Non-goals: RAID/HBA mgmt (storelib), block read/write, GPT, destructive typed commands (FORMAT/SANITIZE/fw-download)

---

## Open backlog

Grouped, terse; **detail lives in the linked design doc or
[ROADMAP-Archive.md](ROADMAP-Archive.md)**. Most are opportunistic / low-priority.

- **Sync→async API split — build on demand** (→ [AXL-Concurrency.md § "Extending
  the model"](AXL-Concurrency.md#extending-the-model-which-apis-go-async-and-when)):
  the net stack's sync-wraps-async shape applies to any op that blocks on
  hardware/firmware completion. Ranked candidates: **IPMI/BMC** (KCS/SSIF
  busy-poll → loop Poll-tick; SoftBMC roadmap) and **storage** (NVMe/ATA/SCSI
  PassThru `Event`, BlockIo2; self-test/SMART/large-read; SoftBMC roadmap) are
  the near-term ones; MP-services (`StartupAllAPs` `WaitEvent`), USB async
  transfers, and TPM are lower. Un-defer each when a consumer needs it — do not
  build speculatively.
- **Hardware-fixture capture — remaining phases** (→ [AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md),
  Archive): TPM/PCR + TCG event-log capture/replay (swtpm); secure-boot + boot-var
  capture/inject; Redfish-mock capture/replay; in-band IPMI/KCS capture;
  SMBIOS-handle / non-EEPROM SMBus sensors / ESRT / NVMe-Identify replay;
  `--sanitize` (zero serials/asset tags); decide public `axl-fixtures` repo;
  `axl-emulate` polish (ACPI drop/keep flags, manifest summary, `--` passthrough);
  fold `qemu_launch` into `run-qemu.sh` as a daemon.
- **EFI encapsulation / portability** (→ [AXL-EFI-Encapsulation-Plan.md](AXL-EFI-Encapsulation-Plan.md),
  Archive): classify remaining UEFI-coupled modules; promote
  `axl_backend_{locate_protocol,alloc_pages,create_event,install_protocol,
  get/set_variable,exit}`; optional `src/core` / `src/platform/{uefi,coreboot}`
  split + a coreboot/Linux backend for `libaxl-core.a`.
- **C++ bindings (AxlMM)** (→ [AXLMM-Design.md](AXLMM-Design.md)): CPP1.7+ wrapper
  phases (Stream/Event/StrBuf/Arena, containers, networking, Sphinx docs).
- **API hygiene:** `AxlTcpCb` `int status` → `AxlStatus`; other multi-outcome
  return-value flips as audits surface them; AxlPubsub typed payloads.
- **Correctness / perf** (→ Archive §"Known Gaps"): a benchmark suite; AxlLoop
  fully event-driven driver mode (drop `driver_tick_ms`); async-TCP `Configure`
  retry non-blocking; `axl_yield()` API instrumentation; release-mode heap
  auto-sweep; robust exception-backed physical-access fault gate; watchdog
  opt-in helper.
- **Packaging / distro** (→ Archive): `-devel` split + `debian/` + `.spec` for
  upstream submission; evaluate meson/cmake+ninja build.
- **Real-hardware follow-ups** (→ Archive §"Real-hardware findings" — Dell
  PowerEdge / iDRAC10): assorted open items (iDRAC HTTPS routed-address timeout,
  USB-NIC unload entry points, `Load Error` on repeat `.efi` launch, log-timestamp
  `.usec` always zero, console-aware tool output mode, a real-hardware test
  runner). Several consumer-repo items (uefi-devkit / axl-webfs) live here too.
- **SDK/test:** consumer build verification (axl-webfs / uefi-devkit) in
  `test-axl.sh`; migrate tools' inline hex parsers onto `axl_hex_parse_u64`.

---

## Done / decided (one-liners, full detail in Archive)

- **Phase B2 Redfish** — *no library module*; shipped `rfbrowse.efi` (HTTP client
  + JSON cover it). Extract a session helper later if SoftBMC needs one.
- **Phase 10 networking** — folded into the SoftBMC full-port sub-project above.
- **Repo merge / backend removal / project restructure** — complete.
- **AML interpretation, HTTP/2, in-library asset bundler, `$filter`** — explicit
  non-goals (rationale in the respective design docs).
