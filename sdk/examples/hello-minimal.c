/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * hello-minimal.c — the smallest useful UEFI app, in C, with no libaxl.
 *
 * Prints its single command-line argument. That is deliberately the smallest
 * task that still needs the two things a real launcher needs — console output
 * and an argument — so the number it produces is a floor a launcher could
 * actually be built on, not a do-nothing lower bound.
 *
 * WHY THIS EXISTS. `--minimal-runtime` cannot reach this size, and the reason
 * is structural rather than a missing flag. Measured with `ld -Map --cref` on
 * a do-nothing minimal-runtime image:
 *
 *     axl-crt0-minimal.o  --(gBS)-->        axl-backend-native.o
 *     axl-backend-native.o --(axl_log_full)--> axl-log.o
 *     axl-log.o           --(axl_strlen)-->  axl-str.o  --> axl-format.o ...
 *
 * The BACKEND LOGS. So every image that touches `gBS` -- which is every image
 * -- links the log layer, which links the printf engine (`axl_vformat` 4,087 B,
 * `axl_dtoa` 1,084, `kCachedPowers` 696) and the console. That is the ~34 KB
 * between a bare PE and the smallest AXL app, and no CRT0 or link flag moves
 * it: the references are real.
 *
 * So the only way to the bare-PE floor is to not link libaxl at all, which is
 * what this file does. What it costs is stated plainly at the bottom.
 *
 * NOT the shape a normal consumer wants -- reach for AXL_APP first. This is
 * the reference for a command-named launcher that must be tiny, and the
 * price is spelled out at the bottom. It hand-declares the three UEFI structures it
 * needs rather than including <uefi/axl-uefi.h>, because that header is
 * deliberately walled off from applications (AXL_ALLOW_UEFI) and pulling it in
 * would defeat the point of the measurement.
 *
 * ENTRY POINT is `_AxlEntry`, not `_start`, and the image still links AXL's
 * assembly CRT0 + axl-reloc.o. Those two are NOT part of libaxl -- they are
 * the PE bootstrap: `_start` zeroes .bss and applies the image's own
 * relocations (AXL's are ELF-style `.rela`, which firmware does not process,
 * so the image walks DT_RELA itself) and then tail-calls `_AxlEntry`.
 *
 * Skipping them does not work: a hand-linked image with `-e` pointed straight
 * at C code is REJECTED by the shell -- "Script Error Status: Invalid
 * Parameter" -- measured, not guessed. So ~1 KB of bootstrap is not optional
 * for any UEFI image, minimal or not.
 *
 *   Build:  make hello-minimal        Test: test-hello-minimal-qemu.sh
 *   Notes:  docs/AXL-Minimal-Image-Notes.md
 */

typedef unsigned long long U64;
typedef unsigned short     U16;
typedef unsigned char      U8;

/* CALLING CONVENTION, and this is the trap that costs a whole debugging
 * session if you skip it. UEFI uses the MICROSOFT x64 ABI (args in RCX, RDX,
 * R8, R9); this toolchain compiles SysV (RDI, RSI, RDX, RCX) by default. Both
 * the image entry point AND every firmware function pointer must therefore be
 * declared ms_abi on x86-64.
 *
 * It fails at RUN TIME, not compile time, and the failure is a bare
 * `#GP - General Protection` with a CPU dump and no symbols. Measured here:
 * the offsets were already correct -- the disassembly showed a correct
 * `call *0x98(%rax)` with %rax from SystemTable+0x60 -- and it still faulted,
 * because the System Table was being read from %rsi (SysV arg 2) when the
 * firmware had put it in %rdx.
 *
 * AArch64 has one calling convention, so this is a no-op there. That asymmetry
 * is itself worth knowing: an aa64-only spike would appear to work and would
 * break the moment it was built for x64.
 */
#if defined(__x86_64__)
#  define SPIKE_EFIAPI __attribute__((ms_abi))
#else
#  define SPIKE_EFIAPI
#endif

/* __builtin_offsetof: offsetof() without <stddef.h>, and unlike the
 * null-deref idiom it is a constant expression C++ accepts. */
#define OFFSET_OF(T, m) __builtin_offsetof(T, m)

/* OFFSETS ARE VERIFIED, NOT GUESSED. Derived with offsetof() against the
 * generated headers (see the note in the Makefile target); the first hand-
 * computed set had three of six wrong and the image triple-faulted at its
 * entry point with no diagnostic beyond a CPU dump. The _Static_asserts below
 * catch a typo in the padding; they cannot catch a spec change, which is
 * precisely the fragility this spike exists to price.
 *
 *   EFI_SYSTEM_TABLE.ConOut                 64
 *   EFI_SYSTEM_TABLE.BootServices           96
 *   EFI_BOOT_SERVICES.HandleProtocol       152
 *   EFI_LOADED_IMAGE_PROTOCOL.LoadOptionsSize  48
 *   EFI_LOADED_IMAGE_PROTOCOL.LoadOptions      56
 *   EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.OutputString  8
 */
typedef struct SimpleTextOut {
    void *Reset;                                        /* 0 */
    U64 (SPIKE_EFIAPI *OutputString)(struct SimpleTextOut *, U16 *); /* 8 */
} SimpleTextOut;

typedef struct { unsigned int d1; unsigned short d2, d3; U8 d4[8]; } Guid;

typedef struct BootServices {
    U8    pad[152];
    U64 (SPIKE_EFIAPI *HandleProtocol)(void *handle, Guid *proto, void **iface);
} BootServices;

typedef struct {
    U8             pad0[64];
    SimpleTextOut *ConOut;      /* 64 */
    U8             pad1[24];
    BootServices  *BS;          /* 96 */
} SysTab;

typedef struct {
    U8            pad0[48];
    unsigned int  LoadOptionsSize;  /* 48 */
    U8            pad1[4];
    U16          *LoadOptions;      /* 56 */
} LoadedImage;

_Static_assert(OFFSET_OF(SysTab, ConOut) == 64, "ConOut offset");
_Static_assert(OFFSET_OF(SysTab, BS) == 96, "BootServices offset");
_Static_assert(OFFSET_OF(BootServices, HandleProtocol) == 152, "HandleProtocol offset");
_Static_assert(OFFSET_OF(LoadedImage, LoadOptionsSize) == 48, "LoadOptionsSize offset");
_Static_assert(OFFSET_OF(LoadedImage, LoadOptions) == 56, "LoadOptions offset");
_Static_assert(OFFSET_OF(SimpleTextOut, OutputString) == 8, "OutputString offset");

static const Guid LOADED_IMAGE_GUID = {
    0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

/* The stack protector is ON by default in this tree (-fstack-protector-strong
 * -mstack-protector-guard=global), and its two hooks live in libaxl, which
 * this spike does not link. Providing them locally rather than passing
 * -fno-stack-protector is the honest choice: it keeps the safety property a
 * real launcher should not silently drop, and prices it at a few dozen bytes.
 *
 * `guard=global` means the canary is read from THIS symbol, not from %fs -- 
 * which is why a bare UEFI image can have a stack protector at all (UEFI never
 * sets up the TLS segment glibc's default would read). */
unsigned long __stack_chk_guard = 0x00000aff0d0a0000UL;

void __stack_chk_fail(void);
void __stack_chk_fail(void)
{
    /* No console guaranteed at this point; spin rather than return into a
     * smashed frame. A real launcher would reset the machine. */
    for (;;) { }
}

static void
put(SimpleTextOut *out, const U16 *s)
{
    out->OutputString(out, (U16 *)s);
}

/* Copy the FIRST whitespace-delimited token after argv[0] out of the shell's
 * UCS-2 command line. No allocation, no conversion: the console takes UCS-2,
 * which is exactly what LoadOptions already holds. That is the whole reason a
 * launcher can skip AXL's argv machinery -- the conversion to UTF-8 char** is
 * a convenience the consumer may not need. */
static int
first_arg(const U16 *cmdline, unsigned int chars, U16 *out, int out_max)
{
    int i = 0;
    /* skip argv[0] */
    while (i < (int)chars && cmdline[i] && cmdline[i] != ' ') { i++; }
    while (i < (int)chars && cmdline[i] == ' ') { i++; }
    int n = 0;
    while (i < (int)chars && cmdline[i] && cmdline[i] != ' ' && n < out_max - 1) {
        out[n++] = cmdline[i++];
    }
    out[n] = 0;
    return n;
}

U64 SPIKE_EFIAPI
_AxlEntry(void *image, SysTab *st)
{
    static U16 greet[] = { 'h','e','l','l','o',',',' ', 0 };
    static U16 nl[]    = { '\r','\n', 0 };
    static U16 nobody[] = { '(','n','o',' ','a','r','g',')', 0 };
    static U16 arg[128];

    LoadedImage  *li  = 0;
    int           n   = 0;

    if (st->BS->HandleProtocol(image, (Guid *)&LOADED_IMAGE_GUID,
                                (void **)&li) == 0
        && li != 0 && li->LoadOptions != 0) {
        n = first_arg(li->LoadOptions, li->LoadOptionsSize / 2u,
                      arg, (int)(sizeof arg / sizeof arg[0]));
    }

    put(st->ConOut, greet);
    put(st->ConOut, n > 0 ? arg : nobody);
    put(st->ConOut, nl);
    return 0;
}

/*
 * WHAT YOU GIVE UP, so the number is not read as free:
 *
 *   - No axl_printf / formatting. Console writes are raw UCS-2 strings.
 *   - No UTF-8. LoadOptions is UCS-2 and stays that way.
 *   - No argv splitting, quoting or shell-parameter protocol -- this takes the
 *     first token, and the UEFI shell's quoting rules are not honoured.
 *   - No heap, no leak tracking, no atexit, no exit-status arming.
 *   - Hand-declared firmware structs: a layout change in the spec or a
 *     different ABI breaks it silently. Real code should use the generated
 *     headers, which is exactly the trade this spike exists to price.
 */
