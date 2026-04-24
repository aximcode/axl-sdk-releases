# AXL Fuzz Harnesses

Host-side libFuzzer harnesses for AXL parsers. Complements the
OOM fault injection in `src/mem/` — that covers resource-exhaustion
paths, this covers hostile-input paths.

These are **not** wired into the default `make` target or CI. They
live out-of-tree from the freestanding library build because
libFuzzer and AddressSanitizer require host-side instrumentation
that doesn't work in `-ffreestanding` UEFI builds.

## Requirements

- `clang` with libFuzzer + AddressSanitizer support (any recent
  upstream clang; tested with clang 20).

## Build

```sh
make -C test/fuzz
```

Output binaries land in `test/fuzz/`. Override the compiler with
`make CC=/path/to/clang` if the default `clang` on PATH isn't the
one you want.

## Targets

| Harness     | Fuzzed API                                | Seed corpus    |
|-------------|-------------------------------------------|----------------|
| `url_fuzz`  | `axl_url_parse`                           | `url_corpus/`  |
| `json_fuzz` | `axl_json_parse`                          | `json_corpus/` |
| `ipmi_fuzz` | `axl_ipmi_get_device_id`, `_sel_info`, `_sel_get_entry`, `_sdr_info`, `_sdr_get`, `_get_sensor_reading`, `_fru_info`, `_fru_read`, `_chassis_status`, `_chassis_control` + format helpers | `ipmi_corpus/` |

`axl_url_parse` was wired up first because it's pure string
manipulation with a small dependency surface (`axl_mem`, `axl_str`,
`axl_log`), which keeps `fuzz_shim.c` minimal. `axl_json_parse`
was added as a second target and reuses the same shim unchanged —
the vendored `jsmn.h` is self-contained and has no AXL dependencies
beyond what the URL parser already needed.

## Run

Point the harness at its seed corpus and let libFuzzer run:

```sh
./test/fuzz/url_fuzz  test/fuzz/url_corpus/
./test/fuzz/json_fuzz test/fuzz/json_corpus/
```

Useful flags:

- `-runs=N` — stop after N executions (default: infinite).
- `-max_len=N` — cap generated input size (default: 4096).
- `-max_total_time=N` — stop after N seconds.
- `-jobs=N -workers=N` — parallelize across cores.

A short sanity run:

```sh
./test/fuzz/url_fuzz -runs=50000 -max_len=256 test/fuzz/url_corpus/
```

## Reproducing a crash

libFuzzer writes the offending input as `crash-<sha1>` in the current
directory. Re-run the harness with just that file as the argument to
reproduce:

```sh
./test/fuzz/url_fuzz crash-deadbeef...
```

AddressSanitizer will print a stack trace on the crash path. Add the
minimized crash file under a `regressions/` subdirectory if you want
to keep regression coverage — the harness will load everything passed
on the command line at startup.

## How the shim works

Fuzz targets compile the parser `.c` file (e.g. `src/net/axl-url.c`)
directly against `fuzz_shim.c`, which provides libc-backed
implementations of the AXL allocator and string primitives. This
avoids pulling in the freestanding `src/mem/axl-mem.c` (which has
its own page allocator and layout) or the logging stack. The shim is
deliberately tiny — extend it only if a new parser target needs
a symbol that isn't already there.

## Not yet wired up

- CI integration. Fuzz runs are time-unbounded and don't fit a PR
  gate naturally. A nightly job with an artifact-upload for crashes
  is the likely shape, and is tracked separately.
- More parser targets: `axl_http_parse_*`, digest block feeding,
  WebSocket frame parser. Each needs its own `*_fuzz.c` and Makefile
  entry, plus whatever additional shim symbols its parser
  references.
