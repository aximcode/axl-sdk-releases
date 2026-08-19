/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * hello-minimal.cpp — the same app in C++, priced against the C one.
 * Companion to hello-minimal.c; see that file for why the spike exists and
 * what it gives up.
 *
 * THE C++ QUESTION AT THIS SIZE is not "does it work" but "what does the
 * language pull in that C does not". Three things could, and this file is
 * arranged so the measurement separates them:
 *
 *   1. `.init_array` — a global with a non-trivial constructor needs the
 *      walker to have run. There is no AXL runtime here, so NOTHING runs it:
 *      this file therefore uses only constant-initialised globals, and the
 *      build asserts `.init_array` is empty rather than trusting that. That is
 *      the same class of silent failure as the driver `.init_array` defect
 *      (a constructor that is registered and never called), so it is checked,
 *      not assumed.
 *   2. libstdc++ / libsupc++ — not linked at all. No <string>, no <vector>,
 *      no operator new. `-fno-exceptions -fno-rtti` and nothing that throws.
 *   3. The Itanium ABI hooks (`__cxa_atexit`, `__dso_handle`) — reached only
 *      by a static object with a DESTRUCTOR, which this file also avoids.
 *
 * What C++ buys here is real but small: type-safe wrappers over the raw
 * firmware structs, `constexpr` string literals sized at compile time, and
 * scoped helpers -- with no runtime cost if the three rules above hold.
 */

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

using U64 = unsigned long long;
using U16 = unsigned short;
using U8  = unsigned char;

/* OFFSETS ARE VERIFIED, NOT GUESSED — see hello-minimal.c for the full note and
 * how they were derived (offsetof against the generated headers). The first
 * hand-computed set had three of six wrong; this file kept them one round
 * longer than the C one did and the symptom was a HANG, not a fault:
 * `call *0x140(%rax)` -- BootServices+320 instead of +152 -- jumped into
 * whatever lay there. A wrong offset is not reliably a crash. */
/* __builtin_offsetof: offsetof() without <stddef.h>, and unlike the
 * null-deref idiom it is a constant expression C++ accepts. */
#define OFFSET_OF(T, m) __builtin_offsetof(T, m)

struct SimpleTextOut {
    void *Reset;                                                  /* 0 */
    U64 (SPIKE_EFIAPI *OutputString)(SimpleTextOut *self, U16 *); /* 8 */
};

struct Guid { unsigned int d1; unsigned short d2, d3; U8 d4[8]; };

struct BootServices {
    U8    pad[152];
    U64 (SPIKE_EFIAPI *HandleProtocol)(void *handle, Guid *proto, void **iface);
};

struct SysTab {
    U8             pad0[64];
    SimpleTextOut *ConOut;      /* 64 */
    U8             pad1[24];
    BootServices  *BS;          /* 96 */
};

struct LoadedImage {
    U8            pad0[48];
    unsigned int  LoadOptionsSize;  /* 48 */
    U8            pad1[4];
    U16          *LoadOptions;      /* 56 */
};

static_assert(OFFSET_OF(SysTab, ConOut) == 64, "ConOut offset");
static_assert(OFFSET_OF(SysTab, BS) == 96, "BootServices offset");
static_assert(OFFSET_OF(BootServices, HandleProtocol) == 152, "HandleProtocol offset");
static_assert(OFFSET_OF(LoadedImage, LoadOptionsSize) == 48, "LoadOptionsSize offset");
static_assert(OFFSET_OF(LoadedImage, LoadOptions) == 56, "LoadOptions offset");
static_assert(OFFSET_OF(SimpleTextOut, OutputString) == 8, "OutputString offset");

/* `constexpr`, so it is a .rodata constant with no constructor -- the C++
 * spelling that does NOT create an .init_array entry. A `const Guid` at
 * namespace scope with a braced initialiser would be equally fine; the trap is
 * only a NON-trivial constructor. */
static constexpr Guid LOADED_IMAGE_GUID = {
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
extern "C" {

/* Inside the block, not `extern "C" unsigned long x = ...` on one line: that
 * spelling means "declare with C linkage" and gcc warns that an initialiser
 * makes it a definition anyway. */
unsigned long __stack_chk_guard = 0x00000aff0d0a0000UL;

void __stack_chk_fail(void);
void __stack_chk_fail(void)
{
    /* No console guaranteed at this point; spin rather than return into a
     * smashed frame. A real launcher would reset the machine. */
    for (;;) { }
}

}   /* extern "C" */

/* A tiny type-safe wrapper -- the thing C++ actually buys at this size. It has
 * no data members beyond the pointer and no destructor, so it compiles to the
 * same code as passing the pointer around. */
class Console {
public:
    explicit Console(SimpleTextOut *out) noexcept : out_(out) { }

    void write(const U16 *s) const noexcept
    {
        out_->OutputString(out_, const_cast<U16 *>(s));
    }

private:
    SimpleTextOut *out_;
};

static int
first_arg(const U16 *cmdline, unsigned int chars, U16 *out, int out_max) noexcept
{
    int i = 0;
    while (i < static_cast<int>(chars) && cmdline[i] && cmdline[i] != ' ') { i++; }
    while (i < static_cast<int>(chars) && cmdline[i] == ' ') { i++; }
    int n = 0;
    while (i < static_cast<int>(chars) && cmdline[i] && cmdline[i] != ' '
           && n < out_max - 1) {
        out[n++] = cmdline[i++];
    }
    out[n] = 0;
    return n;
}

extern "C" U64 SPIKE_EFIAPI
_AxlEntry(void *image, SysTab *st) noexcept
{
    static U16 greet[]  = { 'h','e','l','l','o',',',' ', 0 };
    static U16 nl[]     = { '\r','\n', 0 };
    static U16 nobody[] = { '(','n','o',' ','a','r','g',')', 0 };
    static U16 arg[128];

    LoadedImage *li = nullptr;
    int n = 0;

    if (st->BS->HandleProtocol(image,
                                const_cast<Guid *>(&LOADED_IMAGE_GUID),
                                reinterpret_cast<void **>(&li)) == 0
        && li != nullptr && li->LoadOptions != nullptr) {
        n = first_arg(li->LoadOptions, li->LoadOptionsSize / 2u,
                      arg, static_cast<int>(sizeof arg / sizeof arg[0]));
    }

    const Console con{st->ConOut};
    con.write(greet);
    con.write(n > 0 ? arg : nobody);
    con.write(nl);
    return 0;
}
