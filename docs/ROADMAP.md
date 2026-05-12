# AXL Roadmap

Unified phase tracker for the AXL library and SDK.
Phases from [AXL-Design.md](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Design.md) and
[AXL-SDK-Design.md](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-SDK-Design.md) are combined here in
execution order.

Legend: [x] done, [ ] pending, [-] in progress

---

## Library Phases (AXL-Design.md)

### Phase R: Rename UdkLib to AXL — DONE

- [x] Global symbol rename (UDK_ -> AXL_, Udk -> Axl)
- [x] Directory and header renames
- [x] Update all consumer repos (uefi-devkit, axl-webfs, softbmc, ipmitool)

### Phase S1: axl_mem — DONE

- [x] axl_malloc / axl_free / axl_realloc / axl_calloc
- [x] axl_strdup / axl_memdup
- [x] Debug features: fill patterns, fence checks, leak dump, stats

### Phase S2: axl_strbuf — DONE

- [x] AxlStrBuf string builder
- [x] UTF-8 / UCS-2 conversion
- [x] Base64 encode/decode
- [x] axl_strlcpy / axl_strlcat
- [x] axl_asprintf

### Phase S3: axl_io — DONE

- [x] AxlStream abstraction (console, file, buffer)
- [x] axl_printf / axl_fprintf / axl_print / axl_printerr
- [x] axl_fopen / axl_fread / axl_fwrite / axl_readline
- [x] axl_file_get_contents / axl_file_set_contents

### Phase S4: AXL_APP — DONE

- [x] Entry point macro: int main(argc, argv)
- [x] Shell argument conversion (UCS-2 to UTF-8)
- [x] axl.h umbrella header (self-contained, no EDK2 leaks)

### Phases M1-M6: Module Migration — DONE

- [x] M1: AxlLog — GLib-style logging API
- [x] M2: AxlData — hash, array, string, JSON
- [x] M3: AxlUtil — file, path, args, hexdump, time, smbios
- [x] M4: AxlLoop — event loop, timers
- [x] M5: AxlTask — arena allocator, worker pool
- [x] M6: AxlNet — TCP, HTTP server/client, URL parsing

### Phase C1: Style Guide Compliance — DONE

- [x] STATIC -> static, TRUE -> true, FALSE -> false, BOOLEAN -> bool
- [x] Spaces around operators, ///< doc comments
- [x] All axl-*.c files pass style audit

### Phase C2: Dogfooding — DONE

- [x] Replace AllocatePool/FreePool with axl_malloc/axl_free in migrated modules
- [x] Replace AsciiStrLen/AsciiStrCmp with axl_strlen/axl_strcmp
- [x] Replace CopyMem/ZeroMem with axl_memcpy/axl_memset
- [x] Replace AsciiSPrint with axl_snprintf
- [x] AXL_LOG_DOMAIN in all modules
- [x] Consumer projects updated (uefi-devkit, axl-webfs)

### Phase C3: Test Modernization — DONE

- [x] Shared test header (axl-test.h)
- [x] All 9 test files converted to AXL_APP entry points
- [x] Wide-string test output replaced with axl_printf
- [x] Test .inf files updated (_AxlEntry, AxlAppLib)
- [ ] Consumer build verification in test-axl.sh (axl-webfs, uefi-devkit)

### Phase C4: Style Compliance Pass 2 — DONE

- [x] Add Doxygen @brief/@return to all existing functions
- [x] Multi-line params with ///< on all function definitions
- [x] Replace remaining UEFI types with standard C in non-backend code
- [x] Remove unnecessary #include <Uefi.h> from abstracted files

### Phase C5: GLib API Alignment — DONE

Audit all public APIs against GLib naming and align before the API
stabilizes.  AXL is "GLib for UEFI" — the API should feel familiar
to anyone who knows GLib.

- [x] Audit every public function name against GLib equivalents
- [x] Audit argument order (GLib puts the "object" first)
- [x] Audit return conventions (GLib returns the modified list head from list ops)
- [x] Rename AxlStrBuf → AxlString / axl_string_* (matches GString)
- [x] Rename AxlHash → AxlHashTable / axl_hash_table_* (matches GHashTable)
- [x] Swap axl_strjoin arg order to (separator, arr) matching g_strjoinv
- [x] axl_string_new(init) takes optional init string matching g_string_new
- [x] axl_string_append_c (was putc), axl_string_append_len (was append_n)
- [x] Add GLib string search/test functions (strstr_len, strrstr, has_prefix, has_suffix, etc.)
- [x] Add str_equal, strncasecmp, strv_contains, strv_equal
- [x] AxlHashTable: generic keys, insert/replace/lookup/contains/steal/foreach_remove/iterator
- [x] AxlHashTable: drop _w API, add AxlDestroyNotify, AxlHashFunc, AxlEqualFunc
- [x] AxlString: rename printf→append_printf, add prepend/insert/erase/truncate/overwrite
- [x] AxlArray: add remove_index, remove_index_fast, remove_range, set_size, sort_with_data
- [x] AxlList: add insert_before/after, remove_all/link, sort_with_data, copy_deep
- [x] AxlSList: add insert_before, remove_all/link, sort_with_data, copy_deep
- [x] AxlQueue: add find, find_custom, remove, remove_all
- [x] New shared types: AxlCompareDataFunc, AxlCopyFunc
- [x] Update all consumers (tests, examples, axl-webfs)

**Resolved divergences:**

| AXL (before) | AXL (after) | GLib equivalent | Status |
|-------------|-------------|-----------------|--------|
| `AxlStrBuf` / `axl_strbuf_*` | `AxlString` / `axl_string_*` | `GString` / `g_string_*` | DONE |
| `AxlHash` / `axl_hash_*` | `AxlHashTable` / `axl_hash_table_*` | `GHashTable` / `g_hash_table_*` | DONE |
| `axl_hash_table_set/get` | `axl_hash_table_insert/replace/lookup` | `g_hash_table_insert/replace/lookup` | DONE |
| string-only keys + `_w` API | generic keys via `new_full(hash, equal, ...)` | `g_hash_table_new_full(...)` | DONE |
| (missing) | `axl_hash_table_contains/steal/foreach_remove` | `g_hash_table_contains/steal/foreach_remove` | DONE |
| (missing) | `AxlHashTableIter` | `GHashTableIter` | DONE |
| `axl_strbuf_putc` | `axl_string_append_c` | `g_string_append_c` | DONE |
| `axl_strbuf_append_n` | `axl_string_append_len` | `g_string_append_len` | DONE |
| `axl_strjoin(arr, sep)` | `axl_strjoin(sep, arr)` | `g_strjoinv(sep, arr)` | DONE |
| (missing) | `axl_strstr_len` | `g_strstr_len` | DONE |
| (missing) | `axl_str_has_prefix/suffix` | `g_str_has_prefix/suffix` | DONE |
| (missing) | `axl_strcmp0` | `g_strcmp0` | DONE |
| (missing) | `axl_str_equal` / `axl_str_hash` | `g_str_equal` / `g_str_hash` | DONE |
| (missing) | `axl_strv_contains/equal` | `g_strv_contains/equal` | DONE |

**Intentional divergences (keeping):**

| AXL | GLib | Why |
|-----|------|-----|
| `axl_fopen`/`axl_fread` | GIOChannel | POSIX names are universal |
| `axl_loop_new` | `g_main_loop_new` | Shorter, UEFI has one loop |
| `axl_strsplit(s, char)` | `g_strsplit(s, string, max)` | Single-char delimiter sufficient |
| `axl_dir_read` returns struct | `g_dir_read_name` returns string | Struct has size+is_dir metadata |
| `size_t` for sizes | `guint` | More correct on 64-bit |
| `int` error returns | abort-on-OOM | UEFI must handle OOM |
| `axl_dir_open` | `g_dir_open` | OK (matches) |
| `axl_dir_read` | `g_dir_read_name` | Evaluate: return name only vs struct? |

---

## SDK Phases (AXL-SDK-Design.md)

### SDK Phase 1: Core — DONE

- [x] install.sh: build library, package SDK
- [x] axl-crt0.c: UEFI entry point stub
- [x] axl-cc: command-line build wrapper
- [x] axl.cmake: CMake integration
- [x] hello.c example verified in QEMU
- [x] X64 architecture support

### SDK Phase 2: Polish — DONE

- [x] AARCH64 cross-build support (build + tests + SDK + axl-cc aa64)
- [x] Test CMake build end-to-end (verified in QEMU)
- [x] Verify net module works (SDK includes AxlNetLib + all protocol GUIDs via sdk-ref target)
- [x] Better error messages in axl-cc (source validation, tool checks, install hints)
- [x] `--verbose` flag for axl-cc
- [x] `axl-cc --version` / `axl-cc --help`

### SDK Phase 3: Distribution — DONE

- [x] GitHub Actions: `.github/workflows/release.yml` builds SDK on
      tag push for x64 / aa64, both with and without TLS.
- [x] Versioned releases: `axl-sdk-<version>-<arch>[-tls]-linux.tar.gz`
      uploaded to GitHub Releases. Version comes from `VERSION` file
      at repo root (currently `0.1.0`).
- [x] Version stamp in `axl-cc --version` output (shown by the
      generated wrapper in `scripts/install.sh`).
- [x] Release notes: auto-generated body in `release.yml` — download
      table, prerequisites, quick start, docs links.

### SDK Phase 4: Advanced Features — DONE

- [x] Multi-file projects in axl-cc (works, tested)
- [x] `--net` not needed — network GUIDs are already in libaxl.a
- [x] `axl-cc --type driver|runtime` for DXE/runtime driver targets
- [x] `axl-cc --entry <name>` for custom driver entry points
- [x] `axl-cc --debug` — debug build (-Og, DWARF symbols, leak tracking, .map file)
- [x] `axl-cc --release` — release build (-Os, -DNDEBUG) [default]
- [x] `axl-cc --run` — build and launch in QEMU via run-qemu.sh

### SDK Phase 5: Backend Abstraction — DONE

Created `axl-backend.h` internal abstraction layer, migrated all
library modules to use it. Originally supported EDK2 and gnu-efi
backends (Phases 5a-5e); both were removed in Phase N7 in favor
of the native backend.

- [x] Backend API: memory, console, time, file I/O, wide-string, events
- [x] All library modules migrated to backend API
- [x] Portable replacements for EDK2 intrinsics (hex parser, CAS, CpuPause)

---

## Native UEFI Backend (AXL-Native-Backend-Design.md)

AXL provides its own UEFI type definitions, CRT0, and build toolchain.
No external dependencies (no EDK2, no gnu-efi). Supports applications,
boot service drivers, and runtime drivers.

### Project Restructure — DONE
- [x] Standard C project layout: `include/` + `src/` at root
- [x] Tests moved to `test/unit/` + `test/integration/`

### Phase N1: UEFI type headers — DONE
- [x] Create `include/uefi/` with 7 self-contained headers (1540 lines)
- [x] Types, status codes, system tables, protocols, GUIDs from UEFI 2.10
- [x] Compiles clean on x86_64 and AARCH64 (-Wall -Wextra -Wpedantic)

### Phase N2-N4: Native backend + CRT0 + tests — DONE
- [x] Create `src/backend/native/axl-backend-native.c` (37 backend functions)
- [x] Create `src/crt0/axl-crt0-native.c` (app entry point)
- [x] Add `AXL_BACKEND_NATIVE` case to `axl-backend.h`
- [x] `Makefile` — gcc + ld + objcopy, builds libaxl.a + all test EFIs
- [x] 411/411 tests pass on native backend
- [x] Remove EDK2 header dependencies from unit tests
- [x] Delete compat shim layer (12 files, -377 lines)
- [x] Backend directory restructure (gnuefi/, native/ subdirs)
- [ ] Type remaining BS slots for drivers (InstallProtocolInterface, etc.)
- [ ] Add EFI_DRIVER_BINDING_PROTOCOL to protocols header
- [ ] Shell argument parsing (EFI_SHELL_PARAMETERS_PROTOCOL)

### Phase N5: SDK integration — DONE
- [x] Update `install.sh` to support native backend
- [x] Generate `axl-cc` that uses native backend
- [x] `axl-cc --type driver|runtime` support
- [x] CMake integration for SDK consumers
- [x] Test: `axl-cc hello.c -o hello.efi` with zero external deps

### Phase N6: UEFI header generation from spec HTML — DONE
- [x] Manifest-driven generator (`scripts/generate-uefi-headers.py`)
- [x] Manifest (`scripts/uefi-manifest.json5`): 275 definitions, dependency-ordered
- [x] Spec downloader (`scripts/download-uefi-specs.py`): UEFI 2.11, PI 1.8, ACPI 6.5, Shell 2.2
- [x] Extracts from `<pre>` blocks (struct, enum, funcptr, define, typedef) and Table 2-4
- [x] Unknown struct member types auto-replaced with `void *`
- [x] 327 GUIDs with EDK2-style aliases
- [x] `--check` flag validates source code against manifest (integrated into build.sh)
- [x] Shell protocols hand-written (PDF-only spec)
- [ ] Add more protocols to manifest as needed (PCI, USB, HII, etc.)

### Phase N7: Remove EDK2/gnu-efi backends — DONE
- [x] Remove EDK2 backend (AxlPkg/, .inf files, axl-backend-edk2.c, deps.conf)
- [x] Remove gnu-efi backend (Makefile.gnuefi, axl-backend-gnuefi.c, compat headers)
- [x] Simplify axl-backend.h, axl.h, source files — remove all backend conditionals
- [x] Simplify scripts (build.sh, install.sh, test-axl.sh, test-all.sh)
- [x] Rename Makefile.native to Makefile
- [x] Native backend is the only backend

---

## Networking Phases (Future)

These phases add features to the existing AxlNet module.

### Phase 8: HTTP Server Features — DONE

- [x] Middleware pipeline (`axl_http_server_add_middleware`, runs in order)
- [x] Static file serving (`axl_http_server_add_static`)
- [x] Route-based dispatch with prefix matching
- [x] WebSocket (RFC 6455) — upgrade handshake, frame parser/builder,
      per-connection state, ping/pong/close, broadcast
- [x] Authentication — `axl_http_server_use_auth` + per-route auth
      flags enforcement (401/403)
- [x] Response caching — AxlHashTable-based with TTL, cache
      check/store in dispatch, invalidation
- [x] Upload streaming — route registration with `AxlUploadHandler`

**Hardening pass complete (April 2026 code review):**

- [x] Cache key heap corruption — `axl_http_dispatch.c` now strdup's
      `dup_key` before inserting into the cache table.
- [x] NULL upload handler dereference — dispatch guards on
      `route->handler != NULL`.
- [x] WebSocket broadcast nested event loop —
      `axl_http_server_ws_broadcast` uses async `send_start` on
      `s->loop`, not a temporary sync loop.
- [x] TLS + WebSocket data path — `on_conn_data` TLS block now
      branches on `is_websocket || is_upload_stream` and decrypts
      into the chunk buffer.
- [x] WebSocket ping/close bypassing TLS — `process_websocket_data`
      sends pong/close via `axl_tls_write` when `tls_ctx != NULL`.
- [x] DISCONNECT firing after socket close — `reset_connection`
      fires `AXL_WS_DISCONNECT` while the transport is still open.

**Cache policy — all three addressed in commit `ece9317`:**

- [x] `cache_max` enforced via FIFO eviction on the hash table.
- [x] `axl_http_server_set_route_ttl(path, ttl)` stores per-route
      TTLs in a `route_ttls` hash on the server and consults it at
      cache-store time.
- [x] `axl_http_server_cache_invalidate(prefix)` removes matching
      entries via `axl_hash_table_foreach_remove` with a prefix
      predicate.

18 integration tests added at the same time.

### Phase 9: TLS Support — DONE

- [x] TLS support via mbedTLS 3.6.3 (optional: AXL_TLS=1)
- [x] axl_tls_generate_self_signed (ECDSA P-256)
- [x] HTTP server HTTPS (axl_http_server_use_tls)
- [x] HTTP client HTTPS (auto-detect https:// URLs)

### Phase 10: SoftBMC Migration

- [ ] Migrate SoftBMC to AXL networking stack
- [ ] Use AxlAsync (Phase A2) for firmware update endpoint
- [ ] Use AxlBufPool (Phase A1) for VNC tile buffers

---

## BMC Access Phases (Future)

### Phase B1: AxlIpmi — DONE

Local BMC access via IPMI. Four transports, auto-selected: EDKII
`IPMI_PROTOCOL` → Dell `EFI_IPMI_TRANSPORT` → SMBIOS Type 38
(KCS or SSIF) → x86 default KCS 0x0CA2/0x0CA3.

- [x] `axl_ipmi_session_new()` + `_free()` + AUTOPTR cleanup
- [x] `axl_ipmi_raw(netfn, cmd, req, resp)` lowest-level entry
- [x] `axl_ipmi_get_device_id()` — BMC info
- [x] `axl_ipmi_get_sensor_reading()`
- [x] `axl_ipmi_sdr_info()` / `axl_ipmi_sdr_get()` — SDR iteration
- [x] `axl_ipmi_sel_info()` / `axl_ipmi_sel_get_entry()` — SEL iteration
- [x] `axl_ipmi_get_chassis_status()` / `axl_ipmi_chassis_control()`
- [x] `axl_ipmi_fru_info()` / `axl_ipmi_fru_read()`
- [x] Formatting helpers: `axl_ipmi_completion_code_string()`,
      `_sensor_type_string()`, `_entity_id_string()`
- [x] KCS transport (`src/ipmi/axl-ipmi-kcs.c`)
- [x] SSIF transport (`src/ipmi/axl-ipmi-ssif.c`) — multi-part framing +
      60 ms inter-command delay
- [x] EDKII vendor protocol (`src/ipmi/axl-ipmi-edkii.c`)
- [x] Dell vendor protocol (`src/ipmi/axl-ipmi-dell.c`) — CC synthesis
- [x] Auto-detection via SMBIOS Type 38
- [x] Backend hooks: `axl_backend_io_read8/write8` (SMBus access was
      originally a backend hook; Phase B1a promoted it into its own
      `AxlSmbus` Platform Access Module — see below.)
- [x] `axl_ipmi_session_new_with_callback()` for unit tests + pluggable
      transports
- [x] 43 unit tests (mock-callback transport, every typed wrapper +
      negative paths)
- [x] `tools/ipmi.efi` — stripped-down ipmitool-equivalent
      (info / chassis / sel / sdr / sensor / fru / raw)

**Consumer projects:** uefi-ipmitool (stays EDK2-based for now;
sunset once `tools/ipmi` reaches feature parity), SoftBMC EC module.

### Phase B1a: AxlSmbus module split — DONE

Extracted the SMBus / I2C block transfer primitives from the backend
layer into a first-class Platform Access Module. Motivation: a second
SMBus consumer is imminent (AxlSpd, Phase B3 below), so the
anonymous `axl_backend_smbus_*` pair graduates to a proper module
with session handle, transport vtable, and its own tests before
AxlSpd lands on top.

- [x] `include/axl/axl-smbus.h` — public API (opaque `AxlSmbus`
      session, `axl_smbus_read_block` / `_write_block`, transport
      enum + string, `AXL_SMBUS_BLOCK_MAX`)
- [x] `src/smbus/` module: `axl-smbus.c` (dispatcher + auto-detect),
      `axl-smbus-hc.c` (EFI_SMBUS_HC_PROTOCOL pass-through),
      `axl-smbus-i2c.c` (EFI_I2C_MASTER_PROTOCOL framing — the B1
      code path), `axl-smbus-format.c`, `axl-smbus-internal.h`
- [x] `test/unit/axl-test-smbus.c` — capturing-mock I2C Master +
      SMBus HC protocols via `gBS->InstallProtocolInterface`. 40
      test_check calls across 9 test functions, including direct
      regression coverage for B1's byte-count prefix (writes) and
      count-byte strip (reads).
- [x] AxlIpmi SSIF migrated: `SsifCtx` holds an `AxlSmbus *` and all
      calls go through the public module; `axl_backend_smbus_*`
      removed from `src/backend/axl-backend.h` and
      `src/backend/native/axl-backend-native.c`.
- [x] Ratchet bumped 1216 → 1255 on both X64 and AArch64 (the
      churn-collapse cleanup in a later test-hygiene pass dropped
      one install-success pass line).

### Phase B1b: SSIF end-to-end QEMU regression — DONE

Closed the B1 regression-coverage gap end-to-end. B1 (I2C Master
fallback framing) now has both unit-level regression (Phase B1a's
`AxlTestSmbus` via capturing mock) and integration-level regression
exercising real SMBus wire traffic through QEMU's ICH9 controller.

- [x] `sdk/examples/smbus-hc-shim.c` — DXE driver that finds the
      ICH9 SMBus PCI function (8086:2930), enables HOSTC + I/O
      decode via raw CF8/CFC config-space I/O, and publishes
      `EFI_I2C_MASTER_PROTOCOL` backed by the ICH9 `pm_smbus`
      register model. Intentionally publishes *only* I2C Master
      (not SMBus HC) so AxlSmbus's I2C fallback — which is where
      the B1 bug lived and what real Dell/Grace firmware exposes —
      is the code path exercised.
- [x] `test/integration/common-test.sh` gains
      `test_add_ipmi_bmc_sim_ssif` (sibling to the KCS helper).
- [x] `test/integration/test-ipmi-ssif-qemu.sh` loads the shim,
      then runs `AxlTestIpmi hw` against QEMU's `smbus-ipmi`
      device. 66 passes (61 unit + 5 hardware path) in ~7 s.
- [x] Regression proof: reverting B1's byte-count prefix in
      `src/smbus/axl-smbus-i2c.c` fails the SSIF script with
      `FAIL: real_hw: Get Device ID succeeds against live BMC`
      (2 failures, 61 passes). CI would catch a future regression.
- [x] The "SSIF (B1) is *not* covered here" note removed from
      `test-ipmi-qemu.sh`; the two scripts now cross-reference.

### Phase B3: Platform Access — follow-on modules

R+1 (downstream-consumer-driven; the motivating scenario is a
hardware-diagnostic CLI port from Linux to UEFI):

- [x] **AxlAcpi** — ACPI table discovery + typed readers. RSDP →
      RSDT/XSDT walk, signature lookup with cursor iteration matching
      `axl_smbios_find_next`, public checksum verifier, typed
      readers for MCFG (PCIe ECAM segments), MADT (IOAPIC on x86 +
      GIC regions on aa64), and FACP/FADT (SMI cmd port, PM1 blocks,
      DSDT pointer). **No AML interpretation** — that's ACPICA-sized
      and out of scope. Header: `axl/axl-acpi.h`. Source:
      `src/acpi/axl-acpi.c`.
- [x] **`axl_io_port_*`** — promote `axl_backend_io_*` to public
      with 16/32-bit variants. Build-gated to x86 (compile error on
      AArch64, not runtime no-op). Header: `axl/axl-port.h`.
- [x] **`axl_nvstore_*` extensions** — namespace registration so
      vendor GUIDs (Dell/HPE/Lenovo OEM variables) plug in by name
      without exposing GUIDs at access sites; `_delete`, `_iter`,
      `_get_attrs`. Built-in namespaces "global" and "app" remain
      pre-registered. Behaviour change: unregistered namespace is
      now an error (was: silent fallback to global).
- [x] **AxlBoot** — typed boot-option management. `AxlBootOption`
      struct (description / device-path text / opt_data) with
      `_option_get/_set/_delete/_free`, `_order_get/_set`,
      `_next_get/_set/_clear`, `_current_get`. `EFI_LOAD_OPTION`
      codec stays internal — raw wire bytes never cross the public
      API. Header: `axl/axl-boot.h`. Source: `src/util/axl-boot.c`.

R+2:

- [x] **AxlPci** — ECAM-based config-space access via MCFG. Lazy
      on first call; typed `AxlPciAddr` struct (16-bit segment,
      8/5/3-bit bus/dev/func). 8/16/32-bit read+write at any
      register offset 0..4095. `axl_pci_next` cursor enumerates
      every responding function, skips empty slots, honours the
      multi-function header bit. `_find_by_vid_did` and
      `_find_by_class` (24-bit class triplet) for direct lookups.
      Cursor-style legacy cap walk (status bit 4 + cap pointer at
      0x34) and PCIe extended cap walk (offsets 0x100+, 12-bit
      next pointers). VPD reader walks the keyword-tagged
      RO/RW resources (PCI 3.0 §6.4) with the F-bit handshake.
      Header: `axl/axl-pci.h`. Source: `src/pci/axl-pci.c`.
- [x] **AxlImage** — opaque `AxlImage *` handle for executable
      image lifecycle. Thin wrapper over `axl_driver_load`/`_unload`
      adding distinct `_start` semantics that capture the image's
      EFI_STATUS exit code (`axl_driver_start` drops it because
      drivers don't exit cleanly). Backend-neutral name; future
      Linux/coreboot backends map to `posix_spawn`/payload
      loaders. Header: `axl/axl-image.h`. Source:
      `src/util/axl-image.c`.

R+3:

- [x] **AxlMemPhys** — physical-memory access. `_map`/`_unmap`
      held mappings (Linux backend would `mmap("/dev/mem")` here;
      identity-mapped no-ops on UEFI), one-shot
      `_read{8,16,32,64}` / `_write{8,16,32,64}` helpers, and
      `_search` for byte-pattern scans inside a mapped region.
      Header: `axl/axl-mem-phys.h`. Source: `src/util/axl-mem-phys.c`.
- [x] **AxlWatchdog** — `_disarm` / `_set(seconds)` / `_pet`. Wraps
      `gBS->SetWatchdogTimer`. Without this, the firmware's
      5-minute default watchdog kills any long-running diagnostic.
      `_pet` re-arms to the value last passed to `_set`. Header:
      `axl/axl-watchdog.h`.
- [x] **`axl_reset_*`** — already shipped pre-R+3 in
      `axl/axl-sys.h` as `axl_reset(int type)` with
      `AXL_RESET_COLD` / `_WARM` / `_SHUTDOWN` constants. Maps to
      `gRT->ResetSystem`.
- [x] **AxlRng** — `axl_rng_bytes(out, len)` wraps
      `EFI_RNG_PROTOCOL` (UEFI 2.11 §37.5). Returns -1 if the
      protocol isn't published; consumers that need a deterministic
      fallback layer their own. Header: `axl/axl-rng.h`.

R+4 — DONE (2026-05-01):

- [x] **AxlSpd** — DDR4/DDR5 SPD reader on `AxlSmbus`. Cursor
      iteration over 0x50..0x57; key byte at SPD offset 2 selects
      the codec (0x0C=DDR4, 0x12=DDR5). DDR4 uses the EE1004 SPA
      pseudo-slave (0x36/0x37) for the upper 256-byte
      manufacturing block; DDR5 pages via SPD5118 MR11 across
      eight 128-byte windows. `AxlSpdInfo` carries module/DRAM
      JEP-106 codes (raw, parity preserved), part number, serial,
      manufacture year/week, capacity, JEDEC speed grade,
      ECC/registered flags. Pure-decoder entry point
      `axl_spd_decode(buf, len, *out)` runs the same codec on a
      captured buffer with no SMBus involvement — used by the
      cross-arch unit tests and by consumers doing offline
      analysis. DDR3 deliberately deferred. Header: `axl/axl-spd.h`.
      Source: `src/spd/`.

- [x] **`tools/memspd.c`** — verbs `list / show <slot> /
      decode <slot>`. Vendor lookup is data-driven via
      `share/jedec.json` (15-vendor stub; auto-discovered next to
      the .efi or via `--jedec-file`). Missing sidecar → manufacturer
      fields print as raw 16-bit hex codes.

- [x] **`scripts/qemu-patches/0001-smbus-eeprom-add-memdev-link.patch`**
      — adds `memdev=<link<memory-backend>>` to QEMU's smbus-eeprom
      device so `test/integration/test-spd-qemu.sh` can attach a
      canned SPD blob at SMBus 0x50 and exercise the full wire
      path through `SmbusHcShim.efi`. Stock QEMU 10.x rejects
      `memdev=` on smbus-eeprom; the patch is small (~40 lines),
      idiomatic (mirrors `pc-dimm`'s `memdev=` link), and a
      candidate for upstreaming.

- [x] **AxlSmbus byte ops** — `axl_smbus_read_byte` /
      `axl_smbus_write_byte` (SMBus spec §5.5.4 / §5.5.5). Required
      for SPD EEPROMs (24Cxx-style, no SMBus block framing) and
      for SPD5118 hub register access. Implemented in both HC
      (`EfiSmbusReadByte` / `EfiSmbusWriteByte`) and I2C-Master
      transports; SmbusHcShim extended to translate the new shapes
      to ICH9 SMBus Read Byte / Write Byte protocol commands.

- [x] **AxlPlatform tests** — 21 new SPD tests (DDR4 + DDR5 decode
      + bogus-input rejection + probe enumeration). Cross-arch
      balanced; ratchet 1842 → 1863 on both x86 and AArch64.

R+5 — PCI tooling DONE; USB tooling pending:

- [x] **`tools/lspci.c`** — Linux-style PCI lister. Shipped
      2026-05-02 in commit 485c517 (initial flat list + caps + hex
      + filters), extended in commit 316c72c with `-t` tree view
      and JSON5-backed vendor/device name decoding. BAR decoding
      still deferred.
- [x] **`axl_pci_cap_id_str` + `axl_pci_ext_cap_id_str`** — small
      lookup tables in `axl-pci.{h,c}` mapping legacy and PCIe
      extended capability IDs to human strings. Shipped 2026-05-02
      in commit bf0273d.
- [x] **`axl_pci_bridge_info` + `axl_pci_tree_for_each`** — PCI
      topology API. Shipped 2026-05-02 in commit 2d91898. Test
      runner now injects a pcie-root-port + virtio-rng so bridge
      code isn't shipped without coverage (commit 153992f).
- [x] **PCI vendor/device/subsystem name database** — initial cut
      shipped 2026-05-02 in commit eb38bac
      (`axl_pci_ids_load` / `axl_pci_vendor_name` / `axl_pci_device_name`
      + curated `share/pci-ids.json5`). Extended 2026-05-02 per
      consumer feedback into a 5-commit chain that landed:
      handle API + length contracts + load -1/-2 split (commit
      2d30e91); subsystem lookup + iter API + Python S-line
      extractor (commit 938ebcb); class-string format flags via
      `AxlPciClassFmt` enum (commit 13fa258); composed-name helper
      `axl_pci_format_name` with handle parity (commit ba37a8b);
      optional class-name overlay sidecar `pci-class.json5`
      (commit 0d97935). Same JSON5 sidecar pattern memspd uses
      for `jedec.json5`. Test runner stages the JSON5 next to
      test EFIs so the loaders are exercised end-to-end. Shared
      by lspci and downstream consumers.
- [x] **AxlUsb** — USB device enumeration via `EFI_USB_IO_PROTOCOL`
      handles. Shipped May 2026 in six phases:
      - **Phase A** (commit `da71a2e`): enumeration + vid/pid via
        `axl_usb_next` cursor + `axl_usb_get_vid_pid`.
      - **Phase B** (commit `f8e4557`): interface class triplet
        decode (`axl_usb_get_class` + `axl_usb_class_string_fmt`)
        with compiled-in USB-IF Defined Class Codes tables.
      - **Phase C** (commit `d39b7c2`): string descriptor reads —
        `axl_usb_get_manufacturer` / `_product` / `_serial` over
        `UsbGetStringDescriptor` + UCS-2 → UTF-8.
      - **Phase D** (commit `8a04454`): vendor/device-name JSON5
        sidecar (`axl_usb_ids_*` mirroring AxlPciIds), built on
        the shared AxlSidecar scaffold from `f875b36`.
      - **Phase F** (commit `75dcd43`): faithful USB hub-port
        chain via `axl_usb_tree_for_each` — depth derived from the
        EFI device path's USB messaging-node count, parents-
        before-children sort guarantee from the existing dev-key
        order, no second sort pass.
- [x] **`tools/lsusb.c`** — Linux-style USB lister built on AxlUsb
      (Phase E, commit `249c3c4`). Default short form, `-s BBB[:DDD]`
      and `-d V[:P]` filters, `-n` numeric, `-v` / `-vv` verbose
      with class triplet + string descriptors, `-t` tree view
      (real USB hub-port topology via `axl_usb_tree_for_each`).
      `--ids-file` overrides sidecar autodiscovery. Same `--debug`
      / `-v` convention divergence as lspci.
- [x] **AxlSidecar** (commit `f875b36`) — shared JSON5 sidecar
      scaffold (`<axl/axl-sidecar.h>`) consumed by AxlPciIds,
      AxlPciClassDb, AxlSpdIds, AxlUsbIds. `axl_sidecar_open_file`
      / `_open_buffer` / `_check_schema` plus an internal
      singleton-with-atexit + foreach trampoline. `AxlSidecarStatus`
      enum (`AXL_SIDECAR_OK` / `_FILE_MISSING` / `_PARSE_ERROR`)
      replaces the prior `0/-1/-2` magic numbers across every load
      API. `tools/memspd.c` migrated off its inline JEDEC loader
      onto `axl_spd_ids_load`.
- [x] **`scripts/{pci,usb}-ids-to-json5.py`** (commit `872bbf6`) —
      bulk converters for the canonical pci.ids / usb.ids text
      databases. Line-level parser hoisted into shared
      `_ids_parser.py` (~200 lines) since pci.ids and usb.ids use
      the same tab-indented hierarchy. `share/jedec.json5` stays
      hand-curated (no canonical text database upstream). Both
      installed to `/usr/share/axl/scripts/` in the .deb/.rpm
      (commit `85973b4`); both `--self-test`s pass from the
      installed location.

### Post-v0.11.0 follow-ups — vendor-neutral typed wrappers

Round-2 vendor-neutral additions surfaced from a downstream-
consumer session, all retiring re-implemented patterns in consumer
code with public typed wrappers:

- [x] **`axl_ipmi_chassis_identify`** (commit `1264e39`) — typed
      wrapper around IPMI Chassis 0x04 (front-panel ID LED).
      Replaces the raw `axl_ipmi_raw(s, 0x00, 0x04, ...)` consumers
      were writing.
- [x] **`axl_pci_get_header_type`** + **`axl_pci_get_subsystem`**
      (commit `ffc9177`) — typed readers for PCI config offset
      0x0E (header type + multi-function bit) and 0x2C/0x2E
      (SVID/SDID with Type-0 check baked in). New
      `AxlPciHeaderType` enum.
- [x] **`axl_nvstore_get_alloc`** (commit `063f391`) — read-with-
      malloc variant of `axl_nvstore_get` for variable-length NV
      values. Probe-then-grow uses the probe rc to distinguish
      empty (success, 1-byte NUL allocation) from missing (-1).
- [x] **`axl_smbios_get_oem_string`** (commit `063f391`) —
      convenience reader for Type 11 OEM Strings by 1-based global
      index across all Type 11 records.
- [x] **`AXL_ARG_CHOICE` typed positional / flag** (commit
      `063f391`) — string restricted to a caller-supplied set,
      with framework-side rejection + `<a|b|c>` value-hint help.
      Field appended to `AxlArgDesc` so existing designated-
      initializer literals keep working.
- [x] **`assert_in_section LABEL SECTION_MARKER PATTERN`**
      (initial commit `2adc170`; signature simplified in round-3
      commit) — section-aware assertion helper in
      `scripts/axl-common.sh` for nsh-driven QEMU log assertions.
      Reads the log path from `$LOG` (falls back to
      `$TEST_CLEAN_LOG`) so callers don't repeat the path.
- [x] **`axl_stream_set_stdout_tee` + `axl_stream_set_stderr_tee`**
      (round-3 commit) — log-tee primitive. New `tee` field on
      the internal `AxlStream` struct; `axl_write` forwards bytes
      to `s->tee` after the primary write. NULL clears, multiple
      calls replace (no chain). Replaces the ~50-line tee-
      callback + atexit-cleanup pattern consumers wrote per tool
      with a `-o:<file>` log option.
- [x] **`run-qemu.sh --qemu-arg STRING`** (round-3 commit) —
      repeatable literal-token passthrough to qemu's command line.
      Each STRING is shell-word-split; values with embedded spaces
      aren't supported.
- [x] **`run-qemu.sh --ipmi` / `--ipmi-extern SOCK` / `--ipmi-prop K=V`**
      (round-3 commit) — IPMI BMC simulator shortcuts at the
      canonical KCS port 0xca2 (matches AxlIpmi's KCS default and
      `test/integration/test-ipmi-qemu.sh`). `--ipmi-extern`
      paired with `ipmi-sim` is the path to verifying full BMC
      behavior including Chassis Identify and OEM commands on
      QEMU. aa64 warns and skips — no QEMU IPMI device support
      there.
- [x] **`QEMU_DRYRUN=1`** env on `run-qemu.sh` (round-3 commit) —
      prints CMD tokens one per `QEMU_DRYRUN: <token>` line and
      exits 0 without launching qemu. Backs the new
      `test-run-qemu-flags.sh` flag-shape regression tests.
- [x] **`axl_smbios_get_oem_string` truncation contract amended**
      (round-3 commit) — gains a `*required` out param; too-small
      buffers now return -1 with `*required` populated (was: silent
      truncation with rc=0). Lets callers size a follow-up
      allocation exactly.
- [x] **`AxlArgDesc.choices_case_insensitive`** flag on
      `AXL_ARG_CHOICE` (round-4 commit) — additive per-descriptor
      bool that switches CHOICE validation to ASCII case-folded
      comparison. Default false preserves the byte-equal contract.
      Helps consumers migrating from CLIs that accept mixed-case
      variants (e.g. `dd_cfg` / `DD_CFG` both valid). Help-line
      renders `<a|b|c> (case-insensitive)` so users know the
      relaxed match is in effect.
- [x] **`axl_console_read_key` + `axl_console_flush_input`**
      (round-5 commit) — interactive single-keystroke read with
      bounded timeout in new `<axl/axl-console.h>`. Wraps the
      backend ConIn primitives + a freshly-created timer event,
      closed unconditionally on return. Three timeout modes
      (0 non-blocking / `UINT64_MAX` forever / millisecond bound).
      Unblocks any interactive UEFI tool — y/n prompts, "press
      any key", arrow-key menus.
- [x] **`axl_image_verify_signature` + `axl_image_signature_info_free`**
      (round-5 commit; CN extraction in round-5 follow-up) —
      PE Authenticode signature inspection without launching the
      image, in new `<axl/axl-image-verify.h>`. Two-axis check:
      presence (pure PE Certificate-Table parse, no firmware
      dependency) + validity (firmware dry-run via
      `LoadImage(SourceBuffer)` + immediate `UnloadImage` when
      `consult_db=true`). Caller controls the security-protocol-
      callback side-effect cost. `subject_cn` / `issuer_cn`
      populate from the first cert in the PKCS#7 SignedData
      bundle via an in-tree DER walker (PrintableString /
      UTF8String). Best-effort diagnostic-only — not a
      security-decision input.
- [x] **Vendor-neutralization sweep** (commit `8d06e8f`) —
      `axl_smbios_slot_usage_str(0x05)` now returns spec-canonical
      `"Unavailable"` (was `"CPU NOT INSTALLED"`); chassis 0x23
      "Mongoose Mini PC" comments → "Mini PC" per SMBIOS 3.7;
      `AxlIpmiCapabilities.dell_idrac_interface` removed (reverse-
      engineered GUID, no public spec). New "Spec-decoder strings
      are spec-canonical" rule in `docs/AXL-Coding-Style.md`.

### Phase B2: Redfish Support — DONE (as tool, not library)

Decided against a library-level `axl_redfish_*` module — the existing
HTTP client + JSON APIs cover everything Redfish needs. Instead built
`rfbrowse.efi` as a standalone tool.

- [x] `rfbrowse.efi` — Redfish REST API browser (tools/rfbrowse.c)
- [x] Session auth (POST → X-Auth-Token) and Basic auth
- [x] URI shortcuts (systems, thermal, power, chassis, etc.)
- [x] Collection member listing (--members, --expand)
- [x] Colored JSON pretty-print and raw mode
- [x] Python mock server + 12 integration tests (test-redfish.sh)

**No library module needed** — rfbrowse uses axl_http_client + axl_json
directly. If a library API is needed later (ipmitool, SoftBMC), extract
the ~50 lines of session management then.

---

## Async Work Phases (Future)

### Phase A1: AxlBufPool — preallocated buffer pool — DONE

- [x] `axl_buf_pool_new(count, buf_size)` — allocate pool of fixed-size buffers
- [x] `axl_buf_pool_get(pool)` — grab a free buffer (NULL if exhausted)
- [x] `axl_buf_pool_put(pool, buf)` — return buffer to pool
- [x] `axl_buf_pool_available(pool)` — number of free buffers
- [x] `axl_buf_pool_buf_size(pool)` — query buffer size
- [x] `axl_buf_pool_free(pool)` — release pool and all buffers
- [x] Zero-copy design: LIFO free-stack, no memcpy on get/put
- [x] 18 unit tests (basic, exhaustion, distinct, LIFO order, NULL safety)

### Phase A2: AxlAsync — AP-offloaded async work queue — DONE

Bridges AxlTask (AP core dispatch) with AxlLoop (main loop events).
Enables offloading CPU-heavy work to Application Processors while
the BSP continues servicing the main loop (network, timers, UI).

- [x] `axl_async_init(max_pending)` — initialize with configurable queue depth
- [x] `axl_async_submit(loop, work_fn, data, arena, done_cb)` — dispatch
      work_fn to an AP core, fire done_cb on the BSP when complete
- [x] Idle source polls `axl_task_pool_poll()`, auto-removed when idle
- [x] Automatic single-core fallback: runs work_fn + done_cb synchronously
- [x] Cancellation: `axl_async_cancel(handle)` — best-effort (suppresses done_cb)
- [x] `axl_async_pending()` — query in-flight job count
- [x] `axl_async_shutdown()` — drain and free
- [x] 13 unit tests (init, submit, loop integration, cancel, pending, NULL safety)

**File transfer example (firmware update):**

```
AxlBufPool *pool = axl_buf_pool_new(4, 64 * 1024);  // 4 x 64KB

// HTTP handler receives chunks on the BSP:
void on_chunk_received(void *chunk_data, size_t len, void *ctx) {
    void *buf = axl_buf_pool_get(pool);      // grab free buffer
    axl_memcpy(buf, chunk_data, len);          // copy into pool buffer
    TransferCtx *tc = make_ctx(buf, len, offset);
    axl_async_submit(loop, verify_and_stage,  // runs on AP
                     tc, on_chunk_done, tc);  // callback on BSP
    // returns immediately — BSP keeps accepting connections
}

// Runs on AP core (no Boot Services access):
void verify_and_stage(void *arg) {
    TransferCtx *tc = arg;
    tc->crc = crc32(tc->buf, tc->len);       // CPU-heavy work on AP
    tc->status = validate_chunk(tc);
}

// Fires on BSP main loop after AP completes:
void on_chunk_done(void *arg) {
    TransferCtx *tc = arg;
    if (tc->status == OK) {
        flash_write(tc->offset, tc->buf, tc->len);  // BSP: Boot Services OK
    }
    axl_buf_pool_put(pool, tc->buf);          // return buffer to pool
    free(tc);
}
```

**Double-buffer ping-pong pattern:**
- BSP receives data into buffer A, submits A to AP for processing
- BSP starts receiving into buffer B while AP works on A
- AP finishes A → done_cb fires → BSP submits B, starts receiving into A
- Maximizes throughput: network I/O and computation overlap

**AP constraints in UEFI:**
- APs cannot call Boot Services (only BSP can)
- APs can do: memcpy, checksum, CRC, crypto, decompression, parsing
- BSP handles: network I/O, file I/O, flash writes, protocol calls
- ARM: cache flush needed for shared buffers (x86 is coherent)

**Consumer projects:**
- SoftBMC: firmware update endpoint, bulk SMBIOS collection
- axl-webfs: WebDAV PUT (large file writes to UEFI filesystem)
- uefi-devkit: image deployment

### Phase A3: AxlDefer — deferred work queue — DONE

BSP-only work queue drained by the main loop on each tick.  Allows
code in constrained contexts (protocol notifications, nested callbacks,
interrupt-like handlers) to schedule work for "later this tick" without
blocking or re-entering the loop.

- [x] `axl_defer(loop, fn, data)` — enqueue work (function + context pointer)
- [x] `axl_defer_cancel(loop, handle)` — remove pending work before it fires
- [x] FIFO ordering: work fires in submission order
- [x] Loop integration: queue drained at the start of each loop iteration,
      before timer/event sources are checked
- [x] Fixed-capacity ring buffer (no malloc in the hot path)
- [x] 8 unit tests (basic, cancel, FIFO order, re-entrant, null safety)

**Example: protocol notification handler**

```
// This runs in a LocateProtocol notify context — can't do complex
// work here, but can schedule it for the next loop tick:
void on_protocol_installed(void *ctx) {
    axl_defer(loop, initialize_new_protocol, ctx);
}

// Runs safely on the next main loop iteration:
void initialize_new_protocol(void *ctx) {
    locate_and_configure(ctx);  // Boot Services OK here
    start_polling_timer(ctx);
}
```

### Phase A4: AxlPubsub — publish/subscribe event bus — DONE

Decouples event producers from consumers.  Modules publish on named
topics, other modules subscribe with callbacks.  The main loop
dispatches subscriber callbacks (via AxlDefer) so handlers run in a
safe context.

Renamed from `axl_signal_*` to `axl_pubsub_*` pre-1.0 to free the
`axl_signal_*` namespace for the POSIX-style interrupt API (see
Phase A7).

- [x] `axl_pubsub_register(loop, name)` — register a named topic
- [x] `axl_pubsub_subscribe(loop, name, callback, data)` — subscribe
- [x] `axl_pubsub_unsubscribe(loop, handle)` — unsubscribe by handle
- [x] `axl_pubsub_publish(loop, name, event_data)` — publish (deferred delivery)
- [x] Multiple subscribers per topic (linked list, order-independent)
- [x] Payload: opaque `void *` passed to all subscribers
- [x] Auto-create topics on first subscribe or publish
- [x] 13 unit tests (basic, multi-sub, unsubscribe, unknown, auto-create, user_data)
- [ ] Optional: typed variants with compile-time checked payloads

**Example: network state change**

```
// Network module publishes when IP changes:
axl_pubsub_publish(loop, "ip-address-changed", &new_ip);

// Splash screen subscribes:
axl_pubsub_subscribe(loop, "ip-address-changed", splash_update_ip, NULL);

// REST API subscribes independently:
axl_pubsub_subscribe(loop, "ip-address-changed", api_update_endpoint, NULL);

// Both handlers fire on the next loop tick — neither knows
// about the other.  Adding a third subscriber (e.g., mDNS
// announcer) requires zero changes to the network module.
```

**Consumer projects:**
- SoftBMC: decouple modules (network → splash, EC → sensors → REST API)
- axl-webfs: filesystem mount/unmount notifications
- Any multi-module UEFI application built on AXL

### Phase A5: AxlEvent foundation + sync-primitive reorg — DONE

Promoted `AxlEvent` to a first-class struct wrapping `EFI_EVENT` with
signalled/reset state. Relocated the sync primitives (Cancellable,
Wait) out of `src/util/` into a dedicated `src/event/` module.
`AxlCompletion` collapsed into `AxlEvent` (structurally identical;
UEFI-native name wins over the Linux-kernel-struct-completion echo).

Framing (chosen April 2026, executed April 2026): **D + III**.
Directory named `src/event/` after its foundational type; the word
overload between "event loop" and "AxlEvent" embraced explicitly in
docs ("an AxlEvent is a one-shot latch backed by a UEFI event; the
event loop dispatches them").

- [x] **New `src/event/` module.** `axl-event.{c,internal.h}` new;
      `axl-cancellable.{c,internal.h}` and `axl-wait.{c,internal.h}`
      moved from `src/util/`. New `src/event/README.md` is now the
      prose home for the three primitives.
- [x] **Promoted `AxlEvent` to a proper struct** in
      `include/axl/axl-event.h`. Public API: `axl_event_new/free/
      signal/reset/is_set/handle/wait/wait_timeout`. The raw `void *`
      typedef in `axl-loop.h` removed; raw handle type renamed to
      `AxlEventHandle` and promoted to the public header (was
      internal in `src/backend/axl-backend.h`).
- [x] **Collapsed AxlCompletion into AxlEvent.** `axl-completion.h`
      deleted; `AxlCancellable` kept as a typed contract wrapper
      composing `AxlEvent *` with the magic-number UAF guard.
- [x] **Sphinx update.** `docs/sphinx/modules/async.rst` renamed to
      `event.rst`; `axl-event.h` doxygenfile block added;
      `index.rst` toctree updated.
- [x] **Side cleanup.** `src/task/axl-arena.c` moved to `src/mem/`.
      Arenas are allocators, not task/offload primitives.
- [x] **CHANGELOG entry** for the breaking migration
      (`AxlCompletion` → `AxlEvent`, raw-handle API removal,
      `<axl/axl-completion.h>` → `<axl/axl-event.h>`).
- [x] **Updated `CLAUDE.md`** module table + Project Layout tree.
- [x] **Plan deviation (documented).** Original plan had
      `axl_loop_add_event` take `AxlEvent *`. That would have forced
      `src/net/` to wrap every firmware-owned completion token in an
      AxlEvent struct — semantically wrong and adapter overhead on
      every async op. Corrected to keep the entry taking an
      `AxlEventHandle` and exposing `axl_event_handle(e)` as the
      extractor for AXL-managed events.
- [x] **Verify**: X64 + AARCH64 tests **1302/1302 passing** (up from
      1295 — new AxlEvent surface tests); event-demo + cancellable-demo
      clean in QEMU, no leaks.

### Phase A6: Concurrency Model documentation — DONE

Landed after A5. Single authoritative doc telling users which
synchronization primitive to reach for.

- [x] **New `docs/AXL-Concurrency.md`.** Four-axis taxonomy table
      (dispatch / coordination / notification / offload) with a
      loop-integration column, decision guide ("I need to... →
      use..."), the word-overload disclaimer ("event" = loop +
      source + AxlEvent type), and comparison with adjacent
      ecosystems (GLib `GMainLoop` + `GCancellable`, Python asyncio,
      libuv, Linux kernel `struct completion`, C++ `std::latch`).
- [x] Cross-linked from `docs/AXL-Design.md` (after [§API Overview](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Design.md#api-overview)),
      `docs/AXL-SDK-Design.md` ([§Async-op cancellation](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-SDK-Design.md#async-op-cancellation)), and
      `src/event/README.md` (after [§When to use what](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md#when-to-use-what)).
- [x] Sphinx guide page: `docs/sphinx/guides/concurrency.rst`
      `.. include::` of the markdown file; added to the Guides
      toctree in `index.rst` between Design and Coding Style.
- [x] Explicitly documented the "why not" positions (GIL, stackful
      coroutines, protothread macros, macro-async/await) so future
      contributors don't re-litigate them.

### Phase A7: AXL runtime — lifecycle services (signals, yield, atexit, default loop) — DONE

**Status:** landed April 2026 as seven commits on `main`
(`3789aea`...`4368256`). See [docs/AXL-Lifecycle.md](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md)
(status: implemented) and `src/runtime/`.

Since AXL controls every public API, we approximate Linux-style
signal responsiveness cooperatively — `axl_yield()` in tight app
loops, a centralized break handler invoked on Ctrl-C, and a POSIX-
flavored `axl_signal_install` / `axl_atexit` / `axl_exit` surface
for ergonomics. Full preemption is not reachable under UEFI BSP;
CPU-bound code that ignores AXL APIs remains uninterruptible --
documented as such.

What landed:

- [x] CRT0 (`src/crt0/axl-crt0-native.c`) calls
      `_axl_init(ImageHandle, SystemTable)` -> `main` ->
      `_axl_cleanup`. `_axl_init` / `_axl_cleanup` now live in
      `src/runtime/axl-runtime.c`.
- [x] `axl_loop_default()` — singleton, lazy-created on first
      call, freed during `_axl_cleanup`.
- [x] Shell break-flag + break-event detection in
      `axl_loop_next_event` and `axl_yield` calls
      `_axl_signal_on_break`, which sets `g_axl_interrupted` and
      invokes the user handler exactly once.
- [x] `axl_signal_install` / `axl_signal_default` /
      `axl_interrupted` / `axl_exit` public API at
      `include/axl/axl-signal.h` (namespace freed pre-landing by
      renaming pub/sub to `axl_pubsub_*` in PR #1).
- [x] `axl_yield()` public API. Non-blocking default-loop
      dispatch; polls `axl_backend_shell_break_flag` directly
      when no default loop exists so yield-only apps still
      observe Ctrl-C. Default-policy auto-exit via `axl_exit(1)`
      when no user handler is installed.
- [x] `axl_atexit(fn, data)` / `axl_atexit_remove(handle)` --
      `include/axl/axl-atexit.h`, LIFO drain during
      `_axl_cleanup` before the registry sweep.
- [x] Tier-1 firmware-resource registry --
      `src/runtime/axl-registry.c`. AxlArray-backed + monotonic
      seq for true LIFO sweep; always on ([design §9](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#9-design-decisions-locked-in)).
- [x] **`axl_exit(rc)` as the blessed exit path.** NORETURN. Runs
      atexit + sweep, then `axl_backend_boot_exit(rc)` ->
      `gBS->Exit`. Both return-from-main and explicit `axl_exit`
      converge on `_axl_cleanup`; output is byte-identical.
- [x] **`AxlArena` registered as tier-1.** Arena sub-allocations
      bypass individual tracking by design; the arena itself
      carries the registry entry.
- [x] **Caller attribution via macro shims** on `axl_event_new`,
      `axl_loop_new`, `axl_cancellable_new`, `axl_arena_new`.
      Sweep warnings name the user's call site (or library call
      site for library-internal allocations — which correctly
      freed never reach the sweep anyway).
- [x] **`axl_loop_iterate_until`** (nested-wait primitive, [design
      §5.6](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#56-nested-wait-primitive-axl_loop_iterate_until)) — lets callers inside a loop callback wait on an
      event without freezing the outer loop's other sources.
- [x] `runtime-demo.c` — 8 subcommand scenarios covering every
      facet, validated on X64 + AARCH64.
- [x] `test/unit/axl-test-runtime.c` (AxlTestRuntime) — 16
      `test_check` calls covering atexit, registry, yield,
      interrupted, signal-install.
- [x] Cooperative-concurrency caveat documented in
      [docs/AXL-Lifecycle.md §11](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#11-what-this-doesnt-help-with) and
      [docs/AXL-Concurrency.md](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md).

Deferred to a future phase (both captured in
[docs/AXL-Lifecycle.md §10](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#10-deferred-items)):

- [ ] **Release-mode heap auto-sweep.** `mAllocList` exists only
      under `AXL_MEM_DEBUG` today; making release-build sweeps
      possible costs ~16 bytes per allocation. Implement when a
      long-running app like SoftBMC or persistent-service axl-webfs
      needs the firmware-pool safety net. Short-lived tool apps
      don't benefit — firmware reboot reclaims pool memory.
- [ ] **Watchdog opt-in** (`axl_watchdog_enable(seconds)`) --
      library-livelock guard, not a signal mechanism. No concrete
      caller has asked for it yet.

Design decisions locked in (see [design doc §7](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#7-what-we-are-not-doing), [§9](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#9-design-decisions-locked-in)):

- No `longjmp` from break notify — async-signal-unsafety.
- No UEFI watchdog repurpose — reset-only on every platform.
- No NMI or platform-specific preemption hooks.
- CPU-bound no-AXL code is documented as uninterruptible, not
  papered over.

### Phase D1: AxlRadixTree — compact prefix tree — DONE

Radix tree (compact prefix tree) with edge splitting, longest-prefix
lookup, and foreach iteration. Used internally to replace the HTTP
server's fixed 32-route array with O(k) route matching.

- [x] `axl_radix_tree_new` / `axl_radix_tree_new_full` — create tree
- [x] `axl_radix_tree_insert` — insert with automatic edge splitting
- [x] `axl_radix_tree_lookup` — exact key match
- [x] `axl_radix_tree_lookup_prefix` — longest-prefix match (key feature)
- [x] `axl_radix_tree_remove` — remove with node collapse
- [x] `axl_radix_tree_foreach` — depth-first iteration with key reconstruction
- [x] HTTP server route table refactored to use radix tree
- [x] 50 unit tests (insert, lookup, prefix, edge split, foreach, value_free, HTTP keys)

### Phase D2: AxlRingBuf — layered ring buffer — DONE

Byte-oriented ring buffer (kfifo-inspired) with three API layers:

- [x] Layer 1 (Bytes): push, pop, peek, discard, zero-copy scatter/gather regions
- [x] Layer 2 (Messages): push_msg, pop_msg, peek_msg (variable-size, length-prefixed)
- [x] Layer 3 (Elements): push_elem, pop_elem, peek_elem, peek/set_nth_elem (fixed-size)
- [x] Power-of-2 sizing with monotonically increasing uint32_t indices
- [x] Reject-on-full (default) and overwrite-on-full modes
- [x] Struct exposed for embedding (init/deinit, no heap required)
- [x] Custom buf_free callback for pluggable deallocators
- [x] Refactored AxlDefer to use embedded AxlRingBuf (element API)
- [x] Refactored AxlLogRing to use embedded AxlRingBuf (element API + axl_backend_free)
- [x] 56 unit tests (bytes, wrap, overwrite, peek, regions, elements, messages, init, user buffer)

---

## Graphics Phases (Future)

### Phase G1: Graphics Output Protocol support

- [x] GOP types in uefi-manifest.json5 (extracted from spec)
- [x] AxlGfx module: basic framebuffer ops (fill, blit, capture)
- [x] gfx-demo.c example
- [x] Bitmap font renderer (8x16 VGA font, scalable)
- [x] Text drawing API (axl_gfx_draw_text)

### Phase G2: AGL (AximCode Graphics Library) — not started, separate project

GTK-like widget toolkit built on AxlGfx. Would be a separate repo.
Blocked on a consumer need (SoftBMC local UI is the first candidate,
but SoftBMC hasn't migrated to AXL yet — see Phase 10).

- [ ] Basic widgets: label, button, panel, list
- [ ] Layout engine (vertical/horizontal box)
- [ ] Input handling (keyboard + pointer via UEFI protocols)
- [ ] Theming / color scheme support

---

## Shell Integration Phases (Future)

### Phase S5: Environment and working directory — DONE

- [x] `axl_getenv(name)` / `axl_setenv(name, value, overwrite)` / `axl_unsetenv(name)`
- [x] `axl_get_current_dir()` / `axl_chdir(path)`
- [x] Type `GetEnv`, `SetEnv`, `GetCurDir`, `SetCurDir`, `Execute` in EFI_SHELL_PROTOCOL
- [x] Backend: `axl_backend_shell_getenv/setenv/getcwd/chdir/execute` in all 3 backends
- [x] 10 unit tests (set, get, overwrite, unset, missing, cwd, chdir)

### Phase S6: System operations — DONE

- [x] `axl_reset(type)` — system reset (AXL_RESET_COLD/WARM/SHUTDOWN)
- [x] `axl_map_refresh()` — rescan device mappings via Shell "map -r"
- [x] `axl_driver_load/start/connect/disconnect/unload` — driver lifecycle

### Phase S7: Socket abstraction layer — DONE

GLib-style socket layer wrapping existing AxlTcp/AxlUdp. Unifies
inconsistent address handling (hostname strings, AxlIPv4Address, raw bytes)
behind a clean API.

- [x] AxlInetAddress — IPv4 address with parsing, formatting, comparison
- [x] AxlSocketAddress — address + port pair, interop with AxlIPv4Address
- [x] AxlSocket — unified stream/datagram socket, delegates to TCP/UDP
- [x] AxlSocketClient — high-level DNS + connect helper
- [x] Async variants: connect_async, accept_async, send_start, receive_start
- [x] Tests: 12 address tests (no network) + 6 socket tests (network)

---

## Configuration Framework (Future)

### Phase CF1: AxlConfig — unified options system — DONE

Typed configuration framework with descriptors, auto-apply, and
command-line parsing. Used by HTTP client, HTTP server, and
available to consumer apps.

```c
static const AxlConfigDesc descs[] = {
    { "timeout.ms", AXL_CFG_UINT, "10000", 't', "Per-operation timeout", 0, 0 },
    { "keep.alive", AXL_CFG_BOOL, "true",  'k', "Reuse connections",     0, 0 },
    { "port",       AXL_CFG_UINT, "8080",  'p', "Listen port",           0, 0 },
    { 0 }
};

AxlConfig *cfg = axl_config_new(descs, NULL, NULL);
axl_config_set(cfg, "timeout.ms", "30000");       // programmatic
axl_config_parse_args(cfg, argc, argv);           // command-line
size_t timeout = axl_config_get_uint(cfg, "timeout.ms");
```

- [x] `AxlConfig` type with typed get/set (`get_uint`, `get_bool`, `get_string`)
- [x] Option descriptors with type, default, description
- [x] `axl_config_parse_args` — populate from argc/argv
- [x] `--help` generation from descriptors (via `axl_config_usage`)
- [x] Type validation on set
- [x] Embed in HTTP client and HTTP server (`http_client_descs`, `http_server_descs`)
- [x] Option cascade: command-line overrides config overrides defaults
- [x] Unify `axl_args_*` and `axl_config_*` into a single API —
      `axl_args_*` removed; every tool, test, and example now uses
      `AxlConfigDesc` for both config and CLI.

---

## Tools

UEFI command-line utilities built on AXL, plus host-side developer tools.

### UEFI Tools (tools/)

- [x] hexdump.efi — hex/ASCII file viewer
- [x] fetch.efi — HTTP client (curl-like)
- [x] find.efi — recursive file finder
- [x] grep.efi — pattern search
- [x] sysinfo.efi — system inventory (firmware, SMBIOS, memory)
- [x] netinfo.efi — network diagnostics and ping
- [x] mkrd.efi — RAM disk management
- [x] rfbrowse.efi — Redfish REST API browser
- [x] lspci.efi — Linux-style PCI lister (Phase B3 R+5; tree view,
      JSON5-backed vendor/device names, --bridges in run-qemu.sh)
- [x] lsusb.efi — Linux-style USB lister (Phase E of AxlUsb,
      commit `249c3c4`; tree view via `axl_usb_tree_for_each`,
      JSON5-backed vendor/device names, qemu-xhci + usb-mouse +
      usb-hub + usb-tablet topology in common-test.sh)
- [x] memspd.efi — DDR4/DDR5 SPD reader on AxlSpd
      (Phase B3, JEDEC ids JSON5 sidecar via `axl_spd_ids_*`)
- [x] cat.efi — concatenate files to stdout

### Host Tools (scripts/)

- [x] rsod-decode.py — UEFI crash dump (RSOD) decoder with MAP file support
- [x] run-qemu.sh `--interactive` + `--mount` — interactive UEFI shell
      with virtiofs host-fs mount (v0.2.8)
- [x] axl-sdk-host-tools tarball + .deb — packaged for downstream
      consumers without source clone (v0.2.9)
- [ ] **Fold `qemu_launch` into `run-qemu.sh` as a daemon-lifecycle
      mode.** uefi-devkit's `common.sh` currently provides
      `qemu_launch {start|stop|status|logs}` — a long-running QEMU
      lifecycle manager used by softbmc, videoterm, uefi-ipmitool
      for BMC/server-style components. run-qemu.sh's `--background`
      mode is bare-bones (just emits PID/SOCKET to stdout). Plan:
      add `run-qemu.sh --start --name=foo`, `--stop --name=foo`,
      `--status --name=foo`, `--logs --name=foo` with the same
      file-layout convention `qemu_launch` uses (build/qemu/<name>.{pid,sock,log,qcow2,vars.fd}).
      Then convert uefi-devkit's `qemu_launch` to a thin wrapper
      around the host-tools `run-qemu.sh`, retiring the duplicate
      QEMU-invocation code path.

      Migration: v0.3.x once consumers (softbmc, videoterm,
      uefi-ipmitool) have been updated to reference the
      consolidated invocation. axl-common.sh stays put as the
      shared discovery library — no churn for downstream.

---

## Hardware Fixture Capture & Replay (Future)

Vendor-neutral capture-and-replay of UEFI platform identity
(SMBIOS, ACPI, PCI/USB/video manifests, SPD, TPM, NVRAM/Secure
Boot, ESRT, CPU, network details, Redfish, IPMI) so axl-sdk tools
can be exercised against real-world platforms under QEMU without
lab access. New dedicated UEFI capture tool (`tools/mkfixture.c`
→ `mkfixture.efi`, mirroring the `mkrd.efi` naming pattern) writes
a fixture directory; `axl-emulate <fixture-dir>` (Python wrapper
around `run-qemu.sh`) replays it. Keeping replay in a separate
tool stops `run-qemu.sh` from ballooning as HF4–HF8 layer in
USB shims, EDID injection, CPU mapping, TPM seeding, Secure Boot
vars, Redfish mock, IPMI sim, etc.
Reuses the existing
[`scripts/qemu-patches/`](../scripts/qemu-patches/) infrastructure
(originally added for AxlSpd's wire-path test) for command-line
device injection, plus host-side daemons (`swtpm`, DMTF Redfish
Mockup-Server, OpenIPMI `ipmi_sim`) wired by lifecycle code modeled
on the existing virtiofsd handling.

Full design: [AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md).

### Phase HF1: run-qemu.sh low-level flags

- [ ] `--smbios-file FILE` → `-smbios file=FILE`
- [ ] `--acpi-table FILE` (repeatable) → `-acpitable file=FILE`
- [ ] `--spd ADDR:FILE` (repeatable) → `memory-backend-file` +
      `smbus-eeprom,memdev=` (depends on existing patched QEMU;
      probe and error clearly when absent; aa64 warn-and-skip)
- [ ] `--tpm` / `--tpm-state DIR` /
      `--tpm-model tpm-tis|tpm-crb|tpm-tis-device`
      → spawn swtpm (raw state passthrough; captured-fixture
      seeding deferred to HF5) and wire `-tpmdev emulator` + tpm
      device. Arch-aware default model
      (tpm-tis on x64, tpm-tis-device on aa64; tpm-crb is x86-only).
      swtpm absent on PATH ⇒ hard error with install hint.
- [ ] **NOT a new flag**: IPMI is already covered by the existing
      `--ipmi` / `--ipmi-extern` / `--ipmi-prop` in run-qemu.sh;
      no `--ipmi-sim` alias added.
- [ ] Hand-craft first fixture from the Proxmox dev VM
      (`dmidecode --dump-bin`, `acpidump -b`) to validate replay
      path before writing the capture tool.

### Phase HF2: mkfixture.efi (manifest-grade UEFI walks)

Phase HF2 ships in slices — HF2.1 covers the smallest viable
fixture (SMBIOS + ACPI + manifest); HF2.2+ adds the per-protocol
JSON manifests; HF2.3 adds alternative write targets.

**HF2.1 — DONE (commit 46a326c)**:
- [x] New tool `tools/mkfixture.c` → `mkfixture.efi` (separate
      from `sysinfo` — capture is a binary-blob writer, not an
      inventory display, and conflating them muddies both;
      cross-tool sharing happens at the library layer)
- [x] Dump SMBIOS3 raw bytes via EFI Config Table
- [x] Walk ACPI RSDT/XSDT, write each table as `acpi/<sig>.dat`
- [x] Write `manifest.json` (vendor, model, BIOS rev/date,
      capture-tool version, fixture format)

**HF2.2 — DONE**:
- [x] CPU capture: direct CPUID (x86) / MIDR_EL1 (aa64), write
      `cpu.json` (vendor, family/model/stepping, brand string,
      feature words). Future axl-emulate `--cpu-from-fixture`
      maps to a QEMU `-cpu MODEL` choice.
- [x] ESRT capture: read EFI Config Table
      `EFI_SYSTEM_RESOURCE_TABLE_GUID`, write `esrt.json`
      (per-component FwClass GUID + version + last-attempt status)
- [x] New public API `axl_efi_find_config_table` (axl-runtime.h)
      so tools can do one-shot config-table lookups without
      duplicating the EFI Configuration Table walk

**HF2.3 — TODO** (manifest expansion):
- [ ] Enumerate PCI via `EFI_PCI_IO_PROTOCOL`, write `pci.json`
      (VID/DID/class/subsys/BARs) — manifest only, not replayed
- [ ] Walk USB via `EFI_USB_IO_PROTOCOL` + `EFI_USB2_HC_PROTOCOL`,
      write `usb.json` (topology, VID/PID, class/subclass/protocol,
      strings) and per-device descriptor blobs in `usb/*.bin`
- [ ] Walk GOP/EDID via `EFI_GRAPHICS_OUTPUT_PROTOCOL` +
      `EFI_EDID_DISCOVERED_PROTOCOL`, write `video.json` (mode list,
      current mode, FB base, pixel format) and `edid/*.bin` per
      display; GPU option ROM (`gpu-rom/*.bin`) on-demand only
- [ ] Network details: per-NIC MAC + link state via
      `EFI_SIMPLE_NETWORK_PROTOCOL`, SR-IOV VF count from PCIe
      extended config, write `net.json`
- [ ] NVMe capture: per-controller Identify Controller / Identify
      Namespace via `EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL`, write
      `nvme/<bdf>.json` (manifest only — replay is HF9 patch
      candidate)

**HF2.4 — TODO** (alternative write targets):
- [ ] Support write targets: local FS (`fs0:\fixtures\...`) is the
      default and shipped in HF2.1; add virtiofs `--mount` and
      HTTP POST

### Phase HF3: scripts/axl-emulate (replay wrapper)

New Python tool, ships in host-tools tarball. Wraps `run-qemu.sh`
rather than extending it — keeps run-qemu.sh focused on QEMU
launching primitives, gives fixture-replay logic its own home so
HF4–HF8 doesn't balloon run-qemu.sh further.

```
axl-emulate <fixture-dir> [efi-file] [args...]
            [--keep-acpi NAME] [--drop-acpi NAME] [--strict-acpi]
            [--arch X64|AARCH64]
            [-- run-qemu-args...]
```

- [ ] `scripts/axl-emulate` (Python, no extension; `#!/usr/bin/env
      python3`) — auto-discover and translate `smbios.bin`,
      `acpi/*.dat`, `spd/*.bin`, `tpm/` (presence triggers
      `--tpm-state`) into the corresponding run-qemu.sh primitives
- [ ] Default-drop ACPI tables that describe captured-platform
      topology incompatible with QEMU (`MCFG`, `MPST`, `PMTT`,
      `HMAT`, `SLIT`, `SRAT`, `SPCR`, `DBG2`); detection via
      4-byte signature read from each `.dat`
- [ ] `--keep-acpi NAME` / `--drop-acpi NAME` / `--strict-acpi`
      overrides for the denylist
- [ ] Print `manifest.json` summary at startup if present so user
      sees which machine the guest is impersonating
- [ ] Warn (not block) when fixture metadata disagrees with `--arch`
- [ ] Pass-through `--` separator: anything after lands as
      additional run-qemu.sh args (compose with `--background`,
      `-i`, `--mount`, `--gdb`, etc.)
- [ ] **Future-phase wiring (NOT in HF3 scope)** — `--usb-shim`
      (HF4), `--edid` / `--gpu-rom` (HF4), `--cpu-from-fixture` /
      `--mac` (HF4), TPM event-log seeding (HF5), Secure Boot /
      boot-vars injection (HF6), Redfish mock spawn (HF7), IPMI
      sim spawn (HF8) all land in axl-emulate as their phases ship.

### Phase HF4: SPD capture

- [ ] Add SPD walk to `mkfixture` (NOT extending `memspd`, which
      stays an inspection tool — same separation argument as
      sysinfo-vs-mkfixture). Dump every populated SMBus EEPROM at
      0x50–0x57 to `spd/0xNN.bin`
- [ ] Validate: capture on a real box, replay via Phase HF3
      `--fixture` path, AxlSpd output should match bit-for-bit
- [ ] Document SmbusHcShim.efi requirement on QEMU + native ICH/PCH
      driver expectation on real Intel platforms

### Phase HF5: TPM capture & replay

- [ ] Capture: PCR values (SHA-1 + SHA-256 banks), capabilities,
      manufacturer ID, firmware version via `EFI_TCG2_PROTOCOL`;
      write `tpm.json`
- [ ] Capture: full TCG event log via `EFI_TCG2_PROTOCOL.GetEventLog()`;
      write `tpm/event-log.bin`
- [ ] Replay: spawn `swtpm` with seeded state directory, wire
      `-tpmdev emulator` + `tpm-tis` (default) or `tpm-crb` per
      fixture; lifecycle modeled on virtiofsd
- [ ] Document caveat: seeded PCRs reflect source platform; replay
      guest's OVMF measurements diverge by design

### Phase HF6: UEFI Variable injection (Secure Boot + boot order)

- [ ] Capture: walk `EFI_GLOBAL_VARIABLE` GUID for `Boot####`,
      `BootOrder`, `BootCurrent`, `BootNext`, `Timeout`; write
      `vars/global/<name>.bin`
- [ ] Capture: PK / KEK / db / dbx via Variable Services; write
      `vars/secureboot/{PK,KEK,db,dbx}.bin` (raw
      `EFI_SIGNATURE_LIST` bytes)
- [ ] Replay: `--secureboot DIR` and `--boot-vars DIR` inject into
      OVMF `vars.fd` copy before QEMU launch (likely via
      `virt-fw-vars` from libvirt)
- [ ] Detect non-secboot OVMF (default `OVMF_CODE.fd` ignores
      Secure Boot vars) and warn clearly when `--secureboot` is
      given against it

### Phase HF7: Redfish capture & replay

- [ ] Capture: walk service root via axl HTTP client, save JSON
      tree mirroring `/redfish/v1/...`
- [ ] Replay: `--redfish-mock DIR` spawns DMTF `Redfish-Mockup-Server`,
      adds `--hostfwd <port>:443` (lifecycle modeled on virtiofsd)
- [ ] Reference / validation: OpenBMC firmware running in a sibling
      QEMU instance — real bmcweb stack, no lab hardware
- [ ] `--openbmc-qemu PATH` flag for direct OpenBMC sibling launch

### Phase HF8: IPMI capture & replay

- [ ] Capture: in-band KCS sweep (Get-* commands), write raw
      response bytes to `ipmi/<cmd>.bin`
- [ ] Replay: `--ipmi-extern PATH` wires QEMU's `ipmi-bmc-extern`
      to OpenIPMI's `ipmi_sim` seeded with captured replies
- [ ] Standard commands only — no vendor OEM in the initial pass

### Phase HF9: Additional QEMU device-injection patches

Add as future fixture artifacts demand. The existing
`0001-smbus-eeprom-add-memdev-link.patch` is the canonical example
of the pattern. Likely candidates:

- [ ] SMBIOS handle preservation for OEM Type-N injection
- [ ] Non-EEPROM SMBus sensors (LM75-style temp, fan controllers)
- [ ] IPMI FRU storage seeding for `ipmi-bmc-sim`
- [ ] `usb-stub` device for non-class-compliant USB descriptor
      replay (enables "device appears in bus walk" tests)
- [ ] ESRT publication: a QEMU pseudo-device that publishes a
      captured `esrt.json` as the EFI_SYSTEM_RESOURCE_TABLE config
      table at boot
- [ ] NVMe Identify Controller replay: extend `-device nvme` (or
      add a sidecar device) to source Identify Controller / Identify
      Namespace responses from a captured blob

### Phase HF10: Sanitization & public fixtures

- [ ] `--sanitize` flag (or post-processor): zero serials/asset
      tags, replace MACs with locally-administered ranges, review
      OEM strings for proprietary data
- [ ] Decide: sibling repo `aximcode/axl-fixtures` vs. local-only
- [ ] Contributor sanitization-review process (if going public)

---

## Documentation Phases (Future)

### Phase D1: API reference

- [x] Sphinx+Breathe auto-generates API docs from header comments
- [x] `docs/AXL-API-Reference.md` removed (redundant with generated docs)
- [x] Add examples to each module section (in src/*/README.md, included by Sphinx)

### Phase D2: Generated documentation — DONE

- [x] Doxyfile + Sphinx + Breathe for HTML/man generation
- [x] 17 module pages with prose overviews, code examples, UEFI glossary
- [x] CI integration: auto-deploy to axl.aximcode.com on push (Cloudflare Pages)
- [x] Man pages generated for all modules
- [x] Landing page: version, license, header, source metadata
- [x] Guides section: Getting Started, Design, Coding Style, SDK, Porting, Roadmap
- [x] Shared Types reference page (all callback types indexed)
- [x] Design docs (AXL-Design.md, etc.) integrated into Sphinx sidebar

---

## Platform Abstraction (Future — coreboot support)

Separate UEFI-specific code from platform-agnostic code so AXL can
target coreboot (and potentially other firmware environments) in
addition to UEFI.

**Current state:** 29 of 47 source files are already platform-agnostic.
18 files make direct UEFI calls (87 call sites) outside the backend
abstraction layer. The backend header (`src/backend/axl-backend.h`)
defines the abstraction API but not all modules use it consistently.

### Phase P1: Audit and classify modules

Categorize every source file:
- **Core** (platform-agnostic): mem, format, data, str, string,
  json, cache, list, slist, queue, hash-table, args, config,
  hexdump, log-ring, defer, signal, arena, buf-pool, url,
  http-middleware
- **Backend-abstracted** (uses backend API, not UEFI directly):
  io, log, loop, time, task-pool, async, io-buf, io-file, log-file
- **UEFI-coupled** (calls gBS/gRT/gST/protocols directly):
  tcp, udp, net-util, http-server, http-client, http-core,
  gfx, mem (pages), driver, nvstore, service, smbios, sys, app,
  tls, mbedtls-platform

- [ ] Document the classification in a table
- [ ] Identify which UEFI calls in coupled modules should become
      backend functions vs. staying in a UEFI platform module

### Phase P2: Expand backend abstraction

Move direct UEFI calls behind new backend functions:

- [ ] `axl_backend_locate_protocol(guid, interface)` — wraps
      gBS->LocateProtocol, LocateHandleBuffer, HandleProtocol
- [ ] `axl_backend_alloc_pages(count, phys_addr)` — wraps
      gBS->AllocatePages/FreePages
- [ ] `axl_backend_create_event(type, callback, ctx, event)` — wraps
      gBS->CreateEvent/CloseEvent/CheckEvent/SignalEvent
- [ ] `axl_backend_install_protocol(handle, guid, interface)` — wraps
      gBS->InstallProtocolInterface/UninstallProtocolInterface
- [ ] `axl_backend_get_variable / set_variable` — wraps
      gRT->GetVariable/SetVariable
- [ ] `axl_backend_exit(status)` — wraps gBS->Exit

**Networking is the largest task**: TCP, UDP, and HTTP use UEFI
protocol calls extensively (service binding, completion tokens,
Poll, Configure). Options:
  a) Abstract each protocol behind a backend socket API
  b) Keep networking as a UEFI-only module, provide a separate
     coreboot networking module later (Linux socket API)
  c) Define a portable socket API in the backend, implement for
     UEFI (TCP4/UDP4) and coreboot (Linux sockets) separately

Option (c) is cleanest but most work. Option (b) is pragmatic.

### Phase P3: Split source tree

Reorganize into platform-agnostic and platform-specific directories:

```
src/
  core/          ← platform-agnostic (mem, str, data, format, log, etc.)
  platform/
    uefi/        ← UEFI backend + UEFI-specific modules
    coreboot/    ← future: coreboot backend
  net/           ← networking (may stay UEFI-specific initially)
```

- [ ] Move core modules to src/core/
- [ ] Move UEFI-specific code to src/platform/uefi/
- [ ] Update Makefile, install.sh, and header paths
- [ ] Verify all builds and tests pass

### Phase P4: coreboot backend stub

- [ ] Create `src/platform/coreboot/axl-backend-coreboot.c`
- [ ] Implement core backend functions (console, memory, time)
- [ ] Build `libaxl-core.a` (platform-agnostic subset)
- [ ] Test core modules on coreboot (or Linux as a proxy)

**Dependencies:** This is a large architectural change. Should be
done after the API stabilizes (post-1.0) to avoid churn during the
refactor. The backend abstraction layer was designed for this split
from the beginning.

**Estimated effort:** P1 (1 day), P2 (3-5 days), P3 (2-3 days),
P4 (3-5 days). Total: ~2-3 weeks.

---

## C++ Bindings — `axlmm` (Future)

Sibling C++ wrapper library over the C public surface, modeled on
GLib's `glibmm`. The C library remains canonical; `axlmm` is a
thin, optional layer for consumers who prefer C++ ergonomics
(RAII, exceptions, range-based for, `std::string_view`) without
requiring the C surface to grow C++-aware.

**Why a sibling library, not a rewrite:**
- Most UEFI tooling is plain C; the C library carries no C++
  toolchain, runtime, or ABI cost for those consumers.
- C++ consumers (UEFI test harnesses, vendor diagnostic tools that
  already use C++, future axl-sdk-built apps with richer object
  models) get an idiomatic surface without losing access to the C
  primitives.
- GLib/glibmm precedent — independently versioned, additive, never
  blocks the C library's release cadence.

**Scope (in):**
- RAII wrappers for handle-bearing types: AxlStream, AxlEvent,
  AxlCancellable, AxlHashTable, AxlArray, AxlList, AxlSlist,
  AxlQueue, AxlStrBuf, AxlHttpServer, AxlHttpClient, AxlPciIds,
  AxlUsbIds, AxlSpdIds, AxlSidecar, AxlArena, AxlBufPool,
  AxlRingBuf, AxlRadixTree, AxlCache.
- `std::string_view` / `std::span<const std::byte>` friendly
  overloads where the C API takes `const char *, size_t` or
  `const void *, size_t`.
- Exception-throwing variants (`axlmm::Exception` derived from
  `std::runtime_error`) of the error-returning C functions, gated
  per call site (the underlying C function stays available for
  consumers that prefer error codes).
- Range-based-for adapters for the `_foreach` / `_iter` /
  `_next`-cursor APIs (AxlPci cursor, AxlUsb cursor, AxlHashTable
  iter, AxlArray iter, AxlSidecar foreach).
- Type-safe wrappers for the variadic format APIs (`axl_printf`
  family) using parameter packs.

**Scope (out — non-goals):**
- No STL allocator integration. AxlMem stays the allocation root;
  `axlmm` containers wrap AxlArray/AxlHashTable rather than
  re-implementing `std::vector`/`std::unordered_map` over AxlMem.
- No rewrite of any C internals. `axlmm` is consumer-only headers
  + a small linkable shim where exception translation needs a
  hidden symbol.
- No template-heavy headers — UEFI binary size matters. Templates
  used sparingly (handles, span overloads); no header-only
  generic-algorithms library.
- No runtime polymorphism for handle types — the C API's opaque
  pointer model maps to a single `class` per type, not an
  inheritance hierarchy.
- No coroutine / `std::future` integration in the first cut. The
  AXL async model is callback-based; bridging it to coroutines is
  a separate larger design (revisit if a consumer asks).

**Toolchain:**
- New `axl-c++` driver (or `axl-cc --lang=cpp`) — same flag
  surface as `axl-cc`, invokes `g++` for compilation, otherwise
  identical link flow. Build-gated by `AXL_CPP=1` at SDK install
  time so the C-only install stays minimal.
- Headers under `include/axlmm/*.hpp`. Umbrella `<axlmm.hpp>`
  mirroring `<axl.h>`. Namespace `axlmm`.
- Shipped as a separate package: `axl-sdk-cpp.deb` /
  `axl-sdk-cpp.rpm` depending on `axl-sdk`. The base `axl-sdk`
  package never grows a libstdc++ runtime dependency.
- Unit tests under `test/unit/axlmm-test-*.cpp`, run by the same
  `test-axl.sh` ratchet (separate count tier so the C-side
  ratchet isn't perturbed).

### Phase CPP1: Foundation + handle-bearing wrappers

- [ ] `axl-c++` toolchain driver (or `axl-cc --lang=cpp`)
- [ ] `<axlmm/handle.hpp>` — common RAII handle template
      (move-only, configurable deleter)
- [ ] `<axlmm/exception.hpp>` — `axlmm::Exception` base + per-status
      derived types (`CancelledError`, `NoMemoryError`, etc.)
- [ ] First wrapper pass: AxlStream, AxlEvent, AxlCancellable,
      AxlStrBuf, AxlArena
- [ ] Build-gate `AXL_CPP=1` in `scripts/install.sh`; new package
      target in release flow
- [ ] `sdk/examples/hello.cpp` builds via `axl-c++` and prints to
      a wrapped AxlStream

### Phase CPP2: Containers + iteration

- [ ] AxlArray, AxlList, AxlSlist, AxlQueue, AxlHashTable wrappers
      with `begin()`/`end()` and range-for support
- [ ] AxlSidecar foreach adapter
- [ ] AxlPci / AxlUsb cursor adapters (range-for over enumeration)
- [ ] `std::string_view` + `std::span` overloads across the
      Phase CPP1 surface

### Phase CPP3: Networking + format

- [ ] AxlHttpServer / AxlHttpClient wrappers (route registration
      via lambdas with capture-friendly storage)
- [ ] AxlTcp / AxlUdp socket wrappers
- [ ] Type-safe `axlmm::format` / `axlmm::print` parameter-pack
      wrappers over `axl_printf` family

### Phase CPP4: Polish

- [ ] Sphinx documentation generation for `<axlmm/*.hpp>` (Doxygen
      already supports C++; needs Breathe pages alongside the C
      module pages)
- [ ] Cross-arch CI coverage (X64 + AARCH64 axlmm test build)
- [ ] Migration recipe doc: "porting a C consumer to axlmm" with
      before/after examples

**Open questions (defer until CPP1 lands):**
- Coroutine bridge for async APIs — `co_await`-able wrappers
  around AxlCancellable-aware async ops. Probably worth it
  eventually but a real consumer should drive the design.
- C++20 `std::expected` vs. exceptions for the error-returning
  variants — exceptions are the GLib precedent; `expected` is
  closer to the underlying C semantics. Likely both, with the
  caller picking per call site.
- libstdc++ in a UEFI environment — exception unwinding, RTTI,
  and `std::string` allocation all have caveats in firmware.
  CPP1 must validate the toolchain end-to-end before CPP2 commits
  to STL types in the public headers.

---

## AxlXml — generic XML reader + writer (LANDED 2026-05-10)

**X1 + X2 shipped.** Writer in `src/data/axl-xml-writer.c`, reader in
`src/data/axl-xml-parse.c`, public surface
`<axl/axl-xml.h>`. 53 unit tests. The pre-landing draft below is
kept for the design history; the shipped API differs in detail
(value-typed `AxlXmlWriter` per JSON's pattern, pull-token reader
`AxlXmlReader` returning `AxlXmlToken` rather than callback-SAX,
`AXL_XML_WRITER_PRETTY` flag rather than separate pretty-print
setter). Remaining work:

- **X3: WebDAV PROPFIND emit migration** — replace the hand-rolled
  XML emit in `src/net/axl-http-webdav.c::emit_entry` with
  `axl_xml_writer_*` calls. ~50 lines deleted + 7-line escaper
  removed. Existing integration tests should be unchanged.
- **WebDAV W6 (class-2 verbs)** — PROPPATCH / LOCK / UNLOCK request
  bodies become implementable with the reader. Gated on a consumer
  asking.
- **Dell delldiagslinux port** — `stout::XmlSink` migrates to
  `AxlXmlWriter`; `pugi::xml_document` consumers in
  `systemconfig/*` and every module's `configuration.cpp` migrate
  to the pull-token reader. Pre-1.0 axl-sdk Linux port is the
  trigger.

### Original design notes (kept for history)

**Surfaced 2026-05-10** during the WebDAV W2 landing. WebDAV's
PROPFIND emit currently hand-rolls XML via `axl_string_append`
calls in `src/net/axl-http-webdav.c::emit_entry` (~50 lines) +
a 7-line escaper for `<>&"'`. Two design pressures converging:

1. **WebDAV growth** — PROPPATCH and LOCK request bodies are
   real XML documents that need parsing (out of scope for v1
   per `sdk-prompts/2026-05-10-webdav-server.md`, but next on
   the WebDAV roadmap when a consumer asks). The hand-rolled
   emit also picks up a second site (PROPPATCH response,
   LOCK response) — the escaper and structural correctness
   start fragmenting.
2. **Second consumer** — at least one other AximCode use case
   has surfaced (user noted 2026-05-10, not yet documented in
   the SDK; will pull the requirements in when the
   consumer-side prompt lands).

### Phase X1: AxlXmlWriter — minimal streaming writer

```c
AxlXmlWriter *w = axl_xml_writer_new();
axl_xml_writer_decl(w, "1.0", "utf-8");
axl_xml_writer_start(w, "D:multistatus");
axl_xml_writer_attr(w,  "xmlns:D", "DAV:");
  axl_xml_writer_start(w, "D:response");
    axl_xml_writer_start(w, "D:href");
    axl_xml_writer_text(w,  "/dav/preset-stat");   // auto-escaped
    axl_xml_writer_end(w);
    /* ...propstat... */
  axl_xml_writer_end(w);
axl_xml_writer_end(w);
char *xml = axl_xml_writer_steal(w);  // caller owns
axl_xml_writer_free(w);
```

**Scope:**
- Element start / end with tag-balance enforcement (writer
  refuses to emit `</D:foo>` when the open stack top is
  `<D:bar>`).
- Attributes with auto-escaping of `"` and `&`.
- Text content with auto-escaping of `<`, `>`, `&`.
- Backed by `AxlString` for buffer growth.
- Optional pretty-print (newlines + indent — useful for
  PROPFIND responses; off by default for compactness).
- Namespace-prefix handling stays caller-managed: writer
  treats `D:multistatus` as an opaque qname; namespace
  declarations are normal attributes via `axl_xml_writer_attr`.
  No xmlns scope tracking.

### Phase X2: AxlXmlReader — minimal SAX/event reader

```c
typedef int (*AxlXmlEventCb)(void *user, const AxlXmlEvent *ev);

axl_xml_reader_parse(buf, len, on_event, user);
```

Events: `START_ELEMENT(qname, attrs)`, `END_ELEMENT(qname)`,
`TEXT(content, len)`. Caller owns state-machine assembly into
whatever shape they need (DOM, application records).

**Scope:**
- UTF-8 input; reject UTF-16 / EBCDIC declarations.
- Decode the five named entities (`&amp;` `&lt;` `&gt;` `&quot;`
  `&apos;`) and decimal/hex character references (`&#NNN;`,
  `&#xHHHH;`). Reject unknown named entities (no DTD support).
- Skip XML decl, comments, processing instructions, CDATA
  sections (treat CDATA content as TEXT events).
- Reject DOCTYPE — no entity expansion = no billion-laughs
  vector.
- Strict well-formedness: tag balance, single root.
- Stop calling consumer callbacks on first error; return
  `AxlStatus` so caller can distinguish parse error from
  callback abort.

### Phase X3: WebDAV migration

Once X1 ships, replace `emit_entry` + `xml_escape_append` in
`src/net/axl-http-webdav.c` with `AxlXmlWriter` calls. Net LOC
roughly the same, structural correctness by construction.

Once X2 ships, add PROPFIND request-body parsing (defer for
WebDAV v1 today): `<allprop/>` vs `<prop>...</prop>` vs
`<propname/>`. Then PROPPATCH and LOCK request bodies become
implementable.

### Out of scope (intentional)

- DTD validation, schema validation (XSD/RelaxNG), XPath,
  XSLT — none of these have an axl-sdk consumer driving them.
- DOM API on top of the SAX reader. Add iff a consumer wants
  it; meanwhile callers build their own state machines on
  the events.
- XML signatures / encryption (XMLDSig / XMLEnc) — out of
  scope unless an EDK2 capsule manifest consumer asks.

### Tests

Unit:
- Writer: round-trip via reader (build a doc, serialize,
  reparse, compare event sequence).
- Writer: tag-balance refusal (start `<a>`, attempt `end("b")`).
- Writer: auto-escape on `<>&"'` in text and attr values.
- Reader: well-formed input → expected event sequence.
- Reader: malformed input (mismatched tags, unclosed
  element, DOCTYPE, unknown entity) → AXL_ERR.
- Reader: entity decoding (`&amp;`, `&#65;`, `&#x41;`).

Integration:
- WebDAV PROPFIND emit migrated; existing assertions
  unchanged.
- (Once X2 + PROPPATCH lands) cadaver / davfs2 round-trip
  PROPFIND with non-allprop bodies.

---

## API Hygiene — Return Value Conventions (Future)

The public API surface mixes return-value shapes that pre-date a
unified convention. A 2026-05-04 audit of `include/axl/*.h` (~705
non-void public functions) categorized them and the result is on
file in this section. The motivation was a discussion about whether
to introduce typed `Axl*Status` enums for return values; the answer
turned out to be "narrower than that, but also wider than that"
once the data was in.

### Background

- **AxlSidecar precedent (v0.11.0)** — `AxlSidecarStatus` (`OK /
  FILE_MISSING / PARSE_ERROR`) replaced 0/-1/-2 magic in 15 sidecar
  loaders. Set the project pattern: typed status enum **only** when
  3+ outcomes are genuinely distinguishable and consumers branch on
  them.
- **AxlStatus introduction (post-v0.11.2)** — promoted the
  pre-existing `AXL_OK / AXL_ERR / AXL_CANCELLED` `#define` triple
  to a typed enum and added `AXL_TIMEOUT (-3)` to disambiguate
  deadline-elapsed from invalid-arg in the wait/event family.
  Adopted by the wait/event/Tier-4-net cluster (~12 functions).

### Phase H1: named-constants hygiene + targeted predicate flip

**Reframed 2026-05-04** after an aborted "bool sweep" attempt
(commits cbc26e3..5132f76, reverted in 2bd2942). The original
2026-05-04 audit found ~190 `int`-returning functions whose
docstrings only mentioned `0` and `-1` and labeled them
"bool-in-disguise." That framing was wrong — it conflated
"binary outcome" with "predicate."

Per [AXL-Coding-Style.md §"Return Value Conventions"](AXL-Coding-Style.md),
the table draws a sharper line:

| Pattern | Type | Examples |
|---|---|---|
| **Predicates** (yes/no question) | `bool` | `axl_dir_read`, `axl_net_is_available` |
| **Operations** (do something, can fail) | `int` AXL_OK/AXL_ERR | **`axl_file_delete`, `axl_file_get_contents`** |

The flipped-and-reverted batches were operations, not predicates:
file/dir/volume ops, ring-buf push/pop, TLS connect/write/read,
mem-phys read/write/map, driver load/start/unload, string
builders. Those belong in the int row. The diagnostic was every
batch produced boundary-translation sites of the form
`rc = axl_foo(...) ? 0 : -1;` — the call sites telling you the
callee wanted to remain int. Compounding signal: 3 of 8 batches
shipped sign-flip bugs caught only by independent code review
(HTTPS client, mkrd, axl_string_len).

**Reframed plan, two distinct passes:**

#### H1a: Named-constants hygiene — DONE (2026-05-04)

Shipped across 16 module commits in two passes after the bool-
sweep revert. ~280 single-failure-mode operations across 35 public
headers now return `AXL_OK`/`AXL_ERR` named constants. Signatures
unchanged — semantically a no-op since AXL_OK==0 and AXL_ERR==-1
by AxlStatus enum contract — but call sites read as status checks
instead of magic-int comparisons.

**First pass — 9 modules from the original audit (b53ab94..8f96c67):**

| # | Header | Ops | Commit |
|---|---|---|---|
| 1 | axl-fs.h | 9 | b53ab94 |
| 2 | axl-ring-buf.h | 10 + 1 helper | 22187dc |
| 3 | axl-tls.h | 5 (multi-shape kept literal) | fc6a366 |
| 4 | axl-mem-phys.h | 10 | 0938fa8 |
| 5 | axl-stream.h | 5 (count returners kept literal) | beffdfd |
| 6 | axl-sys.h | 9 (DP iterator kept literal) | 291af6a |
| 7 | axl-driver.h | 12 + statics | 13a41cd |
| 8 | axl-string.h | 11 builders (axl_string_len kept literal) | db3c66f |
| 9 | axl-http-server.h | 17 + 3 callback typedefs | 8f96c67 |

**Re-audit pass — 26 additional headers (1b4f0fa..3740a12):**

The post-H1a re-audit revealed 37 untouched headers with int 0/-1
docstrings beyond the original audit set. Cluster commits:

| # | Cluster | Headers | Ops | Commit |
|---|---|---|---|---|
| 10 | smbios | 1 | 19 | 1b4f0fa |
| 11 | pci | 1 | 13 | 35bb8a9 |
| 12 | hardware | acpi+smbus+ipmi+usb+spd | 18 | dc59472 |
| 13 | data-structures | array+cache+queue+radix-tree | 11 | aad7df3 |
| 14 | util-storage | nvstore+config+env+path+boot | 22 | 95d4120 |
| 15 | util-misc | gfx+watchdog+rng+console+str+mem+digest+image+image-verify+diag+log | 30 | b4993b1 |
| 16 | networking | tcp+udp+socket+socket-client+net+http-client+http-core+url | ~50 | 3740a12 |

Two headers explicitly EXCLUDED as non-single-failure shape:
  - `axl-loop.h` — multi-shape (event loop returns 0/1/-1 for
    different states; some funcs propagate callback rcs).
  - `axl-subcommand.h` — pass-through (returns whatever the
    subcommand returned).

Plus axl-tls.h's `axl_tls_handshake` and `axl_tls_read` keep their
0/1/-1 multi-shape docstrings literal.

A regression caught in the re-audit pass: the hardware cluster
(commit dc59472) wrongly converted `axl_usb_get_string`'s `-1`
returns to `AXL_ERR`. That function is a count returner where -1
is the error sentinel for a positive-int-returning function, NOT
a status code. Reverted in the util-misc cluster commit (b4993b1).
The h1a-convert.py tooling was upgraded to use header-driven scope
filtering (extracts ops from `@return AXL_OK` docstrings + contract-
sharing chains via `Like X / as X / @ref X`) so the script no
longer touches count returners or comparison functions.

Tests 2555/2555 both arches at every commit; HTTP integration
62/62 verified at the http-server commit.

Methodology lessons captured for future H1 work:

- **Independent code review caught real issues in 2 of 9 modules**:
  6 missed call sites in axl-fs (regex blind spot for callers
  using `int rc = axl_foo(...)` then `if (rc != 0)`), 1 missed
  rc-indirection in axl-ring-buf, 1 wrapper-chain inconsistency
  in axl-tls (client_send wrapping axl_tls_write). Per-module
  `grep` for both direct-call comparisons AND intermediate-rc
  comparisons is now the standard pre-commit check.
- **Some headers needed `#include <axl/axl-macros.h>` added**:
  axl-mem-phys.h, axl-string.h. The macros include is required
  whenever the public docstrings reference AXL_OK/AXL_ERR so
  consumers comparing against the constants get them defined.
- **Multi-shape functions stayed literal**: axl_tls_handshake,
  axl_tls_read (return 0/1/-1 for "more data needed"),
  axl_dir_walk + dir_walk_recursive (callback-rc pass-through),
  axl_device_path_for_each (multi-shape iterator),
  axl_stream_for_each_line. Recognizing these by their
  contract — anything returning more than success+failure — is
  the discriminator.
- **Count returners stayed literal**: ring_buf push/pop/peek
  (uint32_t bytes), axl_string_len (size_t), axl_pread/pwrite
  (axl_ssize_t), axl_device_path_size (size_t),
  ring_buf get_length/readable/writable/capacity. The `0`
  return means "0 bytes," not "OK."

#### H1b: Targeted predicate flip — NO-OP (2026-05-04)

A post-H1a sweep for int-returning predicates found **zero
candidates needing flip**. Every genuine predicate in the SDK
already uses `bool`:

- `axl_*_is_*` / `axl_*_has_*` / `axl_*_contains` — none exist
  as `int`-returners.
- Existence/availability/state predicates already bool:
  `axl_acpi_checksum_ok`, `axl_gfx_available`,
  `axl_loop_is_running`, `axl_net_is_available`,
  `axl_queue_is_empty`, `axl_ring_buf_is_empty`,
  `axl_ring_buf_is_full`, `axl_task_pool_done`,
  `axl_tls_available`.
- Iteration predicates already bool: `axl_dir_read`,
  `axl_log_read`, `axl_json_array_iter_next`,
  `axl_stream_read_line`, etc.

Two int-returning functions have docstrings with `1 if X, 0 if Y`
shape (axl_hash_table_insert / axl_hash_table_replace), but they
return THREE distinct values (1 = new entry, 0 = existing
replaced, -1 = error). That's multi-shape, not a yes/no predicate
— would be a Phase H3 candidate for a per-module typed enum if
3+ branchable outcomes earn their keep.

H1b is closed. The convention rule (predicates → bool) was
already in force; the bool sweep had it backward.

#### What we explicitly walked away from

- **No bulk `int → bool` sweep.** Tried it; reverted it.
  Operations stay `int` AXL_OK/AXL_ERR per the style guide.
- **No new `Axl*Status` enums** unless 3+ outcomes earn their
  keep (Phase H3 rule, unchanged).

### Phase H2: AxlStatus expansion — opportunistic

The async TCP cluster documents `AXL_CANCELLED` in its callback
signature (`void (*AxlTcpCb)(AxlTcp *, int status, void *)` —
`int status` should be `AxlStatus status`). Migration was deferred
from the initial AxlStatus commit because the callback type change
ripples through every consumer's TCP callback. Worth doing the next
time a TCP-touching change is in flight anyway.

- [ ] `AxlTcpCb` `int status` → `AxlStatus status` (and the
      analogous HTTP-client callback). Updates axl-webfs callsites.
- [ ] Other multi-outcome candidates surfaced by future audits.

### Phase H3: Per-module status enums — case-by-case

Don't create them prophylactically. Future test before adding a new
`Axl*Status`: "do consumers need to write three or more distinct
branches based on the rc?" If yes, typed enum (sidecar/wait
precedent). If no, `bool` (or `int`/`AxlStatus` if it fits the
existing conventions).

### What we're explicitly NOT doing

- **Not promoting `axl_args_run` to AxlStatus.** It's POSIX-exit-
  code shaped (returns from `main()` straight into the process exit
  code, where AxlStatus's negative values would round to 254/255).
  Documented inline in `axl-args.h`.
- **Not changing comparison-style functions.** `axl_strcmp`,
  `axl_memcmp`, etc. return libc-style sign — that's information,
  not a status code.
- **Not changing count-returning functions.** `int axl_args_get_pos_count`,
  `axl_snprintf`, `axl_args_get_multi_count`, etc. return values,
  not statuses.
- **Not adding numeric-value mapping macros.** `AXL_CANCELLED` is
  `(-2)` and stays `(-2)`; the enum members ARE the contract, the
  numeric values ARE the contract, both work, no glue needed.

---

## Repo Merge (Complete)

- [x] Rename axl-sdk -> axl-sdk-old on GitHub
- [x] Rename libaxl -> axl-sdk on GitHub
- [x] Copy SDK files into merged repo
- [x] Rework install.sh for local library
- [x] Update docs and consumer projects

---

## Known Gaps and Issues

Items that are not part of any phase but should be tracked. Discovered
during code review and refactor work, not during original planning.

### Testing / tooling gaps

- [x] **OOM injection testing.** `axl_mem_fail_next_alloc(N)` in
      `include/axl/axl-mem.h` (implemented in `src/mem/axl-mem.c`)
      arms the Nth next allocation to return NULL without touching
      the backend. 13 allocator-primitive tests in
      `test/unit/axl-test-mem.c` and 13 container tests in
      `test/unit/axl-test-data.c` exercise the silent-OOM paths
      that were otherwise unreachable. Hook is gated on
      `AXL_MEM_DEBUG`; no-op in release.
- [x] **Static analysis in CI.** `clang-tidy` runs as a third job
      in `.github/workflows/ci.yml`. The policy lives in
      `.clang-tidy` at the repo root (`bugprone-*` +
      `clang-analyzer-*` minus five documented noisy checks,
      `WarningsAsErrors: '*'`). 11 pre-existing findings fixed at
      the same time, including two real null-derefs in
      `src/data/axl-list.c` caught by
      `clang-analyzer-core.NullDereference`.
- [~] **Fuzz harness — scaffold landed, more targets pending.**
      `test/fuzz/` now holds a standalone host-side libFuzzer build
      (`clang -fsanitize=fuzzer,address`) with two harnesses wired
      up: `url_fuzz` for `axl_url_parse` and `json_fuzz` for
      `axl_json_parse`. Both reuse a shared `fuzz_shim.c` that
      provides libc-backed implementations of the AXL mem/str/log
      primitives so parser .c files can be compiled directly against
      the host libc without pulling in the freestanding allocator.
      Not wired into the default `make` target (fuzzing is opt-in)
      and not wired into CI — a nightly job with crash artifact
      upload is the likely shape and is a separate follow-up.
      Remaining parser targets to cover: `axl_http_parse_request_line`
      / `_header_line`, `axl-digest-*` (block feeding), WebSocket
      frame parser.
- [ ] **Benchmark suite.** No benchmarks for the library. The hash
      table, radix tree, ring buffer, format engine, and JSON parser
      are the obvious candidates.
- [x] **`axl_http_parse_request_line` already public; kernel servers
      deduped.** The parser was already exposed via
      `<axl/axl-http-core.h>` (umbrella'd through `<axl.h>`); the
      ROADMAP entry was wrong about needing to promote it. Real
      win: added `axlk_http_read_request_line` in
      `experiments/axl-kernel/include/axl-kernel.h` that wraps the
      read loop + the public parse, and migrated all three kernel
      POC servers (hwinfo, bootconfig, reqlog) to use it. ~70 LOC
      of duplicated byte-fiddling removed.
- [x] **AxlJsonWriter — JSON output API.** Landed. Renamed
      `AxlJsonCtx`/`AxlJsonBuilder` to `AxlJsonReader`/`AxlJsonWriter`
      for symmetry. Writer now AxlString-backed, with orthogonal
      container/key/atom calls, optional `AXL_JSON_WRITER_PRETTY`
      flag, sticky error flag (covers OOM + structural misuse),
      and `axl_json_write_token` bridge for parse → mutate → emit.
      Migrated tests, fuzz, tools/rfbrowse, sdk/examples. Three
      kernel POC servers now build their endpoint JSON via the
      writer, not snprintf chunks.
- [x] **Kernel-server endpoint builder cleanups.** Done as part of
      the AxlJsonWriter migration. axlk-reqlog-server's manual
      NUL-terminated copy loops swapped to `axl_strlcpy`; all three
      servers' endpoint builders now use the writer.
- [x] **AxlRingBuf push/lost stats counters.** Landed. Added
      `pushes_total` + `pushes_lost` as cumulative byte counters on
      every push path (push, push_msg, push_elem, push_advance —
      including reject-mode rejection and overwrite-mode
      input-drop / old-data-displacement). Accessors are
      `axl_ring_buf_pushes_total/_lost`; struct fields are private
      and reset on `axl_ring_buf_clear`/init. axlk-reqlog-server
      now uses AxlRingBuf for its 8-element log ring (replacing
      the hand-rolled struct + head index + counters), and computes
      received/dropped by dividing the byte counters by element size.
      19 new unit tests cover reject-rejected, overwrite-displaced,
      oversized-overwrite-input-drop, element-mode counts, and
      clear() reset.

### Correctness / performance gaps

- [ ] **AxlLoop event-driven driver mode (eliminate `driver_tick_ms`).**
      Driver-mode dispatch is currently polled: `axl_loop_attach_driver`
      installs a periodic `EVT_TIMER | EVT_NOTIFY_SIGNAL` at
      `TPL_CALLBACK` whose notify drains all sources every
      `tick_ms` (default 50 ms). Every source whose readiness IS a
      UEFI event (TCP4 receive completion, EVT_TIMER, protocol-installed
      notify) currently re-enters via the loop's polled dispatch
      cycle, which means up to ~`tick_ms / 2` average added latency
      and a TPL_CALLBACK budget that has to be hand-managed (per
      `axl-loop.h` "notify-budget rule"). A genuinely event-driven
      driver mode would attach each source's underlying UEFI event
      to its OWN `EVT_NOTIFY_SIGNAL` callback that runs that source's
      dispatch directly. Net effect:
        - `axl_loop_attach_driver` / `_detach_driver` go away
        - `AxlService.driver_tick_ms` field deleted
        - `AXL_SERVICE_DEFAULT_TICK_MS` deleted
        - average dispatch latency drops from ~25 ms to firmware-
          callback latency (~µs)
        - "AxlLoop is event-driven" stops being a half-truth in
          driver mode
      Trade-off: real surgery on the source abstraction. Every
      source type (TCP, timer, idle, defer, pubsub, raw event) needs
      its dispatch path rewired to fire from its own firmware notify
      event. Idle and defer don't have a UEFI-event analogue and
      need a different home (idle = "run after current notify
      drains," defer = its own EVT_TIMER). Foreground mode still
      wants the centralized loop cycle so you end up with two
      source-dispatch paths instead of one. The integration tests
      are currently load-bearing on tick semantics in places (the
      drain-cap fix from the b0e567b HTTP-server starvation bug).
      Not blocking anything today — axl-webfs serve works fine at
      50 ms — so this is post-1.0 work. Revisit when latency or
      conceptual cleanup actually starts costing something.

- [x] **Sync ops busy-poll instead of blocking on events — new
      AxlCompletion module.** Landed April 2026. AxlCompletion +
      AxlWait helpers (`axl_wait_for_flag/word/ms`, `axl_wait_for`,
      `axl_wait_for_with_tick`) on a shared internal primitive
      `_axl_event_wait_timeout_with_tick` that delegates to AxlLoop.
      Per-protocol Tier 4 wrappers (`_axl_{udp,tcp,dns,ip4}_wait`)
      absorb the `EFI_*_COMPLETION_TOKEN` + Poll plumbing once. Ten
      src/net sites and three SSIF sites ported. Two KCS sites left
      as spin with explanatory comments (100 us cadence is below
      firmware timer resolution). Async TCP no-mapping retry got a
      partial fix (CPU-idle sleep; async-start blocking is tracked
      below as follow-up). Measured test-axl.sh CPU dropped from
      ~70% avg to 22% avg; wall-clock unchanged because the
      remaining time is legitimate protocol timeouts. AARCH64
      QEMU-TCG flake rate went from 30% baseline (3/10) to 0/5 with
      deterministic 61.0s ±0.05s wall-clock — the stable timing is
      itself a signature of idle-CPU waits (old busy-polls created
      guest/host scheduler contention noise). AxlTestCompletion
      runs via an auxiliary runner (`test/integration/test-axl-completion.sh`)
      because AxlTestNet has a pre-existing FAT-image-timing UAF in
      its UDP-async teardown path — tracked as a follow-up below.

      **Follow-ups from this rework (tracked separately):**

      - [x] **AxlTestNet UDP-async teardown UAF.** Fixed
            2026-04-18. The test was calling `axl_loop_free(loop)`
            before `axl_socket_free(receiver)`. The socket's UDP
            async receive state still held `sock->loop` (now
            dangling) and a stale source id; the subsequent
            `axl_udp_recv_stop` → `axl_loop_remove_source`
            dereferenced freed loop memory (filled with
            `0xAF` poison by AXL_MEM_DEBUG), which then propagated
            into `UDP4->Cancel` via a corrupted token event pointer
            and tripped a #GP in DxeCore. Fix: swapped the free
            order in `test/unit/axl-test-net.c`'s UDP async recv
            test so the socket is freed before the loop it was
            registered against. AxlTestCompletion folded back into
            `test/integration/test-axl.sh`'s TEST_APPS; auxiliary
            runner deleted. Full suite: 1277/1277 on both X64 and
            AARCH64.

      - [x] **Sync TCP wrappers orphan their async socket on timeout.**
            Fixed via `AxlCancellable`, landed 2026-04-18.
            `axl_tcp_{connect,accept,send,recv}_async` grew an optional
            `AxlCancellable *` parameter; sync wrappers now allocate an
            ephemeral cancellable, wire it to the 10 s timeout, and let
            the async op's cancel path handle uniform teardown (cancel
            UEFI token, drop loop sources, close events, fire user cb
            with `AXL_CANCELLED`). All four loop_free external-source
            warnings cleared. `AxlCompletion` + `axl_wait_*` also
            accept a cancellable for symmetry. Original description
            retained below.
            Surfaced by the `axl_loop_free` diagnostic added in
            `3216c86`. `axl_tcp_connect` (and by extension the other
            sync wrappers: accept/send/recv) creates an ephemeral
            loop, calls `axl_tcp_connect_async` (which allocates an
            `AxlTcp` and registers a `connect_source` on the loop),
            and runs the loop with a 10-second timeout. On timeout,
            the sync wrapper has no handle to the in-flight AxlTcp
            (connect_async only exposes it to the user callback, which
            never fires on timeout) and no public cancel API, so the
            AxlTcp + its loop source + the UEFI event leak. The
            ephemeral loop is freed with the source still active —
            would have been a UAF if anyone else held the source id.
            Run `TEST_KEEP_LOG=/tmp/out.log ./test/integration/test-axl.sh`
            and grep for "axl_loop_free: caller-owned event source" to
            see 4 hits per run (TCP connect to ports 9999, 9998, 9996,
            9994 all fail under SLIRP, exercising this path).
            Fix: either pass an out-pointer back through `connect_async`
            so the sync wrapper can close on timeout, or add a public
            `axl_tcp_connect_cancel` function. Same pattern likely
            applies to the other three sync wrappers. Discovered
            2026-04-18.

      - [ ] **Async TCP `Configure` retry blocks its caller
            (API-contract issue; no observed impact).**
            [src/net/axl-tcp-async.c:540](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-tcp-async.c#L540).
            The no-mapping retry loop inside an async-start function
            still blocks the caller for up to
            `TCP_MAPPING_RETRIES * TCP_MAPPING_DELAY` (~10 s) while
            DHCP is pending. Commit `7c98082` swapped
            `axl_backend_stall` for `axl_wait_ms` so the CPU idles
            and Ctrl-C still works — the user-visible symptoms are
            gone. What remains is a pure API-contract violation:
            async-start shouldn't block. In practice no caller
            observes this: the only one that reaches Configure
            during the DHCP window is the sync wrapper, which
            wants to block. **Not fixing preemptively.** Revisit
            when a real caller runs `axl_tcp_connect_async` on a
            shared loop and observes other sources going silent
            during the Configure-retry window (candidates: axl-webfs
            long-running server mode, SoftBMC-on-AXL). Proper fix
            shape: on `EFI_NO_MAPPING` store pending config in
            `AxlTcp`, register `axl_loop_add_timeout` for
            re-Configure, return 0 from the async-start call, and
            report final success/failure through the user callback.
            Discovered 2026-04-18; documented and deferred
            2026-04-19.

      - [ ] **AxlAsync dogfooding of AxlEvent.**
            [src/task/axl-async.c](https://github.com/aximcode/axl-sdk-releases/blob/main/src/task/axl-async.c) currently
            hand-rolls its completion reporting via idle-source
            polling and the defer queue. Natural consumer of the
            `AxlEvent` primitive — signal-from-worker, wait-
            from-caller is the exact pattern. Additive (no API
            change), not a refactor. Low priority; raise only if
            someone touches AxlAsync for another reason or if a
            consumer needs a wait-for-async-work primitive.
            Discovered 2026-04-18.

      - [ ] **`axl_yield()` instrumentation of AXL APIs.**
            [docs/AXL-Lifecycle.md §3.1](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#31-where-axl-apis-inject-yields-automatically) lists the
            targets: file I/O (`axl_file_get_contents` /
            `axl_fread` / directory iteration), HTTP body-read
            loops in `src/net/axl-http-client.c`, `axl_digest_update`
            on large buffers, IPMI KCS 100 µs busy polls, SMBIOS
            table walks, `axl_array_sort`, hash-table rehash. Grep
            shows zero call sites in `src/` as of Phase A7 landing.
            Consequence: Ctrl-C works through any code path that
            goes through `AxlLoop` (HTTP server, sync TCP wrappers,
            `axl_wait_*`) but is silently ignored by CPU-bound or
            retry-loop paths that don't. Scope: seed `axl_yield()`
            at outer-loop boundaries in ~10–15 sites, tested by
            running a CPU-heavy call under QEMU and verifying
            Ctrl-C terminates it. Discovered 2026-04-20.

      - [ ] **Minimal runtime opt-out via `axl-cc --minimal-runtime`.**
            CRT0 unconditionally installs the registry, atexit list,
            signal notify, and default loop during `_axl_init`. [§9
            of `AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#9-design-decisions-locked-in) locked in "registry is always on"
            with the rationale that drivers don't link CRT0 anyway;
            that's true but leaves size-constrained or exit-managed
            apps with no way out. Ship `axl-crt0-minimal.o` as a
            peer to `axl-crt0-native.o`: sets firmware globals,
            inits console for `axl_printf`, parses argv, calls
            `main`, returns. No registry, no atexit, no signal
            notify, no default loop. The registry and atexit APIs
            already no-op safely when their storage is NULL, so
            `libaxl.a` stays unchanged. Consumers pick via
            `axl-cc --minimal-runtime`. Discovered 2026-04-20.

      - [ ] **Idle source fires on non-blocking dispatch —
            revisit when a real caller bites.**
            [src/loop/axl-loop.c:241-254](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/axl-loop.c#L241-L254).
            Idle callbacks run once per `axl_loop_next_event` pass
            regardless of `blocking`. Under `axl_loop_run` that's
            naturally throttled by `WaitForEvent`; under an
            `axl_yield`-driven tight CPU loop it fires every yield
            — potentially millions of times per second. Matches
            GLib / libuv / Node convention, and two in-tree
            consumers depend on it: `src/task/axl-async.c`'s AP
            completion poll registers an idle source that expects
            to fire every tick, and `test/unit/axl-test-runtime.c`
            has `test_yield_dispatches_ready_work` explicitly
            asserting the current semantics. Kept as-is; documented
            as a footgun in [`AXL-Lifecycle.md` §2.6](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#26-idle-callbacks-and-yield-driven-loops) (recommend
            `axl_loop_add_timer` or `axl_defer` for tight-yield
            apps). Revisit if a real caller hits unresponsive
            idles or unwanted saturation — at which point the
            right fix is probably to skip idle when `blocking ==
            false` and update the test + AxlAsync together.
            Discovered 2026-04-20.

      Original description (retained for context):

      Multiple call sites across `src/net/`
      and `src/ipmi/` wait for UEFI operations by repeatedly calling
      `protocol->Poll()` separated by `axl_backend_stall(1000)`. As
      a spec-mandated busy-wait, `Stall` burns host CPU for the
      entire timeout window. Measured impact: AxlTestNet pins
      99-100% QEMU CPU for 28 consecutive seconds during DNS / UDP /
      HTTP-setup tests whose intervals match UEFI network timeouts;
      the AARCH64 QEMU-TCG integration run flakes ~30% of the time
      because the busy-waits race other timers under slower
      emulation.

      **Full `_stall` site audit (April 2026).** 15 calls in the
      tree: 4 legitimate busy-wait by design, 11 misuse candidates.
      Priority by measured impact.

      _src/net (high — this is the 99% CPU burn AxlTestNet shows):_
      - [src/net/axl-udp.c:244](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-udp.c#L244) — `axl_udp_send_to` timeout loop (10s default)
      - [src/net/axl-udp.c:319](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-udp.c#L319) — `axl_udp_receive` timeout loop
      - [src/net/axl-net-resolve.c:184](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-net-resolve.c#L184) — DNS 5s primary poll
      - [src/net/axl-net-resolve.c:202](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-net-resolve.c#L202) — DNS secondary poll
      - [src/net/axl-net-dhcp.c:198](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-net-dhcp.c#L198) — DHCP first-stage wait (100ms stalls)
      - [src/net/axl-net-dhcp.c:250](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-net-dhcp.c#L250) — DHCP IP-assignment wait (1s stalls)
      - ~~src/net/axl-tcp-sync.c — TCP close drain~~ FIXED 2026-04-28 in
        b46686d: close finalizes via the loop when the firmware
        signals SockConnClosed (no per-close busy-wait); the only
        remaining `_axl_tcp_wait` site is the sync fallback used when
        no event loop is running (shutdown / sync-only CLI use).
      - [src/net/axl-tcp-sync.c:209](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-tcp-sync.c#L209) — Configure mapping retry (TCP_MAPPING_DELAY)
      - [src/net/axl-tcp-async.c:540](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-tcp-async.c#L540) — same Configure retry on the async path (extra-bad: async code should never block the loop)
      - [src/net/axl-net-ping.c:260](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/axl-net-ping.c#L260) — ping response wait (1ms stalls, full timeout)

      _src/ipmi (medium — SSIF's 60ms inter-command delay is the largest):_
      - [src/ipmi/axl-ipmi-ssif.c:109](https://github.com/aximcode/axl-sdk-releases/blob/main/src/ipmi/axl-ipmi-ssif.c#L109) — SSIF write-retry delay (60ms)
      - [src/ipmi/axl-ipmi-ssif.c:124](https://github.com/aximcode/axl-sdk-releases/blob/main/src/ipmi/axl-ipmi-ssif.c#L124) — SSIF read-retry exponential backoff (starts 60ms)
      - [src/ipmi/axl-ipmi-ssif.c:304](https://github.com/aximcode/axl-sdk-releases/blob/main/src/ipmi/axl-ipmi-ssif.c#L304) — SSIF 60ms inter-command delay (spec-mandated for iDRAC/Grace)
      - [src/ipmi/axl-ipmi-kcs.c:93](https://github.com/aximcode/axl-sdk-releases/blob/main/src/ipmi/axl-ipmi-kcs.c#L93) — KCS IBF-clear poll (100µs cadence, 5s timeout)
      - [src/ipmi/axl-ipmi-kcs.c:125](https://github.com/aximcode/axl-sdk-releases/blob/main/src/ipmi/axl-ipmi-kcs.c#L125) — KCS OBF-set poll (100µs cadence)

      KCS's 100µs interval sits at the edge of `gBS` timer
      granularity (firmware timers typically snap to 100µs–1ms).
      Evaluate per-platform before converting; leaving the two KCS
      sites as spin is defensible on latency grounds.

      _Shim / tests (low — negligible wall-time contribution):_
      - [sdk/examples/smbus-hc-shim.c:267](https://github.com/aximcode/axl-sdk-releases/blob/main/sdk/examples/smbus-hc-shim.c#L267) — SMBus wait-ready poll (1ms × 1s)
      - [sdk/examples/smbus-hc-shim.c:287](https://github.com/aximcode/axl-sdk-releases/blob/main/sdk/examples/smbus-hc-shim.c#L287) — SMBus run-and-wait poll (1ms × 1s)
      - [test/unit/axl-test-net.c:247](https://github.com/aximcode/axl-sdk-releases/blob/main/test/unit/axl-test-net.c#L247) — `axl_spin_usleep(10000)` in socket accept test
      - [test/unit/axl-test-net.c:1502](https://github.com/aximcode/axl-sdk-releases/blob/main/test/unit/axl-test-net.c#L1502) — same pattern in socket stream test

      _Legitimate busy-wait (leave alone):_
      - [src/backend/native/axl-backend-native.c:1162](https://github.com/aximcode/axl-sdk-releases/blob/main/src/backend/native/axl-backend-native.c#L1162) — the `gBS->Stall` wrapper itself
      - [src/util/axl-time.c:47](https://github.com/aximcode/axl-sdk-releases/blob/main/src/util/axl-time.c#L47), [:56](https://github.com/aximcode/axl-sdk-releases/blob/main/src/util/axl-time.c#L56) — `timer_sleep_us` fallback when timer creation fails
      - [src/util/axl-time.c:140-152](https://github.com/aximcode/axl-sdk-releases/blob/main/src/util/axl-time.c#L140-L152) — `axl_spin_{sleep,msleep,usleep}` public busy-wait API
      - [src/util/axl-sys.c:79](https://github.com/aximcode/axl-sdk-releases/blob/main/src/util/axl-sys.c#L79) — `axl_stall()` public busy-wait wrapper

      **Fix direction — layered API built on top of AxlLoop.**

      AxlLoop already multiplexes arbitrary EFI_EVENTs through
      `gBS->WaitForEvent` with the shell-break event appended to
      every wait, so Ctrl-C detection is built-in. Its FUSE-style
      `axl_loop_next_event` / `axl_loop_dispatch_event` primitives
      let a caller drive one iteration at a time without running the
      loop to completion. The sync-wait primitives below should be
      thin wrappers around AxlLoop, NOT a parallel implementation of
      event multiplexing + break handling. See `include/axl/axl-loop.h`
      and `axl_backend_shell_break_event` / `axl_backend_shell_break_flag`
      in `src/backend/axl-backend.h` for the primitives to reuse.

      **Tier 1 — AxlCompletion (public, zero callbacks, signal/wait):**

      ```c
      /* include/axl/axl-completion.h */
      typedef struct AxlCompletion AxlCompletion;

      AxlCompletion *axl_completion_new(void);
      void           axl_completion_free(AxlCompletion *c);
      AXL_DEFINE_AUTOPTR_CLEANUP(AxlCompletion, axl_completion_free)

      void  axl_completion_signal(AxlCompletion *c);       /* idempotent */
      void  axl_completion_reset(AxlCompletion *c);         /* reusable */
      int   axl_completion_wait(AxlCompletion *c);          /* infinite, 0/-2 */
      int   axl_completion_wait_timeout(AxlCompletion *c,
                                        uint64_t timeout_us);
      ```

      Parallels Linux kernel `struct completion`. Internally: wraps
      an EFI_EVENT (signal = `SignalEvent`) and implements `wait*` by
      creating an ephemeral AxlLoop, adding the event as a source,
      running until fired. Ctrl-C handling comes for free from the
      loop. Valuable beyond this refactor for AxlDefer completion,
      AxlAsync result reporting, any cross-BSP/AP signaling.

      **Tier 2 — zero-callback convenience (most call sites):**

      ```c
      /* Wait until *flag becomes true. */
      int axl_wait_for_flag(volatile const bool *flag,
                            uint64_t timeout_us);

      /* Wait until *word stops matching not_ready_value.
         Covers the UEFI-token Status pattern, DMA flags, etc. */
      int axl_wait_for_word(volatile const uint64_t *word,
                            uint64_t not_ready_value,
                            uint64_t timeout_us);

      /* Interruptible sleep — what today's axl_msleep should have been. */
      int axl_wait_ms(uint64_t ms);
      ```

      Zero callbacks, zero allocations at the callsite, no session
      object. Covers pure sleeps and simple flag/word waits.

      **Tier 3 — callback form for genuinely complex conditions:**

      ```c
      typedef bool (*AxlCondFn)(void *ctx);
      typedef void (*AxlTickFn)(void *ctx);

      int axl_wait_for(AxlCondFn cond_fn, void *cond_ctx,
                       uint64_t timeout_us);

      int axl_wait_for_with_tick(
              AxlCondFn cond_fn, void *cond_ctx,
              AxlTickFn tick_fn, void *tick_ctx,
              uint64_t  tick_us,
              uint64_t  timeout_us);
      ```

      The `_with_tick` form is the actual vehicle for the sync-net
      refactor — `tick_fn` drives the UEFI protocol state machine
      forward between waits. All return values follow the same
      convention (0 = condition, -1 = timeout, -2 = interrupted).

      **Tier 4 — internal per-protocol wrappers (one-liner callsites):**

      Each sync-net module gets a single helper that absorbs the
      tick + cond callbacks once. These live in internal headers
      (UEFI types allowed), so the 11 callsites become a single
      function call with no user-written predicates:

      ```c
      /* src/net/axl-net-internal.h */
      int _axl_tcp_token_wait(EFI_TCP4_COMPLETION_TOKEN *t,
                              EFI_TCP4_PROTOCOL *tcp4,
                              uint64_t timeout_us);
      int _axl_udp_token_wait(EFI_UDP4_COMPLETION_TOKEN *t,
                              EFI_UDP4_PROTOCOL *udp4,
                              uint64_t timeout_us);
      int _axl_dns_token_wait(EFI_DNS4_COMPLETION_TOKEN *t,
                              EFI_DNS4_PROTOCOL *dns4,
                              uint64_t timeout_us);
      ```

      Each implementation is ~5 lines, built on Tier 3 with the
      protocol-specific Poll as the tick callback. Every `src/net/`
      sync callsite collapses from 5 lines to 1:

      ```c
      /* Before — 5 lines, 100% CPU: */
      while (elapsed < UDP_SEND_TIMEOUT_US) {
          axl_efi_call(udp4->Poll, 1, udp4);
          if (tx_token.Status != EFI_NOT_READY) break;
          axl_backend_stall(1000);
          elapsed += 1000;
      }

      /* After — 1 line, idle CPU, zero callbacks: */
      _axl_udp_token_wait(&tx_token, udp4, UDP_SEND_TIMEOUT_US);
      ```

      **Why not just `axl_usleep`:** a minimum
      `axl_backend_stall → axl_usleep` swap drops CPU to ~0% during
      the wait, but each iteration creates + destroys a one-shot
      timer, there's up-to-1ms latency between completion and wake,
      and Ctrl-C still can't interrupt the loop.

      **Open question for the implementation session:
      how should these layer on AxlLoop?** Two candidate shapes,
      discuss at session start before writing code:

      1. **Each wait creates an ephemeral AxlLoop.** Simple, no
         caller state. Costs a few allocations per call. The
         ephemeral loop inherits shell-break handling automatically.
         Clean separation: sync callers never touch a loop object.

      2. **Add `axl_loop_wait_condition(loop, cond, ctx, tick_us,
         timeout_us)` as a new AxlLoop method.** Tier 3 becomes a
         one-liner that creates a throwaway loop and forwards.
         Callers who already own a loop can skip the allocation by
         calling the method directly. More composable but bigger
         API surface.

      Option 2 feels like the right direction (more composable,
      AxlLoop users reuse their loop, also better integrates with
      AxlDefer/AxlPubsub which live inside a loop), but confirm by
      sketching both and looking at what AxlDefer/AxlPubsub do with
      their own loop handles. The `axl_loop_next_event` /
      `axl_loop_dispatch_event` FUSE primitives already allow
      driving a loop iteration-by-iteration — that's what
      `wait_condition` would be built on. Zero new multiplexing code.

      **Work plan for the dedicated session:**
      1. Discuss and pick ephemeral-loop vs loop-method shape.
         Sketch each with one callsite ported both ways, compare
         the internal-header shape.
      2. Implement chosen shape in `src/util/axl-completion.c` +
         `src/util/axl-waiter.c` (Tier 2/3), reusing AxlLoop's event
         multiplexing via `axl_loop_next_event` /
         `axl_loop_add_event` / `axl_loop_add_timer`. Never
         re-implement `gBS->WaitForEvent` or break-flag handling.
      3. Unit tests in `test/unit/axl-test-completion.c`: signal
         before wait, signal after wait, timeout path, reset +
         reuse, wait_for_flag, wait_for_word, wait_for_with_tick on
         a mock state machine, NULL-safety on all entry points,
         Ctrl-C interruption (simulate via `axl_backend_shell_break_flag`
         injection).
      4. Write per-protocol helpers in `src/net/axl-net-internal.h`
         + `src/ipmi/axl-ipmi-internal.h`. One each for TCP4, UDP4,
         DNS4, DHCP4, IP4 (ping), SSIF.
      5. Port the 10 `src/net/` sites. Verify the async TCP path
         (`axl-tcp-async.c:540`) threads through AxlLoop rather
         than blocking — it currently busy-waits regardless of
         context.
      6. Port the 3 SSIF sites in `src/ipmi/axl-ipmi-ssif.c`. Leave
         the 2 KCS sites with an explanatory comment (100µs cadence
         below firmware timer resolution).
      7. Convert the shim (2 sites) and tests (2 sites) — minor.
      8. Update `docs/AXL-Design.md` §Async Work Phases with
         AxlCompletion + wait helpers alongside AxlBufPool /
         AxlAsync / AxlDefer / AxlPubsub; new `src/util/README.md`
         section; Sphinx page.
      9. Re-measure: AxlTestNet guest CPU ~72% → sub-20% avg;
         AARCH64 flake rate 3/10 → expected 0/10; Net binary wall
         time may also drop (state machine advances per event
         instead of per 1ms tick).
      10. Ratchet bump per new test count.

      **Out of scope for the rework:** the AxlTestNet DNS tests that
      time out (no loopback DNS under `-netdev user` in typical
      config) — those SHOULD time out, and the framework skips them
      with `SKIP: …`. The rework just stops the timeouts from
      burning CPU.

      Discovered 2026-04-18 while investigating AARCH64 test flakes
      during the AxlSmbus Phase B1a work. Phases B1a and B1b have
      since landed; this AxlCompletion rework is next in the queue.

### API / packaging gaps

- [x] **`AXL_VERSION` macro** in `include/axl/axl-version.h` with
      `AXL_VERSION_MAJOR/MINOR/PATCH/STRING/NUMBER` and
      `AXL_VERSION_AT_LEAST(M, m, p)`. Kept in sync with the
      repo-root `VERSION` file via the Makefile's `check-version`
      target (hard-errors on drift) and `scripts/bump-version.sh`.
      Shipped with v0.1.1.
- [x] **pkg-config file** (`axl.pc`, plus per-arch `axl-x64.pc` and
      `axl-aa64.pc`) generated by `install.sh` at
      `<prefix>/lib/pkgconfig/`. Relocatable via pkg-config's
      `${pcfiledir}`. Consumers can `pkg-config --cflags --libs axl`
      once the SDK is installed.
- [x] **CMake package config** for consumers using CMake.
      `install.sh` generates `lib/cmake/axl/axl-config.cmake` with a
      relocatable `AXL_SDK_DIR` lookup, so consumers can
      `find_package(axl REQUIRED)` and call `axl_add_app()`. Still
      a raw macro (no imported targets), which is fine for the
      non-library image `.efi` output shape — add imported targets
      only if someone actually wants them.
- [ ] **Proper `-devel` split for distro upstream submission.**
      Today we ship a single `axl-sdk` package with
      `Provides: axl-sdk-devel` so `dnf install axl-sdk-devel`
      resolves via the RPM alias. That's sufficient for
      self-distribution via GitHub Releases, but Fedora/Debian
      upstream reviewers will ask for a real split before acceptance.
      Mirror gnu-efi's shape:
      - `axl-sdk`: `/usr/lib/axl/<arch>/*.o` (CRT0, reloc, debug) +
        `/usr/lib/axl/elf_*_efi.lds` (linker scripts) — the firmware
        glue.
      - `axl-sdk-devel` (Requires: axl-sdk): headers, `libaxl.a`,
        `axl-cc`, `pe-set-debug`, pkg-config, cmake, docs, examples
        — everything a developer directly touches.
      ~30 lines in `release.yml` + `scripts/build-packages.sh` to
      split one `fpm` invocation into two per matrix entry with
      correct `--exclude` patterns. Defer until actually submitting
      upstream — not worth the complexity while self-hosting.
- [ ] **`debian/` directory + `.spec` file for distro submission.**
      Required for official Debian/Fedora upstream packages. Separate
      from CI `.deb`/`.rpm` generation — those will happily keep
      using `fpm`, but distros want native spec + rules files they
      can review and patch. A few days' polishing to pass
      `lintian`/`rpmlint` clean. Ties directly to the `-devel` split
      above.
- [ ] **Migrate library build to meson+ninja or cmake+ninja.**
      Parked April 2026 as "wait for a concrete pain point"; the
      first one landed during the v0.7.3 AxlArgs unification:
      restructuring a public-header struct (`AxlArgsApp`/`AxlVerb` →
      `AxlArgsNode`) produced an inconsistent incremental build
      where `libaxl.a` had members rebuilt against the new header
      but the test binaries linked a `.a` snapshot that the runtime
      hit as stale state — `axl_mem_fail_next_alloc` test counter
      drifted, AxlMem stalled. `make clean && make` cleared it; the
      fix went into project CLAUDE.md as a manual workaround.
      Root cause is `ar rcs` not tracking per-member timestamps
      cleanly + GNU make's `-MD` deps not catching cross-archive
      dependencies. Both meson and cmake (with ninja generator)
      handle this naturally via their internal dep DBs.
      **Preference**: meson — it's lighter, the toolchain-file
      story for freestanding UEFI cross-compile is cleaner than
      CMake's, and it generates compile_commands.json for free.
      Open question: do we keep the `.so → .efi objcopy` step in
      the build system or move it to a shared script that both
      meson and the consumer-side `axl-cc` invoke. Real work
      (multi-day); revisit only when this bites again or a
      consumer asks. Triggering condition: another stale-build
      incident, OR landing IDE integration that needs
      `compile_commands.json` (clangd works without it but the
      experience is degraded).

### Documentation gaps

- [ ] **Design doc for the coding style** already exists, but there
      is no "how to add a new module" walkthrough. Useful when
      onboarding contributors or adding modules like `axl-ipmi`.
- [x] **Changelog.** `CHANGELOG.md` at the repo root, maintained
      per-release. Entries moved under a version heading at tag time
      (see the v0.1.2 entry for an example). Release notes in
      `release.yml` link to it rather than duplicating content.

---

## Real-hardware findings — Dell PowerEdge XE7745 (iDRAC10), May 2026

Empirical session against actual hardware (svc tag KCH0TD1) shook out a
number of discoveries and followups that would never have surfaced in
QEMU. Tracked as a single section so the cross-cutting fixes don't get
lost in per-phase backlogs.

### Confirmed working on hardware

- [x] axl-sdk USB-NIC bundle (`NetworkCommon.efi` + `UsbRndis.efi`,
      ~20KB total) brings up the iDRAC virtual USB-NIC on the host
      UEFI shell. iDRAC enumerates as Microsoft RNDIS, NOT
      CDC-ECM/NCM. Dell's BIOS DXE volume on this platform has the
      full TCP/IP stack above SNP and the full USB stack below, but
      no USB-NIC class driver in between — that's exactly the gap
      axl-sdk fills. Verified empirically (5 independent checks
      including `dh -p`, `dh -p LoadedImage`, `bcfg driver dump`,
      explicit `connect <usb-io-handle>` against every USB endpoint).
- [x] axl-webfs `--help` and `list-nics` work cleanly on real
      hardware (mount/serve blocked by L3 routing — see below).
- [x] `racadm console com2` works from the laptop SSH wrapper as
      long as `BIOS.SerialCommSettings.SerialComm = OnConRedir` is
      applied. Disconnect: `Ctrl-\\`.

### Bugs found and fixed (2026-05-04)

- [x] **iPXE leaves boot-services watchdog armed.** When axl-sdk
      loads `ipxe-intel.efi` or any iPXE-derived driver via
      `axl_net_ensure_drivers`, iPXE's `efi_watchdog.c` arms a
      5-min `SetWatchdogTimer` and re-arms every 10s while alive.
      iPXE's shutdown handler only disarms when chaining to an OS
      (`booting==true`); when iPXE exits without booting (the
      axl-sdk SNP-binding case), the watchdog stays armed → host
      resets ~5 min later (Dell BIOS extends the timeout to ~30 min
      in BDS phase). Fixed by adding `axl_watchdog_disarm()` at end
      of `axl_net_ensure_drivers`. Cannot be unit-tested in QEMU
      (real hardware only); regression covered via on-hardware
      cycle test.
- [x] **`lsusb` leaks per-entry device-path bytes.** `axl_memdup` at
      `src/usb/axl-usb.c:354` allocates one buffer per USB interface
      entry; entries array is process-lifetime by design but cleanup
      was missing → `axl_mem_dump_leaks()` flagged ~2.7KB on a real
      Dell with 16+ USB devices. Fixed by registering an
      `axl_atexit` cleanup that frees each entry's `bus_key` (alias
      of `dev_key`) and the array itself.
- [x] **`lspci` sidecar singleton ctx leaks ~32 bytes.** The
      atexit-thunk in `src/data/axl-sidecar.c` explicitly didn't
      free its `SingletonAtexitCtx`, on the rationale "process exit
      reclaims the small allocation." But our `axl_mem_dump_leaks()`
      runs at axl_runtime_cleanup BEFORE that, so the comment was
      contradictory in our environment. Fixed by freeing ctx in the
      thunk (callers are forbidden from running `_free` after
      atexit-thunks have fired, which is the established axl-sdk
      convention).

- [x] **AXL_TLS toggle silently produced non-TLS libaxl.a + tools.**
      Two related Makefile bugs surfaced while wiring TLS into
      uefi-devkit's tool build pipeline. (1) Toggling `AXL_TLS=1`
      grows `LIB_OBJS` with ~50 mbedtls .o files but make sees
      libaxl.a (from prior non-TLS run) as up-to-date and skips
      `ar rcs` — TLS symbols never enter the archive. (2) Tool
      .efis from prior runs are NEWER than the (about-to-be-rebuilt)
      libaxl.a, so make decides they're up-to-date and skips
      re-linking — observed `fetch.efi` 22:23, libaxl.a 22:25.
      Both produce non-TLS binaries with no warning. Fixed by
      detecting the AXL_TLS state-change at make parse time (via
      `$(BUILDDIR)/.axl-tls-state`) and wiping `.o`s, libaxl.a, and
      tools when it flips. Also added the missing `clean-tools`
      target uefi-devkit references in its `tools-clean` recipe.
      Detected via `fetch.efi` size: 185KB (non-TLS) vs 433KB (TLS).

### Bugs found while exercising tools end-to-end (2026-05-05/06 sweep)

Re-tested every shipped tool against XE7745 via the axl-webfs PUT
loop (`docs/HW-Testing-Workflow.md`). Catalog of new findings:

- [x] **Dell IPMI `chassis status` returned all zeros.** Verified
      fixed on hardware: `Power State: on`, `Current Power State:
      0x2d` after the buffer-shift fix below. Was: all-zero response
      on iDRAC10 (server obviously powered on) — root cause was the
      Dell vendor protocol writing into `Response[0]` not `&Response[1]`.
      Reference comment in `uefi-ipmitool/IpmiToolPkg/Library/
      IpmiTransportLib/IpmiTransportLib.c:184-188` documents the exact
      empirical finding ("Previous testing showed that &Response[1]
      returns zeros"). Fix: hand the firmware `resp[0..N-1]`, shift
      right by one after the call, synthesize CC=0x00 at `resp[0]`.
      Also fixed the `DELL_IPMI_SEND_COMMAND` typedef which was
      missing the mandatory `Lun` parameter at slot 3 — without it
      every following arg slid into the wrong register/stack slot.
      Regression test in `test/unit/axl-test-ipmi.c`
      (`test_dell_transport_dispatch`) installs a mock vtable, runs
      `axl_ipmi_get_device_id`, and asserts both quirks are honored.

- [x] **IPMI auto-detect now follows the spec: SMBIOS Type 38 first,
      vendor protocols as fallback.** Previously the chain was
      EDKII → Dell → SMBIOS → default-KCS, so on Dell hardware the
      vendor protocol always won even when SMBIOS Type 38 explicitly
      pointed at a working KCS port. New chain: SMBIOS Type 38 →
      EDKII → Dell → default-KCS. Also added a public
      `axl_ipmi_session_new_with_transport(hint)` API and an
      `ipmi.efi --transport kcs|ssif|edkii|dell` flag so users can
      pin a specific transport (escape hatch when auto-detect
      misbehaves on a quirky platform).

- [x] **iDRAC10 IPMI now works via KCS** — fixed 2026-05-06 with a
      run of four state-machine fixes against the live hardware
      (see commits 2aecf16 + the staging-buffer + read-loop
      cleanup). Get Device ID returns byte-for-byte the same
      response Linux's `ipmitool raw 6 1` does
      (`20 81 01 20 02 df a2 02 00 00 01 00 08 3c 37`); chassis
      status, sel list, raw passthrough all functional with auto-
      detect (no `--transport` flag needed) since the SMBIOS
      Type 38 path now picks the right ports + drives the state
      machine compatibly. Original investigation findings on the
      Dell-vendor-protocol garbage problem preserved below for
      historical context — that path is still broken on iDRAC10
      but no longer matters since the spec-defined KCS works.

      **Resolved root causes** (each one would have masked the next
      if not all four had been fixed):

      1. SMBIOS Type 38 BaseAddressModifier register-spacing bits
         live at bits 7..6, not bits 1..0 as I'd memorized from a
         stale read of the spec. `(modifier >> 6) & 3` matches
         dmidecode + Linux ipmi_si. Wrong bits gave 16-byte stride
         (cmd at 0xCB8, unmapped) instead of 4-byte (cmd at 0xCAC).

      2. Missing `clear_obf` calls during write phase. iDRAC10 sets
         OBF=1 during the WRITE_START echo; without draining, the
         BMC sees OBF=1 while we drive the next write and treats it
         as a protocol violation (status went to 0xC1 = ERROR).
         Linux's `ipmi_kcs_sm.c` drains OBF at every state-machine
         transition.

      3. `kcs_wait_obf_set` rejected `state != expected` even when
         OBF=0 (BMC still transitioning). Real BMCs have a window
         between a status field flipping and OBF asserting.

      4. Read loop assumed every byte arrived in `state == READ`.
         Last response byte routinely arrives with `state == IDLE`
         already (BMC set the byte then immediately transitioned).
         New loop matches Linux's WAIT_READ pattern: handle
         `state in {READ, IDLE}`, drain final OBF, complete.

      Plus a structural fix: `kcs_send_raw` now stages the wire
      response into an internal 258-byte buffer (echo + max IPMI
      response) and only copies post-echo body bytes into the
      caller's buffer. Previously the typed wrappers' small
      buffers (chassis_status used resp[5]) had no room for the
      2-byte echo + body.

      **Original (now mostly historical) capture state on iDRAC10:**

      * Dell EFI_IPMI_TRANSPORT (vendor) — published, accepts
        commands, returns EFI_SUCCESS. **chassis_status (3-byte
        response) returns real BMC data** (Power State: on, byte
        0x21 = power_on + restore_policy bits set). **Get Device ID
        (11-byte response) returns garbage** — the buffer is filled
        with stack contents (Dell protocol GUID bytes
        `7409d614-5abf-4869` are observable mid-buffer across runs).
        uefi-ipmitool's reference impl shows the same brokenness
        (`mc info: failed to get device ID (Time out)`,
        `raw 6 1: transport error (Time out)`), so this isn't an
        axl-sdk bug per se — Dell changed something between iDRAC9
        (which uefi-ipmitool was developed against on PowerEdge
        XE8712 ARM64) and iDRAC10.

      * KCS at SMBIOS Type 38 port (0xCA8/0xCA9) — port-IO read
        returns 0xFF idle. The phantom-BMC guard now retries with
        a Get Status command (write 0x60 to cmd port, wait 1ms,
        re-read) per the IPMI spec — designed to catch BMCs that
        *do* idle at 0xFF but respond to commands. **iDRAC10 fails
        even this**: the byte stays 0xFF after the Get Status write,
        confirming the port is truly unmapped. Bypassing the guard
        entirely and forcing the KCS sequence hits IBF-clear
        timeout. The LPC/KCS path is genuinely locked down on
        iDRAC10 (likely intentional — production servers commonly
        disable host→BMC I/O port access for security).

      * SSIF (I2C @ slave 0x20) — uefi-ipmitool's I2C probe shows
        write succeeds but read returns 32 bytes of 0xFF on the one
        I2C master that responds (handle [4]). Disconnected bus or
        unprogrammed slave; SSIF not viable.

      * **NEW:** `DELL_IDRAC_INTERFACE_PROTOCOL`
        (E0E4EBAD-A45E-419E-852E-AEDE2C2BDE6C) is published on
        iDRAC10. uefi-ipmitool labels it "non-IPMI" — probably the
        new iDRAC config/management interface that supersedes the
        legacy 7409D614 Dell IPMI vendor protocol. No public docs
        found; understanding its API would require Dell datasheets
        or reverse engineering.

      Bottom line: on iDRAC10 from the host UEFI shell, the only
      reliable BMC path is the Dell vendor protocol limited to
      small-response commands. Linux on iDRAC10 likely uses
      either LANplus over the iDRAC USB-NIC (port 623) or the new
      Dell iDRAC interface protocol. Suggested next step: add an
      out-of-band IPMI-over-LANplus transport to axl-sdk (it would
      reuse the existing TLS+TCP stack and would work on any iDRAC
      where the USB-NIC is functional, sidestepping the broken
      in-band path entirely).

- [x] **`memspd list` finds no DIMMs on AMD EPYC** — resolved
      2026-05-06 with two changes:

      1. New public API `axl_smbus_new_with_probe(probe, user)` +
         `axl_smbus_visit_all(visit, user)` walk every published
         `EFI_SMBUS_HC_PROTOCOL` and `EFI_I2C_MASTER_PROTOCOL`
         handle (Dell PowerEdge XE7745 publishes 1 HC + 12 I2C
         masters). axl-spd's `ensure_session` now uses the probe
         API with a strict predicate: claim a controller iff some
         slave at 0x50..0x57 returns a plausible SPD type byte
         (0x09..0x12 per JEDEC spec table 4). New `memspd scan`
         verb dumps SPD-range byte 0 from every visited
         controller — diagnostic gold for "which bus has my
         SPDs?" investigations.

      2. **Empirical finding from running the scan on iDRAC10**:
         *no* I2C bus reachable from UEFI carries the DIMM SPDs.
         13 controllers visited (1 HC + 12 I2C masters); only one
         I2C master returned anything at 0x50, and that was 0xFF
         (bus floating, no slave). The DIMM SPDs are gated behind
         BMC/SMM on iDRAC10 — same lockdown pattern as KCS.
         memspd's `list` therefore falls back to enumerating
         SMBIOS Type 17 records (BIOS-populated at POST) and
         emits the standard size/speed/manufacturer/part-number
         summary. HW-validated on XE7745: 4 DDR5 64GB SK Hynix
         HMCG94AHBRA487N @ 6400 MT/s in slots A1/A2/B1/B2,
         matching the BIOS DIMM map.

      The `axl_smbus_new_with_probe` API is reusable for any
      caller that needs to pick a specific SMBus controller —
      e.g., a future SSIF backend that needs to find the BMC's
      bus rather than just the first I2C master.

- [x] **`netinfo diag` IPv4 column blank for DHCP-bound NICs.**
      Resolved 2026-05-05: `axl_net_list_interfaces` now enumerates
      IP4Config2 handles separately and correlates to SNP by MAC,
      so it picks up Dell's child-handle binding correctly. HW-
      validated on XE7745 — eth0 (DHCP) now shows 10.9.177.98 with
      netmask 255.255.254.0; eth1 shows static 169.254.1.2 with
      gateway 169.254.1.1.

- [x] **`netinfo` DEBUG file-open spam.** Resolved 2026-05-05:
      `axl_backend_file_open` now suppresses the DEBUG log line
      for `EFI_NOT_FOUND` (the normal "file doesn't exist" case
      flooded by `axl_driver_locate` probing every mounted volume).
      Real errors (media, permissions) still surface. HW-validated
      — `netinfo diag` Driver Bundle section is now clean.

- [x] **`memspd` and `mkrd` dumped `--- AXL diag ---` block on every
      run unconditionally.** `axl_diag_startup` is documented in
      `axl-diag.h` and `src/util/README.md` as `-v`/`--verbose`
      gated, but both tools called it from `main()` before arg
      parse. Fixed: gated behind the parsed verbose flag (mkrd had
      the flag; memspd grew one).

- [N/A] **fetch "eager TLS init" was actually iDRAC HTTP→HTTPS
      redirect.** `fetch.efi --head http://169.254.1.1/` shows
      `tls: initialized (mbedTLS)` because iDRAC10's HTTP server
      301-redirects to HTTPS, and fetch transparently follows the
      redirect — the TLS init fires for the second (HTTPS) hop.
      Verified empirically 2026-05-05 by inspecting the response
      headers: the `< HTTP 401` reply included
      `strict-transport-security: max-age=...; includeSubDomains;
      preload`, which only an HTTPS server emits. Not a bug.

- [x] **`make tools` from a fresh state fails on missing crt0/reloc/
      debug-info `.o` files.** Resolved 2026-05-05: `tools` target
      now depends on `all`, so the per-arch object set + libaxl.a
      are built before any tool tries to link.

- [ ] **HTTPS to iDRAC eth0 routed address times out cleanly.**
      `https://10.215.120.97/` from the host's RTL8153 NIC stalls
      ~10s then "request failed". `http://10.215.120.97/`, ICMP, and
      `https://169.254.1.1/` (same iDRAC, different NIC) all work —
      so axl-sdk's TLS path is healthy. Strongly suggests corp
      network policy blocks 443/tcp on the segment between the host
      eth0 and 10.215.120.97. Captured here so a future investigator
      doesn't waste time chasing it as an axl-sdk bug.

### Confirmed-still-working on real hardware (2026-05-05/06 sweep)

End-to-end re-tested via the axl-webfs PUT loop. All passed
(no leaks, real microsec timestamps in tool output):

- [x] `lspci` — both PCI segments enumerated (118 entries in seg 0,
      87 in seg 1). The earlier-flagged "lspci skips segment 0" is
      no longer reproducible on tip; segment 0 prints without the
      `0000:` prefix.
- [x] `lsusb` flat + `--tree` — full hierarchy (RTL8153, iDRAC USB
      composite, hubs, HID, CDC interfaces). Per-entry leak from
      handoff fixed by commit 50aed78.
- [x] `sysinfo` — full CPU/memory/firmware/SMBIOS block.
- [x] `dmidecode --type 42` — VLAN sentinel `<none>` (not
      `4294967295`), full IPv4 + IPv6 record bodies decoded.
- [x] `cat`, `hexdump` — files from cross-volume paths.
- [x] `fetch` over both HTTP and HTTPS via the iDRAC USB-NIC.

### Pre-existing open followups

- [x] **`dmidecode --type 42` decode bodies.** Resolved 2026-05-05
      in commits 6b256ad + e9ec5c8 and **HW-validated on XE7745
      2026-05-06**: dmidecode now decodes both Type 42 records
      cleanly. IPv4 record: Host DHCP → 169.254.1.2/24, Service
      static → 169.254.1.1:443, hostname `idrac.local`. IPv6
      record: Host AutoConfigure, Service static
      `fde1:53ba:e9a0:de11::1` port 443, same hostname. New public
      API: `AxlSmbiosRedfishOverIp` + `axl_smbios_read_redfish_over_ip`
      (parses the 91-byte fixed prefix + variable hostname per
      SMBIOS 3.x §7.43.3); plus `axl_ipv6_format` sibling of
      `axl_ipv4_format` (RFC 5952 canonical form). dmidecode prints
      structured Redfish-over-IP fields, hex-dumps `interface_data`
      and unrecognized protocol payloads. 2591/2591 unit tests both
      arches; original entry kept below for posterity.

- [x] **OBSOLETE — `dmidecode --type 42` decode bodies.** Currently prints only
      the header (`Interface Type: Network Host Interface (0x40)`,
      protocol count, lengths). The 7-byte interface-data and
      102-byte Redfish-over-IP protocol-data bodies aren't parsed.
      `axl-smbios.c` already has `axl_smbios_get_host_interface()`
      that returns a parsed `AxlSmbiosHostInterface` with the
      Redfish-over-IP fields (host MAC, IP, port, hostname, service
      UUID); the dmidecode tool just doesn't call it. Extend
      `tools/dmidecode.c` case 42 to print:
      - For interface-type 0x40 (Network): USB vendor:product or
        PCI BDF
      - For protocol 0x04 (Redfish over IP): host IP/mask/gateway,
        service port, hostname, service UUID
      Use case: dmidecode --type 42 should tell you *which* USB
      device is the iDRAC Redfish gateway and how to reach it.

- [ ] **USB-NIC driver image-unload entry points.** Per the
      `third_party/edk2/README.md`, these drivers are upstream
      EDK2 NetworkPkg vendored binaries — axl-sdk doesn't own the
      source. Adding `Image Unload` would have to be upstreamed.
      Also worth noting: `axl_net_ensure_drivers` deliberately
      leaves these loaded between tool invocations (so subsequent
      tools find SNP already up). An unload entry-point that
      actually disconnected SNP would defeat that contract. So
      the practical fix is "live with EFI_UNSUPPORTED on `unload`,
      it's the right behavior for the ensure pattern" — the
      original framing of this as a bug was overcautious. Mark
      the entry to make this clear next time.
      `NetworkCommon.efi`, `UsbCdcEcm.efi`, `UsbCdcNcm.efi`,
      `UsbRndis.efi` all return `EFI_UNSUPPORTED` on `unload <handle>`
      because they don't register an unload entry-point. Only
      `RtkUsbUndiDxe.efi` (third-party Realtek) gets it right. Each
      affected driver's `DriverEntry` should:
      1. Register an unload handler that `DisconnectController`'s
         every controller it bound, then `UninstallMultipleProtocolInterfaces`
         for any image-handle-installed protocols
      2. Verify by `load X.efi` + `connect -r` + `unload <handle>`
         expecting Success
      Source for these is likely EDK2 `USBNetworkPkg` derivatives —
      patches go upstream-of-axl-sdk. Quality-of-life for
      development testing without rebooting the host.

- [x] **`fetch.efi --secure` flag (and flip default to insecure).**
      Resolved in commit 4120a6a: `fetch.efi` now defaults to
      `tls.verify=false` when built with `AXL_TLS=1`, prints
      "TLS certificate verification disabled (use --secure to
      enable)" in verbose mode, and the `--secure` short-`-S` flag
      opts back into verification. Long-term Mozilla CA bundle
      remains a future enhancement.

- [x] **`lspci` doesn't enumerate PCI segment 0** — **NOT A BUG.**
      Diagnostic logging from commit 48f5b39 confirms both segments
      enumerate cleanly on XE7745:
      `MCFG[0]: seg=0x0000 base=0x60000000 bus=0x00..0xff`
      `MCFG[1]: seg=0x0001 base=0x70000000 bus=0x00..0xff`
      Tree output shows entries from both: segment 0 prints in the
      bare `bus:dev.func` format (`00:00.0`), segment 1 prints with
      the explicit prefix (`0001:80:02.0`). The original observation
      was an output-reading artifact — `grep '^[0-9a-f]{4}:'` skips
      the bare-format segment-0 entries because lspci omits the
      `0000:` prefix unless `--show-domain` is passed.
      Lesson: when lspci output looks suspicious, verify with
      `--show-domain` (force the seg prefix) or `--debug` (which
      prints the MCFG table) before filing a bug. Original entry
      preserved below.

- [x] **OBSOLETE — `lspci` doesn't enumerate PCI segment 0** (filed
      saw `MCFG: 2 segment(s)` in debug log but the lspci output
      only listed segment `0001:` devices — every host bridge,
      bridge, and endpoint missing from segment 0. Bug in
      `axl-pci.c` enumeration walk; either off-by-one on segment
      iteration or assumes single-segment.

- [x] **`netinfo --no-load` does not stop ConnectController.**
      Resolved: `--no-load` now still calls `axl_driver_connect(NULL)`
      to bind firmware-provided drivers — only the disk-driver
      loading is skipped. The two operations were bundled inside
      `axl_net_ensure_drivers`; netinfo's `--no-load` early-return
      bypassed both. Now the early-return path explicitly runs the
      connect, so "use only firmware-provided drivers" is reachable
      via the flag (originally needed a manual shell `connect -r`).

- [ ] **AXL_TLS=1 in uefi-devkit's pinned axl-sdk build.**
      Tracked as a uefi-devkit-side task: enable `AXL_TLS=1` so the
      bundled `fetch.efi` and friends gain HTTPS support. iDRAC
      Redfish is HTTPS-only; without this flag, `fetch.efi` errors
      with "HTTPS requires AXL_TLS=1 build." Source already supports
      TLS via mbedtls (`src/net/axl-tls.c`); just a build-flag flip.

- [ ] **Stage `rfbrowse.efi` in uefi-devkit images.** rfbrowse is
      built and works (Phase B2 done) but not staged in
      `uefi-devkit/build/staging/<arch>/`. Add a manifest entry so
      it ships with uefi-devkit ISOs.

- [x] **`UsbRndis.efi` data plane silently drops packets — RESOLVED
      via axl-sdk-side workaround.** Tip 2acf40f, **HW-validated on
      XE7745 2026-05-06**:
      - `RndisFix.efi` walks USB interfaces, finds RNDIS comms
        (vid:pid 413c:a102, class 02/02/ff), sends
        `REMOTE_NDIS_SET_MSG / OID_GEN_CURRENT_PACKET_FILTER = 0x0D`
        via `SEND_ENCAPSULATED_COMMAND` directly, bypassing the
        EDK2 `SetUsbRndisPacketFilter` stub.
      - After `RndisFix`, `ping 169.254.1.1` goes from 100% loss to
        0% loss (same TCP/HTTPS works too).
      - Auto-pick TCP routing in commit 22410f8 makes
        `RfBrowse -b 169.254.1.1` zero-config: subnet-match picks
        eth1 (169.254.1.0/24). Full Redfish service-root JSON
        returned.
      - `--source` flag tested with valid pin (works), invalid IP
        string (hard error, no silent fallback), and IP no
        interface owns (hard error).
      The EDK2 stub remains a defect upstream — see commit message
      for details. axl-sdk's workaround makes the issue invisible
      to consumers without requiring an EDK2 build environment.

- [ ] **OBSOLETE — `UsbRndis.efi` data plane silently drops packets — root cause
      identified.** EDK2's `MdeModulePkg/Bus/Usb/UsbNetwork/UsbRndis/`
      driver has a stub `SetUsbRndisPacketFilter` at
      `UsbRndisFunction.c:903-911` that just `return EFI_SUCCESS`
      without ever sending a `REMOTE_NDIS_SET_MSG` to the device.
      Wiring at `UsbRndis.c:669` exposes this stub through the
      `EDKII_USB_ETHERNET_PROTOCOL.SetUsbEthPacketFilter` vtable
      slot; both `RndisUndiReceiveFilter` (via `PxeFunction.c:854`)
      and any caller hoping to set the packet filter call into the
      no-op. Result: the iDRAC's RNDIS endpoint stays in its
      post-INITIALIZE default state with packet filter = 0; it
      accepts our TX bulk-out frames but never delivers RX frames
      up the bulk-in pipe. SNP comes UP, link reports "Media
      present", `ifconfig` accepts a static IP — but ICMP / TCP /
      DHCP / anything to 169.254.1.1 gets 100% packet loss, exactly
      what we see on XE7745. Linux's `drivers/net/usb/rndis_host.c`
      always sends `RNDIS_MSG_SET` with
      `OID_GEN_CURRENT_PACKET_FILTER = NDIS_PACKET_TYPE_DIRECTED |
      _BROADCAST | _ALL_MULTICAST = 0x0D` during `rndis_bind()` —
      that is the step EDK2 omits.

      Confirmed empirically 2026-05-06: with eth1 statically
      assigned 169.254.1.2/24 + default gateway 169.254.1.1, both
      UEFI shell `ping -s 169.254.1.2 169.254.1.1` and
      `NetInfo.efi ping 169.254.1.1` time out 100%.

      Fix paths, in increasing order of effort:
      1. **EDK2 upstream patch** (~25 lines): implement
         `SetUsbRndisPacketFilter` to build a `REMOTE_NDIS_SET_MSG`
         (struct already exists at `UsbRndis.h:469`), populate
         `Oid = OID_GEN_CURRENT_PACKET_FILTER (0x0001010E)`,
         payload = 0x0D, send via the existing `RndisControlMsg`
         path (model on `RndisUndiInitialize` at
         `UsbRndisFunction.c:1063-1116`). Ship a fixed
         `UsbRndis.efi` in `third_party/edk2/`.
      2. **axl-sdk-side workaround**: post-load, walk USB-IO
         handles bound by UsbRndis, issue the
         `SEND_ENCAPSULATED_COMMAND` USB control transfer
         directly (bmRequestType=0x21, bRequest=0x00) carrying
         the `REMOTE_NDIS_SET_MSG`. Doesn't require modifying the
         vendored binary; can ship as a small `axl-rndis-fix.c`
         in `src/net/`. Calling
         `EDKII_USB_ETHERNET_PROTOCOL.SetUsbEthPacketFilter` from
         axl-sdk is useless — that's the stub.

      Sources:
      - Linux canonical: `drivers/net/usb/rndis_host.c` —
        `RNDIS_DEFAULT_FILTER` definition and `rndis_bind()`
        sequence
      - Microsoft NDIS: OID_GEN_CURRENT_PACKET_FILTER spec
      - EDK2 source: `MdeModulePkg/Bus/Usb/UsbNetwork/UsbRndis/`
        in edk2-stable202505 (same defect in edk2-stable202408 —
        long-standing, not a regression)

- [ ] **OBSOLETE — `UsbRndis.efi` data plane silently drops packets on iDRAC10
      USB-NIC** (was filed as "L3 to iDRAC USB-NIC fails," now
      narrowed). Confirmed 2026-05-04 on XE7745:
      - Bind chain is correct: with `UsbCdcEcm`+`UsbCdcNcm`+`UsbRndis`
        all loaded and competing on the iDRAC USB-IO handle, RNDIS
        wins (ECM/NCM `Supported()` both return NOT_FOUND for this
        device descriptor). So the class is genuinely RNDIS.
      - SNP comes UP, MNP children spawn, EDK2 `ifconfig -s eth0
        static 169.254.1.2 255.255.0.0 169.254.1.1` applies cleanly
        (verified via `ifconfig -l`: routes set, gateway set).
      - Both ICMP ping and TCP/80 + TCP/443 connect attempts to
        iDRAC's `169.254.1.1` produce 100% packet loss / "login
        request failed."
      - `delldiagslinux` libredfish on the same hardware works
        fine from a booted Linux (kernel `cdc_rndis`) — so iDRAC
        backend IS responsive; the gap is purely in our UEFI
        RNDIS data plane.

      Most likely root causes inside `UsbRndis.efi`:
      - `REMOTE_NDIS_INITIALIZE_MSG` over the RNDIS control endpoint
        either not sent or not negotiated correctly (without it the
        device buffers and discards frames silently)
      - `REMOTE_NDIS_SET_MSG` with `OID_GEN_CURRENT_PACKET_FILTER`
        not configured (default filter rejects all received frames)
      - MTU / max-transfer-size mismatch from a partial init

      Audit `UsbRndis` source against Linux's `drivers/net/usb/rndis_host.c`
      and Microsoft's RNDIS spec sections 2.2.4–2.2.7. Adding verbose
      RNDIS-init log lines behind a debug build flag would make the
      next investigation tractable. Without USB packet capture, this
      is otherwise blind.

      Side note: axl-sdk's `netinfo list` doesn't reflect static IP
      set by `ifconfig` either (column shows `IPv4=-` when ifconfig
      reports `169.254.1.2`); netinfo is reading from a different
      source than IPv4Config2. Filed separately as a netinfo display
      bug.

- [x] **axl-webfs serve volume listing corrupted (multi-volume) +
      teardown leaves loop with active event sources.** RESOLVED
      2026-05-05, **HW-validated on XE7745 2026-05-06**: serve
      lists `fs0:` through `fs4:` cleanly (all five ASCII names,
      no garbage, fs4 present), and ESC-stop produces just
      `mem: no leaks detected` with no AxlLoop-free-with-active-
      events warning. Three commits:
      - axl-webfs `c5328ac` aliases FtVolume to AxlVolume so the
        struct layout can't drift. The 8-byte size mismatch caused
        every entry past index 0 in the volume listing to read
        garbage (because file-transfer.c casts mVolumes directly
        to AxlVolume*).
      - axl-webfs `a3c3611` reverses the cmd-serve teardown order
        so the HTTP server frees BEFORE its parent AxlLoop, which
        clears the TCP listener event sources.
      - axl-sdk `e642c9c` adds `<image_fs>:<name>` to the driver
        candidate search list so "drop driver next to app at
        volume root" works (covers both axl-webfs's mount tests
        in QEMU and the user-friendly install pattern).
      Tests: 47/47 X64 + 67/67 X64+AARCH64 axl-webfs full suite.

- [ ] **axl-webfs serve volume listing is corrupted on real
      hardware** (XE7745, 2026-05-05). axl-webfs's volume enumeration
      emits volumes with mangled labels: expected fs0/fs1/fs2/fs3/fs4,
      actual `fs0`, `<garbage>2`, ` <garbage>2`, ` `, `fs3`,
      `<garbage>p1`, AND fs4 (the volume with the actual tools) is
      missing entirely. `curl /fs4/` returns "Volume not found."
      Looks like volume names are read as multi-byte chars (likely
      UCS-2 from EFI_FILE_INFO->VolumeLabel) and emitted raw,
      producing invalid UTF-8 over HTTP. fs0 and fs3 happen to
      survive because their labels are ASCII-clean coincidentally.
      Fix lives in axl-webfs (its volume enumerator) — tracking
      here since it surfaced during axl-sdk hardware validation.

- [x] **`rfbrowse` against iDRAC10 over TLS-RSA crashes host with
      #GP** — RESOLVED in commit 33d5d55. Original entry preserved
      below for context; root cause was rsa.c not being in
      MBEDTLS_SOURCES, leaving mbedtls_rsa_init as an undefined-but-
      PLT-stubbed symbol whose GOT slot held the link-time RVA
      (0x56D00) rather than a runtime address. **Empirically
      confirmed working on real hardware 2026-05-05** against XE7745
      (svc tag KCH0TD1, iDRAC10 at 10.215.120.97):
      `rfbrowse.efi -v -u root -p calvin -b 10.215.120.97` returned
      HTTP 200 with full Redfish service-root JSON (1805 bytes,
      RedfishVersion 1.22.0). TLS handshake completed (`tls:
      initialized (mbedTLS)`); SNP came up via core drivers in 4s,
      iPXE fallback correctly skipped per commit 6bde651. End-to-end
      validation of the 15-commit chain (build-system + linker +
      mbedtls + iPXE-fallback + watchdog).

- [x] **`axl_http_client` does not decode `Transfer-Encoding:
      chunked` response bodies.** Surfaced + RESOLVED 2026-05-05 on
      XE7745, **empirically validated end-to-end** against iDRAC10:
      `RfBrowse.efi -v -u root -p calvin -b 10.215.120.97
      /redfish/v1/Systems/System.Embedded.1` returns the full
      multi-KB chunked Dell ComputerSystem JSON (PCIe device list,
      DIMM topology, OEM Dell extensions, etc.). The fix landed in
      four commits:
      - `b0f5cef` — `read_chunked_body` state machine (ST_SIZE →
        ST_DATA → ST_TRAIL → ST_TRAILERS → ST_DONE) plus
        `axl_hex_parse_u64` public helper, dogfooded by
        `axl-http-request.c` and `axl-pci.c`.
      - `b9c88dd` — don't treat 0-byte recv as EOF (TLS WANT_READ
        case).
      - `dfad8a4` — first attempt at draining staged TLS data; had
        a buffer-aliasing flaw.
      - `e9df883` — adopt softbmc's canonical pattern: persistent
        per-client `tls_rx_buf`, `tls_drain` loop, separate
        plaintext destination. Cross-checked against EDK2
        `NetworkPkg/HttpDxe/HttpsSupport.c` (alternative manual-
        framing design) and Mongoose's same-bug fix
        (cesanta/mongoose#2668). HTTP integration: `/chunked`,
        `/chunked-ext`, `/chunked-with-cl`. 2569/2569 unit +
        72/72 HTTP integration both arches.

- [x] **Content-Length body-read loop hangs on TLS `WANT_READ`.**
      Originally filed against the old `client_recv` (TCP→stage→read,
      one shot per call). The rewrite in commit e9df883 (persistent
      `tls_rx_buf` + `tls_drain` loop) made `client_recv` always
      drive progress: each call does a TCP recv that adds bytes to
      mbedtls's internal record-assembly state, even if the round
      doesn't yet yield plaintext. So the Content-Length loop's
      `recv_len == 0` rounds are guaranteed to be transient — the
      next iteration's TCP recv will eventually complete a record.
      Resolved as a side-effect of the chunked-decode fix.

- [x] **216-byte leak in `src/net/axl-mbedtls-platform.c:31`** on
      rfbrowse exit, surfaced 2026-05-05 by the in-tree leak tracker
      after the chunked-decode validation succeeded. RESOLVED in
      commit 4e9ff8d, **empirically validated on XE7745**: same
      RfBrowse system-inventory probe now reports
      `mem: no leaks detected` on exit. Root cause was that
      `axl_tls_init` set up five mbedtls globals (config / cert /
      PK / CTR-DRBG / entropy) without registering the matching
      `axl_tls_cleanup` anywhere; commit hooks it via `axl_atexit`,
      matching the AxlSpd / AxlUsb pattern.

- [ ] **Tools have inline hex-string parsers worth migrating to
      `axl_hex_parse_u64`.** Reviewer flagged 2026-05-05:
      `tools/lspci.c` (parse_hex_field/pair pattern, same as
      pre-refactor axl-pci.c), `tools/lsusb.c` (same), `tools/ipmi.c`
      around `(v << 4) | n` loops, `src/data/axl-json-parse.c` for
      JSON5 `0x...` numeric literal parsing. Migrating buys free
      uint64-overflow detection. `axl-url.c` and the JSON5 `\xHH`
      decoder are lower priority — `axl_hex_nibble × 2` is arguably
      clearer for fixed-2-digit decoding.

- [x] **OBSOLETE — original entry kept for posterity:** `rfbrowse`
      against iDRAC10 over TLS-RSA crashes host with #GP** (XE7745,
      2026-05-05, mbedtls config commit a7a7cf2). Now
      that ECDHE-RSA / RSA-PKCS1 cipher suites are enabled, the
      handshake gets past `NO_CIPHER_CHOSEN` and proceeds — but
      crashes the host:
      ```
      UEFI0011: CPU Exception Type 0x0D: General Protection (Software)
      ```
      from iDRAC lifecycle log timestamped 05:24:56, exactly when
      rfbrowse was invoked. RSOD text (currently uncaptured) is
      needed to identify the faulting RIP. Likely candidates inside
      mbedtls/RSA path:
      - Misaligned bignum access (mbedtls's `mbedtls_mpi` arrays
        need natural alignment; if our PEM/X509 parser hands it
        unaligned bytes the hardware traps)
      - Stack overflow — RSA-2048 needs more stack than ECDHE-ECDSA;
        EDK2 default per-app stack is 128KB, mbedtls might overflow
        when chains/key-sizes get larger
      - Buffer overrun in our `axl-mbedtls-platform.c` shim (alloc/
        free/snprintf wrappers) that's only exercised on the RSA
        path (extra alloc/free pressure from RSA blinding etc.)

      Plan: capture the RSOD screenshot, run through
      `~/projects/aximcode/rsod-decode/rsod-decode.py` with the
      built rfbrowse.efi to map RIP→source line, fix at root cause.
      Until fixed, ECDHE-RSA / RSA cipher suites should perhaps be
      enabled behind a build flag (default off) so non-iDRAC
      consumers don't pay for an unstable codepath.

- [ ] **`Load Error` on subsequent `.efi` launches after running
      axl-sdk tools.** User-attributed 2026-05-05 (corrects two
      prior wrong hypotheses): the failure is NOT iDRAC-virtual-
      media-related (user reproduced with the bundle copied to
      other media), and is NOT iPXE-LoadImage-hook-related (iPXE
      not loaded in any of the failing sessions). At least three
      distinct bites observed in this session, all requiring a
      host reboot to recover.

      **DO NOT DISMISS — the bug is real and is expected to
      recur.** User's standing instruction 2026-05-05: keep this
      open until paired captures definitively pin the cause; the
      "stress test didn't reproduce it" finding below is data, not
      a fix.

      **Empirical findings from a stress test post-commit 4e9ff8d
      (mbedtls atexit cleanup):** ran 7 distinct tools (NetInfo,
      LsPci, LsUsb, SysInfo, Dmidecode, RfBrowse, Hexdump) plus
      3 sequential RfBrowse invocations — all completed cleanly,
      0 Load Errors, every run reported "mem: no leaks detected",
      and `memmap` totals (LoaderCode/BS_Code/BS_Data/RT_Code/
      RT_Data) were byte-identical across captures. **This rules
      out one specific hypothesis** — "pool fragmentation from
      cumulative leaks" — because per-run accounting is now exact.
      It does NOT mean the bug is fixed; the failure is reportedly
      random and tens of clean runs don't disprove a sporadic
      failure mode. The user has explicitly observed this
      symptom with the bundle copied to non-iDRAC media, so the
      cause is in axl-sdk-tool execution residue of some kind.

      Remaining candidates worth investigating:
      1. Some specific tool sequence that this stress test didn't
         hit (e.g., killing a tool with Ctrl-C mid-network-op,
         tools that fail vs. tools that succeed, etc.).
      2. State accumulating outside what `memmap` and the
         AxlMem leak tracker observe — protocol installations on
         the handle table, event/timer registrations, driver
         binding state, OpenProtocol agent records that aren't
         CloseProtocol'd, etc.
      3. Race / TOCTOU in the loader itself triggered by axl-sdk
         driver-binding or signal-handler installation.

      Capture ritual when it next bites (memorize this, the bug
      window is short — recovery requires reboot):
      ```
      dh -p LoadedImage         # count loaded images
      dh -p Image               # broader image protocol coverage
      memmap                    # page totals + free fragmentation
      ```
      Both at a known-good state (after a successful tool run)
      and at a failing state (right before re-trying a tool that
      Load-Errors). The diff is the shortest path to root cause;
      a single failing-state capture without a paired good
      baseline is much weaker.

- [ ] **uefi-devkit's `net-init.nsh` bypasses the iPXE-fallback
      protection in `axl_net_ensure_drivers`.** The script does:
      ```nsh
      for %d in drivers\%arch%\*.efi
          load %d
      endfor
      connect -r
      ```
      This wildcard-loads `ipxe-intel.efi` / `ipxe-broadcom.efi`
      eagerly, regardless of whether SNP comes up from non-iPXE
      class drivers — so the protection added in axl-sdk commit
      6bde651 (load iPXE only as fallback) doesn't apply when users
      run `net-init.nsh` directly. Result: same LoadImage poisoning
      and watchdog-armed states the axl-sdk fix was supposed to
      prevent.

      Two options for uefi-devkit-side fix:
      (a) Drop `net-init.nsh` entirely. Tools like `NetInfo list`
          already trigger `axl_net_ensure_drivers` with the right
          fallback semantics — that's the correct entry point.
      (b) Rewrite `net-init.nsh` to skip iPXE explicitly:
          ```nsh
          for %d in drivers\%arch%\*.efi
              if not %d eq drivers\%arch%\ipxe-intel.efi then
                if not %d eq drivers\%arch%\ipxe-broadcom.efi then
                  if not %d eq drivers\%arch%\ipxe-all.efidrv then
                    load %d
                  endif
                endif
              endif
          endfor
          connect -r
          ```
      (a) is cleaner and matches axl-sdk's intent. Track in
      uefi-devkit, but log here since it surfaced during axl-sdk
      hardware validation.

- [ ] **axl-webfs serve frees AxlLoop with active TCP event source**
      (XE7745, 2026-05-05). Shutdown emits:
      ```
      loop: axl_loop_free: caller-owned event source id=2 still active
      loop: axl_loop_free: 1 caller-owned event source(s) still active
      — free will proceed but consumers may crash on next use
      ```
      Use-after-free vector on next loop reuse. axl-webfs's serve
      cleanup needs to disconnect the listener and remove event
      sources before `axl_loop_free`. Real bug; not theoretical.

### Test infrastructure — quirky KCS BMC fixture (medium priority)

Our QEMU IPMI testing uses a clean reference BMC simulator that
exposes none of the timing/state-transition quirks real Dell
hardware has. The four iDRAC10 KCS bugs (commit 2aecf16 and
its follow-ups) shipped through QEMU clean and only surfaced on
real hardware:

  - SMBIOS Type 38 BaseAddressModifier in QEMU is `0x00` →
    1-byte stride either way (bits 1..0 OR bits 7..6 read as 0).
    Real Dell publishes `0x4A` → 4-byte stride.
  - QEMU's BMC keeps OBF=0 during the WRITE_START echo phase;
    real Dell sets OBF=1 as a side effect of state-machine
    internals.
  - QEMU transitions OBF + state atomically; real Dell has a
    few-microsecond window where OBF rises before state flips.
  - QEMU keeps state==READ until host acks the last byte; real
    Dell flips state to IDLE at the moment it places the final
    response byte.

Plus a coverage gap: `axl-test-ipmi.c` unit tests use
`axl_ipmi_session_new_with_callback` which bypasses the wire
protocol entirely. The KCS state machine is only exercised by
the `test_real_hw` path that runs against an external BMC
simulator started by `test-ipmi-qemu.sh` — and only validates
`resp_len >= 12` for Get Device ID, doesn't stress the quirky
transitions.

Plan: write a "mean BMC" fixture (Python or QEMU device device
patch) that exposes each of the above quirks behind flags, plus
optional misbehaviors (slow OBF, non-spec spacing, more body
bytes than the spec mandates). Wire it into `test-ipmi-qemu.sh`
as a parallel KCS test job; either as random fuzz or a curated
matrix.

Concrete acceptance: a future change to `axl-ipmi-kcs.c` that
re-introduces *any* of the four bugs above MUST cause this test
to fail in CI before it can land.

### ~~New tool — `i2c` / SMBus low-level explorer~~ — DONE (commit c3fe679)

Shipped: tools/i2c.efi with verbs `list`, `probe`, `get`, `set`,
`dump` and Linux i2cdetect-style argument compatibility (`--quick`,
`--read`, `--all`, optional `[first] [last]` positionals, AUTO mode
mirroring Linux's per-address mode selection). Plus three new public
AxlSmbus APIs (`axl_smbus_describe`, `axl_smbus_quick`,
`axl_smbus_receive_byte`) so the tool can do everything Linux's
i2c-tools does.

Diagnostic value already realized — probing the AMD FCH AUX
controller (where DDR5 SPDs live on AMD server boards) in all three
modes from UEFI returns zero ACKs, while Linux on the same hardware
sees the full address range respond. Confirms the UEFI silent-
failure isn't byte-data-specific; the controller is gated at the
protocol level until OS handoff. The original problem section is
preserved below for context.

### Original problem statement — `i2c` / SMBus low-level explorer

axl-sdk has the SMBus library piece (`AxlSmbus` with HC + I2C
master backends, multi-handle walker as of 2026-05-06) but no
tool that exposes it. Linux's `i2c-tools` (`i2cdetect`, `i2cget`,
`i2cset`, `i2cdump`) is the canonical reference for what such a
tool needs to do; the same diagnostic gap is what made the
"memspd finds no DIMMs on AMD EPYC" investigation harder than it
should have been (we had to add an ad-hoc `memspd scan` verb to
get visibility into per-controller behavior).

Proposed surface (`tools/i2c.c`):

  - `i2c list` — enumerate every published `EFI_SMBUS_HC_PROTOCOL`
    + `EFI_I2C_MASTER_PROTOCOL` handle with its kind label and
    handle pointer (parallels `i2cdetect -l`).

  - `i2c probe <bus>` — Linux's `i2cdetect -y -r <bus>` equivalent.
    Walk every 7-bit slave address, print which respond; mark
    slaves that look like SPD by reading byte 0 and matching
    JEDEC type-byte range (0x09..0x12).

  - `i2c get <bus> <slave> <reg>` — single-byte read.
    `i2c get <bus> <slave> <reg> <count>` — block read. Mirrors
    `i2cget [-y] <bus> <slave> <reg>`.

  - `i2c set <bus> <slave> <reg> <byte> [<byte>...]` — block
    write. Refuse without an interactive confirmation (writes can
    brick devices); add `--force` to bypass.

  - `i2c dump <bus> <slave>` — full 256-byte hex dump (parallels
    `i2cdump`).

Why it matters: ANY future tool that needs to read a non-SPD
SMBus device (FRU EEPROMs at 0xA0..0xAE, fan controllers,
voltage monitors, temperature sensors at 0x4C/0x4D, USB-C TCPCs,
PCIe retimers, etc.) needs the same enumerate-and-probe machinery
that `memspd scan` has. Centralizing it in `tools/i2c.c` makes
the next investigation 5 minutes instead of an hour.

Empirical motivation captured in `DellXE7745/02-spd-and-i2c.log`:
on this Dell, Linux's `i2cdetect -l` shows 4 buses (3 PIIX4
ports + 1 MGA i2c) but UEFI exposes a different set (1 SMBus HC +
12 I2C masters, none of which carries DIMM SPDs). A tool would
make this immediately visible without writing one-off probe code.

### OEM CPLD SMBus adapter — vendor-protocol consumer (medium priority)

On some AMD server platforms the DDR5 SPDs aren't routed to the
FCH SMBus at all — they sit behind an OEM-specific CPLD ("FPGA
hub PLD") accessed via a vendor UEFI protocol. BIOS reads from
the CPLD and publishes a subset of the data via SMBIOS Type 17,
which is what memspd's existing fallback path consumes.

The CPLD memory map carries more than DIMM SPDs: common /
control / inventory / error-event / misc / sticky / payload /
riser-slot-map sections — totalling around 35 KB of platform
state. A shaped consumer module would unlock telemetry that
isn't in SMBIOS Type 17 (fan/temp/power detail, riser-slot
inventory, PCIe topology hints, etc.).

Reference shape (Dell BIOS reference impl studied 2026-05-06):

  - `DELL_CPLD_SMBUS_PROTOCOL` GUID
    `6B14C95E-84FF-477D-16AF-168FCE8B7D99`
  - Two function pointers: `DellCpldReadByte(Offset, *Data,
    Location)` and `DellCpldWriteByte(Offset, *Data)`.
    `Location=0` returns the byte from a BIOS-cached host memory
    map; `Location=1` reads live from the CPLD slave at SMBus
    address `0xC4` over SSIF (the EFI_I2C_MASTER protocol path).
  - 0x8A10-byte memory map split into named regions
    (CPLD_COMMON_OFFSET, CPLD_CONTROL_OFFSET, CPLD_INVENTORY,
    CPLD_MISC, etc.).

Proposed shape — parallel to existing `src/ipmi/axl-ipmi-dell.c`:

  - `src/oem/axl-oem-cpld-dell.c` — vendor adapter consuming
    `DELL_CPLD_SMBUS_PROTOCOL` when published. Internal-only; no
    public header — exposes its data through the generic AxlSmbus
    descriptor or a future `axl-oem.h` umbrella.
  - Probe at session-open time. Falls through if the protocol
    isn't installed (other platforms, other vendors).
  - Vendor-name policy applies: file naming is OK
    (`axl-oem-cpld-dell.c` is proper-noun protocol identity,
    parallel to `axl-ipmi-dell.c`); incidental empirical
    comments stay generic.

Why "medium" not "high":

  - SMBIOS Type 17 already covers the common DIMM-info case, so
    memspd doesn't need this path
  - No current consumer needs the extended telemetry
  - Adding it is ~200 LOC + tests; not free
  - The mechanism is now documented in
    `src/smbus/axl-smbus-piix4.c` and `<axl/axl-spd.h>` so
    future maintainers see the route exists

Lift the priority if a consumer ever needs riser-slot or
fine-grained thermal data from UEFI on these platforms.

### Console + tooling improvements (lower priority)

- [ ] **Log timestamp `.usec` field is always `.000000`.**
      `print_console_timestamp` in `src/log/axl-log.c:116` formats
      6 fractional-second digits from `AxlTime.nanosecond / 1000`,
      but the backend at `src/backend/native/axl-backend-native.c:178`
      copies the raw `EFI_TIME.Nanosecond` straight from
      `gRT->GetTime`. Most firmware leaves Nanosecond=0 — the UEFI
      spec lets firmware populate it but doesn't require it, and
      the Dell PowerEdge / OVMF / iDRAC platforms we test on all
      report 0. Result: every log line ends in `.000000` which is
      worse than just hiding the field.
      Fix options:
      1. Detect Nanosecond=0 and fall back to a monotonic counter
         delta — supplement wallclock with elapsed-since-boot in
         milliseconds (we can read it via a UEFI Stall(0)-anchored
         timer or, on x64, the TSC + frequency from CPUID).
      2. Hide the fractional field when Nanosecond=0 — minimum
         viable, no resolution improvement but stops lying.
      Surfaced 2026-05-06 by user during the in-band Redfish
      validation session.

- [ ] **`axl-webfs serve` should accept `--source <ip>` to bind a
      specific listening interface.** `axl_tcp_listen` currently
      auto-picks via `tcp_find_service_binding(NULL, NULL, ...)`
      which lands on the first non-zero handle. On a multi-NIC
      host (laptop curl path needs eth0 = 10.9.177.98; in-band
      BMC path needs eth1 = 169.254.1.2), the user has no way to
      pick. The TCP layer already plumbs a source-IP through
      `axl_tcp_connect_via`; the listen path needs a sibling.
      Plan:
      1. Add `axl_tcp_listen_via(port, source_ip, &listener)` to
         the public `axl-tcp.h`. `source_ip == NULL` keeps current
         auto-pick.
      2. Plumb through `axl_http_server` so the server config
         exposes a "listen.ip" option.
      3. Wire `--source <ip>` into axl-webfs's serve subcommand.
      Surfaced 2026-05-06 during PUT/GET validation: with eth0
      and eth1 both configured, auto-pick chose handles[0]
      consistently which happened to be the correct one — but
      that's fragile. Explicit pin is the right shape.

- [ ] **Console-aware tool output mode.** Tools like `lspci`,
      `drivers`, `netinfo`, `dmidecode` emit ANSI escape sequences
      (color, cursor positioning) that are noise when consumed via
      IPMI SOL or piped capture. UEFI `ConOut->Mode` lets tools
      detect serial-console mode (or expose an env var); switch to
      flat output when set.

- [ ] **Real-hardware test runner.** Today axl-sdk has 2565
      ratcheted unit tests in QEMU. The QEMU↔real-Dell coverage gap
      is real (mkrd Load Error on the R6725 didn't reproduce in
      QEMU; `--no-load` semantics are hardware-dependent). A
      `scripts/test-axl-hw.sh` that:
      1. Mounts the test bundle via iDRAC virtual media
      2. Cold powercycles the host
      3. Watches IPMI SOL for the UEFI shell prompt
      4. Drives test EFIs sequentially, captures output
      5. Reports pass/fail in ratchet style
      Closes the QEMU-coverage gap for pre-release CI.
