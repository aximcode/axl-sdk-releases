# AXL

AXL (AximCode Library) is a GLib-inspired C library for UEFI, plus an
SDK for building UEFI applications and drivers without EDK2.

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

- **GCC** + **binutils** for the host toolchain, plus
  `gcc-aarch64-linux-gnu` + `binutils-aarch64-linux-gnu` if you want
  to cross-build aa64 UEFI binaries.
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

**Pin a specific version:** use the versioned URL pattern
`https://github.com/aximcode/axl-sdk-releases/releases/download/v<version>/<file>`.
Each release publishes a `SHA256SUMS` alongside the packages.

**Build from source:** `git clone https://github.com/aximcode/axl-sdk-releases.git`
(checkout a `v*` tag for a specific release), or download the
**Source code (tar.gz)** archive linked on each
[release page](https://github.com/aximcode/axl-sdk-releases/releases),
then run `./scripts/install.sh --prefix /opt/axl-sdk` for the
same FHS layout under `/opt/axl-sdk/`.

### Pre-built UEFI tools (USB-stick use)

For quick UEFI-shell troubleshooting without installing the SDK,
download a flat tarball of the tool `.efi` binaries:

```bash
# x86_64
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk-tools-x64.tar.gz
# AArch64
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk-tools-aa64.tar.gz
```

Extract to a FAT-formatted USB stick, boot to the UEFI Shell, run
a tool with `--help`. Includes `mkrd`, `hexdump`, `fetch`, `find`,
`grep`, `sysinfo`, `netinfo`, `ipmi`, `rfbrowse`. Built with TLS
so `fetch` handles HTTPS and `rfbrowse` (Redfish) works.

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
# Unit tests (776 tests)
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
- [Coding Style](docs/AXL-Coding-Style.md) — naming conventions, formatting
- [Porting Guide](docs/AXL-Porting-Guide.md) — how to port EDK2 apps to axl-cc
- [Design](docs/AXL-Design.md) — architecture, phases
- [Roadmap](docs/ROADMAP.md) — phase tracker

## Architecture

### AXL Library

- **`include/axl/`** — public headers. `axl_snake_case` functions,
  `AxlPascalCase` types, standard C types, UTF-8 strings.
- **`src/`** — module implementations. Each directory has a
  `README.md` with overview, examples, and usage guidance:
  [mem](src/mem/README.md),
  [data](src/data/README.md) (str, string, hash, array, list, queue, json, cache),
  [io](src/io/README.md),
  [log](src/log/README.md),
  [util](src/util/README.md) (args, config, path, env, sys, driver),
  [loop](src/loop/README.md) (event loop, defer, signal),
  [task](src/task/README.md) (arena, task pool, buf pool, async),
  [net](src/net/README.md) (tcp, udp, http, tls),
  [gfx](src/gfx/README.md).
- **Backend** (`src/backend/`) — platform abstraction over UEFI
  firmware services. Single native implementation.

### AXL SDK

- **`axl-cc`** — compiler wrapper. Invokes clang + lld-link (or GCC)
  with the right flags, includes, and libraries.
- **`axl-crt0`** — UEFI entry point stub. Bridges `EFI_HANDLE` +
  `EFI_SYSTEM_TABLE` to `int main(int argc, char **argv)`.
- **`axl.cmake`** — CMake integration via `axl_add_app()`.
- **`include/uefi/`** — auto-generated UEFI type definitions from
  the UEFI spec HTML. No dependency on EDK2 headers.

### Optional: TLS

TLS support uses [mbedTLS](https://github.com/Mbed-TLS/mbedtls)
(v3.6.3) as a git submodule. Build with `AXL_TLS=1` to enable
HTTPS server/client and self-signed certificate generation.

## Status

AXL is under active development. The core library is stable with
776 unit tests + 16 tool tests + 17 HTTP integration tests + 3 UDP
tests + 5 HTTPS tests. Apps and drivers build with just clang (or
GCC) — no EDK2 or external UEFI SDK needed.

## License

Licensed under the [Apache License, Version 2.0](LICENSE). See
[NOTICE](NOTICE) for copyright and [THIRD_PARTY.md](THIRD_PARTY.md)
for attribution of vendored components.

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for
the DCO sign-off requirement and the contributor-license grant that
keeps commercial-licensing options open for the project.
