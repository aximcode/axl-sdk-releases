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

### Phase B3: Platform Access — follow-on modules (Future)

- [ ] **AxlAcpi** — ACPI table discovery + parsing. Scope: RSDP → XSDT
      walk, header parsing, GAS (Generic Address Structure) decoder,
      accessors for FADT / MADT / HPET / MCFG. **No AML
      interpretation** — that's ACPICA-sized and out of scope. Consumer
      tool: `tools/acpi.c`. Scope anchored on SoftBMC's current usage
      (`/home/mgosha/projects/aximcode/softbmc/SoftBmcPkg/Application/
      SoftBmc/Modules/Feature/HwInfo/Acpi*.c`, ~560 LOC), which
      AxlAcpi should be able to cut by ~60%. Estimate: 1–2 weeks.
- [ ] **AxlPci** — ECAM / `PciExpressBaseAddress` config-space access
      via MCFG. Consumer tool: `tools/pci.c` lspci-like walker.
      Depends on AxlAcpi for MCFG location. Estimate: 1 week.
- [ ] **AxlSpd** — DDR4/5 SPD readers via `AxlSmbus`. Consumer tool:
      `tools/memspd.c`. Estimate: 1 week.

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

GLib-style socket layer wrapping existing AxlTcp/AxlUdpSocket. Unifies
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

### Host Tools (scripts/)

- [x] rsod-decode.py — UEFI crash dump (RSOD) decoder with MAP file support

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
- [ ] **CMake migration of the library build itself.** Parked April
      2026. The current Makefile is 466 lines and works fine; CMake
      is awkward for freestanding UEFI cross-compile (toolchain files,
      two build trees, custom `.so → .efi objcopy` rules). Revisit
      only if a concrete pain point justifies the cost — e.g., a
      consumer wants to embed AXL as a CMake subdirectory or a GUI
      IDE needs CMake project files for navigation.

### Documentation gaps

- [ ] **Design doc for the coding style** already exists, but there
      is no "how to add a new module" walkthrough. Useful when
      onboarding contributors or adding modules like `axl-ipmi`.
- [x] **Changelog.** `CHANGELOG.md` at the repo root, maintained
      per-release. Entries moved under a version heading at tag time
      (see the v0.1.2 entry for an example). Release notes in
      `release.yml` link to it rather than duplicating content.
