# AXL Fuzz Harnesses

Host-side libFuzzer harnesses for AXL parsers. Complements the
OOM fault injection in `src/mem/` — that covers resource-exhaustion
paths, this covers hostile-input paths.

They live out-of-tree from the freestanding library build because
libFuzzer and AddressSanitizer require host-side instrumentation
that doesn't work in `-ffreestanding` UEFI builds.

Fuzzing itself is opt-in, but they are no longer unreferenced:
`make check-fuzz-link` builds all three and replays their committed
seed corpora, and `scripts/verify.sh` runs it. That gate exists
because nothing used to build this directory, and `json_fuzz` sat
un-linkable for months as a result — ASan/LSan coverage of the JSON
read path was silently zero across the entire JSON flag redesign.

## Requirements

- `clang` with libFuzzer + AddressSanitizer support (any recent
  upstream clang; tested with clang 20).

## Build

```sh
make -C test/fuzz            # build the harnesses
make check-fuzz-link         # from the repo root: build + replay seeds
```

Output binaries land in `test/fuzz/`. Override the compiler with
`make CC=/path/to/clang` if the default `clang` on PATH isn't the
one you want; the gate uses `FUZZ_CC` for the same purpose
(`make check-fuzz-link FUZZ_CC=/opt/llvm/bin/clang`) and SKIPS rather
than fails when no clang is installed.

## Targets

| Harness     | Fuzzed API                                | Seed corpus    |
|-------------|-------------------------------------------|----------------|
| `url_fuzz`  | `axl_url_parse`, `axl_url_build`, `axl_url_encode`/`_decode` | `url_corpus/`  |
| `json_fuzz` | the JSON reader, the accessor family, `axl_json_error_format`, and the WRITER (`axl_json_write_token`) | `json_corpus/` |
| `ipmi_fuzz` | `axl_ipmi_get_device_id`, `_sel_info`, `_sel_get_entry`, `_sdr_info`, `_sdr_get`, `_get_sensor_reading`, `_fru_info`, `_fru_read`, `_chassis_status`, `_chassis_control` + format helpers | `ipmi_corpus/` |

`axl_url_parse` was wired up first because it's pure string
manipulation with a small dependency surface (`axl_mem`, `axl_str`,
`axl_log`). `axl_json_parse` needs rather more — the accessors reach
`axl_utf8_decode`, `axl_str_to_double` and the hash table — which is
why it pulls `AXL_BASE`; see "How the shim works".

`json_fuzz` compiles `axl-json-parse.c` (accessors) and
`axl-json-lex.c` (the parser); they were one file back when the parser
was the vendored header-only jsmn, and deleting jsmn split them.
Fuzzing this target matters more than it used to: the strict path is
no longer battle-tested third-party code but AXL's own lexer, reached
by every flag value. Seeding from the JSONTestSuite corpus
(`deps/jsontestsuite/test_parsing`, fetched by
`scripts/generate-json-corpus.py --fetch`) gives libFuzzer 318
hand-crafted edge cases to mutate from. Count the `*.json` files, not
the directory entries: a previous run seeded straight into it, so it
also holds ~1850 extensionless libFuzzer units, which are
machine-generated and the opposite of hand-crafted. That is the
scratch-directory hazard above, already sprung once.

It runs each input through a MATRIX of read flags, rather than the two
bare dialects it used to: both dialects against all three UTF-8 modes,
plus
`AXL_JSON_REJECT_DUPLICATES` on each. Dialect alone left every flag
the redesign added with no coverage — and `REJECT_DUPLICATES` is
exactly where both OOM defects found by code review lived. Note the
UTF-8 mode is a **two-bit field** and `AXL_JSON_RELAXED` already names
`AXL_JSON_UTF8_RAW`, so ORing `AXL_JSON_UTF8_STRICT` onto `RELAXED`
produces the reserved value 3 rather than strict mode; the matrix
spells out `AXL_JSON_JSON5` for those rows.

## Run

Give libFuzzer a SCRATCH directory first, then the seed corpus:

```sh
mkdir -p /tmp/axl-fuzz
./test/fuzz/url_fuzz  /tmp/axl-fuzz test/fuzz/url_corpus/
./test/fuzz/json_fuzz /tmp/axl-fuzz test/fuzz/json_corpus/ \
    -dict=test/fuzz/json.dict
```

**Pass `-dict=json.dict` to `json_fuzz`.** Structure and punctuation a mutator
finds on its own; multi-byte KEYWORDS it effectively cannot — `-Infinity` is
nine specific bytes, and each of `NaN` / `Infinity` / `0x1F` / `\x41` gates a
distinct branch. Measured: with the pull-mode oracle below, deleting
`parse_number`'s at-end-of-window guard goes UNNOTICED by a 25-second run
without the dictionary and is caught by the same run with it. The gate replays
seeds rather than fuzzing, so it does not use the dictionary.

The scratch directory is not optional bookkeeping. libFuzzer writes every
coverage-increasing input it discovers into the FIRST directory on the command
line, so passing the seed corpus alone makes it grow the tracked directory --
one 120-second run added 2587 files, and a careless `git add -A` will commit
all of them. The seed corpora are a dozen or fewer hand-written documents
each and are meant to stay that way; generated units belong outside the repo.

`json_corpus/` is curated, not arbitrary. Each seed buys a specific path:
`many_tokens.json` crosses the token-array growth boundary (the cap starts
at 16 and doubles — before it existed, the whole corpus fitted under one
allocation and a mis-sized `alloc_tok` could not be caught by replaying the
seeds), `duplicate_keys.json` reaches `dup_check_subtree` and the hash
table, `utf8_multibyte.json` separates the three UTF-8 modes, and the two
`deep_*.json` seeds sit either side of `AXL_JSON_DEPTH_DEFAULT` (32) so both
the accept and the `AXL_JSON_ERR_DEPTH` reject arms are walked.

`json5_features.json` opens all ten dialect gates at once — verified by
clearing one `ALLOW_*` bit at a time and confirming each removal makes the
document fail, not by inspection. It is easy to get wrong: an earlier version
of this seed missed `ALLOW_EXTRA_ESCAPES` and `ALLOW_EXTRA_WHITESPACE`,
because those two are not reached by any ordinary-looking JSON5 construct —
they need `\x##` / `\v` / `\0` / `\'` / a line continuation, and a raw VT
between tokens or a raw TAB inside a string, respectively.

For a real fuzzing session, seed from the larger JSONTestSuite corpus too:

```sh
./test/fuzz/json_fuzz /tmp/axl-fuzz test/fuzz/json_corpus/ \
    deps/jsontestsuite/test_parsing/
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
directly, together with `AXL_BASE` — the string/format substrate from
`src/data` and `src/format` — and `fuzz_shim.c`.

The split is the point. `fuzz_shim.c` shims only what must NOT be real:

| shimmed | why |
|---|---|
| `axl_malloc_impl` and friends | libc `malloc`, so ASan owns every allocation. The freestanding `src/mem/axl-mem.c` has its own page allocator, which ASan cannot see into. |
| `axl_log_full` / `axl_log` | noise; the harness cares about crashes, not log text. |
| `axl_file_get_contents` | `axl_json_load_file` must resolve, but a byte-buffer harness can never reach it. Failing is the honest stub. |
| `axl_ferror`, `axl_fwrite`, `axl_read`, `axl_stream_can_write` | only `axl-json-io.c`'s STREAM source/sink reach these, and a byte-buffer harness cannot. The real `axl-stream.c` would drag in the UEFI backend, which does not build for the host. |
| `_axl_poll_break` | `axl_qsort` polls the shell's Ctrl-C flag through it, and `SORT_KEYS` sorts. No shell here. |

Everything else is the REAL implementation. `axl_strlen`, `axl_memcpy`,
`axl_snprintf` and four more used to be libc wrappers in the shim, which
meant a harness reporting a clean run had partly been fuzzing glibc. They
were also what blocked `json_fuzz`: the JSON accessors need `axl_memchr`,
`axl_memmove` and `axl_utf8_decode`, so `axl-str.c` had to be compiled in
— and it then collided with all seven wrappers. Removing them fixed both
problems at once.

`AXL_BASE` is six files and is the transitive closure, established by
following undefined symbols rather than by guessing: no backend, no
`axl-mem.c`, no `axl-fs`, no event loop. Add to it only when a link
fails — which is the designed failure mode, and what `make
check-fuzz-link` exists to surface.

## What these can and cannot see

A fuzzer with no notion of the right answer can only report a memory error.
That is a poor fit for this code: every notable JSON defect in the tree's
history — the `\uXXXX` mis-decode, the split UTF-8 sequence, the over-trim —
was perfectly memory-safe and simply returned the wrong bytes. So
`json_fuzz` and `url_fuzz` carry ORACLES, which are the only assertions here
that can fail for a reason other than a crash:

| oracle | property | catches |
|---|---|---|
| `round_trip` | the writer's output re-parses under its own reader | the library disagreeing with itself about the grammar |
| `round_trip` | serializing is idempotent | lossy or non-deterministic re-serialization |
| `compare_representations` | plain and `ENSURE_ASCII` spellings decode identically | a wrong-but-well-formed re-encoding, e.g. a mangled surrogate pair |
| `codec_round_trip` | `decode(encode(x)) == x` | any byte the percent-codec loses or alters |
| `url_round_trip` | `parse(build(parse(x)))` keeps scheme and host | the builder emitting what the parser will not take |
| `chunked_scan_matches` | a PULL source gives the same events as a contiguous one | the scanner's window arithmetic — compaction, growth, and the base every offset is measured from |

`chunked_scan_matches` PADS its input with leading whitespace before scanning
it, and that is not incidental. A refill fills the scanner's window, which is
at least a kilobyte, so any document shorter than that arrives whole on the
first read and NO TOKEN EVER STRADDLES A REFILL — the oracle would run on every
input and exercise none of the code it exists to cover. The unit suite learned
this the expensive way: a chunk-size sweep over small fixtures stayed green
with the guard it was written to protect deleted outright. The pads bracket the
window boundary so a token near the front of the input is cut at a different
place in each, and pad 0 keeps the unpadded case honest.

Those found three real defects within seconds of first running — two in the
`ENSURE_ASCII` re-encoding of a JSON5 `\<char>` escape, and one in
`SORT_KEYS` ordering an ill-formed key by bytes it would never emit (see
decisions 34-35 in `AXL-JSON-Design.md`). Worth being precise about why the
earlier 54.9M-execution run missed them: that harness did not compile the
writer at all, so the writer path absorbed *zero* of those executions. The
lesson is about REACH, not about run length.

**Still not covered, deliberately or otherwise:**

- **`axl-json-print.c`** — `axl_json_console_print` writes through the UEFI
  console backend, which does not build for the host. Not reachable from a
  byte-buffer harness in any case.
- **Stream-backed source/sink.** `axl_json_source_init_stream` and
  `sink_init_stream` resolve against stubs in `fuzz_shim.c`; the mem, string,
  buffer and callback forms are real and are exercised.
- **A decode bug that is CONSISTENT across spellings.** The representation
  oracle compares two encodings against each other, so a reader that decodes
  both the same wrong way looks correct to it. That class needs an external
  oracle — `test-json-corpus-qemu.sh`'s jq differential and the unit
  assertions — not this harness.
- **Wrong ANSWERS from the accessors.** `exercise_value_accessors` ignores
  return values on purpose; it is checking for out-of-bounds reads, not for
  `get_int` returning the wrong integer.
- **Most of `axl-json-io.c`.** It is compiled, but the harness only reaches
  the string SINK, transitively through `axl_json_writer_init`. Nothing calls
  `axl_json_source_init_mem`, `axl_json_parse_source`, `sink_init_buffer` or
  the callback forms, so the whole source half and the buffer sink's
  `needed`/truncation arithmetic are compiled and never entered. Cheap to add,
  and worth it — that arithmetic is exactly the shape that goes wrong.
- **`ipmi_fuzz` has no oracle at all** — the IPMI decoders have no inverse to
  round-trip against, so it remains crash-only.

## Timing

`make check-fuzz-link` prints its own numbers; on an 8-core box they are
roughly:

| step | time |
|---|---|
| build all three harnesses from clean | ~3.8 s |
| replay `url_corpus` (225 seeds) | ~22 ms |
| replay `json_corpus` (16 seeds) | ~34 ms |
| replay `ipmi_corpus` (11 seeds) | ~26 ms |

(`check-fuzz-link` measures these with `date +%s%3N`, which is GNU coreutils
only. On a BSD `date` the numbers degrade to garbage rather than failing the
gate — the pass/fail decision never consults them.)

A real fuzzing session is unbounded and is NOT part of the gate: a 6-worker
run of `json_fuzz` reached ~55M executions in 13 minutes. Note that coverage
saturates far earlier than that — the pre-oracle harness plateaued after
~405k executions and spent the remaining 99% re-treading it, so a long run
buys much less than its execution count suggests. Prefer widening what the
harness REACHES over running it longer.

## Not yet wired up

- A NIGHTLY fuzzing job. Long runs are time-unbounded and don't fit a
  PR gate, so only the fast half is gated (see below); a nightly job
  with artifact-upload for crashes is the likely shape.
  `make check-fuzz-link` does NOT fuzz — it builds and replays the
  committed seeds, so it catches rot and regressions, not new bugs.
- More parser targets: `axl_http_parse_*`, digest block feeding,
  WebSocket frame parser. Each needs its own `*_fuzz.c` and Makefile
  entry, plus whatever additional shim symbols its parser
  references.
