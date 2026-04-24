# Changelog

All notable changes to the AXL SDK are documented here. This project
follows [Semantic Versioning](https://semver.org/).

## Unreleased

_No changes yet._

## 0.2.1 — 2026-04-23

### Legal / Compliance

- **Per-file SPDX headers.** Every `.c` / `.h` under `src/`,
  `include/`, and `tools/` now carries a two-line SPDX +
  copyright header:

  ```c
  /* SPDX-License-Identifier: Apache-2.0 */
  /* Copyright 2026 AximCode */
  ```

  Makes the license machine-discoverable by SBOM / compliance
  tooling (reuse.software, SPDX scanners, distro packagers).
  Auto-generated UEFI headers (`include/uefi/generated/*.h`)
  inherit the same header from the generator script.
  `docs/AXL-Coding-Style.md` documents the convention as the
  first block of every source file.

## 0.2.0 — 2026-04-23

Minor bump marks the **license change** (custom permissive →
Apache-2.0) and a round of distribution / compliance cleanup.
All prior releases (v0.1.0 through v0.1.4) have been removed
from the public release page; v0.2.0 is the new canonical
download. Existing copies remain valid under the terms they
were originally distributed under.

### Legal / Compliance

- **Relicensed to Apache-2.0.** The `LICENSE` file previously held a
  custom BSD-2-Clause-style license with a patent grant; replaced
  with the standard SPDX Apache-2.0 text. Legally similar
  (permissive, attribution, patent grant), standard-library
  recognizable, easier for downstream SBOM/compliance tooling.
  Previously distributed versions (v0.1.0 through v0.1.4) remain
  under the terms they were released under — Apache-2.0 applies
  from the next release forward.
- **Added `NOTICE` file** with copyright attribution and
  entity-clarification (AximCode is the trade name used by the sole
  author, copyright held personally, not by a registered entity).
- **Added `CONTRIBUTING.md`** with DCO sign-off requirement and a
  contributor-grants-relicensing clause. Prevents downstream
  contributions from constraining future licensing decisions.
- **Ship mbedtls LICENSE alongside binaries.** mbedtls is
  statically linked into `libaxl.a` and into the networking
  tools (`fetch`, `rfbrowse`); Apache-2.0 §4(a) requires
  the LICENSE be carried with any binary redistribution. The
  `.deb` / `.rpm` packages now install it at
  `/usr/share/doc/axl-sdk/third_party/mbedtls/LICENSE`, and
  each tools tarball carries it at `third_party/mbedtls/LICENSE`
  inside. A new top-level `THIRD_PARTY.md` documents the
  license election (we elect Apache-2.0 from mbedtls's
  Apache-2.0-OR-GPL-2.0-or-later dual-license).

## 0.1.4 — 2026-04-22

### Distribution

- **Collapsed `axl-sdk` and `axl-sdk-tls` into a single package.**
  The library now ships with mbedtls compiled in; `ar` only
  pulls mbedtls .o files into the final `.efi` when the app
  actually references `axl_tls_*` / `https://`. Apps that don't
  use TLS are unchanged in size; apps that do no longer need
  users to pick a different install. Users who want an even
  smaller `libaxl.a` can rebuild from source with `AXL_TLS=0`.
  Release assets drop from 4 packages to 2:
  `axl-sdk.deb` / `axl-sdk.rpm` only.
- **Tools tarballs now built with TLS.** `fetch` handles HTTPS;
  `rfbrowse` (Redfish) is fully functional. Tools that don't
  reference networking (mkrd, hexdump, find, grep, sysinfo)
  don't pull mbedtls in.
- **Dropped the dedicated `axl-sdk-source.tar.gz` asset.** Now
  that each release's tagged source tree is published to the
  public repo as a snapshot commit, GitHub's auto-generated
  **Source code (tar.gz)** archive is the canonical source
  download. `git clone https://github.com/aximcode/axl-sdk-releases.git`
  + `git checkout <tag>` also works.

## 0.1.3 — 2026-04-22

### Distribution

- **Binary + source packages now published at
  [aximcode/axl-sdk-releases](https://github.com/aximcode/axl-sdk-releases/releases).**
  The upstream development repo remains private; a dedicated
  public repo hosts the release artifacts so anyone can install
  without a GitHub account. Install URLs are version-agnostic via
  `/releases/latest/download/<file>`:

  ```bash
  curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.deb
  sudo apt install ./axl-sdk.deb
  ```

  Each release publishes `SHA256SUMS` alongside the .deb / .rpm /
  source tarball / tool tarballs. Use the versioned URL pattern
  `.../releases/download/v<ver>/<file>` to pin a specific version.
- **Pre-built UEFI tool tarballs** (`axl-sdk-tools-x64.tar.gz`,
  `axl-sdk-tools-aa64.tar.gz`) for USB-stick UEFI-shell use —
  extract to a FAT stick, boot, run with `--help`.
- **Source snapshots on the public repo** so `git clone` yields
  the latest release's source and GitHub's auto **Source code**
  archives are functional (not just the release-repo README).
- **Filenames cleaned up**: host-arch labels dropped from .deb
  and .rpm filenames (`axl-sdk.deb` / `axl-sdk.rpm`, not
  `axl-sdk_amd64.deb` / `axl-sdk.x86_64.rpm`) so the release
  page's only arch labels refer to UEFI target arch (x64 / aa64
  in the tool tarballs).

### Changed (breaking, pre-1.0)

- **Sleep / wait APIs consolidated in `<axl/axl-wait.h>`.**
  Removed the public busy-wait surface (`axl_stall` from
  `axl-sys.h`, `axl_spin_sleep` / `axl_spin_msleep` / `axl_spin_usleep`
  from `axl-time.h`) — the spin family had zero callers, and the
  one `axl_stall` caller (`tools/mkrd.c`) was a bug. Sub-millisecond
  hardware timing (e.g. AxlIpmi KCS) still uses the backend-internal
  `axl_backend_stall` — not exposed to SDK consumers.

  The CPU-idle sleep family (`axl_sleep`, `axl_msleep`, `axl_usleep`)
  moved from `<axl/axl-time.h>` to `<axl/axl-wait.h>` and is now
  implemented as thin void-return wrappers over `axl_wait_ms(NULL, ...)`.
  Ctrl-C now returns from sleep early (matching POSIX intuition);
  it does not auto-terminate the app. `axl-time.h` now contains
  only `axl_time_format` and `axl_time_get_ms`.

  Consumers must include `<axl/axl-wait.h>` for sleep APIs instead
  of `<axl/axl-time.h>`. Sources that use `#include <axl.h>` are
  unaffected.

- **AxlEvent promoted to a first-class struct; AxlCompletion
  collapsed into it.** The old `AxlCompletion` (wrapping an
  EFI_EVENT) and the `AxlEvent` typedef (thin `void *` handle) were
  structurally identical. They're now one primitive: `AxlEvent` is
  an opaque struct with signal/wait/reset/is_set state, and
  `AxlCancellable` is a typed contract layered on top. Migration:
  - `AxlCompletion` → `AxlEvent`
  - `axl_completion_new/free/signal/reset/wait/wait_timeout` →
    `axl_event_new/free/signal/reset/wait/wait_timeout`
  - `<axl/axl-completion.h>` → `<axl/axl-event.h>`
  - `axl_event_create/signal/close` (raw-handle trinity in
    `axl-loop.h`) removed. Use `AxlEvent` and pass
    `axl_event_handle(e)` to `axl_loop_add_event` where a handle is
    required. The raw typedef is now `AxlEventHandle` (public, in
    `axl-event.h`) and still flows through `axl_loop_add_event` for
    firmware-owned events (TCP completion tokens, etc.).
  - New fast-check: `axl_event_is_set(e)` reads a local flag without
    driving the loop.

- **Sync primitives relocated.** `axl-cancellable.{h,c}`,
  `axl-wait.{h,c}`, and the new `axl-event.{h,c}` now live in
  `src/event/` (with matching `include/axl/axl-event.h`). `src/util/`
  is reserved for env/path/time/hexdump/sys-style helpers.

- **Arena moved.** `src/task/axl-arena.c` → `src/mem/axl-arena.c`.
  No API change. Arenas are allocators, not task/offload primitives.

### Added

- **AxlIpmi diagnostics (ported from uefi-ipmitool debugging).**
  Brings over the three hard-learned diagnostic features the
  uefi-ipmitool project accumulated debugging Dell iDRAC on Nvidia
  Grace Arm64:
  - **Chunked SDR reads.** `axl_ipmi_sdr_get()` now fetches records
    in the Linux-ipmitool style — 5-byte header first, then body
    in 23-byte chunks. Full-record reads (BytesToRead=0xFF) produce
    60+ byte IPMI responses that force multi-part SSIF reads,
    which hang the Nvidia UEFI I2C driver + Dell iDRAC combo
    (uefi-ipmitool commit 8c6acdb). Keeps every response in a
    single SSIF block. Public signature is unchanged; callers get
    the full record transparently.
  - **BMC reset wrappers.** `axl_ipmi_bmc_cold_reset()` (App 0x02)
    and `axl_ipmi_bmc_warm_reset()` (App 0x03) exposed as typed
    functions. Surfaced as `ipmi mc reset cold|warm` on the tool.
  - **`axl_ipmi_probe(AxlIpmiProbe *out)`** library helper + `ipmi
    probe` subcommand. Snapshots which IPMI-related firmware
    protocols are present (EDKII, Dell, AMI DXE/SMM, Intel SM,
    Microsoft Project Mu, SMBus HC, I2C Master, Dell iDRAC),
    decodes SMBIOS Type 38, counts I2C Master handles, and does an
    end-to-end Get Device ID through whatever auto-detect picks.
    Indispensable when `axl_ipmi_session_new()` can't find a working
    transport on an unknown platform — mirrors uefi-ipmitool's
    `probe` feature (commits eb90045 / 99c1dad) but uses the
    library's backend + SMBIOS helpers instead of raw LocateProtocol
    boilerplate.
  - **10 new unit tests** (bmc cold/warm reset + 6-check SDR
    chunked-read scenario). Ratchet 1198 → 1208.
- **AxlIpmi polish: test runbook, fuzz target, Sphinx page,
  design-doc family section.** Closes out Phase B1:
  - `test/integration/test-ipmi.sh` — documented manual-hardware
    runbook. Builds the tool, optionally stages it onto a FAT32
    disk, and prints the six-step pass criteria (BMC-backed).
  - `test/fuzz/ipmi_fuzz.c` — libFuzzer target that overrides
    `axl_ipmi_raw()` at link time to feed the fuzzer's bytes as
    IPMI responses. Drives every typed wrapper + all three
    format helpers under ASan. 50k runs green; 131 cov / 144 ft
    reached on a small seed corpus. Added to `test/fuzz/Makefile`
    alongside `url_fuzz` and `json_fuzz`.
  - `docs/sphinx/modules/ipmi.rst` + `index.rst` — AxlIpmi gets
    its own page on axl.aximcode.com, pulling in
    `src/ipmi/README.md` plus Doxygen reference for
    `axl-ipmi.h`.
  - `docs/AXL-Design.md` — new "Platform Access Modules" section
    establishing the shared shape (auto-detect session, raw +
    typed API, transport vtable, backend hooks, dogfood tool)
    that AxlSmbios + AxlIpmi share and that AxlAcpi / AxlPci /
    AxlSpd will inherit.
  - Refactor side effect: `src/ipmi/axl-ipmi-cmd.c` dropped its
    `axl-ipmi-internal.h` include (and therefore the transitive
    backend-header pull). The cmd wrappers only ever needed the
    public types + allocator, and this makes the fuzz build
    dramatically simpler.
- **AxlIpmi — phase 7 (format helpers + ROADMAP).** Adds
  `src/ipmi/axl-ipmi-format.c` with three string-table lookups:
  `axl_ipmi_completion_code_string` covers every standard
  completion code from IPMI v2.0 Table 5-2 plus OEM / command-
  specific ranges; `axl_ipmi_sensor_type_string` covers sensor
  types 0x01–0x2C from Table 42-3; `axl_ipmi_entity_id_string`
  covers the common entity IDs from Table 43-13 plus OEM ranges.
  Wired into `tools/ipmi sensor` (column-aligned output with typed
  sensor + entity names) and `tools/ipmi raw` (CC byte shown with
  its description). `docs/ROADMAP.md` marks Phase B1 complete and
  adds a new "Phase B3: Platform Access follow-on modules" section
  with scoped AxlAcpi / AxlPci / AxlSpd entries (consumer tool,
  backend deps, and effort estimate for each).
- **`tools/ipmi` — stripped-down ipmitool-equivalent.**
  New UEFI tool exercising AxlIpmi end-to-end from the command line.
  Subcommands:
  - `ipmi info` — device ID + detected transport
  - `ipmi chassis status` / `chassis power on|off|cycle|reset|diag|soft`
  - `ipmi sel list` — System Event Log dump
  - `ipmi sdr list` — SDR repository entries
  - `ipmi sensor` — all Full/Compact sensors with raw readings
  - `ipmi fru list` — FRU 0 raw bytes, 16-byte rows
  - `ipmi raw <netfn> <cmd> [<hex>...]` — raw passthrough

  Uses AxlConfig for argument parsing and `AXL_AUTOPTR(AxlIpmiSession)`
  for RAII session cleanup. Output mirrors `ipmitool`'s layout closely
  enough to feel familiar but doesn't try to cover the full feature
  set — formatted sensor/entity/event strings are deferred until the
  format helpers ship (phase 7). Builds on both x64 and aa64; 9 tools
  are now built (was 8). Real-hardware smoke coverage comes via the
  manual `test-ipmi.sh` script against a live BMC.
- **AxlIpmi — phase 5 (typed command wrappers + callback transport).**
  Ten typed wrappers covering the commands a tool needs for
  `info` / `chassis` / `sel` / `sdr` / `sensor` / `fru` subcommands:
  `axl_ipmi_get_device_id`, `axl_ipmi_get_chassis_status`,
  `axl_ipmi_chassis_control`, `axl_ipmi_sel_info`,
  `axl_ipmi_sel_get_entry`, `axl_ipmi_sdr_info`, `axl_ipmi_sdr_get`,
  `axl_ipmi_get_sensor_reading`, `axl_ipmi_fru_info`,
  `axl_ipmi_fru_read`. Each builds the request per IPMI v2.0 spec,
  verifies the completion code, and decodes the response into a
  public `AxlIpmi*` struct using little-endian byte extractors.

  New `axl_ipmi_session_new_with_callback(kind, callback,
  user_data)` lets callers plug in a send-raw function pointer.
  Primary use is unit testing (exercise the wrappers with canned
  responses, no BMC hardware required) but it also opens the door
  to pluggable out-of-process transports (IPMI-over-LAN, test
  rigs) without baking them into auto-detect.

  **43 new unit tests** in `test/unit/axl-test-ipmi.c` (1155 → 1198
  baseline). Every typed wrapper gets a happy-path test plus two
  negative-path assertions: non-zero completion code and truncated
  response both correctly fail. Ratchet bumped to 1198.
- **AxlIpmi — phase 4 (EDKII + Dell vendor protocol dispatchers).**
  Adds `src/ipmi/axl-ipmi-edkii.c` and `-dell.c`, both resolving
  their protocol via `gBS->LocateProtocol` and forwarding
  `axl_ipmi_raw()` through the firmware-provided function pointers.
  EDKII uses the standard MdeModulePkg `IPMI_PROTOCOL` shape; Dell's
  proprietary protocol returns response data without a completion
  code, so the dispatcher synthesizes `CC=0x00` at `resp[0]` and
  shifts the vendor bytes up — callers see the same layout
  (`[CC, data...]`) regardless of transport.

  Auto-detect priority in `axl_ipmi_session_new()` is now:

      1. EDKII IPMI_PROTOCOL        (firmware-mediated; preferred)
      2. Dell EFI_IPMI_TRANSPORT    (vendor; Dell HW only)
      3. SMBIOS Type 38             (KCS or SSIF from iface byte)
      4. x86 default KCS            (0x0CA2 / 0x0CA3 last resort)

  Firmware-mediated transports handle platform quirks and reach
  BMCs on buses we can't enumerate directly, so they always win
  over direct physical access when both are available. All four
  transports plug into the same vtable, so `axl_ipmi_raw()` calls
  are identical downstream regardless of which transport the
  auto-detect landed on.
- **AxlIpmi — phase 3 (SSIF transport).** Adds
  `src/ipmi/axl-ipmi-ssif.c`: SMBus-based framing per IPMI v2.0
  Section 12, on top of the `axl_backend_smbus_*` hooks (which
  already handle the `EFI_SMBUS_HC_PROTOCOL` → `EFI_I2C_MASTER_PROTOCOL`
  fallback). Transport layer owns the protocol's timing hazards:
  60 ms inter-command delay after every completed transaction,
  5-retry write loop at 60 ms intervals, 10-retry read loop with
  exponential backoff starting at 60 ms. Multi-part
  write/read framing handles requests / responses beyond the 32-byte
  SMBus block limit (reads reassemble blocks prefixed with the
  sequence byte until the `0xFF` end marker arrives). Auto-detect
  in `axl_ipmi_session_new()` now picks SSIF when SMBIOS Type 38
  reports interface type 4; the I2C slave address is shifted out
  of the SMBIOS-encoded wire address before handoff. Cross-compiles
  on both X64 and AARCH64; 1155 unit tests still pass on both.
- **AxlIpmi — phase 2 (skeleton + KCS transport).**
  New public header `<axl/axl-ipmi.h>` with an opaque session type
  (`AxlIpmiSession`), transport enum, and the lowest-level raw
  command entry point:
  ```c
  AXL_AUTOPTR(AxlIpmiSession) ipmi = axl_ipmi_session_new();
  uint8_t resp[16];
  size_t  resp_len = sizeof(resp);
  axl_ipmi_raw(ipmi, 0x06, 0x01, NULL, 0, resp, &resp_len);
  ```
  Auto-detects the best available transport (Phase 2 covers
  SMBIOS Type 38 → x86 default KCS at `0x0CA2`/`0x0CA3`; SSIF,
  EDKII and Dell vendor protocols follow in later phases). The
  KCS transport is a polled FSM ported from uefi-ipmitool on top
  of the new `axl_backend_io_read8/write8` hooks. Module lives
  in `src/ipmi/`; see `src/ipmi/README.md` for the layout and
  transport priority table. Public header is pulled into
  `<axl.h>` so consumer apps can `#include <axl.h>` as usual.
  Typed command wrappers, unit tests, and `tools/ipmi.c` land
  in subsequent phases.
- **UEFI generator: normalize trailing `[]` flex arrays to `[1]`.**
  `scripts/generate-uefi-headers.py` rewrites `Name[]` →
  `Name[1]` in extracted struct members, matching EDK2's
  convention and sidestepping GCC's rejection of `[]` as the sole
  named member of a struct. Removes the standing hand-edit in
  `include/uefi/generated/media.h` that kept getting clobbered on
  every regeneration.
- **AxlIpmi prep: UEFI protocol types + backend I/O hooks.** New
  SMBus / I2C protocol extractions in the manifest
  (`EFI_SMBUS_HC_PROTOCOL`, `EFI_I2C_MASTER_PROTOCOL`, and
  supporting enums/structs), plus hand-written additions to
  `include/uefi/axl-uefi-extra.h` for `IPMI_PROTOCOL` (EDKII),
  `DELL_IPMI_TRANSPORT` (vendor-proprietary), and
  `SMBIOS_TABLE_TYPE38` (DMTF SMBIOS spec). Adds four new backend
  hooks to `src/backend/axl-backend.h`:
  `axl_backend_io_read8/write8` (inline asm on x86, `-1` on aa64),
  `axl_backend_smbus_read_block/write_block` (SMBus with I2C
  fallback, matching uefi-ipmitool's dual-backend pattern).

## 0.1.2 (2026-04-18)

### Removed

- **`axl_args_*` API** (`include/axl/axl-args.h`, `src/util/axl-args.c`)
  merged into `AxlConfig`. There is now a single descriptor type,
  `AxlConfigDesc`, which drives both runtime configuration and
  command-line parsing. Callers that used `axl_args_parse(argc, argv, opts)`
  switch to:
  ```c
  AxlConfig *cfg = axl_config_new(descs, NULL, NULL);
  axl_config_parse_args(cfg, argc, argv);
  ```
  with an `AxlConfigDesc` table in place of the old `AxlOpt` array.
  `axl_config_get_bool` / `_get` / `_get_multi` / `_pos` replace the
  corresponding `axl_args_*` getters. All 8 in-tree tools, the
  `args-demo` example (merged into `config-demo`), and the unit
  tests were migrated in the same commit.

### Added

- **`scripts/build-packages.sh`** mirrors what the release workflow
  does for one TLS variant — stages the SDK via `install.sh`, runs
  `fpm` for `.deb` + `.rpm`, and smoke-tests the RPM by extracting
  it and invoking the installed `axl-cc` against `examples/hello.c`.
  Useful for smoke-testing a release before tagging and for
  producing packages on machines without CI access. Packages advertise
  `Provides: axl-sdk-devel` so `dnf install axl-sdk-devel` resolves
  correctly on Fedora/RHEL (matches the muscle-memory Fedora
  convention for dev-only packages like `gnu-efi-devel`).

### Changed

- **SDK layout polished for lintian/rpmlint compliance.** Files that
  previously sat bare at the top of `<prefix>/lib/` have moved to
  properly namespaced subdirectories, matching what distro packaging
  tools expect:
  - `lib/<arch>/` → `lib/axl/<arch>/` (per-target-arch libs and objs).
  - `lib/elf_*_efi.lds` → `lib/axl/elf_*_efi.lds` (linker scripts).
  - `lib/axl.cmake` → `lib/cmake/axl/axl-config.cmake` (now findable
    via `find_package(axl REQUIRED)`).
  - `lib/{version,build-date,backend}` → `share/axl/{...}` (SDK
    metadata; arch-independent plain text belongs under `share/`).

  The `lib/pkgconfig/axl.pc` path is unchanged. All paths remain
  relocatable — pkg-config uses `${pcfiledir}`, axl.cmake uses
  `${CMAKE_CURRENT_LIST_DIR}`, and axl-cc uses `$(dirname "$0")`.
  The embedded axl-cc wrapper and axl-config.cmake were updated to
  resolve their files from the new locations.

  **Consumer impact:** existing CMake projects that did
  `include(/path/to/sdk/lib/axl.cmake)` must switch to
  `find_package(axl REQUIRED)` (or update the explicit path to
  `/path/to/sdk/lib/cmake/axl/axl-config.cmake`). No API changes;
  `axl_add_app()` and every flag/option are unchanged.
- **Release artifacts are now `.deb` and `.rpm` packages, not tarballs.**
  `.github/workflows/release.yml` uses `fpm` against an `install.sh`
  staging tree to produce two Debian packages (`axl-sdk` and
  `axl-sdk-tls`) and two RPMs per release, each bundling both x64
  and aa64 UEFI target libraries. Consumers install with
  `sudo apt install ./axl-sdk_*.deb` or
  `sudo dnf install ./axl-sdk-*.rpm` — no tarball extraction, no
  manual PATH setup. The per-arch `axl-sdk-{x64,aa64}-linux.tar.gz`
  artifacts are no longer produced; a single `git archive`
  `axl-sdk-<ver>-source.tar.gz` is attached for source builds. See
  `packaging/postinst.sh` for the minimal after-install hint.
  Contributors and users without a matching distro package can still
  run `./scripts/install.sh --prefix /opt/axl-sdk` to stage the
  same FHS layout locally.
- **AUTO_FREE + steal_pointer applied to three internal parsers.**
  `axl_http_parse_request_line`, `axl_http_parse_headers`, and
  `axl_json_parse` now use `AXL_AUTO_FREE` on their heap scratch
  buffers and `axl_steal_pointer(&p)` to transfer ownership on
  success. Eliminates repeated `axl_free(...)` calls on error
  paths and closes two latent leaks: a silent OOM in
  `axl_http_parse_request_line` (a query-string `axl_strndup`
  failure would previously return success with `*query = NULL`,
  indistinguishable from "no query present"), and a leak in
  `axl_http_parse_headers` if `axl_hash_table_replace` rejected
  an entry (the name/value buffers were not freed on that path).
  All 1155 unit tests and 62 HTTP integration tests still pass.

### Added

- **JSON5-lite parsing in `scripts/generate-uefi-headers.py`.** The
  UEFI manifest now accepts `//` line comments, `/* */` block
  comments, and trailing commas, so entries can carry inline "why"
  notes and a file header instead of cramming them into a `_comment`
  array at the top. File renamed from `scripts/uefi-manifest.json`
  to `scripts/uefi-manifest.json5`; old `_comment` array lifted out
  into a real header comment block at the top of the file. The
  loader is a ~50-line preprocessor in the generator (`_strip_json5`)
  that walks the text string-aware and feeds the stripped result to
  stdlib `json`. No pip dependencies added. The JSON5-lite grammar
  covers only comments and trailing commas — single-quoted strings,
  unquoted keys, and numeric extensions are **not** supported. All
  six in-tree references (`CLAUDE.md`, `scripts/build.sh`, three
  `docs/*` files) updated to the new filename. Generator output is
  byte-identical to pre-rename.
- **`axl_steal_pointer()` in `<axl/axl-macros.h>`.** GLib-style
  ownership-transfer helper that returns the pointed-to value and
  NULLs the source in one step. Designed to pair with `AXL_AUTOPTR`
  on constructor success paths — `return axl_steal_pointer(&foo);`
  hands the resource to the caller while disarming the cleanup
  attribute. Type-preserving via `__typeof__`, so the result is
  assignment-compatible with the caller's variable. Applied to
  `axl_http_client_new`, `axl_http_server_new`, and `axl_url_parse`
  in this release, replacing ~30 lines of manual cascading cleanup
  code with straight-line early returns. As a side effect,
  `axl_url_parse` now correctly reports OOM if the query-string
  `strdup` fails — previously that case silently returned success
  with a NULL query, indistinguishable from "no query present".
- **Fuzz harness scaffold under `test/fuzz/`.** Standalone host-side
  libFuzzer build (`clang -fsanitize=fuzzer,address`) with two
  initial targets: `url_fuzz` covering `axl_url_parse` and
  `json_fuzz` covering `axl_json_parse`. Parser sources compile
  directly against a tiny libc-backed shim (`fuzz_shim.c`) rather
  than the freestanding library, so they run under AddressSanitizer.
  Not wired into the default `make` target or CI — fuzzing is opt-in,
  and a nightly job with crash artifact upload is tracked separately.
  Seed corpora live in `test/fuzz/{url,json}_corpus/`; see
  `test/fuzz/README.md` for build and run instructions.
- **OOM fault injection for testing error paths.**
  `axl_mem_fail_next_alloc(N)` in `<axl/axl-mem.h>` arms the Nth
  subsequent allocation to return NULL without touching the backend.
  Because `axl_calloc`, `axl_realloc`, `axl_strdup`, and `axl_memdup`
  all route through `axl_malloc_impl`, one hook catches every
  allocation path. 26 new unit tests use it to exercise the silent
  OOM paths added during the April 2026 logging work — 13
  allocator-primitive tests in `test/unit/axl-test-mem.c` and 13
  container tests in `test/unit/axl-test-data.c`. Unit-test baseline
  1129 → 1155. The hook is gated on `AXL_MEM_DEBUG` and compiles to
  a no-op in release. See `src/mem/README.md` for usage details.
- **clang-tidy static analysis in CI**
  (`.github/workflows/ci.yml`). The `.clang-tidy` config enables
  `bugprone-*` and `clang-analyzer-*` minus five documented noisy
  checks, with `WarningsAsErrors: '*'`. 11 pre-existing findings
  fixed (including two real null-deref bugs in `axl-list.c` caught
  by `clang-analyzer-core.NullDereference`).

### Changed

- **Unit test baseline**: 1162 → 1129 → 1155 across the Unreleased
  window. The `test_args` removal dropped 33 assertions when
  `axl_args_*` was unified into `AxlConfig`; the OOM injection work
  added 26 new tests. Net: -7 from the 0.1.1 baseline, but with
  stronger coverage of error paths. HTTP integration tests
  unchanged at 62.

### Fixed

- `test_args`/`axl_args_*` surface removed cleanly — no lingering
  references in docs, Sphinx, Makefile, or `include/axl.h`.
- Two real null-deref bugs in `src/data/axl-list.c` `merge_sorted` /
  `merge_sorted_data` helpers: `head.next->prev = NULL` could
  dereference NULL when both inputs to the merge were empty. Guard
  added; caught by `clang-analyzer-core.NullDereference`.

## 0.1.1 — 2026-04-13

### Added

- Compile-time version macros in `include/axl/axl-version.h`:
  `AXL_VERSION_MAJOR`, `AXL_VERSION_MINOR`, `AXL_VERSION_PATCH`,
  `AXL_VERSION_STRING`, `AXL_VERSION_NUMBER`, and the
  `AXL_VERSION_AT_LEAST(major, minor, patch)` convenience macro.
  Consumers can now gate features on a specific AXL version at
  compile time.
- `lib/pkgconfig/axl.pc` installed with the SDK so host toolchains
  that don't use `axl-cc` can `pkg-config --cflags --libs axl`
  instead of hard-coding `-Iinclude -laxl`.
- `scripts/bump-version.sh` helper that atomically updates
  `VERSION` and `include/axl/axl-version.h` so the two files never
  drift. The Makefile runs `check-version` on every build and
  hard-errors on mismatch.

### Fixed

- **HTTP server cache `max_entries` is now enforced** via FIFO
  eviction using the existing `CachedResponse.timestamp_ms` field.
  0.1.0 stored the limit but never applied it, so long-running
  servers grew the cache without bound.
- **`axl_http_server_set_route_ttl(path, ttl_ms)` honors the path
  argument.** A per-server `path → ttl_ms` map is consulted at
  cache-store time and falls back to the server-wide default on
  miss. 0.1.0 silently updated the default and discarded the path.
- **`axl_http_server_cache_invalidate(prefix)` honors the prefix
  argument.** Uses `axl_hash_table_foreach_remove` with a
  predicate that skips the `METHOD ` token in the cache key and
  compares the path portion. NULL or empty prefix still clears
  the whole cache. 0.1.0 cleared the whole cache regardless of
  prefix.
- Pre-existing leak of `key_dup` on OOM paths in
  `src/net/axl-http-route.c` fixed incidentally during the
  hash-table return-value refactor.

### Changed

- `axl_hash_table_insert` and `axl_hash_table_replace` now return
  a tri-state `int`: `1` if a new entry was added, `0` if an
  existing entry was replaced, `-1` on allocation failure. 0.1.0
  returned `int` but only signaled `0` / `-1`, so callers could
  not distinguish "replaced" from "inserted new." Most call sites
  in the tree ignored the return and need no update; the one
  exception (`src/net/axl-http-route.c`) was updated in place.
  GLib's `g_hash_table_insert` uses a bool for this distinction,
  but GLib aborts on OOM — AXL recovers, which is why we need
  three states instead of two.

### Logging

- `axl_error` / `axl_warning` calls added to ~26 silent OOM
  failure paths across `src/mem/`, `src/data/`, `src/io/`,
  `src/net/`, and `src/util/`. Every AXL allocation failure now
  surfaces with the function name, requested size, and caller
  file:line.
- `axl_calloc` gained a `count * size` integer-overflow guard
  with its own log line before the multiplication.

### Refactoring (no behavior change)

- `src/data/axl-digest.c` (645 lines) split into per-algorithm
  files: `axl-digest-md5.c`, `axl-digest-sha1.c`,
  `axl-digest-sha256.c`, plus a shared `axl-digest-internal.h`.
  Adding new algorithms is now a single-file add.
- `src/net/axl-net-util.c` (1080 lines) split into five focused
  files: `axl-net-ping.c`, `axl-net-resolve.c`,
  `axl-net-interfaces.c`, `axl-net-addr.c`, `axl-net-dhcp.c`.
- `src/net/axl-tcp.c` (1160 lines) split into `axl-tcp-sync.c`
  and `axl-tcp-async.c` with shared struct and helpers in
  `axl-tcp-internal.h`.

### Tooling

- `.clangd` config at the repo root silences
  `UnusedIncludes` / `MissingIncludes` diagnostics so the editor
  stops flagging intentional umbrella includes like
  `axl-backend.h`.
- `compile_commands.json` regenerated via `bear -- make tests tools`
  to cover all refactored files. Gitignored per-dev.

### Tests

- 18 new HTTP integration assertions covering the three cache
  policy fixes (eviction, per-route TTL, prefix invalidation).
  HTTP integration goes from 44 to 62 passed; unit tests
  unchanged at 1162 on X64 and AARCH64.

## 0.1.0 — 2026-04-06

Initial public release. Core library ported from UdkLib
(uefi-devkit), native UEFI backend (GCC + ld + objcopy), and the
first GitHub release workflow.

### Modules

- **AxlLog** — structured logging with domains, levels, handlers
  (console, ring, file).
- **AxlData** — hash table (CHAR8 + CHAR16), dynamic array, string
  utilities, JSON parser, radix tree, ring buffer, message digests.
- **AxlUtil** — file I/O, path manipulation, SMBIOS, hex dump,
  time, argument parsing, environment, system info, NVRAM.
- **AxlLoop** — event loop with timers, idle dispatch, Ctrl-C
  handling.
- **AxlTask** — AP worker pool, arena allocator, buffer pool.
- **AxlNet** — TCP / UDP sockets, HTTP server with routing and
  middleware, HTTP client, URL parser, TLS via mbedTLS (optional).
- **AxlGfx** — Graphics Output Protocol, bitmap font, primitives.
