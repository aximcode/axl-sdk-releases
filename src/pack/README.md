# AxlPack: packed-image format

`axl-pack-format.{h,c}` is the freestanding packed-image format: a 40-byte
header followed by one LZMA stream of the inner PE image. It includes no AXL
headers and no libc; memory comes from the caller's `AxlPackAlloc`. The
encoder (`axl_pack_build`) is the only reference to `LzmaEnc`, so
`--gc-sections` drops it from a decode-only image.

## Header (little-endian, serialised byte-wise)

| off | size | field |
|-----|------|-------|
| 0   | 8    | magic `AXLPACK1` |
| 8   | 2    | version (1) |
| 10  | 2    | machine (PE Machine of the inner image) |
| 12  | 1    | filter |
| 13  | 5    | lzma_props (lc/lp/pb byte + LE32 dictionary size) |
| 18  | 2    | reserved (0) |
| 20  | 4    | raw_size |
| 24  | 4    | packed_size |
| 28  | 4    | raw_crc32 (of the ORIGINAL image) |
| 32  | 4    | hdr_crc32 (bytes 0..31) |
| 36  | 4    | reserved (0) |

## Filters

| value | name  | converter |
|-------|-------|-----------|
| 0     | NONE  | none |
| 1     | X86   | `z7_BranchConvSt_X86_{Enc,Dec}`: E8/E9 rel32 to absolute |
| 2     | ARM64 | `z7_BranchConv_ARM64_{Enc,Dec}`: BL and ADRP to absolute |

The converters make the targets of repeated calls identical bytes, which LZMA
then matches. One whole-buffer call at pc 0. The encoder filters a COPY of the
input; the decoder reverses the filter in place on the decoded output and only
then checks `raw_crc32`, which covers the original image. Any other filter
value is `AXL_PACK_E_HEADER`.

## LZMA parameters

Level 9, end marker written, dictionary sized by the encoder from the input
length (`reduceSize`: the smallest 2^k or 3*2^(k-1) that is not below the image
size, so the whole image is in window). lc/lp/pb are fixed per filter, chosen
by measurement; there is no per-image search at build time.

Measured with the production `axl_pack_build`, over every `tools/*.efi` of the
current RELEASE build of each architecture (then `out/native-x64-release`,
`out/native-aa64-release`: 39 images each, sshd.efi included; 4,466,688 raw
bytes x64 and 4,642,850 aa64). Total packed bytes, smaller is better:

| arch / filter | lc3 lp0 pb2 | lc1 lp0 pb2 | lc0 lp0 pb2 | lc0 lp2 pb2 |
|---------------|-------------|-------------|-------------|-------------|
| x64 NONE      | 2,047,726   | 2,058,228   | 2,060,135   | 2,071,303   |
| x64 X86       | **1,980,960** | 1,991,199 | 1,992,337   | 2,003,738   |
| aa64 NONE     | 1,862,462   | 1,860,332   | 1,861,360   | 1,839,639   |
| aa64 ARM64    | 1,803,303   | 1,800,037   | 1,801,165   | **1,779,879** |

sshd.efi alone (the image the packed format exists for), best setting:

| arch | raw bytes | packed (filter, setting) | ratio |
|------|-----------|--------------------------|-------|
| x64  | 534,528   | 196,396 (X86, lc3 lp0 pb2)   | 36.7% |
| aa64 | 555,022   | 178,146 (ARM64, lc0 lp2 pb2) | 32.1% |

Packed sizes include the 40-byte header.

Chosen: X86 uses lc3 lp0 pb2 (the LZMA default); ARM64 uses lc0 lp2 pb2
(fixed-width 4-byte instructions reward position-aware literals). The X86
filter saves 3.3% against NONE on x64 (same setting), the ARM64 filter 3.3%
against the best NONE setting on aa64. NONE keeps the default (lc3 lp0 pb2); it is not a
shipped configuration.

The numbers will drift as the tools change; the ordering of the settings is
what matters, and a re-measure is a throwaway host build of this one file with
the constants varied.

## Vendored code

`deps/lzma/` (README there records the tag and per-file SHA-256).

## The stub

`axl-pack-stub.c` is the front of a wrapped image: raw UEFI, no libaxl
runtime. It decodes `axl_embedded_pack` with `axl_pack_unpack` (pool memory of
the image's own data type) and has the firmware `LoadImage` the original from
that buffer, then `StartImage`s it. It never executes decoded bytes itself.

### Where the payload lives

The payload is linked into its own PE section, `.axlpack`
(`AXL_PACK_SECTION`): the build's wrap (`axl_wrap_efi`) emits it there, both
`scripts/elf_*_efi.lds` KEEP it as an output section after `.rodata`, and only
the stub's objcopy (`cmake/AxlWrapEfi.cmake`'s, and `axl-cc --compress`'s
`pack_stub_link`) carries it with `-j`. The stub reads it
through its link symbols; anything holding only the FILE finds it by name in
the section table with `axl_pack_find_payload`, which bounds every offset the
file supplies against the bytes it was given. That is what AXL's own driver
loaders do (src/util/README.md, "Packed (Compressed) Images"): a raw image is
recognised from its headers alone, and a raw image that happens to embed a
packed blob in `.rodata` is never mistaken for a wrapped one -- the magic is
never scanned for.

The section costs a wrapped image up to one extra FileAlignment step (512 B
on x64) over the old `.rodata` placement: the payload no longer shares the
stub's `.rodata` padding. In the empty-payload stub the section is empty and
dropped, so the sizes below -- and the `--stub-size` the wrap passes -- do
not include it.
What the inner image is given, so it behaves exactly as the raw file would:

| channel | how | case in `test-efi-pack-qemu.sh` |
|---------|-----|---------------------------------|
| argv | the stub's `LoadOptions`/`LoadOptionsSize` copied to the inner `LoadedImage` | `raw-vs-wrapped`, `argv-quoting` |
| stdio | the stub's `EFI_SHELL_PARAMETERS_PROTOCOL` installed on the inner handle for the length of `StartImage` | `redirect` (the `\|` pipes; a `>` file redirect also works without it, because the Shell swaps `gST->ConOut`) |
| identity | `LoadImage` gets the stub's `LoadedImageDevicePath` (else `DevicePath(DeviceHandle)` + `FilePath`); if the firmware leaves the inner `DeviceHandle`/`FilePath` NULL the stub sets them (a copy of `FilePath`) | `raw-vs-wrapped` and `subdir` (`SELF=`, `LIDP=`), `driver-load` |
| exit status | `StartImage`'s status returned | `exit-status` (`%lasterror%`) |
| ExitData | forwarded through `Exit(stub, status, size, data)` | `exit-data` (the Shell discards ExitData, so the fixture's `--launch` mode starts the image itself and prints what `StartImage` returned) |

OVMF (x64) fills the inner `DeviceHandle`/`FilePath` from the device path, so
the NULL fallback is not reached there; it is reached, and gives the right
`SELF=`, when `LoadImage` is handed no device path at all.

When the image's path exists but cannot be copied (pool exhausted), the stub
fails with `EFI_OUT_OF_RESOURCES` rather than hand `LoadImage` a NULL path.

A failure prints one line on standard error, `axl-pack: <reason>` (the
format module's status text, or `<step> failed: EFI status 0x...`), and
returns `EFI_LOAD_ERROR` for a bad payload, the firmware's status otherwise.
Standard error is libaxl's (`src/stream/README.md`, *Standard error on the
console*), decided by the stub itself, by the same identity test: when
the image's Shell StdErr handle IS the Shell's console file, the line goes
through the console's ConOut splitter (the SimpleTextOut sharing `Mode`
with the Shell's console logger), so the Shell's next prompt lands below it
rather than on it -- under `>` and `|` too -- and to every StdErr device
that is not a ConOut device; otherwise (`2>`, `2> NUL`, `2>v`, no Shell
parameters, no splitter found) to `gST->StdErr`.

At 80 columns the re-read line below is exactly the row's width, and a UEFI
console wraps it before its CR LF. The stub writes its lines through libaxl's
own line writer (`axl-backend-native-lines.o`, freestanding over the boot
services it is handed; `src/stream/README.md`, *A line exactly the console's
width*), so the two re-read lines and the final one sit on consecutive rows
on every console, and a serial capture still has one line per line.

### Re-reading a corrupt payload

Why: on a server booted through BMC virtual media (the image served from a
browser), large single reads come back intermittently corrupt while reads of
64 KB or less are reliable. Measured on real hardware, fresh boots, no AXL
network code running:

| test | source | result |
|------|--------|--------|
| packed `sshd.efi --version`, loaded by the firmware's `LoadImage` in one large read | virtual media | 3 ok / 7 `packed image is corrupt` |
| the same | local USB copy | 10 / 10 ok |
| the same, an earlier session | RAM disk | 8 / 8 ok |
| `sha256sum` of the same file (64 KB reads) | virtual media | correct 5 / 5 |

The file was good; the firmware's one read of it was not, and the payload's
`raw_crc32` caught that. The firmware's read happens before any AXL code
runs, so for a packed tool launched from the Shell this re-read is the only
defence. (Every read AXL makes itself is limited to 64 KB the same way, and
AXL's driver loaders read an image themselves rather than let the firmware
read it in one call: `src/fs/README.md`, `src/util/README.md`. An UNPACKED
image has no CRC, so a bad firmware read of one runs the bad bytes.)

So when the payload in memory fails with `AXL_PACK_E_CORRUPT`, or
`AXL_PACK_E_HEADER` (the header CRC can be hit by the same glitch), the stub
reads its own FILE again and decodes the payload from that, up to twice.
Before each re-read it prints one line:

```
axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)
```

(the status text is whichever check failed). A good re-read runs the image
as usual. If both fail, the final line is the in-memory failure's, exactly as
before the retry existed; a file corrupt ON DISK therefore prints the two
re-read lines and then `axl-pack: packed image is corrupt`.

The file is found through the stub's `LoadedImage`: `DeviceHandle`'s
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, then `FilePath` opened one
`MEDIA_FILEPATH` node at a time (as EDK2's `EfiOpenFileByDevicePath` does).
The whole file is read in `Read` calls of at most 64 KB each
(`AXL_FS_READ_CHUNK`, `src/fs/axl-fs-read-chunk.h`) -- never in one, because
one large read is exactly the read that came back corrupt. Its size comes
from `GetInfo`'s `EFI_FILE_INFO` (one call, into a stack buffer with room
for a 255-character name), as the firmware's own file loader gets it --
every file system implements `GetInfo`, while `GetPosition` may be left
unsupported (AXL's own `AxlFsProvider` volumes do) -- and is capped at
`AXL_PACK_MAX_RAW`. That buffer fits a file name of up to 255 characters
(FAT's limit); for a longer name `GetInfo` fails, the re-read is not
attempted, and the stub reports the original error. The payload is located with
`axl_pack_find_payload` -- the section table, every offset bounded against
the bytes read, since the file is untrusted -- and decoded with the same
`axl_pack_unpack`. An image with no file to re-read (no `DeviceHandle` or
`FilePath`, as when it is `LoadImage`d from a buffer, or no file system on
its device) stops after the first retry line with the original error. A
file that cannot be opened or read whole counts as one failed attempt, not
the end of the retries: the media that spoiled the first read can spoil a
later one. Every buffer and file handle is released on every path.

What it costs: 1,024 bytes of RELEASE stub on both architectures (the table
below), which every wrapped image pays; the 64 KB read loop costs nothing
more there than a single `Read` did. At the 16 KB / 20% rule that moved
`axbench` and `sed` (x64) and `hexdump` (aa64) from wrapped to raw; the
RELEASE tool set grew 3,034,624 -> 3,090,432 B (x64) and 2,756,670 ->
2,805,310 B (aa64), and x64 RELEASE `sshd.efi` 235,008 -> 236,032 B. Three
strings live in `.data` rather than `.rodata` on purpose: the stub's
`.rodata` sits exactly on a FileAlignment step, and one more byte there is
512 more in every wrapped image.

Tested in `test-efi-pack-qemu.sh` with a TEST build of the stub
(`AXL_PACK_STUB_TEST_CORRUPT_FIRST`, linked only into `pack-fixture-rt.efi`
through `axl_wrap_efi`'s `STUB` option): it flips one payload byte in a pool COPY
before the first decode only, so the file on disk is good -- the shape of
the field failure. The shipped stub carries no such seam. Cases: `retry`
(one re-read line, then the image's exact output), the buffer launch (one
line, the original error, the launcher carries on), and `corrupt`, the
on-disk control (two re-read lines and the final one).

The PE module name in `.dbgdir` is `axl-pack-stub`, so a crash dump names the
stub and the image it loaded differently. App and driver variants are one
object; only the PE subsystem differs (10 / 11).

Firmware code decodes with ITS OWN architecture's branch converter only:
the stub and libaxl's in-process loaders link the same `axl-pack-format.o`,
whose decoder references only the native converter, so `--gc-sections` drops
the other from every image. `axl_pack_unpack` refuses any other filter as
`AXL_PACK_E_HEADER` before allocating or decoding (`filter` case: an x64
payload relabelled ARM64 with a valid header CRC prints
`axl-pack: packed image header is invalid`; AxlTestPack asserts the same
refusal, and that it allocates nothing). The ENCODER keeps both filters
everywhere, so either architecture's payload can be built anywhere. The host
packer is the one decoder built with both (`-DAXL_PACK_ALL_DECODERS`), and
`test-efi-pack-host.sh` round-trips branch-dense images of both machines
through `axl-efi-pack --verify`, so each converter's decode is proven on the
host whichever architecture's firmware never links it.

A file whose `.axlpack` section does not start with the pack magic is not a
packed image: `axl_pack_unpack` answers `AXL_PACK_E_NOT_PACKED`, and AXL's
loaders treat the file as raw, which is the intended reading of "loaders
decide by content". The firmware then loads what the file really is -- for
a file that is the stub, the stub, which refuses its payload with one line
(`axl-pack: not a packed image`). Only a section that IS a packed image and
fails a check is refused by the loaders themselves.

### Secure Boot

The stub and AXL's in-process loaders both hand the firmware the UNPACKED
inner image, and `LoadImage` authenticates that image on its own. A
signature on the wrapped `.efi` covers only the stub (and the in-process
loaders never load the stub, so for them it covers nothing): under Secure
Boot a wrapped image runs only if its inner image was signed before it was
packed. Neither the build's wrap nor `axl-cc --compress` signs anything; the
shipped release tools are unsigned, so nothing changes for them. The same
holds for a payload the stub RE-READ from its file (above): those bytes are
not the ones the firmware authenticated as the stub, and they do not need to
be -- the inner image they decode to goes through `LoadImage`'s own check.

### Size

The stub linked with an empty payload (`<tree>/build/axl-pack-stub-{app,drv}-empty.efi`,
built by the `axl-pack-stub` goal, `scripts/axl-build.sh axl-pack-stub`). This
is the number the wrap passes to `axl-efi-pack --stub-size`.

| build | app | driver |
|-------|-----|--------|
| x64 RELEASE  | 22,016 | 22,016 |
| x64 DEBUG    | 26,112 | 26,112 |
| aa64 RELEASE | 21,518 | 21,518 |
| aa64 DEBUG   | 25,614 | 25,614 |

(The x64 stub's .rodata sits exactly on a FileAlignment step, so the line
writer builds the few characters it writes on its own on the stack: as
string literals they took it to 22,528 B, and `grep.efi` -- whose wrap
saves little more than the stub costs -- then shipped raw. Before a break
the devices disagree on went through the console: 22,016 / 26,112 /
21,006 / 24,590. When the console line writer,
`axl-backend-native-lines.o`, first joined the stub, before it judged the
wrap per device: 20,480 / 24,064 / 19,982 / 23,054. Before it joined: 18,944 / 21,504 / 18,446 / 21,006. Before standard error went through
ConOut: 17,920 / 20,992 / 17,934 / 20,494. Before the re-read: 16,896 /
19,968 / 16,910 / 18,958.)

x64 files are in 512-byte steps (FileAlignment), so a few dozen bytes of code
can move the size by 512 or not at all.

The x64 stub carries no relocations; the aa64 stub has three
`R_AARCH64_RELATIVE` (so check-reloc-coverage has something to check there).

## axl_wrap_efi

`axl_wrap_efi(<target> RAW <raw.efi> OUT <final.efi> [FORCE] [STUB <obj>...])`
(`cmake/AxlImages.cmake`; the steps run in `cmake/AxlWrapEfi.cmake`). It runs
`axl-efi-pack` with the measured stub size; on exit 0 it embeds the `.pack`
and links the stub variant RAW's PE subsystem selects, on exit 3 it copies
RAW to FINAL, on anything else the build fails. Configuring with
`-DAXL_PACK=OFF` always copies (debugging a wrapped tool); the mode is part of
each wrap's command line, so toggling it reruns the wraps and nothing else.
`FORCE` wraps regardless of the saving (tests); `STUB` swaps the stub's
objects (the retry-test stub). FINAL is only ever replaced by a rename after
every step succeeded, so a failed wrap leaves the previous FINAL, still out of
date. Intermediates go under `<tree>/pack/`; nothing is written beside FINAL,
whose `.so` stays the inner image's symbols.

## axl-cc --compress

A consumer's copy of `axl_wrap_efi`, for images built with the installed SDK
(CMake: `COMPRESS` on `axl_add_app` / `axl_add_driver`). Same decision, same
exit codes, same atomic rename; the stub size is measured by linking the stub
with an empty payload in the same invocation. The linked image is kept as
`<out>.raw.efi`, `<out>.so` holds its symbols, and with `--service` only the
launcher is packed. The stub objects ship prebuilt in the SDK's
`lib/axl/<arch>/pack/` (`axl-pack-stub.o`, `axl-pack-format.o`,
`axl-backend-native-lines.o`, `LzmaDec.o`, `Bra.o`, `Bra86.o`) beside
`bin/axl-efi-pack`.
`gate.check-flag-parity` resolves both paths' stub objcopy `-j` lists and
`axl-efi-pack` arguments and fails when they differ
(`test-flag-parity-gate.sh`); `test-axl-cc-compress-qemu.sh` boots the result.

## The shipped set

Every image the release stages from `tools/` -- `TOOL_NAMES`,
`TOOL_EXTRA_APPS` and `kbtune-drv`, named by `SHIPPED_TOOL_NAMES` -- links to
`<tree>/tools/raw/<n>.efi` and ships as `<tree>/tools/<n>.efi` through
`axl_wrap_efi`, in DEBUG and RELEASE alike, so every integration test that
boots a tool boots what ships. The `.so` stays at `tools/<n>.so`, beside the
shipped name, and holds the RAW image's symbols. Anything
that inspects an image's own bytes reads `tools/raw/` (test-tls-strippable,
test-build-id). `crashhandler` is not in the set: it is not staged by the
release. Embedded intermediates (`<tree>/build/*-dxe.efi`) stay raw.

The `release-drivers` goal does the same for the release's `drivers/<arch>/`
set: every `third_party/edk2/<Name>-<arch>.efi` as `<Name>.efi`, into
`<tree>/release-drivers/<arch>/`. The EDK2 drivers are smaller than the
stub and ship as copies, so no shipped driver is wrapped. (The bundled iPXE
driver, which wrapped -- x64 1,149,440 -> 474,624 -- was removed on
2026-10-21.)

The `tool-sizes` goal prints one `<name> raw <n> shipped <m>` line per shipped
image and a `total raw <n> shipped <m>` line ("raw" is `tools/raw/<n>.efi`).

### Sizes, both architectures

RELEASE, measured 2026-10-22, after standard error moved to the console
splitter (`tool-sizes`); "(raw)" where the rule kept the raw image. The
stub is 18,944 B (x64) and 18,446 B (aa64), +1,024 B and +512 B over the
stub before that change. aa64 images compress further (`sshd`: 34% of raw
against 39% on x64), so more of them clear the 16 KB / 20% rule: 33 of 42
wrap on aa64, 23 of 42 on x64. No tool's wrap decision changed with the
larger stub (the same 23 and 33 wrap with the stub before it); the shipped
totals grew 3,100,160 -> 3,158,528 B (x64) and 2,798,142 -> 2,844,734 B
(aa64), the stub and libaxl's error-console routing together.

| tool | x64 raw | x64 shipped | aa64 raw | aa64 shipped |
|------|--------:|------------:|---------:|-------------:|
| crashtest | 48,128 | 48,128 (raw) | 51,726 | 51,726 (raw) |
| ata | 51,200 | 51,200 (raw) | 54,286 | 54,286 (raw) |
| nvme | 51,200 | 51,200 (raw) | 53,774 | 53,774 (raw) |
| scsi | 51,200 | 51,200 (raw) | 54,286 | 54,286 (raw) |
| kbtune-drv | 51,712 | 51,712 (raw) | 54,798 | 54,798 (raw) |
| tr | 54,784 | 54,784 (raw) | 58,382 | 58,382 (raw) |
| find | 55,296 | 55,296 (raw) | 58,382 | 58,382 (raw) |
| timetest | 55,296 | 55,296 (raw) | 58,894 | 58,894 (raw) |
| paste | 56,832 | 56,832 (raw) | 60,942 | 60,942 (raw) |
| hexdump | 58,368 | 58,368 (raw) | 61,454 | 45,070 |
| clip | 58,880 | 58,880 (raw) | 62,990 | 46,606 |
| sysinfo | 59,392 | 59,392 (raw) | 62,990 | 45,582 |
| smart | 59,904 | 59,904 (raw) | 64,014 | 46,094 |
| dmidecode | 60,928 | 60,928 (raw) | 64,014 | 46,094 |
| cat | 61,440 | 61,440 (raw) | 65,038 | 46,606 |
| cut | 63,488 | 63,488 (raw) | 68,110 | 47,118 |
| axbench | 67,072 | 67,072 (raw) | 70,158 | 48,654 |
| fwtool | 67,584 | 67,584 (raw) | 70,158 | 49,166 |
| sed | 69,120 | 69,120 (raw) | 73,230 | 50,702 |
| grep | 71,168 | 54,272 | 75,278 | 50,702 |
| lsacpi | 73,728 | 56,320 | 77,838 | 52,750 |
| i2c | 74,240 | 54,784 | 62,990 | 45,070 |
| md5sum | 74,240 | 55,808 | 78,862 | 52,750 |
| sha256sum | 74,240 | 55,808 | 78,862 | 52,750 |
| rndisfix | 83,968 | 60,928 | 87,054 | 55,822 |
| lsusb | 86,528 | 61,952 | 91,662 | 57,358 |
| ipmi | 93,696 | 62,976 | 92,686 | 55,822 |
| lsproto | 102,912 | 62,464 | 106,510 | 59,406 |
| lspci | 104,448 | 67,584 | 110,094 | 62,478 |
| memspd | 107,008 | 72,192 | 98,318 | 59,918 |
| tar | 112,128 | 75,776 | 117,262 | 69,646 |
| kbtune | 114,176 | 76,288 | 118,798 | 70,670 |
| mkrd | 133,632 | 82,432 | 145,934 | 75,278 |
| netinfo | 142,848 | 84,480 | 147,982 | 77,326 |
| fbcon | 157,696 | 82,944 | 163,854 | 76,302 |
| netload | 195,072 | 110,592 | 203,790 | 100,878 |
| fetch | 288,256 | 153,600 | 296,462 | 138,254 |
| rfbrowse | 307,712 | 162,816 | 317,454 | 146,446 |
| mkfixture | 373,760 | 190,976 | 381,454 | 170,510 |
| 9p | 443,904 | 132,096 | 461,838 | 119,310 |
| sshd | 616,960 | 239,616 | 638,478 | 218,126 |
| **total** | 4,934,144 | 3,158,528 | 5,121,086 | 2,844,734 |

At the end of P4g, before the stub's re-read, 39 tools shipped
x64 4,466,688 -> 2,866,688 B (35.8% smaller) and aa64 4,642,850 ->
2,615,842 B (43.7% smaller) against the plain images they replaced, and
`sshd.efi` x64 shipped at 201,728 B against the 204,800 B target. It was
239,616 B when the table was measured and is 250,368 B now (the per-launcher
standard error, the raw-stdio fix, the console line writer that keeps a
line exactly the console's width from leaving an empty row -- in the stub
too -- and the embedded pre-UEFI-2.4 IPv4-config driver, +3,584 B), and
`test-efi-pack-release-qemu.sh` fails above the 250,368 B limit:
raised from 215,040 B when `sshd` gained its start-up
diagnostics, `-v` and `LogLevel`; from 219,136 B and 235,520 B when it
gained netload's driver sweep with its companion-driver loader; then with
the stub's re-read, the driver loader's chunked reads, the serial log port
the error-console routing and the console line writer, each by its measured
512 B steps.

A raw image is not always the image that shipped before. Every image that
links AXL's driver loaders now carries the in-process unwrap (the LZMA
decoder, this architecture's branch converter and the format module, about
12 KB on x64): ten x64 tools grew by 11-13 KB raw, and `9p` (a launcher plus
an embedded server driver, both linking the loaders) by 38,912 B. `sshd`
shrank 17,920 B raw despite carrying it twice, because AxlPk's ECDSA/RSA
provider became opt-in and `sshd` uses only Ed25519 (CHANGELOG, Breaking).
Every one of those images wraps, so all of them ship smaller than before.

Drivers (`release-drivers`, the release's `drivers/<arch>/`):

| driver | x64 raw | x64 shipped | aa64 raw | aa64 shipped |
|--------|--------:|------------:|---------:|-------------:|
| NetworkCommon.efi | 13,632 | 13,632 (raw) | 24,576 | 24,576 (raw) |
| RamDiskDxe.efi | 32,896 | 32,896 (raw) | 40,960 | 40,960 (raw) |
| UsbCdcEcm.efi | 7,680 | 7,680 (raw) | 20,480 | 20,480 (raw) |
| UsbCdcNcm.efi | 8,384 | 8,384 (raw) | 20,480 | 20,480 (raw) |
| UsbRndis.efi | 12,160 | 12,160 (raw) | 24,576 | 24,576 (raw) |

At the end of P4g, DEBUG builds wrapped more (35 of 39 x64, 39 of 39
aa64), as DEBUG images are larger: x64 DEBUG shipped 5,698,048 -> 3,080,704
B, aa64 DEBUG 5,884,962 -> 2,699,810 B (raw -> shipped).

## The release, run as a user runs it

`test-efi-pack-release-qemu.sh` (both architectures) stages the RELEASE
`tools/*.efi`, `share/*.json5` and `release-drivers/<arch>/` the way
release.yml lays out the tools tarball, onto a USB stick attached READ-ONLY
(as BMC virtual media often is; `TEST_DISK_READONLY=1` in common-test.sh), with
a second, writable USB disk for `>` redirection. It runs the wrapped `grep`
(args, `> fs1:\...`, `%lasterror%`), `lspci` (its `pci-ids.json5` beside it),
`9p` and `netload` (`--version`, usage), and `sshd` (a key login from the
default `authorized_keys` on its own read-only volume, a password login,
`sshd stop`). A force-wrapped `crashtest.efi` faults under
the crash handler: the report lists two `crashtest.efi` images (the stub and
the original, one file path), names the faulting one with the ORIGINAL's base
and size, and `rsod-decode.py --syms tools/crashtest.so` resolves the frame to
`trigger_invalid_opcode`. The decoder takes that base from the report's
`Image:` line (the faulting image's); a decoder that resolved the name to the
stub's entry instead fails the test (a sabotage did, at frame #0).
