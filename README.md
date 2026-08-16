# AXL

AXL (AximCode Library) is a UEFI C library — with first-class C++
support — and SDK aimed at Linux systems C and C++ developers — the
audience that already writes C or C++ against
the kernel, glibc, and GLib but doesn't necessarily want to learn
EDK2 conventions, the gnu-efi runtime, or the EFI_ / CHAR16 / PascalCase
universe to ship a UEFI binary. AXL hides that and exposes the C API
shape you already know: `axl_snake_case` functions, `AxlPascalCase`
types, UTF-8 strings, `int`/`size_t`/`uint64_t` everywhere, and an
event loop and data structures modeled directly on GLib (`AxlLoop`
≈ `GMainLoop`, `AxlHashTable` ≈ `GHashTable`, …). If you've used
GLib, you've used 80% of AXL's API surface.

The SDK packages the library with headers, a CRT0 entry point, and
`axl-cc` (a compiler wrapper) so you build `.efi` binaries with one
command — no EDK2 source tree, no gnu-efi.

**How AXL avoids the EDK2 dependency.** The UEFI types (`EFI_*`
structs, status codes, protocol GUIDs, service tables) are
auto-generated from the published [UEFI](https://uefi.org/specifications)
and [PI](https://uefi.org/specifications) specifications via
[`scripts/generate-uefi-headers.py`](scripts/generate-uefi-headers.py)
+ [`scripts/uefi-manifest.json5`](scripts/uefi-manifest.json5).
The generator walks the spec HTML, extracts each declared type by
name + kind, and emits a header layout-compatible with EDK2's. The
output lives in [`include/uefi/generated/`](include/uefi/generated/),
is rebuilt on demand, and never leaks across the public API
boundary — application code never includes a UEFI header. Any
declaration the spec doesn't cover (Shell protocol etc.) lives in
the small hand-written [`include/uefi/axl-uefi-extra.h`](include/uefi/axl-uefi-extra.h).
This means AXL has no source-tree dependency on EDK2 (or gnu-efi);
spec drift is a manifest update + regeneration, not a vendor merge.

- **AXL Library** (`libaxl.a`) — the library itself: data structures,
  file I/O, networking (TCP, UDP, HTTP, TLS), graphics, event loop,
  logging, and more. UTF-8 everywhere, standard C types,
  `axl_snake_case` API.

- **AXL SDK** — packages the library with headers, a CRT0 entry point,
  and `axl-cc` (a compiler wrapper). Build `.efi` binaries with a
  single command — no EDK2 source tree, no gnu-efi, just clang or GCC.

```c
#include <axl.h>

int main(int argc, char **argv) {
    axl_printf("Hello from %s\n", argv[0]);

    AXL_AUTOPTR(AxlString) s = axl_string_new("AXL ");
    axl_string_append_printf(s, "v%d", 1);
    axl_printf("%s\n", axl_string_str(s));

    return 0;
}
```

```
$ axl-cc hello.c -o hello.efi    # 11KB binary, zero external deps
```

Include `<axl.h>` for the full API, or individual headers for specific
modules (e.g., `<axl/axl-mem.h>`, `<axl/axl-net.h>`).

### C++ also works

axl-sdk ships first-class C++ support: pass `.cpp` to `axl-cc` (or
use the `axl-c++` alias) and the same library works from C++ with
RAII handles via `AXL_AUTOPTR`, type-safe enums, and `<span>` /
`<string_view>` / `<expected>` for ergonomic API boundaries.

```cpp
#include <axl.h>

int main(int, char **) {
    AXL_AUTOPTR(AxlLoop) loop = axl_loop_new();   // auto-freed at scope exit
    axl_printf("Hello from C++!\n");
    return 0;
}
```

```
$ axl-c++ hello.cpp -o hello.efi
```

Compile-time hard defaults: `-std=c++23 -fno-exceptions -fno-rtti
-fno-threadsafe-statics`.

**One C++ mode, no flag.** The whole standard library you would
expect is available — `std::vector`, `std::string`, `std::map`,
`std::unordered_map` alongside `<array>`, `<span>`, `<string_view>`,
`<optional>`, `<variant>`, `<expected>` — on both arches:

```console
$ axl-c++ containers.cpp -o app.efi
```

There used to be a `--hosted` flag for the containers, because C++
compiled `-ffreestanding` and libstdc++ refuses them under it at
`bits/requires_hosted.h`. It is removed — passing it is an error that
says so. C sources stay freestanding, and a mixed image links.

The SDK links **no `libstdc++.a`** — AXL supplies the eleven functions
that would otherwise require it, so nothing of GCC's runtime library
is redistributed. The whole libc footprint is `memcpy`, `memmove`,
`memset`, `memcmp` and `strlen`, which `libaxl.a` already defines.
(`-frtti` is the one opt-in exception; it needs the type_info vtables
from your own installed libstdc++.) Full rationale and the measured
header table: [`AXL-Cxx-Design.md` §6a](docs/AXL-Cxx-Design.md).

C++ needs a bare-metal cross on BOTH arches — ARM's
`aarch64-none-elf-g++`, and AXL's own `x86_64-elf-g++` (nobody
publishes one, so AXL builds and hosts it).
`scripts/install-toolchain.sh all` fetches both.

See [`AXL-SDK-Design.md` §"C++ support"](docs/AXL-SDK-Design.md) +
[`AXLMM-Design.md` §"Toolchain & constraints"](docs/AXLMM-Design.md)
for the full subset and forbidden-features list.  The
[AGT widget toolkit](https://github.com/aximcode/agt) is the first
C++ consumer.

## AXL Library API

| Category | Functions | GLib equivalent |
|----------|-----------|-----------------|
| Memory | `axl_malloc`, `axl_free`, `axl_calloc`, `axl_new` | `g_malloc`, `g_free`, `g_new0` |
| Auto-cleanup | `AXL_AUTO_FREE`, `AXL_AUTOPTR(Type)` | `g_autofree`, `g_autoptr` |
| Strings | `axl_strdup`, `axl_strsplit`, `axl_strjoin` | `g_strdup`, `g_strsplit` |
| String builder | `axl_string_new`, `axl_string_append` | `GString` |
| String search | `axl_strstr`, `axl_strchr`, `axl_str_has_prefix` | `g_strstr_len`, `g_str_has_prefix` |
| Printf | `axl_printf`, `axl_fprintf`, `axl_asprintf` | `g_print` |
| File I/O | `axl_fopen`, `axl_fread`, `axl_fseek`, `axl_readline` | POSIX-style |
| File ops | `axl_file_get_contents`, `axl_file_info`, `axl_file_delete` | `g_file_get_contents` |
| Directories | `axl_dir_open`, `axl_dir_read`, `axl_dir_mkdir` | `g_dir_open` |
| Hash table | `axl_hash_table_new`, `axl_hash_table_insert` | `GHashTable` |
| Dynamic array | `axl_array_new`, `axl_array_append` | `GArray` |
| Linked lists | `axl_list_append`, `axl_slist_prepend` | `GList`, `GSList` |
| Queue | `axl_queue_push_tail`, `axl_queue_pop_head` | `GQueue` |
| JSON | `axl_json_parse`, `axl_json_get_string` | json-glib |
| Cache | `axl_cache_new`, `axl_cache_put`, `axl_cache_get` | — |
| Config + CLI | `axl_config_new`, `axl_config_parse_args`, `axl_config_get_*` | `GOptionContext`, `GKeyFile` |
| Event loop | `axl_loop_run`, `axl_loop_add_timer` | `GMainLoop` |
| Deferred work | `axl_defer`, `axl_pubsub_publish` | — |
| Logging | `axl_info`, `axl_debug`, `axl_error` | `g_info`, `g_debug` |
| HTTP client | `axl_http_get`, `axl_http_request` | libsoup |
| HTTP server | `axl_http_server_new`, `axl_http_server_add_route` | libsoup |
| TCP sockets | `axl_tcp_connect`, `axl_tcp_listen` | `GSocket` |
| UDP sockets | `axl_udp_open`, `axl_udp_send`, `axl_udp_sendrecv` | — |
| TLS (optional) | `axl_tls_init`, `axl_tls_generate_self_signed` | — (mbedTLS) |
| Graphics | `axl_gfx_fill_rect`, `axl_gfx_draw_text` | — (UEFI GOP) |
| Task pool | `axl_async_submit`, `axl_buf_pool_new` | — (UEFI MP) |
| Environment | `axl_getenv`, `axl_setenv`, `axl_chdir` | `g_getenv`, `g_chdir` |
| System | `axl_reset`, `axl_driver_load`, `gBS`, `gST` | — (UEFI-specific) |
| SMBIOS | `axl_smbios_find`, `axl_smbios_get_string` | — (UEFI-specific) |
| Utilities | `AXL_ARRAY_SIZE`, `AXL_CONTAINER_OF` | `G_N_ELEMENTS` |

## Quick start

### Requirements

- **Nothing from the distro's toolchain.** `axl-cc` compiles and
  links with bare-metal crosses that `scripts/install-toolchain.sh`
  fetches to `/opt` — ARM's `aarch64-none-elf` and AXL's own
  `x86_64-elf` — so the `.deb` and `.rpm` need no compiler, assembler
  or linker from the distro (only `curl` and `xz`, to fetch those
  toolchains). A host GCC is needed only to build axl-sdk itself from
  source.
- No EDK2, no gnu-efi, no external UEFI SDK.

### Install the SDK

Binary packages are published on each
[release](https://github.com/aximcode/axl-sdk-releases/releases/latest).
Each package bundles both x64 and aa64 UEFI target libs.

**Debian / Ubuntu:**

```bash
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.deb
sudo apt install ./axl-sdk.deb
```

**Fedora / RHEL:**

```bash
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.rpm
sudo dnf install ./axl-sdk.rpm
```

Packages ship with mbedtls compiled in, so apps that use
`https://` URLs link and run without extra setup. Apps that
don't reference TLS don't incur any binary-size cost — the
linker only pulls in mbedtls .o files when actually used. Users
who want an even smaller `libaxl.a` can rebuild from source
with `AXL_TLS=0`.

**Windows (WSL):** install the Debian or RHEL package inside
Ubuntu / Debian / Fedora WSL — the `.deb` / `.rpm` paths above
work unchanged. `axl-cc` runs in the WSL shell; point your
Windows editor at the WSL filesystem (`\\wsl$\Ubuntu\...`) and
the resulting `.efi` is a real PE32+ that boots on any UEFI
system. This is the path we recommend — no separate packaging,
no parallel toolchain to maintain.

**Windows (native MSYS2 / MinGW-w64):** no binary package yet.
Build from source under an MSYS2 UCRT64 shell with
`pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils`
installed, then `make` as on Linux. Cross-aa64 needs an
aarch64 ELF cross-toolchain (e.g. an `aarch64-*-gcc` from
MSYS2 or from Arm's GNU toolchain archives); point at it with
`make ARCH=aa64 CROSS=<prefix>-`.

**macOS:** no binary package yet. Install cross-toolchains via
the [messense tap](https://github.com/messense/homebrew-macos-cross-toolchains):

```bash
brew tap messense/macos-cross-toolchains
brew install x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu
git clone https://github.com/aximcode/axl-sdk-releases.git
cd axl-sdk-releases
make            CROSS=x86_64-unknown-linux-gnu-              # x64
make ARCH=aa64  CROSS=aarch64-unknown-linux-gnu-             # aa64
```

Native Apple Clang can't produce the ELF intermediates AXL
needs (we run `gcc → ld -T linker.lds → objcopy --target pei-*`);
a GNU cross-toolchain is required. The `linux-gnu` triple is
fine — `libaxl.a` is built `-ffreestanding -nostdlib`, so none
of the glibc baggage gets linked into your `.efi`.

**Pin a specific version:** use the versioned URL pattern
`https://github.com/aximcode/axl-sdk-releases/releases/download/v<version>/<file>`.
Each release publishes a `SHA256SUMS` alongside the packages.

**Air-gapped / corporate MITM:** the binary tarballs verify
against `SHA256SUMS` with `curl -kfsSLO`, no `git clone` over
HTTPS required:

```bash
curl -kfsSLO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.deb
curl -kfsSLO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS && sudo apt install ./axl-sdk.deb
```

**Build from source (power-user):** for working on the SDK
itself, or platforms without a binary package:
`git clone https://github.com/aximcode/axl-sdk-releases.git`
(checkout a `v*` tag for a specific release), or download the
**Source code (tar.gz)** archive linked on each
[release page](https://github.com/aximcode/axl-sdk-releases/releases),
then run `./scripts/install.sh --prefix /opt/axl-sdk` for the
same FHS layout under `/opt/axl-sdk/`.

`./scripts/install.sh` is the entry point even for an in-tree
build: it compiles the library and stages a complete SDK under
`./out` by default (`out/bin/axl-cc`, `out/lib/axl/<arch>/`,
`out/include/`), so the driver to invoke is **`./out/bin/axl-cc`**.
`scripts/axl-cc` is not a standalone driver — run from a raw
checkout it fails with `no SDK libraries ... lib/axl/<arch>`
because it resolves its SDK root to the repo top, where nothing is
staged yet. Stage first, then use `out/bin/axl-cc` (or add it to
`PATH`):

```bash
./scripts/install.sh --arch x64        # stages ./out
./out/bin/axl-cc hello.c -o hello.efi
```

### Host-side QEMU testing tooling

For **downstream consumers** who want `run-qemu.sh` and friends
to test their UEFI apps under QEMU but don't need the build-side
SDK (no `axl-cc`, no library), there's a separate tarball
and `.deb`:

```bash
# Debian/Ubuntu (.deb pulls system QEMU + OVMF + virtiofsd):
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk-host-tools.deb
sudo apt install ./axl-sdk-host-tools.deb
run-qemu my-app.efi

# Any distro (tarball — install QEMU/OVMF separately):
curl -kfsSLO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk-host-tools.tar.gz
mkdir -p ~/axl-sdk-host-tools && tar xf axl-sdk-host-tools.tar.gz -C ~/axl-sdk-host-tools
~/axl-sdk-host-tools/scripts/run-qemu.sh my-app.efi
```

Discovery falls through `$QEMU_DIR` → `$PATH` → legacy custom
build, so the tarball Just Works against the system QEMU/OVMF
package on Debian, Ubuntu, Fedora, RHEL, Alma, and Arch. Missing
dependencies print actionable `apt`/`dnf`/`pacman`/`brew`
install commands.

### Pre-built UEFI tools (USB-stick use)

For quick UEFI-shell troubleshooting without installing the SDK,
download a flat tarball of the tool `.efi` binaries:

```bash
# x86_64
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk-tools-x64.tar.gz
# AArch64
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk-tools-aa64.tar.gz
```

Extract to a FAT-formatted USB stick, boot to the UEFI Shell, and
run any tool with `-h` / `--help` for its option list. The tarball
ships these `.efi` binaries:

| Tool       | Description |
|------------|-------------|
| `9p`       | 9P2000.L client and server. `9p ls/get/put <host>[:port] <path>` for one-shot access; `9p serve <root>` exports a subtree so a Linux host can `mount -t 9p -otrans=tcp`; `9p mount <host>` publishes a remote export as a local `fsN:` volume. `serve` and `mount` run as resident drivers — stop them with `serve-stop` / `umount`. |
| `cat`      | Concatenate files to stdout (UEFI `cat(1)` equivalent). |
| `cut`      | Select sections from each line — bytes `-b`, characters (UTF-8 codepoints) `-c`, or fields `-f` split on `-d` DELIM (default TAB), with `-s` (only-delimited), `--complement`, `--output-delimiter`, `-z` (NUL-terminated). Reads files or stdin. Port of POSIX `cut(1)`. |
| `tr`       | Translate, squeeze (`-s`), or delete (`-d`) bytes from stdin, with `-c` complement and `-t` truncate. SET syntax: ranges `a-z`, C escapes (`\n \t \\ \ooo`…), and POSIX `[:classes:]`. Byte-oriented like GNU `tr`. Port of POSIX `tr(1)`. |
| `clip`     | Copy stdin to the AXL clipboard (`pbcopy`-style) — `some-tool \| clip`, `-m` MIME tag, `--clear`. Cross-app within a boot (AxlShm-backed). |
| `dmidecode`| SMBIOS / DMI table decoder (UEFI `dmidecode(8)` equivalent) — dumps every record or filter by `-t <type>`; single-value query via `-s <keyword>` (`bios-vendor`, `system-uuid`, etc.). |
| `fetch`    | HTTP/HTTPS client (curl-like) — `GET`/`POST`/`PUT`/`DELETE`/`HEAD` with custom headers, file upload (`-T`), and response-to-file (`-o`, `-O`). |
| `find`     | Recursive file and directory finder — glob patterns (`--name '*.efi'`) and type filter (`--type f` or `d`). UEFI `find(1)` equivalent. |
| `grep`     | Pattern search across files — case-insensitive (`-i`), line numbers (`-n`), match count (`-c`), recursive (`-r`). UEFI `grep(1)` equivalent. |
| `hexdump`  | Hex/ASCII file viewer (`xxd`-style) — seekable with `--offset`/`--length`. |
| `sed`      | Stream editor — POSIX sed(1) plus common GNU extensions (`s y d D p P n N h H g G x b t : a i c r w q Q = l`, ranges/`$`/`0,/re/`/`first~step`, `-n -e -f -E -s -z`). BRE by default (`-E` for ERE), with the `\xHH` escape. UEFI `sed(1)` equivalent. |
| `ipmi`     | Stripped-down `ipmitool` built on AxlIpmi: `info`, `chassis status`/`power on\|off\|cycle\|reset`, `sel list`, `sdr list`, `sensor`, `fru list`, and raw command passthrough (`raw <netfn> <cmd> ...`). |
| `lspci`    | PCI/PCIe device lister (UEFI `lspci(8)` equivalent) — `-s` BDF filter, `-d` VID:DID filter, `-n` numeric, `-v`/`-vv`/`-vvv` verbose, `-x`/`-xx`/`-xxx` hex dump, `-t` tree (PCI bridge topology). Decodes vendor/device/class names from `pci-ids.json5` sidecar (vendors[] + classes[] in one file). |
| `lsusb`    | USB device lister (UEFI `lsusb(8)` equivalent) — `-s BBB[:DDD]`, `-d VID[:PID]`, `-n` numeric, `-v`/`-vv` verbose (per-interface class triplet + iManufacturer/iProduct/iSerial), `-t` tree (real USB hub-port topology). Decodes vendor/device names from `usb-ids.json5`. |
| `lsproto`  | UEFI protocol lister — names live protocols by their **canonical spec identifier** (`EFI_RAM_DISK_PROTOCOL`, not the Shell's short `RamDisk`), from a table generated with AXL's UEFI headers. Default lists every protocol present with its GUID + handle count; `<pattern>` filters the name; `-p <name\|guid>` shows a protocol's handles; `-H` groups by handle; `-u` shows only unnamed (vendor/OEM) GUIDs; `-a` dumps the full known dictionary; `-v` names handles; `-j` JSON; `--sort name\|guid\|count`. Answers "what's actually on this box, by spec name" — which `dh decode` (short labels, curated subset) can't. |
| `memspd`   | DDR4/DDR5 SPD reader over the platform SMBus — `list` populated slots, `show <slot>` decoded fields, `decode <slot>` raw hex + decoded. JEDEC manufacturer codes resolved via `jedec.json5`. |
| `mkrd`     | Create / list / destroy FAT16/FAT32 RAM disks in the UEFI Shell (`mkrd <label> [-s size]`, `-l`, `-d <label>`). Handy for staging files without writing to flash. |
| `netinfo`  | Network interface diagnostics and ping — lists NICs with IP/MAC/link state, pings with `-c <count>`. UEFI `ifconfig`/`ping` equivalent. |
| `paste`    | Print the AXL clipboard to stdout (`pbpaste`-style) — `paste > file`, `--mime` prints the MIME type. Reads what a separate `clip` stored in the same boot. |
| `rfbrowse` | Redfish browser — connects to a BMC over HTTPS and walks resources interactively (shortcut verbs for `/Systems`, `/Managers`, etc., plus arbitrary paths). |
| `sysinfo`  | System inventory summary — compact `cpu`, `mem`, `fw`, `smbios`, `arch` subsections. Use `dmidecode` for the full per-record dump. |

`lspci`, `lsusb`, and `memspd` consult JSON5 sidecar databases
auto-discovered next to the `.efi` (or via explicit `--ids-file` /
`--jedec-file`). The tools tarball ships curated starter sets;
extend or replace via `scripts/{pci,usb}-ids-to-json5.py` against
the canonical `pci.ids` / `usb.ids` text databases.

Built with TLS enabled so `fetch` handles HTTPS and `rfbrowse`
(Redfish over HTTPS) works against real BMCs.

### Build an app

```bash
axl-cc hello.c -o hello.efi
```

### Cross-build for AARCH64

```bash
axl-cc --arch aa64 hello.c -o hello-aa64.efi
```

### CMake

```cmake
find_package(axl REQUIRED)
axl_add_app(hello hello.c)
```

### Build a driver

```bash
axl-cc --type driver mydriver.c -o mydriver.efi
```

### Run tests

```bash
# Unit tests (3000+ assertions, both X64 and AARCH64)
./test/integration/test-axl.sh

# Tool tests (hexdump, grep, find, sysinfo, etc.)
./test/integration/test-tools.sh

# HTTP integration tests
./test/integration/test-http.sh

# UDP integration tests
./test/integration/test-udp.sh

# HTTPS integration tests (requires AXL_TLS=1 build)
./test/integration/test-https.sh

# All architectures (x64 + aa64)
./test/integration/test-all.sh
```

## Documentation

- [API Reference](https://axl.aximcode.com/) — auto-generated from headers (Sphinx + Breathe)
- [Coding Style](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Coding-Style.md) — naming conventions, formatting
- [Porting Guide](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Porting-Guide.md) — how to port EDK2 apps to axl-cc
- [Design](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Design.md) — architecture, phases
- [Roadmap](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/ROADMAP.md) — phase tracker

## Architecture

### AXL Library

- **`include/axl/`** — public headers. `axl_snake_case` functions,
  `AxlPascalCase` types, standard C types, UTF-8 strings.
- **`src/`** — module implementations. Each directory has a
  `README.md` with overview, examples, and usage guidance:
  [mem](https://github.com/aximcode/axl-sdk-releases/blob/main/src/mem/README.md),
  [data](https://github.com/aximcode/axl-sdk-releases/blob/main/src/data/README.md) (str, string, hash, array, list, queue, json, cache),
  [io](https://github.com/aximcode/axl-sdk-releases/blob/main/src/stream/README.md),
  [log](https://github.com/aximcode/axl-sdk-releases/blob/main/src/log/README.md),
  [util](https://github.com/aximcode/axl-sdk-releases/blob/main/src/util/README.md) (args, config, path, env, sys, driver),
  [loop](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/README.md) (event loop, defer, signal),
  [task](https://github.com/aximcode/axl-sdk-releases/blob/main/src/task/README.md) (arena, task pool, buf pool, async),
  [net](https://github.com/aximcode/axl-sdk-releases/blob/main/src/net/README.md) (tcp, udp, http, tls),
  [gfx](https://github.com/aximcode/axl-sdk-releases/blob/main/src/gfx/README.md).
- **Backend** (`src/backend/`) — platform abstraction over UEFI
  firmware services. Single native implementation.

### AXL SDK

- **`axl-cc`** — compiler wrapper. Invokes clang + lld-link (or GCC)
  with the right flags, includes, and libraries.
- **`axl-crt0`** — UEFI entry point stub. Bridges `EFI_HANDLE` +
  `EFI_SYSTEM_TABLE` to `int main(int argc, char **argv)`.
- **`axl.cmake`** — CMake integration via `axl_add_app()`.
- **`include/uefi/`** — auto-generated UEFI type definitions from
  the UEFI spec HTML. No dependency on EDK2 headers. **Not available to
  applications**: these headers require `AXL_ALLOW_UEFI`, which `axl-cc`
  grants to `--type driver` / `--type runtime` and to an explicit
  `--allow-uefi` (CMake: `axl_add_driver`, or `ALLOW_UEFI` on
  `axl_add_app`).

### Optional: TLS

TLS support uses [mbedTLS](https://github.com/Mbed-TLS/mbedtls)
(v3.6.3) as a git submodule. Build with `AXL_TLS=1` to enable
HTTPS server/client and self-signed certificate generation.

## Built with AXL

Real projects that use this SDK as their only UEFI dependency:

- **[aximcode/axl-webfs](https://github.com/aximcode/axl-webfs)** — bidirectional
  file transfer and remote filesystem access for UEFI. Ships a CLI app
  (`serve`, `mount`, `umount`) *and* a resident DXE driver
  (`axl-webfs-dxe.efi`) that exposes a workstation directory as a UEFI
  volume (`fsN:`) — live-edit files on your laptop, run them immediately
  in the UEFI shell. Built entirely with `axl-cc`, no EDK2.

If you've built something with AXL and want it listed here, open a PR.

## Status

AXL is under active development. The core library is stable with
776 unit tests + 16 tool tests + 17 HTTP integration tests + 3 UDP
tests + 5 HTTPS tests. Apps and drivers build with just clang (or
GCC) — no EDK2 or external UEFI SDK needed.

## License

Licensed under the [Apache License, Version 2.0](https://github.com/aximcode/axl-sdk-releases/blob/main/LICENSE). See
[NOTICE](https://github.com/aximcode/axl-sdk-releases/blob/main/NOTICE) for copyright and [THIRD_PARTY.md](https://github.com/aximcode/axl-sdk-releases/blob/main/THIRD_PARTY.md)
for attribution of vendored components.

Contributions are welcome — see [CONTRIBUTING.md](https://github.com/aximcode/axl-sdk-releases/blob/main/CONTRIBUTING.md) for
the DCO sign-off requirement and the contributor-license grant that
keeps commercial-licensing options open for the project.

## Contact

- **Questions, issues, bug reports** — file an issue on
  [aximcode/axl-sdk-releases](https://github.com/aximcode/axl-sdk-releases/issues).
- **Security reports** — see [SECURITY.md](https://github.com/aximcode/axl-sdk-releases/blob/main/SECURITY.md).
- **Other inquiries** — `support@aximcode.com`.

AXL is developed by [AximCode](https://aximcode.com).
