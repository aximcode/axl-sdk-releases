# Changelog

All notable changes to the AXL SDK are documented here. This project
follows [Semantic Versioning](https://semver.org/).

## 0.8.1 — 2026-05-01

Backward-compatible polish on top of v0.8.0's AxlArgsNode unification:
branch nodes can now carry a default handler (the `do bios` →
"print summary" pattern), AXL_ARG_S64 honors min/max bounds, two
new generic PCI helpers, and follow-up fixes from the post-v0.8.0
review.

### Added

- **Branch nodes with a default handler.** A node can now set BOTH
  `.verbs` AND `.handler`; the handler runs only when no sub-verb
  is supplied. Useful for the established CLI pattern where a
  category like `do bios` has subverbs (info, test, pci) but also
  prints a default summary when invoked alone. Dispatch is
  unambiguous: matched verb → recurse, unmatched non-flag → error
  ("unknown verb"; the handler is not a catch-all), no non-flag
  args → handler with parsed branch flags. `--help` still wins
  at every level. Help output annotates the verb whose handler
  matches the default with `(default)` so users see which sub-verb
  the no-arg form is equivalent to.
- **`axl_pci_dump(addr, buf, bytes, *out_read)`** — single-call
  PCI config-space dump. Reads up to 4096 bytes (PCIe ECAM cap)
  in 32-bit chunks; folds endian-pack, absent-detection (VID ==
  0xFFFF at offset 0), and ECAM cap into one call. Replaces the
  per-tool hand-rolled `for (reg = 0; reg + 4 <= bytes; reg += 4)
  read32; pack into buf` loop.
- **`axl_pci_class_string(class24, buf, buflen)`** — decode the
  24-bit PCI class code to a human string per the PCI Code and
  ID Assignment Spec. Output shape: `"<base> / <sub> / <prog>"`
  (e.g. `"Serial bus controller / USB / xHCI"`). ~140 LOC of
  static lookup tables; vendor/device-name lookup intentionally
  out of scope (pci.ids is too large for AXL).
- **`AXL_ARG_S64` min/max enforcement.** Pre-existing v0.7.x gap
  closed: U8/U16/U32/U64 enforced bounds but S64 silently
  accepted any in-type value. Same independent-bound check (each
  0 = no bound) with int64 cast — for negative lower bounds the
  descriptor sets `.min = (uint64_t)(int64_t)-N`, two's-complement
  round-tripped via the cast. Documented in `AxlArgDesc`.

### Changed

- **Documented vendor-naming policy** in `docs/AXL-Coding-Style.md`.
  AXL is hardware-vendor-agnostic at its public API surface (no
  `axl_dell_*` function names), but vendor-named files
  (`axl-ipmi-dell.c`) and vendor-named enum values
  (`AXL_IPMI_TRANSPORT_DELL`) / probe-struct fields are explicitly
  allowed where they describe vendor-specific external state.
  Codifies existing convention; no code changes.

### Fixed

- **Two nested-args tests no longer pass trivially.** Post-v0.8.0
  review found `test_args_nested_unknown_verb_at_branch` and
  `test_args_nested_branch_help_lists_subverbs` only asserted
  `rc + handler-not-called`; nothing verified the breadcrumb
  appeared in the error message or that help listed the right
  subverbs. Both now swap `axl_stdout` for an `axl_bufopen()`
  in-memory stream during the test and assert on captured bytes.
- **Whitespace alignment in 11 tools.** The earlier
  `.global_flags` → `.flags` sed left a 10-space indent that
  diverged from the 8-space sibling alignment. Tightened.

### Compatibility

Backward-compatible with v0.8.0:

- Branches without a default handler keep the v0.8.0 "show help
  on no-verb" behavior (regression-tested).
- Consumers that previously got the leaf+branch config error
  now silently work — additive, not breaking. Note: a consumer
  who *relied* on that error to catch an accidental
  handler-pasted-onto-branch programming mistake loses that
  signal; if you depend on validation-as-lint, audit branch
  nodes after upgrade.

## 0.8.0 — 2026-05-01

Source-incompatible AxlArgs unification: the dual `AxlArgsApp` /
`AxlVerb` types collapse into one naturally-recursive `AxlArgsNode`
that describes the program root, every inner branch ("category"),
and every leaf verb. Surfaced by delldiags do.efi cat-3 migrating
off the deprecated `axl_subcommand_dispatch` and discovering the
single-level constraint that forced consumers to either flatten
unhelpfully or hand-roll the deprecated dispatch pattern.

Also: `k`-prefix removed from every static-const-table identifier
across the codebase (33 names: `kVerbs`/`kFlags`/`kPositionals`/etc.
→ `verbs`/`flags`/`positionals`/etc.) to match the documented
naming convention.

### Breaking

- **`AxlArgsApp` removed; `AxlVerb` removed.** Both replaced by a
  single recursive `AxlArgsNode`:

  ```c
  struct AxlArgsNode {
      const char           *name;
      const char           *help;
      const AxlArgDesc     *flags;         // per-node, was global_flags at root
      const AxlArgDesc     *positionals;   // leaf only
      const AxlArgsNode    *verbs;         // branch only
      AxlVerbHandler        handler;       // leaf only — mutually exclusive with verbs
      AxlPreRunFunc         pre_run;
      void                 *user_data;
  };
  ```

  Mechanical migration: rename `AxlArgsApp` → `AxlArgsNode`,
  `AxlVerb` → `AxlArgsNode`, `global_flags` → `flags`. Existing
  single-level apps work unchanged after the rename.

- **`AxlArgsApp.usage` field removed.** Was unused by any in-tree
  consumer.

- **k-prefix dropped from static const tables.** Any consumer
  referencing in-tree symbols by name (none expected) needs
  `kFoo` → `foo`. Internal-only convention change otherwise.

### Added

- **Nested verb trees in AxlArgs.** A node is leaf-XOR-branch:
  leaves set `handler` + optional `positionals`; branches set
  `verbs` (NULL-terminated array of child nodes). Branch verbs
  recurse via the framework — consumers never call `axl_args_run`
  themselves at non-top level.
- **Parent-flag visibility.** Flags declared on a parent node are
  visible to descendant handlers via the same `axl_args_get_*`
  accessors (which now walk the parent chain on miss). A
  `--verbose` declared on the root is readable from a leaf two
  levels deep without re-declaration. Same pattern for
  `axl_args_user_data`: descendants inherit the nearest non-NULL
  parent value.
- **Breadcrumb error attribution.** Errors are prefixed with the
  full path: `do bios test: missing required <slot>` rather than
  `do: missing required <slot>`. Helps users isolate which level
  rejected their input.
- **`pre_run` per node, runs top-down.** Parent's `pre_run` fires
  before recursion into the child or before the leaf handler, so
  shared resources (config files, opened sessions) can be set up
  at any level and inherited by descendants.

### Changed

- **Documented `snake_case` for static const tables** in
  `docs/AXL-Coding-Style.md` (no `k`-prefix). Existing tables
  renamed to match — `tools/*.c` (11 tools), `src/ipmi/axl-ipmi.c`
  (vendor GUID tables), `test/data/spd-*.h` (auto-generated by
  `gen-spd.py` — also updated), and the `test/unit/axl-test-*.c`
  test files.
- **`docs/ROADMAP.md` build-system entry** rewritten with the
  v0.8.0 build incident (`ar rcs` + GNU make `-MD` not catching
  cross-archive deps after a public-header struct restructure)
  as the concrete trigger to migrate to meson+ninja or
  cmake+ninja. Stated preference: meson.

## 0.7.2 — 2026-05-01

JSON5 reader and writer support; PCI helper expansion driven by the
delldiags do.efi cat-3 PCI subcommand family; nvstore/IPMI quality-
of-life fixes from real-hardware bring-up.

### Added

- **JSON5 reader** — opt-in via `AXL_JSON_PARSER_JSON5` flag on the
  new `axl_json_parse_flags` / `axl_json_load_file_flags`. Accepts
  the json5.org grammar superset: `//` and block comments, trailing
  commas, single-quoted strings, unquoted (identifier-name) object
  keys, hex number literals (`0x...`) and `+`/`-` number prefix,
  extended string escapes (`\'`, `\v`, `\0`, `\x##`, line
  continuations). Strict-JSON consumers (`axl_json_parse`) stay on
  jsmn unchanged. Hand-rolled, no new vendored dependencies. Token
  layout matches `jsmntok_t`, so the existing accessors
  (`axl_json_get_string`, `axl_json_array_begin/next`, ...) work
  without modification.
- **JSON5 writer** — `AXL_JSON_WRITER_TRAILING_COMMAS` flag and a new
  `axl_json_comment(w, text)` call. Pretty mode emits comments on
  their own line at the current indent; compact mode emits inline
  block comments with embedded close-comment sequences split for
  safety. Comments don't disturb the writer's container state.
  Unquoted-key emission deliberately not supported (escape-correctness
  footgun, no consumer benefit).
- **`axl_pci_addr_parse` / `axl_pci_addr_format`** — parse and format
  the canonical lower-hex `SSSS:BB:DD.F` form. Both 3-component
  (`bus:dev.func`, segment defaults to 0) and 4-component
  (`seg:bus:dev.func`) variants accepted; range-checked at parse
  time. Symmetric round-trip.
- **`axl_pci_get_vid_did` / `axl_pci_get_class24`** — boilerplate-
  killer wrappers over the standard PCI header offsets. `vid_did`
  folds the `vid == 0xFFFF` "function absent" sentinel into the
  return code. `class24` folds offsets 0x09/0x0A/0x0B into the
  canonical `(base << 16) | (sub << 8) | prog_if` form consumed by
  `axl_pci_find_by_class`.
- **`axl_pci_vpd_iter`** — full-walk callback complement to the
  existing `axl_pci_vpd_read` (which only fetches a single keyword).
  Visits both Read-Only and Read-Write resource sections; vendor-
  specific `V0..V9` and `Y0..Y9` keywords reach the callback
  alongside the standard set. Internal walker shared with
  `vpd_read` — one cap-list lookup, one tag walk, no duplicated
  logic.
- **`AxlIpmiDeviceId.firmware_minor_decoded`** — BCD-decoded
  companion to the existing raw `firmware_minor` byte. Both stay
  populated; consumers that care about BCD validity can compare
  decoded vs raw.

### Changed

- **AxlSubcommand deprecated.** The framework was superseded by
  AxlArgs in v0.7.0; deprecation warnings ride v0.7.2. Removal in
  a later release once the last out-of-tree consumer (delldiags
  do.efi) migrates.
- **`share/jedec.json` → `share/jedec.json5`.** The file uses the
  JSON5 grammar (real hex numbers `0x002C` instead of hex-encoded
  strings, unquoted keys, single-quoted names, `//` header comment
  block in place of the bogus `_comment` string field). Renamed
  so VS Code's strict-JSON validator stops flagging every JSON5
  feature; `tools/memspd.c`'s auto-discovery default updated.

### Fixed

- **`axl_nvstore_get` / `axl_nvstore_delete` log noise.**
  `EFI_BUFFER_TOO_SMALL` (canonical probe-then-grow size query) and
  `EFI_NOT_FOUND` (key-existence probe) on `get`, plus
  `EFI_NOT_FOUND` (idempotent delete) on `delete`, demoted from
  warning to debug. Both are normal control flow for callers and
  were producing visible WARN noise during `do boot show` on Dell
  hardware.
- **6-second hang on phantom KCS interface.** `axl_ipmi_kcs_open`
  now rejects a status-byte read of `0xFF` as a floating-bus /
  phantom KCS. Previously, SMBIOS Type 38 advertising a KCS that
  no real BMC answered caused the first `axl_ipmi_get_device_id`
  to spin for the full `KCS_POLL_TIMEOUT_US` (~5 s) inside
  `kcs_wait_ibf_clear` — a 6 s hang on every misadvertised
  interface. `axl_ipmi_session_new` doc updated to set the new
  "fast NULL on phantom transport" expectation.

## 0.7.1 — 2026-05-01

CI-only patch — released v0.7.0 artifacts are unaffected.

### Fixed

- **`test_rng` SKIP-path balancer count off by one.** When OVMF
  doesn't publish `EFI_RNG_PROTOCOL`, `test_rng` takes the SKIP
  path and emits balancer `test_check(true)` calls instead of
  exercising the protocol. The populated path emits 4 conditional
  checks (bytes succeeds, second fill, distinct, non-zero); the
  SKIP path was emitting only 3, leaving the count short by one
  vs the populated count. Surfaced as the v0.7.0 CI ratchet
  failure: local OVMF publishes EFI_RNG_PROTOCOL (full path,
  1905 tests) but the GitHub Actions runner's OVMF doesn't (SKIP
  path, 1904 tests). Adds the missing fourth balancer so total
  stays 1905 regardless of environment. Pre-existing class of
  bug per the cross-arch parity convention.

## 0.7.0 — 2026-05-01

Phase B3 (Platform Access) lands in full — AxlAcpi, AxlBoot, AxlPci,
AxlImage, AxlMemPhys, AxlWatchdog, AxlRng, and AxlSpd. Tools layer
gets a new declarative CLI parser (AxlArgs) and five reusable
helpers; all 11 in-tree tools migrate to the new framework, and the
dual-purpose CLI side of AxlConfig retires.

### Added

- **AxlAcpi** — ACPI table discovery via RSDP/RSDT/XSDT walk with
  cursor iteration matching `axl_smbios_find_next`. Public checksum
  verifier; typed readers for MCFG (PCIe ECAM segments), MADT (IOAPIC
  on x86, GIC regions on aa64), and FACP/FADT (SMI cmd, PM1 blocks,
  DSDT pointer). No AML interpretation. Header: `axl/axl-acpi.h`.
- **AxlBoot** — typed boot-option management. `AxlBootOption` struct
  (description / device-path text / opt_data) with
  `_option_get/_set/_delete/_free` and `_order_get/_set`.
- **AxlPci** — PCI/PCIe configuration-space access via ECAM (no
  legacy 0xCF8/CFC). Cursor iteration honours multi-function header
  bit; lookups by class24 and VID/DID; capability-list walker.
  Header: `axl/axl-pci.h`.
- **AxlImage** — load / start / unload UEFI executable images via
  EFI shell-style paths.
- **AxlMemPhys** — typed physical-memory access (read8/16/32/64,
  one-shot or mapped, byte search). Bounds-checked, never crashes
  on bad addresses.
- **AxlWatchdog** — `axl_watchdog_set / pet / disarm` wrapping
  `gBS->SetWatchdogTimer`.
- **AxlRng** — `axl_rng_bytes` over `EFI_RNG_PROTOCOL`. Returns
  -1 if the protocol isn't published.
- **AxlSpd** — DDR4/DDR5 SPD reader on AxlSmbus. Cursor iteration
  over 0x50..0x57; key byte at SPD offset 2 dispatches to the codec
  (0x0C=DDR4, 0x12=DDR5). DDR4 paging via EE1004 SPA pseudo-slaves
  (0x36/0x37); DDR5 paging via SPD5118 MR11 across eight 128-byte
  windows. Pure-decoder entry point `axl_spd_decode(buf, len, *out)`
  for offline analysis. DDR3 deliberately deferred.
- **`axl_io_port_*`** — promoted from internal backend to public.
  16/32-bit variants. Build-gated to x86 (compile error on aa64).
- **`axl_nvstore_*` extensions** — namespace registration for OEM
  variable GUIDs; `_delete`, `_iter`, `_get_attrs`. Behaviour
  change: unregistered namespace is now an error (was: silent
  fallback to global).
- **AxlSmbus byte ops** — `axl_smbus_read_byte` / `_write_byte`
  (SMBus spec §5.5.4 / §5.5.5). Required for SPD EEPROMs (24Cxx-
  style, no block framing) and SPD5118 register access.
- **Five new tool-layer helpers** — `axl_app_argv0` (new
  `<axl/axl-app.h>`), `axl_path_companion`, `axl_format_bytes`
  (IEC binary units), `axl_json_load_file`, plus the AxlHashTable
  adoption pattern in tools/memspd. Caught as gaps during the R+4
  post-commit review; `<axl/axl-spd.h>` was also missing from the
  umbrella `<axl.h>` and is now included.
- **AxlArgs** — new declarative CLI parser (`<axl/axl-args.h>`).
  Tools declare an `AxlArgsApp` tree (name, global flags, verbs,
  per-verb flags, typed positionals) and call `axl_args_run` from
  main; the framework parses argv, validates types and bounds,
  generates `--help`, and dispatches to a verb handler. Supports
  single-handler and multi-verb modes. Flag types: BOOL, STRING,
  U8/U16/U32/U64 with optional [min,max], S64 (delegates to
  `axl_str_to_s64`), MULTI (repeatable). Auto `--help`/`-h`.
  Compact short-flag groups (`-vh`) explicitly rejected; extra
  positionals past a fully-filled list rejected. Lifetime contract
  documented for AxlLoop interop.
- **`tools/memspd`** — `decode-dimms`-equivalent. Verbs `list /
  show <slot> / decode <slot>`. Vendor lookup is data-driven via
  `share/jedec.json` (15-vendor stub); auto-discovered next to the
  `.efi` or via `--jedec-file`. Missing sidecar prints raw 16-bit
  hex codes.
- **`scripts/qemu-patches/0001-smbus-eeprom-add-memdev-link.patch`**
  — adds a `memdev=<link<memory-backend>>` property to QEMU's
  `smbus-eeprom` device. Models on `pc-dimm`'s `memdev=` link.
  Unblocks `test/integration/test-spd-qemu.sh` end-to-end coverage
  of the AxlSpd wire path. Candidate for upstreaming.
- **`test/integration/test-spd-qemu.sh`** — wire-path AxlSpd test
  through the patched QEMU + SmbusHcShim chain.
- **`test/integration/test-net-tools.sh`** — closes the
  `test-tools.sh` coverage gap for the network-using tools (`fetch`,
  `netinfo`). All 11 in-tree tools now have direct tool-binary
  integration coverage.
- **`docs/AXL-Hardware-Fixture-Design.md`** + ROADMAP HF1–HF6 —
  proposal for vendor-neutral capture-and-replay of UEFI platform
  identity (SMBIOS, ACPI, PCI manifest, IPMI, Redfish) so axl-sdk
  tools can be exercised against real-world platforms under QEMU
  without lab access. No code yet; planning artifact.

### Changed

- **All 11 tools migrated to AxlArgs**: `memspd`, `dmidecode`,
  `sysinfo`, `fetch`, `grep`, `find`, `hexdump`, `ipmi`, `netinfo`,
  `mkrd`, `rfbrowse`. Per-tool boilerplate (~12–25 lines of
  AxlConfig + manual dispatch) collapses to ~5–10 lines of
  declarative verb tree. Consistent `--help` format across the
  toolchain; typed positional-arg validation for free.
- **SmbusHcShim** extended with `smb_byte_read` / `smb_byte_write`
  (ICH9 SMBus PROT_BYTE_DATA). Required for AxlSpd over the I2C
  Master path on QEMU.
- **`axl_smbus_write_block`** now rejects `len == 0` (was a hole
  the I2C path could route as a byte write under the new shim).

### Removed

- **AxlConfig CLI surface** — `axl_config_parse_args`,
  `axl_config_pos`, `axl_config_pos_count`, `axl_config_usage`,
  and the `short_flag` field in `AxlConfigDesc`. AxlConfig now
  has a focused purpose: live object property bag with typed
  accessors, auto-apply via offsetof, and dynamic-key callbacks
  (used by AxlHttpClient/Server). The dual-purpose "config or CLI
  parser?" confusion is gone. External consumer migration:
  `aximcode/axl-webfs` cmd-serve + cmd-mount — committed in that
  repository separately.
- `sdk/examples/config-demo.c` — only demonstrated the retired
  CLI surface. Removed.

### Fixed

- **AxlSmbus I2C path** zero-length block-write was misroutable as
  byte-write under SmbusHcShim's byte-op extension; now rejected
  at the public API layer.
- **DDR5 SPD** decoder was masking the ECC-presence bit with
  `& 0x07`, allowing dual-channel non-ECC modules' channel-count
  bits to spoof ECC=true. Fixed to `& 0x03` (caught by independent
  code-review pass before AxlSpd commit).
- **AxlArgs S64** initial impl reproduced the v0.5.0 INT64_MIN
  signed-overflow UB by hand-rolling negate-after-strip-minus.
  Replaced with `axl_str_to_s64` delegation (no live exposure —
  no tool currently uses S64 — but the bug was in new code).
- Several minor doc / lifetime-contract clarifications to the new
  helpers and AxlArgs surface, all caught by the independent
  review passes.

### Test ratchet

1842 → 1905 on both x86_64 and AArch64 (+63 across R+1..R+4 + the
tool-helpers + AxlArgs framework + offsetting -13 retired
test_config_args). All 11 tools have direct integration coverage
(`test-tools.sh`, `test-net-tools.sh`, `test-redfish.sh`,
`test-spd-qemu.sh`, `test-ipmi-{qemu,ssif-qemu}.sh`).

## 0.6.1 — 2026-04-30

### Added

- **`axl-sdk-host-tools.rpm`** — RPM artifact for the host-tools
  package (run-qemu.sh + helpers). Was a v0.2.9 oversight; the SDK
  package always shipped both `.deb` and `.rpm` but host-tools only
  shipped `.deb` and `.tar.gz`. Now `axl-sdk-host-tools.rpm` is on
  the release page next to its `.deb` counterpart, with stable URL
  `…/releases/latest/download/axl-sdk-host-tools.rpm`. Both
  packages are noarch (the host-tools payload is shell + python
  scripts + linker scripts + C source, no pre-compiled binaries).
  Fedora/RHEL deps tracked separately from Debian: `qemu-system-x86`,
  `qemu-system-aarch64`, `edk2-ovmf`, `edk2-aarch64`, `virtiofsd`,
  `mtools`, `dosfstools`.

## 0.6.0 — 2026-04-30

Network tools (`netinfo`, `fetch`, `rfbrowse`) now Just Work on
minimal firmware that doesn't ship a NIC SNP driver — the tools
tarball ships a universal NIC driver bundle and the SDK auto-loads
it on demand. Motivating real-user case: a 2010-era Dell EDK1
firmware that lacks UEFI 2.6+ NIC drivers entirely.

See [`docs/AXL-Network-Driver-Bundle-Design.md`](docs/AXL-Network-Driver-Bundle-Design.md)
for the full design + four-test validation matrix.

### Added

- **`drivers/<arch>/` directory in `axl-sdk-tools-{x64,aa64}.tar.gz`** —
  a self-contained driver bundle that consumers extract alongside
  the tools `.efi` files onto a FAT USB stick:
  - `ipxe-all.efidrv` — universal NIC driver built from upstream
    iPXE at a pinned commit (currently `df4eec8c`). Single ~1.1 MB
    blob covering Intel (e1000 / e1000e / i219 / i225), Broadcom
    (BCM4401 / 5760x / 957454), Realtek PCI/USB (RTL8139 / 8169 /
    8125 / 8153 USB), Atheros, 3Com, AMD, USB CDC-ECM/NCM/RNDIS,
    AX88179/178a, SMSC75xx/95xx — ~2.9k chip IDs total. Built
    fresh in CI by [`scripts/build-ipxe.sh`](scripts/build-ipxe.sh);
    GPL-2.0-or-later, attribution + GPL §3(b) written offer in
    `third_party/ipxe/README.md`.
  - `RamDiskDxe.efi` (also embedded in `mkrd.efi` since v0.5.2).
  - A few small auxiliary EDK2 USB-network drivers for firmware
    that benefits from them. See `third_party/edk2/README.md` for
    details.
- **[`scripts/build-ipxe.sh`](scripts/build-ipxe.sh)** — clones iPXE
  at the pinned commit and builds the universal `.efidrv` for one
  or both architectures. Reproducible build (~35s on 16 threads);
  prints upstream URL+SHA at the end for source-availability
  documentation.
- **[`scripts/run-qemu.sh`](scripts/run-qemu.sh) flags** for testing
  driver-bundle coverage:
  - `--nic-model MODEL` — choose the QEMU NIC type (virtio-net-pci,
    e1000, e1000e, rtl8139, pcnet, ne2k_pci, ...). Implies `--net`.
  - `--nic-no-rom` — suppress QEMU's bundled iPXE PXE option ROM
    (passes `romfile=`). Required for "firmware lacks NIC driver"
    tests; without it OVMF wraps the option-ROM-provided UNDI as
    SNP and hides the gap.
  - `--extra SRC:DEST` — relative-path staging of additional files
    into the boot disk image (was previously root-only). Lets tests
    drop drivers under `drivers/<arch>/...` to exercise the
    canonical search path.
- **`netinfo -v`** prints a "NIC Drivers" section identifying the
  driver image bound to each SNP handle, walking the
  `NII3.1 → NII (legacy) → SNP` fallback chain. Surfaces the
  actual NIC-binding driver (e.g.
  `\drivers\x64\ipxe-all.efidrv`) instead of just the SnpDxe
  wrapper above it.

### Fixed

- **`axl_driver_load(path)` now uses DevicePath, not memory buffer.**
  The previous implementation read the .efi file into memory and
  called `gBS->LoadImage(SourceBuffer=...)`, which leaves
  `LoadedImage->FilePath = NULL`. iPXE's UEFI driver-binding entry
  reads `FilePath` to locate its install directory and bails with
  `EFI_INVALID_PARAMETER` from `StartImage` when it's NULL.
  `axl_driver_load` now constructs a `<volume DP> +
  MEDIA_FILEPATH_DP` device path (via the new
  `driver_build_file_dp` helper) and calls
  `LoadImage(DevicePath=that, SourceBuffer=NULL)` — matching what
  UEFI Shell's `load` command does. Memory-buffer load is preserved
  as a fallback for drivers that don't read `FilePath` (e.g.
  `RamDiskDxe`).
- **`axl_driver_connect(NULL)` actually does something now.**
  UEFI's `gBS->ConnectController(NULL, NULL, NULL, TRUE)` returns
  `EFI_INVALID_PARAMETER` per spec; the old implementation called
  it directly and silently succeeded as a no-op. Now enumerates
  every handle via `LocateHandleBuffer(AllHandles)` and per-handle
  `ConnectController`, mirroring UEFI Shell's `connect -r`.
- **`axl_net_ensure_drivers` candidate-list reordered** so
  `ipxe-all.efidrv` is tried first; existing names retained as
  back-compat for users with their own staged drivers.

### Validated

- 1695/1695 unit tests pass on x64 and aa64 (DEBUG build).
- 23/23 tool integration tests pass.
- End-to-end: `--nic-model {e1000, e1000e, rtl8139, pcnet}
  --nic-no-rom` (firmware lacks NIC driver) → tarball-staged
  `ipxe-all.efidrv` self-loads via `axl_net_ensure_drivers` →
  iPXE binds NII → SnpDxe wraps as SNP → DHCP → HTTP 200.
  Consumer-flow proven: extract release tarball, run
  `fetch.efi http://...`, no env overrides, leak-clean exit.

## 0.5.3 — 2026-04-29

Fixes a regression introduced in v0.2.9 where `--mount` (and other
features that depend on QEMU-bundled firmware) silently fell through
to system OVMF/AAVMF on hosts that have both a system QEMU and a
custom QEMU build with bundled firmware.

### Fixed

- **`QEMU_DIR` not propagating from `find_qemu` to `find_firmware`** —
  v0.2.9 (commit `ae3c3a6`) removed the top-level
  `QEMU_DIR="${QEMU_DIR:-$HOME/projects/qemu/install/bin}"` default
  in `scripts/axl-common.sh` and moved that resolution inside
  `find_qemu()`. But `find_qemu` is called via `QEMU_BIN=$(find_qemu
  "$ARCH")` — the `$()` runs it in a subshell, and its
  `export QEMU_DIR=...` dies with the subshell. `find_firmware` in
  the parent then sees `QEMU_DIR=""`, computes
  `qemu_share="/share/qemu"`, fails the QEMU-bundled firmware lookup,
  and falls through to system OVMF/AAVMF. Symptoms: `--mount`
  produces no `fs1:` (system OVMF lacks `VirtioFsDxe`); aa64 unit
  tests silently produce 0 results when the system aa64 QEMU lacks
  slirp networking. Fix: re-derive and export `QEMU_DIR` in the
  parent shell after the `$()` call in
  [`scripts/run-qemu.sh`](scripts/run-qemu.sh) and
  [`test/integration/common-test.sh`](test/integration/common-test.sh).
  Idempotent on user-pre-set `QEMU_DIR`. Two call sites; no other
  `$(find_qemu)` invocations exist in the tree.

## 0.5.2 — 2026-04-29

`mkrd.efi` now ships as a self-contained binary that runs on minimal
firmware that omits the optional UEFI 2.6 `EFI_RAM_DISK_PROTOCOL`
(observed in the wild on a 2010-era Dell EDK1 firmware). Stock EDK2
`RamDiskDxe.efi` is embedded at build time and `LoadImage`-from-memory'd
at runtime if `LocateProtocol` and the on-disk search both miss.

### Added

- **`axl_driver_ensure_with_embedded()`** — generalization of
  `axl_driver_ensure()` for tools that bake a driver blob into their
  own `.efi`. Resolution order: `LocateProtocol` short-circuit →
  `--driver`-style override (caller-provided name, disk-only) →
  canonical disk search → `LoadImage` from embedded buffer. The old
  `axl_driver_ensure(g, n)` is now a thin wrapper passing
  `embedded_buf=NULL, override_name=NULL`. See
  [`include/axl/axl-driver.h`](include/axl/axl-driver.h).
- **`mkrd --driver <name>`** — override the embedded RamDiskDxe and
  search disk for the named driver instead. When this flag is set,
  the embedded fallback is intentionally disabled — caller explicitly
  opted into a specific external driver. Useful for testing patched
  or vendor-specific RAM-disk drivers.

### Changed

- **`mkrd.efi` is now self-contained.** Embedded
  [`third_party/edk2/RamDiskDxe-{x64,aa64}.efi`](third_party/edk2/)
  (BSD-2-Clause-Patent, stock EDK2 `MdeModulePkg`) — adds ~33 KB
  to the x64 binary and ~41 KB to aa64. Goes into `.rodata`. Most
  OEM firmware ships the protocol baked-in so the embedded blob is
  never executed; it's purely a safety net for minimal firmware.
- **Tools tarballs and SDK packages carry the new
  `third_party/edk2/{LICENSE,README.md}`** — attribution is required
  for binary-form redistribution under BSD-2-Clause-Patent §2.
  Apache-2.0 §4(a) compliance for the SDK package preserved.

## 0.5.1 — 2026-04-29

Cross-compile fix for installed packages on RHEL-family hosts.

### Fixed

- **aa64 cross-build under installed `axl-cc` on AlmaLinux/RHEL** —
  the v0.5.0 package staged headers directly under `<prefix>/include/`,
  so on a system install (`PREFIX=/usr`) `axl-cc` compiled with
  `-I /usr/include`. Host glibc's `stdint.h` then shadowed cross-gcc's
  freestanding `stdint.h`, chaining into `gnu/stubs-32.h`, which
  RHEL/Alma don't ship — failing every `--arch aa64` build with
  "gnu/stubs-32.h: No such file or directory". Ubuntu only masked
  this because `gcc-multilib` happens to ship the 32-bit stubs.

### Changed

- **Installed header layout namespaced under `axl-sdk/`** —
  package now stages headers at `<prefix>/include/axl-sdk/{axl.h,
  axl/, uefi/}` instead of `<prefix>/include/`. `axl-cc`,
  `axl-config.cmake`, and `axl.pc` all point at the namespaced
  subdirectory, so `/usr/include` itself never appears in the
  compiler command line and cross-gcc's freestanding headers win
  via the normal built-in search order. User code (`#include <axl.h>`,
  `#include <axl/axl-mem.h>`) is unchanged. Dev-tree library build
  unchanged — the namespacing is install-time only.
- **`-I` → `-isystem` for the SDK include path** — secondary
  benefit: SDK headers are now treated as system headers, so
  warnings from them don't bubble up into user-app builds.

## 0.5.0 — 2026-04-29

String parsing companion to `AxlString` (the builder). Closes the
"every consumer hand-rolls the same length-guard plumbing" gap one
level up from the v0.4.0 SMBIOS spec-table decoders.

### Added

- **`AxlStrReader`** — cursor-based string parser. Borrows a
  `const char *`, tracks a position + sticky-error flag. Operations
  short-circuit when `ok` is false, so parse chains compose without
  per-call error checking:

  ```c
  AxlStrReader r;
  uint64_t v;
  axl_str_reader_init(&r, "N[03A8]");
  axl_str_reader_consume_char(&r, 'N');
  axl_str_reader_consume_char(&r, '[');
  axl_str_reader_take_u64(&r, 16, &v);
  axl_str_reader_consume_char(&r, ']');
  if (!r.ok || !axl_str_reader_eof(&r)) { /* parse failed */ }
  ```

  Twelve primitives covering init, eof/peek/remaining,
  skip-whitespace, consume-char/literal-string, take-until-delim,
  take-while-pred, take-u64 (auto-detects `0x` prefix or takes an
  explicit base), and take-ident (`[A-Za-z_][A-Za-z0-9_]*`). No
  allocation. Header docs live alongside `AxlString` in
  `axl-str.h`.

- **`axl_sscanf` / `axl_vsscanf`** — printf's symmetric partner,
  built on `AxlStrReader`. Supports a useful subset of C99 sscanf:
  `%c %d %i %u %o %x %X %s (with width) %[set] %% %n`, length
  modifiers `hh h l ll z j`, `*` assignment suppression, and width
  specifiers. Width is required for unsuppressed `%s` so the
  destination buffer is bounded. Returns the count of stored
  conversions or -1 on a malformed format.

  ```c
  unsigned a, b, c, d;
  int n = axl_sscanf("192.168.1.42", "%u.%u.%u.%u", &a, &b, &c, &d);
  /* n == 4 */
  ```

### Changed (dogfood)

- **`axl_strtou64_with_offset`** rewritten on top of `AxlStrReader`.
  Behavior unchanged — all 23 existing test cases still pass — but
  the implementation is now ~14 lines of cursor calls vs. the
  previous 40 lines of hand-rolled `endptr` plumbing.

- **`axl_ipv4_parse`** rewritten on top of `axl_sscanf`. Replaces a
  ~30-line hand-rolled state machine (digit accumulation, dot/NUL
  switching, octet count tracking) with a 4-line scanf call plus
  range checks. The trailing `%n` captures bytes consumed so we
  reject trailing garbage like "1.2.3.4junk" without an extra
  strlen.

  ```c
  unsigned int a, b, c, d;
  int consumed;
  int n = axl_sscanf(str, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed);
  if (n != 4 || str[consumed] != '\0') return -1;
  if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
  ```

### Test stats

1693 unit tests passing (was 1607; 86 new across `AxlStrReader`
and `axl_sscanf`, plus 0 regressions on the dogfooded
consumers).

## 0.4.0 — 2026-04-29

Second consumer-driven release for delldiags `do.efi` (cat 1 / 1.5 SMBIOS
landed; this round is decoder-table consolidation). Pulls four pieces
of SMBIOS spec-table machinery upstream so future spec-fixes propagate
to every consumer via an SDK bump rather than a manual backport. The
motivating fixes are the recent ones in dowin's `fSlotType`
(`Init.cpp:3395`):
- `0c558a930` (2024-09-25) — OCP NIC SFF/LFF + EDSFF E1.S/E1.L + E3.S/E3.L
- `aab01c48d` / `569491b6c` (2025-03-27) — SMBIOS slot type 0x25 is Gen 5,
  not Gen 4

### Added

- **`AxlSmbiosBaseboardInfo.board_type` field** + `AXL_SMBIOS_BOARD_TYPE_*`
  enum — exposes the BoardType byte at offset 0x0D of Type 2. The
  canonical "is this a server blade?" detector is `board_type == 3`,
  NOT Type 3 chassis 0x1C/0x1D (which Dell BIOS doesn't reliably set
  — see `ADDF/Libs/SAL/AddfSAL.cpp:fIsBladeSmbios`). 0 if not
  published (rare; field has been part of Type 2 since spec 2.0).
- **`axl_smbios_slot_type_str` / `axl_smbios_slot_width_str` /
  `axl_smbios_slot_usage_str`** — Type 9 spec-value decoders. Pure
  table lookups, no allocation, return a static const string or
  NULL for unknowns (caller can fall back to raw "0x%02X").
  Values match SMBIOS 3.7 spec / EDK2's `MISC_SLOT_TYPE` enum.
  `slot_type_str` covers PCIe Gen 1..6 (`0xA5`-`0xC4`), M.2 Keys
  A/E/B/M (`0x14`-`0x17`), PCIe Mini variants (`0x21`-`0x23`),
  PCIe SFF-8639 / U.2 family Gen 2-5 (`0x1F`/`0x20`/`0x24`/`0x25`),
  OCP NIC 3.0 SFF/LFF / Prior to 3.0 (`0x26`-`0x28`), and EDSFF
  E1/E3 form factors (`0xC5`/`0xC6`). `slot_usage_str` renders
  0x05 as "CPU NOT INSTALLED" — Dell convention from
  `dowin/Init.cpp:3833` + `SmBioslib.h:391`, what every consumer's
  scripts grep for. **Note**: an earlier draft drew from
  delldiags' `axl-utils/do/cmd_bios.c` table, which had values
  shifted by 4 from the spec (PCIe at `0xA1` instead of `0xA5`)
  and labeled PCIe-Mini / U.2 codes (`0x22`-`0x25`) as M.2 keys.
  This release uses the canonical spec values; downstream
  consumers that switch from a local decoder to
  `axl_smbios_slot_type_str` will see corrected decoding for any
  slot whose firmware reports a value the local table got wrong.
- **`axl_smbios_strings_byte_len(hdr)`** — byte length of the
  inline strings region between the formatted area and the spec's
  end-of-region double-NUL. Useful for "raw record" dumps that
  need to know the full record span on disk including its strings
  (e.g. dowin's `fDumpSmbios` PIMS-381674 fix). Bounded against
  the SMBIOS table memory range so a malformed record without a
  terminator can't run past the end.
- **`AxlSmbiosChassisClass` enum + `axl_smbios_chassis_class(type)`** —
  pure-spec interpretation of the Type 3 chassis byte into
  `DESKTOP` / `NOTEBOOK` / `SERVER` / `EMBEDDED` / `OTHER` /
  `UNKNOWN` buckets. Strips the 0x80 lock bit before classifying.
  Vendor-specific overrides (sysId tables, PCI audio-device
  probes, etc.) live in consumer code. Bucket assignments
  match SMBIOS spec Table 17 +
  `ADDF/Libs/SAL/AddfSAL.cpp:fIsNotebookSmbios`. Pitfalls
  defended in tests:
    - 0x18 ("Sealed-case PC") is **DESKTOP**, not server.
    - 0x23 (Dell convention "Mongoose Mini PC") is **EMBEDDED**.

### Test stats

1598 unit tests passing (was 1555; 43 new across the four
additions, including explicit pitfall coverage for the 0x18 and
0x23 cases the consumer flagged).

## 0.3.1 — 2026-04-29

CI fix on top of v0.3.0 (cut the same day). v0.3.0's release artifacts
are functional — the bug only triggers on `argv == NULL`, which no
real caller does — but the CI green-bar matters for consumers
pinning against a tag.

### Fixed

- **`axl_subcommand_dispatch` clang-tidy null-deref** —
  the early-help check `if (argc < 2 || argv[1] == NULL)` would
  dereference a NULL `argv` when called with `argc >= 2 && argv == NULL`
  (a technically-valid but unlikely shape that test harnesses
  occasionally exercise). Add an explicit `argv == NULL` guard
  short-circuiting the array access. CI was running clang-tidy
  with `-warnings-as-errors='*'`.

## 0.3.0 — 2026-04-29

Consumer-driven release for delldiags `do.efi` (the hardware-diagnostic
CLI port from `efiUtils/doDriver/`). Closes the SMBIOS audit gap on
Types 8/9/11/16/19/20/41, adds a multi-command CLI dispatch helper,
and standardizes the hex+offset parser delldiags categories 3-5 share.

### Added

- **Seven new typed SMBIOS readers** matching the existing pattern
  (`axl_smbios_read_*` returns 0 on success, -1 on NULL/wrong-type/
  too-short record):
    - `axl_smbios_read_port_connector` (Type 8)
    - `axl_smbios_read_system_slot` (Type 9) — length-aware across
      spec-version creep: SegmentGroup/Bus/DeviceFunc (2.6+),
      DataBusWidthBase + PeerGroupingCount (3.2+), all with documented
      `0xFFFF` / `0xFF` / `0` "not published" sentinels
    - `axl_smbios_read_oem_strings` (Type 11) — array-of-pointers
      shape capped at 16 entries (matches existing typed-reader idiom)
    - `axl_smbios_read_physical_memory_array` (Type 16) — resolves
      the 32→64-bit `MaxCapacity` / `ExtendedMaxCapacity` fallback
      automatically
    - `axl_smbios_read_memory_array_map` (Type 19) — same 64-bit
      address fallback story
    - `axl_smbios_read_memory_device_map` (Type 20) — same again
    - `axl_smbios_read_onboard_device_ext` (Type 41)
- **`AXL_SMBIOS_TYPE_*` enum additions** — Type 20
  (`MEMORY_DEVICE_MAP`), Type 41 (`ONBOARD_DEVICE_EXT`), plus a
  `PHYSICAL_MEMORY_ARRAY` alias for the existing Type-16 enum so
  callers can use the spec's full name.
- **`axl_smbios_copy_string_utf8(hdr, idx, buf, buf_size)`** —
  reentrant, length-bounded alternative to
  `axl_smbios_get_string_utf8`. Truncates safely on overflow,
  always NUL-terminates if `buf_size > 0`, returns the byte count
  written. The original `axl_smbios_get_string_utf8` was already
  reentrant after the v0.2.5 string-area refactor — its docstring
  is updated to match.
- **`<axl/axl-subcommand.h>`** — multi-command CLI dispatch helper.
  Caller-owned `AxlSubcommand` table + a single
  `axl_subcommand_dispatch(table, count, argc, argv, prog_name)`
  call covers `<prog>`, `<prog> help`, `<prog> help <cmd>`,
  `<prog> -h`/`--help`, `<prog> <cmd> ...`, "did you mean" typo
  suggestions via edit-distance matching, and shifted argv so
  subcommands see their own name as `argv[0]`. Pairs with
  `axl_config_*` for per-command flag parsing — `mkrd` is the
  "single-purpose" pattern, `do` will be the "multi-command"
  pattern.
- **`axl_strtou64_with_offset(s, &out)`** — hex/decimal value with
  an optional `+offset` suffix. Accepts `"0x100"`, `"256"`,
  `"0x100+0x10"`, `"256+16"`. Strict: trailing garbage, whitespace
  around `+`, dangling `+`, and overflow on the sum all return -1.
  Standardizes the `do crb tag+offset reg` / `do rb physAddr+offset
  count` parsing across delldiags categories 3-5.

### Changed

- `axl_smbios_get_string_utf8` documentation now correctly describes
  it as reentrant. (The v0.2.5 refactor switched it from a static
  buffer to a direct pointer into the SMBIOS table — the docstring
  hadn't caught up.)
- `src/smbios/README.md` rewritten: added the typed-reader table,
  split the string-accessor section into "direct pointer vs
  caller-buffer" guidance, removed the stale "static 128-byte
  buffer" / "future axl_smbios_next" notes that the v0.2.5
  refactor and earlier releases had already obsoleted.
- `docs/AXL-Coding-Style.md` adds a "CLI Patterns" section
  documenting single-purpose vs multi-command tool shapes
  (`mkrd` vs `do.efi`).

### Test stats

1552 unit tests passing (was 1488; 64 new across SMBIOS, subcommand,
and `strtou64_with_offset`).

## 0.2.9 — 2026-04-28

Downstream-consumer release. The motivating case is delldiags
axl-utils, which needs `run-qemu.sh` and the host-side helpers
on machines that have system QEMU/OVMF installed via the package
manager but can't `git clone` the SDK source (corporate MITM
proxies break TLS verification on `git clone`; a pinned
`curl` + `sha256sum` doesn't). Four changes:

### Added

- **`axl-sdk-host-tools.tar.gz` and `axl-sdk-host-tools.deb`
  release artifacts** — flat tarball plus an installable .deb
  carrying just the host-side runtime tooling: `run-qemu.sh`,
  `axl-common.sh`, the ELF/PE linker scripts, `gdb-syms.py`,
  `pe-set-debug.c`, `rsod-decode.py`, and `uefi-manifest.json5`.
  The .deb declares `Depends: qemu-system-x86 qemu-system-arm
  ovmf qemu-efi-aarch64 virtiofsd mtools dosfstools` so a
  single `sudo apt install ./axl-sdk-host-tools.deb` brings the
  whole pipeline. Drops scripts to `/usr/share/axl-sdk-host-tools/scripts/`
  with an `/usr/bin/run-qemu` wrapper on PATH. The tarball
  ships scripts at `scripts/` for unprivileged extraction
  anywhere. CI smoke-tests the .deb install end-to-end.

### Changed

- **`scripts/axl-common.sh` discovery is now a 3-tier search.**
  Previously `QEMU_DIR` defaulted to `$HOME/projects/qemu/install/bin`,
  which broke for downstream consumers without that custom
  build tree. New order: (1) explicit `$QEMU_DIR` override,
  (2) `command -v qemu-system-*` on `$PATH` (system install),
  (3) the legacy `$HOME/projects/qemu/install/bin` path as
  last-resort fallback. `MKIMAGE_DIR` lost its default
  similarly — left unset means the script falls through to
  the mtools recipe (already the working default for
  consumers without the AXL mkimage tree). Power-user
  override semantics preserved.

- **Actionable error messages from `find_qemu` / `find_firmware`.**
  Missing dependencies now print the apt/dnf/pacman/brew
  install one-liners users can copy-paste, plus the env-var
  override hint, instead of a one-line "not found" log:

  ```
  [ERROR] qemu-system-x86_64 not found in $PATH or any known location.

    Install:
      Debian/Ubuntu:  sudo apt install qemu-system-x86 qemu-system-arm \
                                       ovmf qemu-efi-aarch64 \
                                       virtiofsd mtools dosfstools
      Fedora/RHEL:    sudo dnf install qemu-system-x86 ...
      Arch:           sudo pacman -S qemu-system-x86 ...
      macOS:          brew install qemu

    Or set QEMU_DIR=/path/to/your/qemu/install/bin
  ```

  Same shape for missing OVMF / AAVMF.

- **README install path no longer leads with `git clone`.** The
  binary `.deb`/`.rpm` is now the recommended consumer path;
  source builds are documented as the power-user option further
  down. Adds the new host-tools tarball/.deb to the install
  matrix.

## 0.2.8 — 2026-04-28

Two `scripts/run-qemu.sh` UX features for interactive UEFI app
testing: `-i`/`--interactive` to hand the host TTY to the guest,
and `--mount DIR` to expose a host directory as a virtiofs volume
the UEFI shell sees as `fsN:`. Together they enable a
"`./scripts/run-qemu.sh -i --mount ~/efi-apps`" loop where you
build apps on the host and run them straight from the UEFI shell
without rebuilding the disk image each iteration.

### Added

- **`run-qemu.sh --interactive` (`-i`)** — hands the host TTY to
  QEMU so keystrokes reach the guest. Disables the timeout, the
  CPU-spike sampler, and the ANSI-stripping post-filter (the guest
  may legitimately emit cursor moves and colors). Mutually
  exclusive with `--background` and `--screenshot`; composes
  cleanly with `--gdb` (separate TCP port) and with
  `--serial-log` (transcript captured via QEMU's chardev
  `logfile=`). Auto-prints the Ctrl-A C / Ctrl-A X escape hints
  on startup and installs an `EXIT`/`INT`/`TERM` trap that runs
  `stty sane` so an abnormal QEMU exit doesn't leave the parent
  shell in raw mode. Also skips the auto-`reset -s` in the
  generated `startup.nsh` so users land back at the UEFI shell
  after the app exits.
- **Bare-shell mode** — `run-qemu.sh -i` with no `.efi` argument
  boots OVMF straight into the UEFI Shell. Pairs with `--mount`
  for a "drop me into a UEFI shell with my host filesystem"
  workflow.
- **`run-qemu.sh --mount DIR[:TAG]`** — exposes a host directory
  to the guest as a virtiofs volume (default tag `hostfs`).
  Spawns `virtiofsd` as a child, wires the QEMU `vhost-user-fs-pci`
  device with a `memory-backend-file,share=on` over `/dev/shm`,
  and adds `map -r` to `startup.nsh` so the volume shows up as
  `fsN:` from the UEFI shell. Validates virtiofsd availability
  and `/dev/shm` writability up-front with actionable error
  messages (Fedora/Debian/Arch install hints; `VIRTIOFSD=` env
  override). Opportunistically stages a sibling
  `VirtioFsDxe.efi` from the EDK2 build alongside the firmware
  if found — works against OVMF builds that don't have the
  driver baked in. Composes with `--interactive`, `--gdb`,
  `--background`, and an `.efi` arg (host-fs available alongside
  the staged FAT). Trap kills virtiofsd on exit (foreground)
  or emits `VIRTIOFSD_PID=` for the caller (background). Both
  X64 and AARCH64 supported.
- **`test/integration/test-run-qemu-flags.sh`** — host-only
  argument-parsing tests for `run-qemu.sh` (syntax, `--help`,
  mutual-exclusion guards, missing-file guard, `--mount`
  validation).
- **Alternate-screen buffer in `--interactive`** — the script
  now emits `\e[?1049h` on stderr before exec'ing QEMU and
  `\e[?1049l` on exit. UEFI's Terminal driver uses absolute
  cursor positioning against an assumed 80x25 grid; against
  larger SSH PTYs (MacBook Terminal/iTerm/WSL) it produced
  artifacts where the shell prompt rendered mid-`ls` output.
  Alt-screen gives UEFI a fresh canvas with no scrollback
  interaction (same trick vim/less/htop use). Skipped when
  stderr isn't a TTY so escape sequences don't pollute log
  files. Pairs with the `mode <stty cols> <stty rows>` line
  the script already emits in startup.nsh.

## 0.2.7 — 2026-04-28

Polish release on top of 0.2.6 (cut earlier the same day). Two
real changes — a CI-side test fix and a new diagnostic in
`run-qemu.sh` — alongside a substantial documentation refresh
that came out of reviewing the CRT0/runtime conflation in the
docs corpus.

### Added

- **CPU-spike sidecar in `run-qemu.sh`** — samples QEMU's host
  CPU at 5 Hz after a firmware-boot warm-up window and prints a
  `WARN: CPU spike` line on stderr if the process sustains
  ≥1.5 cores for ≥2 s. Default-on, silent on healthy runs.
  Catches the orphan-QEMU-pegging-100%-CPU class of regression
  that bit us mid-session in 0.2.6's prep. Tunable via
  `--cpu-threshold N` / `--cpu-sustain SECS`; opt out with
  `--no-cpu-warn`. CI now smoke-tests it against
  `AxlTestCpuIdle.efi` to catch false-positives.
- **`docs/RELEASING.md`** — step-by-step release-cutting
  playbook (prereqs, `bump-version.sh`, CHANGELOG dating,
  branch-then-tag push order, watching workflows via
  `gh run watch`, recovery from a failed tag). Also surfaces in
  the Sphinx Guides toctree.

### Fixed

- **`/ttl-short` test was racing against UEFI's 1-second clock
  granularity.** The async-close speedup in 0.2.6 made both
  `/ttl-short` requests land in the same wall-clock second, so
  `now - timestamp_ms = 0` and the cache-hit path took it
  before the 150 ms TTL could expire. Bump TTL to 1500 ms and
  the inter-request sleep to 2 s so the test exercises a real
  expiry; comment on the constraint in the test source so the
  next person who tries to tune it down sees the reason.

### Documentation

- **Renamed `AXL-Runtime.md` → `AXL-Lifecycle.md`** and swept
  the corpus for the CRT0-vs-runtime conflation. CRT0 is the
  ~17-line entry stub; the AXL runtime is the lifecycle library
  in `src/runtime/`. The rename + sweep covers ~30 sites
  (design docs, module READMEs, header doc-comments, examples,
  ROADMAP, sphinx pages). The `git mv` keeps blame.
- **Audience framing for Linux systems C developers** — README,
  `AXL-Design.md`, `AXL-SDK-Design.md`, `AXL-Coding-Style.md`,
  and the `axl.h` / `axl-loop.h` umbrella headers now lead with
  the audience AXL targets (glibc / GLib / systemd / libcurl
  developers) rather than the inspiration. Includes an explicit
  GLib-to-AXL mapping table (`GMainLoop` → `AxlLoop`,
  `GHashTable` → `AxlHashTable`, etc.).
- **"How AXL avoids the EDK2 dependency"** — README and design
  docs now explain that `EFI_*` types are auto-generated from
  the published UEFI 2.x and PI 1.x specifications via
  `scripts/generate-uefi-headers.py` + `uefi-manifest.json5`.
  Spec drift is a manifest update + regeneration, not a vendor
  merge. Replaces the stale "EDK2 is invisible" phrasing that
  implied EDK2 was hidden rather than absent.
- **`AXL-Porting-Guide.md` picks up the v0.2.6 surface** —
  modern entry-point pattern (`int main(argc, argv)` with no
  `AXL_APP` macro), a new Step 1.5 covering `axl_net_auto_init`
  / `axl_driver_ensure` / `axl_tcp_close` lifetime for porters
  with networking or driver-needing apps, and the
  `run-qemu.sh --net --hostfwd` shape for testing. Adds seven
  EDK2→AXL mappings to the "What AXL Already Covers" table
  (net auto-init, driver self-load, signal, atexit, default
  loop, yield).
- **Sphinx version auto-syncs from `VERSION`.** `conf.py` reads
  the canonical file at build time; `index.rst` uses the
  `|release|` substitution. axl.aximcode.com's "Version" cell
  now updates automatically on every tagged release.
- **Sphinx full-width override** widens `.wy-nav-content` to
  1200 px so the docs site no longer leaves grey gutters on
  wide monitors.

### Changed

- **`run-qemu.sh` documents `--no-cpu-warn` / `--cpu-threshold`
  / `--cpu-sustain` flags** in `--help` and the file header.

## 0.2.6 — 2026-04-28

Networking-focused release. Headline is a TCP-close lifecycle rework
(heap-owned close token + async-finalize on the running loop) that
fixed three classes of bug at once: a UAF in `axl_tcp_close` that
corrupted nested-loop frames (the Reg A `/client-test` hang), a
busy-wait per active close that pegged the CPU during curl-storm
load, and a FIN drop on `Configure(NULL)` when the bounded close
wait expired before the firmware finished. Plus debugging tooling
(GDB-into-QEMU), CI gains (test-http and test-tcp-echo now run on
every push), debug info in release builds, refreshed audience
framing for Linux systems C developers, and several supporting
fixes that surfaced along the way.

### Added

- **GDB-into-QEMU debugging.** `scripts/run-qemu.sh --gdb [PORT]`
  exposes the QEMU GDB stub; `--debugcon FILE` captures OVMF's
  `Loading driver at 0x... NAME.efi` lines. `scripts/gdb-syms.py`
  consumes the debugcon log and emits `add-symbol-file` directives
  ready to paste into `gdb`. Documented end-to-end in
  `docs/DEBUGGING.md`. Used to bisect the close-event hangs and
  the CoreCheckEvent `#PF` (`docs/DEBUGGING.md § Worked example`).
- **`test/integration/test-tcp-echo.sh`** — minimal TCP-only
  integration test (15 sequential connect/echo/close probes against
  `sdk/examples/tcp-echo-server.efi`). Asserts every probe echoes,
  guest connect/disconnect counts match, and the host TCP table is
  clean after the storm — narrow signal for FIN-delivery and
  close-token regressions that test-http would only show as flake.
  Now part of the CI integration job.
- **`axl_net_ensure_drivers`** — drives auto-load of TCP4 / DNS4
  / IP4 service bindings under `NetInfo`, `Fetch`, `RfBrowse` so
  the tools work on bare-metal images that haven't pre-`connect`'d
  the network stack. Mirrors the `axl_driver_locate` pattern
  introduced for `MkRd` in v0.2.3.

### Fixed

- **`axl_tcp_close` no longer corrupts caller stack frames.** The
  close token is now heap-allocated; EDK2 holds its pointer past
  TIME_WAIT, and the previous stack-allocated token UAF'd whenever
  TIME_WAIT outlived the bounded wait. Manifested as the flaky
  `/client-test` handler hang ("Reg A") in test-http.
- **No CPU spin per active close.** Close registers its
  completion event on the caller's running event loop and finalizes
  asynchronously when SockConnClosed fires; the per-close
  `_axl_tcp_wait` is now a fallback used only outside a running
  loop (CLI tools, shutdown after `axl_loop_run` returned).
  test-http wall time fell ~94 s → ~21 s on the same hardware.
- **`Configure(NULL)` no longer drops the queued FIN** on active
  close. Configure-NULL runs from the close-event finalize callback
  after the firmware has fully completed Close, so `TcpFlushPcb`'s
  buffer flush has nothing left to send.
- **Synchronous `Receive()` errors and re-arm errors get delivered
  through the loop**, not silently swallowed. EDK2 returns
  `EFI_CONNECTION_FIN` synchronously when `SockNoMoreData` ran with
  an empty token list before the user re-armed; without delivery
  the server never observed EOF, never closed, and peers hung in
  FIN-WAIT-2.
- **Loop dispatch epilogue skips slot-reuse stomp.** A callback that
  removed its own source and registered a new one in the same slot
  used to have its new occupant deactivated by the dispatch
  epilogue. The dispatcher now snapshots `src->id` before the
  callback and skips epilogue if the slot was re-issued.
- **Loop event-array off-by-one.** `event_array[AXL_MAX_SOURCES + 2]`
  was sized for two sentinels but `axl_loop_next_event` appended a
  third (intrinsic keypress, added in 0.2.5). Wrote one slot past
  the stack array — surfaced under load as a stale `AxlEventHandle`
  the next iteration handed to `gBS->CheckEvent` (`#PF` in
  `CoreCheckEvent`). Fixed; size is now `+3`.
- **HTTP response cache FIFO eviction is now actually FIFO.**
  `axl_time_get_ms()` is 1-second resolution on UEFI, so many
  inserts within the same second tied on `timestamp_ms` and the
  tiebreak fell back to hash-bucket-walk order. Switched to a
  monotonic `cache_seq` counter; eviction is correct independent
  of clock granularity.
- **Serial Ctrl-C bridge** — `RegisterKeyNotify` is too sensitive
  on real hardware (fires on every keystroke at TPL_NOTIFY); use
  the Simple Text Input Ex `WaitForKeyEx` event under TPL_CALLBACK
  instead. Drops apparent CPU spin on serial-attached boards.

### Changed

- **`AXL_MAX_SOURCES` bumped 16 → 64.** The async-close shape needs
  ~one loop source per outstanding close ctx (held for ~TIME_WAIT
  seconds while SockConnClosed fires). 16 was tight even before —
  an http-server with the default 8 max-connections plus listener
  and per-conn cancellables would peak near the limit and fail
  `axl_loop_add_event` silently. Exhaustion now logs at error
  level instead of failing silently.
- **`axl_tcp_send` / `axl_tcp_recv` save/restore `sock->async_loop`**
  across their ephemeral wrapper loop. Previously each sync wrapper
  left `async_loop` pointing at the just-freed loop — a follow-up
  `axl_tcp_close` on the same sock would dereference freed memory
  while deciding sync-vs-async finalization.
- **CI integration job runs `test-http.sh` and `test-tcp-echo.sh`**
  on every push (was unit + tools + cpu-idle only). KVM
  acceleration is auto-enabled on GitHub-hosted runners via a
  one-shot `chmod 666 /dev/kvm` step.

### Library API

- **`axl_tcp_close` lifetime semantics** are now documented on the
  declaration in `axl/axl-tcp.h`. Callers must close TCP sockets
  before freeing the loop they were registered with; on the async
  path the AxlTcp pointer outlives the call until the firmware
  signals close-complete (treat as freed once the call returns).

### Examples / Tools

- **Examples now self-bootstrap networking** via `axl_net_auto_init`
  in `main` (echo-server, tcp-echo-server, echo-server-sync,
  http-server, net-check, fetch). Running any of them via plain
  `run-qemu.sh --net --hostfwd ...` Just Works — no custom nsh
  needed for `connect -r` / `ifconfig -s eth0 dhcp`. Tools
  (`fetch`, `netinfo`, `rfbrowse`, `mkrd`) already self-loaded
  their drivers; the examples now match.
- **Examples now check fallible-call returns**. `axl_loop_new`,
  `axl_*_accept_async`, `axl_http_server_add_route`, and
  `axl_socket_send` returns are checked in `echo-server.c`,
  `tcp-echo-server.c`, `http-server.c`, and `socket-demo.c` so the
  patterns developers crib from these files include the error
  paths, not just the happy path.
- **`tools/fetch`** now warns when more `-H` headers are passed
  than the static buffer pool holds (16). Previously truncated
  silently.

### Build

- **DWARF debug info in RELEASE builds** (Makefile + axl-cc).
  Both DEBUG and RELEASE now compile with `-g -gdwarf`. The `.efi`
  PE/COFF stays slim because objcopy still strips DWARF; the
  side-by-side `.so/.debug` carries it, and `pe-set-debug` points
  the PE debug-data directory at it. addr2line now works against
  any built artifact — a `#PF` reported by a user is resolvable
  against the binary they already have, without rebuilding with
  debug flags.

### Documentation

- **Audience framing.** README, AXL-Design, AXL-SDK-Design, and the
  `axl.h` / `axl-loop.h` umbrella headers now lead with the audience
  AXL is for — Linux systems C developers (glibc / GLib / systemd /
  libcurl) who need to ship a UEFI binary without learning EDK2.
  GLib-to-AXL mapping table (GMainLoop → AxlLoop, GHashTable →
  AxlHashTable, etc.) lives in AXL-Design.md.
- **How AXL avoids the EDK2 dependency** — README and design docs
  now explain that the `EFI_*` types are auto-generated from the
  published UEFI 2.x and PI 1.x specifications via
  `scripts/generate-uefi-headers.py` driven by
  `scripts/uefi-manifest.json5`. Spec drift is a manifest update,
  not a vendor merge.
- **Sphinx full-width override** — `docs/sphinx/_static/axl.css`
  widens `.wy-nav-content` to 1200 px so axl.aximcode.com no
  longer leaves grey gutters on wide monitors.

### Migration

- **`axl_tcp_close` returns immediately on the async path** when
  called from inside a running loop (typical: server connection
  teardown). The `AxlTcp *` pointer outlives the call by up to
  ~TIME_WAIT (~2 s) while the firmware completes the close.
  Existing callers that already null'd their `AxlTcp` pointer
  after `axl_tcp_close` work unchanged. Callers that read or use
  the pointer after close had a UAF in 0.2.5; those now read
  freed memory more visibly. The header doc on
  `include/axl/axl-tcp.h` describes the lifetime explicitly. No
  source changes required for the common case.

## 0.2.5 — 2026-04-25

Substantial release. Three headline buckets: a JSON-module rework
(reader/writer rename + writer rewrite + stats), `AxlRingBuf` push
statistics, and a stretch of CI/Ctrl-C plumbing that finally got QEMU
integration tests running in CI. Plus the typed numeric parsers and
miscellaneous API hardening that landed on top of v0.2.4.

### Added

- **Typed numeric string parsers.** `axl_str_to_u8/u16/u32/u64` and
  `axl_str_to_s8/s16/s32/s64` (in `<axl/axl-str.h>`) — the underlying
  parser is shared, the typed variants enforce range and sign. Replaces
  hand-rolled port parsers throughout `src/net/` and the
  `echo-server-sync` example.
- **`AxlJsonWriter`** — new orthogonal JSON writer with state machine.
  Backed by a caller-owned `AxlString` (auto-growing, no fixed-buffer
  guesses). Containers, keys, and atoms are independent calls; the
  writer handles comma placement, string escaping, and (optional)
  2-space-indent pretty mode (`AXL_JSON_WRITER_PRETTY` flag). Sticky
  error flag covers OOM and structural misuse; one check after
  `axl_json_writer_finish` is sufficient. Includes `kv_*` convenience
  pairs (`axl_json_kv_str/int/uint/bool/null/hex`) for the dominant
  key+atom shape, and `kv_strn` / `keyn` for non-NUL-terminated input.
- **`axl_json_write_token`** — parse → mutate → emit bridge. Splices a
  parsed token tree (object, array, or atom) into a writer's output
  verbatim, preserving `\uXXXX` escapes and other source representation.
- **`AxlRingBuf` push statistics.** New `pushes_total` and `pushes_lost`
  cumulative byte counters on every push path (`push`, `push_msg`,
  `push_elem`, `push_advance`), with accessors
  `axl_ring_buf_pushes_total / _lost`. Counts attempted bytes vs bytes
  invisible to the consumer (rejected in reject mode, displaced or
  input-dropped in overwrite mode). Reset on `axl_ring_buf_clear` and
  on init. Element-mode consumers divide by element size to get
  element counts.
- **`axl_diag_startup` / `axl_diag_probe_protocol`** — extracted the
  `-v` startup-diagnostics block that lived in tools into a public
  `<axl/axl-diag.h>` API. Tools that want a uniform "what's the
  firmware seeing" dump get it as a one-liner.
- **`axlk_http_read_request_line`** (kernel POC) — wraps the
  read-until-`\r\n\r\n` loop and delegates parsing to the public
  `axl_http_parse_request_line`. The three SoftBMC-shape kernel POC
  ports (hwinfo, bootconfig, reqlog) now share one implementation
  instead of duplicating ~70 LOC of byte-fiddling each.
- **`--log <path>` flag on `test/integration/test-axl.sh`** — captures
  the raw QEMU serial log to the given path. Equivalent to setting
  `TEST_KEEP_LOG`; just discoverable from `--help`.
- **Phase K10 (`axlk_offload`) added to the AXL-Kernel-Design.md phase
  plan.** AP compute pool design — out-of-band CPU work via
  `EFI_MP_SERVICES_PROTOCOL`, with cooperative blocking on completion
  via a new fd kind. Not started; design is staked.

### Changed

- **`AxlJsonCtx` → `AxlJsonReader`, `AxlJsonBuilder` → `AxlJsonWriter`.**
  Type rename for symmetry. Reader function signatures unchanged
  (parameter renamed `ctx` → `r` internally only). All callers
  migrated: tests, fuzz harness, `tools/rfbrowse`, `sdk/examples/json`,
  experiments. Old type names are gone — recompile against the new
  header.
- **`AxlJsonBuilder.overflow` field → `axl_json_writer_error()`
  accessor.** Struct fields are private now. The flag also broadened
  semantically: it's set on either AxlString OOM or structural misuse
  (key in array, value where key expected, mismatched begin/end, etc.).
- **`axl_json_pretty_print` → `axl_json_console_print`.** Renamed to
  make clear it's a *colored UEFI-console* pretty-printer (writes
  attribute-based color codes directly to the console), distinct from
  the writer's `AXL_JSON_WRITER_PRETTY` flag (buffer output, no color).
  rfbrowse keeps its colored Redfish-body output via the renamed call.
- **Three previously-`void` API sites now report failures.** Surfaced
  error returns where the caller had no way to know the operation
  silently failed. Caught by code review of the new typed-parser /
  config OOR work.
- **AxlConfig OOR rejection.** Config values that overflow the declared
  field width are now rejected with a clear warning and the parse
  fails, instead of being truncated silently.
- **AxlHashTable header preamble + per-constructor doc-comments.**
  Ownership semantics across the three constructors (`new_str` copies
  keys, `new` borrows both sides, `new_full` takes ownership when
  destroy callbacks are non-NULL) now spelled out at the top of the
  header and reinforced in each `_new_*` doc-comment. README adds an
  ownership matrix.
- **Three SoftBMC-shape kernel POC ports** (hwinfo, bootconfig, reqlog)
  rewritten to use `AxlJsonWriter` for endpoint JSON, `axl_strlcpy` in
  place of manual NUL-terminated copy loops, and the new
  `axlk_http_read_request_line` helper. axlk-reqlog-server's hand-rolled
  ring buffer replaced with `AxlRingBuf` (received/dropped derived
  from `pushes_total`/`pushes_lost`). On-the-wire output unchanged.
- **`axl_json_print_raw` removed** — it was a one-liner over
  `axl_printf("%.*s", ...)`. The single caller (rfbrowse `--raw` mode)
  updated to call `axl_printf` directly.
- **`AXL-Kernel-Design.md` moved into `experiments/axl-kernel/`** —
  co-located with the source it describes. README cross-refs updated.

### Fixed

- **Cooperative-Ctrl-C plumbing for serial / TCG QEMU.**
  - Backend bridges serial Ctrl-C to UEFI Shell `ExecutionBreak`.
  - `axl_loop_run` no longer busy-spins when there are no user sources
    (was pegging a CPU at 100% in tests with empty source sets).
  - test-yield-ctrlc fixes: FIFO open-order deadlock, two-socat split
    collapsed to one bidirectional pipeline, idle-marker + QEMU
    timeouts bumped to be tolerable on TCG.
- **`run-qemu.sh` interactive-tty silent failure** — script previously
  swallowed errors when stdin wasn't a TTY; now surfaces them and
  cleans up the serial log. Companion `--raw` flag plumbed through
  test-cpu-idle so failures appear in CI logs.
- **`run-qemu.sh` mtools-fallback dest path bug** — `mcopy` got the
  wrong destination when EDK2 mkimage was unavailable; rewritten to
  use `mcopy -s` correctly.
- **`u64_to_hex` truncation safety** — overflow path now writes a
  `"0x0"` placeholder instead of leaving the caller buffer
  uninitialized. Defensive against future buffer-size shrinks (current
  callers always size adequately).
- **Test-log noise:** AxlMem leak-dump test annotated with a
  clarifying printf so its expected-leak report (the AXL runtime's own
  argv/registry/atexit state) is no longer mistakable for a real leak.
  AxlData hash-table fixture leak silenced —
  `test_hash_insert_vs_replace` now reclaims its `axl_strdup`'d test
  keys after the table is freed.

### CI / Build

- **QEMU integration tests now run in CI.** `axl-common.sh` falls back
  to system `PATH` for QEMU binaries when none are pre-staged. `mkimage`
  build-dependency dropped — the mtools fallback covers the path.
  UEFIExtract installed from the LongSoft GitHub releases (correct
  lowercase binary name, A74 / x64_linux asset URL). `chmod 666
  /dev/kvm` so QEMU can use hardware acceleration; `axl-common.sh`
  also gracefully drops `-enable-kvm` when `/dev/kvm` isn't read/writable.

### Documentation

- **§ section anchors linkified** across Sphinx-rendered pages and the
  kernel design doc, so cross-references render as clickable links
  instead of plain text.
- **`src/data/README.md`** rewritten to fix existing-broken examples
  (parser used a wrong `get_string` signature; iterator example
  referenced types that don't exist) and added new JSON Writer +
  Round-Trip Transforms + Error Handling sections.
- **AxlRingBuf README** documents the new push-statistics API + the
  byte-counter unit semantics.

## 0.2.4 — 2026-04-24

### Added

- **`axl_driver_locate(name, out, out_size)`** — find a driver file
  on disk without loading it. Same search order as
  `axl_driver_ensure`: image's `drivers/<arch>/`, image's own
  directory, image's `drivers/`, then other volumes' `drivers/<arch>/`.
  Useful when the caller controls the LoadImage / StartImage
  lifecycle — for example, to set per-invocation load options
  between the two steps for a configurable DXE driver. axl-webfs's
  `mount` command uses this to find `axl-webfs-dxe.efi` regardless
  of which volume the user invoked the CLI from.

### Changed

- **`axl_driver_ensure` review followups.** Header doc example now
  shows the required `(const AxlGuid *)` cast that mkrd actually
  uses (the previous example wouldn't compile against the published
  signature). Added a trust-model paragraph: this function loads the
  first matching .efi off any mounted FAT volume at full firmware
  privilege, so don't pass attacker-controlled driver names. Internal:
  candidate-list dedup so the common "running tool lives at
  drivers/&lt;arch&gt;/" case doesn't double-stat the same path; comment
  on the EFI_GUID const-cast for future readers.

## 0.2.3 — 2026-04-24

### Added

- **`axl_driver_ensure(guid, name)`** — short-circuit on registered
  protocols, otherwise locate, load, and start a named DXE driver
  from `drivers/<arch>/<name>` on the running image's volume, the
  image's own directory, `drivers/<name>` at the volume root, or any
  other mounted FAT volume. Tools that depend on driver-provided
  protocols no longer need a `startup.nsh` that pre-loads them.
- **`tools/dmidecode`** — full SMBIOS record decoder with typed
  output for the common types and a hex/strings fallback for
  undecoded ones; companion to `sysinfo` for hardware introspection.
- **AxlSmbios** promoted to a top-level module
  (`include/axl/axl-smbios.h`) with typed accessors for Type 0/1/2/3/4
  /16/17, Type 38 (IPMI Device Info), and Type 42 (Host Interface),
  plus enumeration helpers (`axl_smbios_next`, `axl_smbios_version`),
  reentrant string access, and a UUID helper.

### Changed

- **`mkrd`** auto-loads `RamDiskDxe.efi` via `axl_driver_ensure`
  before dispatching modes. Prints
  `MkRd: RamDiskDxe.efi not found on any mounted volume.` when the
  driver isn't discoverable; previously the user saw the more
  cryptic `EFI_RAM_DISK_PROTOCOL not available` deep inside the
  create path.

### Fixed

- **Tool argv on Dell firmware.** `_axl_args_init` (in `axl-app.c`)
  no longer depends on `EFI_SHELL_PARAMETERS_PROTOCOL`. Dell's UEFI
  firmware doesn't publish that optional Shell-2.0 protocol for
  cross-volume invocations, which made every AXL tool see `argc=1`
  and miss all user arguments. The implementation now parses
  `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` (the spec-mandated
  primitive) directly, with a `_tokenize_load_options` helper that
  handles whitespace splitting and quoted arguments.

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
