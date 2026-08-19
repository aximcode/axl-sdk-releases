# How small can a UEFI image be?

The measurement behind `sdk/examples/hello-minimal.{c,cpp}` — a supported
example, compile-gated by `make check-examples` and booted on both arches by
`test-hello-minimal-qemu.sh`.

**It is not the shape a normal consumer wants.** Reach for `AXL_APP` first.
This is the reference for a *command-named launcher that must be tiny*, it
hand-declares firmware structures that ordinary code must not copy, and the
price is listed at the bottom.

## The question

If a consumer splits a tool into a thin launcher plus a resident driver, the
launcher's own logic is one function call — so why is a real one 70,731 bytes
(`do.efi`, shipped)? And how small could it be?

## The numbers

x64, RELEASE, images stripped:

| image | bytes | notes |
|---|--:|---|
| `hello-minimal-c.efi` | **4,608** | prints its first argument |
| `hello-minimal-cxx.efi` | **4,608** | identical — C++ costs nothing here |
| do-nothing `main`, full runtime | 36,864 | was 37,376 before the log seam |
| do-nothing `main`, `--minimal-runtime` | **30,720** | was 36,864 |
| smallest AXL app (`hello.efi`) | 47,616 | the AXL floor |
| real thin launcher (`do.efi`) | 70,731 | floor + resolution machinery |

aa64 is 5,134 for both. All four boot under OVMF/AAVMF and print their
argument.

## Why `--minimal-runtime` still cannot get there

Not a missing flag — real dependencies. A do-nothing `--minimal-runtime` image
pulls **51 archive members**, including SHA-256 and the driver module. The
first edge is now broken; the rest are not.

### The edge that was broken: the log layer (−6,144 B x64, −5,632 aa64)

`ld -Map --cref` named this chain, and it is worth reading as a lesson in how
a map misleads:

```
axl-crt0-minimal.o   --(gBS)-->          axl-backend-native.o
axl-backend-native.o --(axl_log_full)--> axl-log.o
axl-log.o            --(axl_strlen)-->   axl-str.o --> axl-format.o ...
```

That reads as "the backend logs, so fix the backend" — and fixing the backend
would have saved **zero bytes**. The map names the FIRST puller in link order,
not the only one: `nm` over the 51 members finds **27** carrying a strong
`U axl_log_full`, among them `axl-mem.o`, whose own root is a porting object
and not the backend at all. Library-wide the count is 682 call sites in 140
files.

So the seam went where the callers are not: `axl_log_full` / `axl_log` are
trampolines in `axl-log-emit.o` forwarding to a **weak** `_axl_log_vdispatch`
defined in `axl-log.o`, and a link asks for the engine with
`-u _axl_log_vdispatch`. Zero call sites changed. See `src/log/README.md`.

The full runtime got 512 bytes smaller too: the emitters and the engine no
longer share an object, so `--gc-sections` can drop whichever of `axl_log` /
`axl_log_full` the image does not call.

**Where the bytes actually are**, by `nm --print-size` diff of the two links
rather than by subtracting totals — 5,232 bytes of `.text`+`.rodata` across 27
symbols, which the PE's 4 KB section alignment rounds to the 6,144 the `.efi`
reports:

| symbol | bytes |
|---|--:|
| `_axl_log_vdispatch` (the dispatcher inlines into it) | 1,713 |
| `axl_log_init_from_env` | 714 |
| `axl_backend_clock_gettime` | 498 |
| `axl_log_set_domain_level` | 294 |
| `axl_utf8_to_ucs2_buf` | 286 |
| `axl_backend_get_time`, `civil_to_unix_seconds`, `axl_time_realtime`, … | 652 |
| level/colour tables, `buf_write`, 19 smaller symbols | 1,075 |

Note what is NOT in that list: the **printf engine stays**. `axl_vformat` (4,087
B), `axl_dtoa` and `kCachedPowers` were reported as log-layer weight because
`axl-log.o` was the first thing in the map to reference them — but `axl-str.o`
references `axl_vformat` on its own account and is pulled by a porting object,
so it survives. Only `axl-log.o` and `axl-time.o` leave the member list.
Member count is a poor proxy for bytes in both directions here.

### What still roots the remaining ~30 KB

Measured after the log fix, on the same do-nothing image:

| root | what it drags |
|---|---|
| `axl-backend-native.o --(axl_protocol_install)--> axl-driver.o` | the driver module, and behind it `axl-app.o`, `axl-atexit.o`, `axl-array.o`, `axl-cxxabi.o`, `axl-attempt.o`, `axl-nvstore.o`, `axl-var.o` |
| `axl-cxxrt-stubs.o --(axl_stdout / axl_fopen / axl_file_info)--> axl-stream.o, axl-stream-file.o, axl-fs.o` | and from `axl-fs.o`, `_axl_poll_break` reaches `axl-runtime.o` -> the loop, event, wait, registry and pubsub modules |
| `axl-cxxrt-alloc.o --(axl_free_impl / axl_getenv / axl_mem_largest_free_run)--> axl-mem.o, axl-env.o, axl-mem-region.o` | and `axl-sys.o -> axl-digest.o` brings MD5, SHA-1 and SHA-256 |

The `axl_protocol_install` edge is a genuine functional dependency (the
dispatch token and the stdio bridge), not a logging accident. Dropping
`$(PORTING_OBJS)` was measured as no help *before* the log fix; that
measurement was confounded by the log layer and is worth redoing.

Two things that were tried and did not move the number, so that nobody pays for
them twice: dropping `$(PORTING_OBJS)`, and weak-linking the CRT0's own calls
(the pull was never from the CRT0).

## Three things the spike cost that estimating would have missed

1. **~1 KB of PE bootstrap is not optional.** Pointing `ld -e` straight at C
   gets `Script Error Status: Invalid Parameter` from the shell. The image
   still needs AXL's assembly CRT0 + `axl-reloc.o`: `.bss` clear and walking
   `DT_RELA`, because AXL's relocations are ELF-style and firmware does not
   process them.

2. **UEFI is the MS x64 ABI.** The entry point *and every firmware function
   pointer* need `ms_abi` on x86-64. It fails at run time with a bare
   `#GP` and no symbols — the struct offsets were already correct and it still
   faulted, because the System Table was read from `%rsi` (SysV arg 2) when the
   firmware had put it in `%rdx`. AArch64 has one convention, so an aa64-only
   spike would look fine and break on x64.

3. **A wrong offset is not reliably a crash.** Three of six hand-computed
   offsets were wrong. The C build faulted; the C++ build *hung* on
   `call *0x140(%rax)` — `BootServices+320` instead of `+152`. Offsets now come
   from `offsetof` against the generated headers and are pinned with
   `static_assert`.

## What C++ costs at this size: nothing, under three rules

Byte-identical to C, provided:

- **no global with a non-trivial constructor** — there is no runtime to walk
  `.init_array`, so it would be registered and never run. The build asserts the
  section is empty rather than trusting the source.
- **no libstdc++/libsupc++**, no `operator new`, nothing that throws.
- **no static object with a destructor**, which is what reaches `__cxa_atexit`
  and `__dso_handle`.

Within those, C++ buys type-safe wrappers over the raw firmware structs at zero
runtime cost.

## What you give up

No `axl_printf` or formatting; no UTF-8 (LoadOptions is UCS-2 and stays so); no
argv splitting, quoting or shell-parameter protocol; no heap, leak tracking,
atexit or exit-status arming; and hand-declared firmware structs that a spec or
ABI change breaks silently.

A launcher built this way would additionally need to install the stdio bridge —
shell redirection applies to the *launcher's* image, so a resident driver needs
the launcher's handles passed across (`src/backend/axl-stdio-bridge.h`).

## Build

```sh
make hello-minimal                       # x64, DEBUG
make ARCH=aa64 BUILD=RELEASE hello-minimal
./test/integration/test-hello-minimal-qemu.sh --arch X64
```

Deliberately not in `all`: it is a reference for one specific shape, not
something every build needs. `check-examples` compiles both sources like any
other example; the boot test is what covers the three failure modes a compile
cannot see.
