#!/usr/bin/env python3
"""check-libc-overlap.py — two providers of one libc name must not both be strong.

`libaxl.a` carries unprefixed libc names (`memcpy`, `strlen`, ...) because a
C-only link is `libaxl.a` and NOTHING else -- no `libc.a`, no `--start-group`.
On that link libaxl IS the libc. The `-fexceptions` C++ link is the one link
that also carries newlib, and `axl-cc` puts both archives in one group.

Two providers of the same name inside a group is decided by whichever reference
happens to be outstanding when each archive is scanned, which is not a property
anyone controls. Adding `src/cxxrt/axl-cxxrt-terminate.o` perturbed it and broke
`axl-c++ -fexceptions` for any program throwing a non-`std::exception`:

    libstdc++(eh_alloc.o) -> getenv -> libc(getenv.o) -> getenv_r.o
                                                      -> libc(strncmp.o)
                                          -> impure.o -> findfp.o
                                                      -> libc(memset.o)
    libgcc(unwind-dw2-fde.o) -> memcpy -> libaxl(axl-intrinsics.o), which ALSO
                                          defines memset          -> collision
    libgcc(unwind-dw2.o)     -> strlen -> libaxl(axl-str-compat.o), which ALSO
                                          defines four more       -> collision

Five `multiple definition` errors, on a one-line program. It linked before only
by luck, and the luck was load-bearing.

WHERE THE LINE FALLS is not a matter of taste. It falls exactly where the
RUNTIME DEPENDENCY falls:

  * A pure leaf function (`memcpy`, `strlen`) depends on no startup and holds no
    state, so it does not matter which copy runs. AXL's is a FALLBACK for links
    with no libc, and is therefore weak.

    WEAK BUYS COEXISTENCE, NOT PRECEDENCE -- do not read this gate as proving
    newlib's version is the one that runs. An archive member is still extracted
    for a weak definition, and `libaxl.a` is scanned before `libc.a`, so which
    archive supplies a name still depends on what drags each member in
    (measured: an image throwing `std::runtime_error` takes newlib's memcpy and
    strlen; one throwing a bare int takes AXL's, because libgcc's unwinder pulls
    those objects first). What weak changes is that both members can be present
    without the link failing.

  * A lifecycle hook only works if AXL's own init/teardown ran. newlib's
    versions are inert under UEFI, every one for the same structural reason --
    they assume a hosted startup that never happens. These must stay STRONG, and
    each says below why newlib's cannot serve:

        __cxa_atexit       newlib registers into its own table, drained by
                           exit(). Nothing calls newlib's exit() under UEFI, so
                           C++ static destructors would silently never run.
        __stack_chk_fail   newlib's does strlen -> write -> raise -> _exit.
                           write and raise are AXL stubs returning -1, so the
                           image halts SILENTLY, naming no frame.
        __stack_chk_guard  newlib's is BSS, filled in by __stack_chk_init, which
                           nothing calls. The canary would stay 0.

So the gate is not "these eleven are weak". It is: for EVERY name both sides
define, AXL's binding is weak unless the name is declared must-win with a
reason. A new overlapping symbol appearing by accident fails here rather than
at some future consumer's link.

"Both sides" means `libaxl.a` AND the staged `axl-cxxrt-*.o` glue objects. The
allocator bridge lives in an object rather than the archive -- an object's
definition displaces an archive member outright, which is the whole mechanism
-- so a gate reading only the archive is blind to exactly the symbols most
worth watching. It was: P1 added `_malloc_r`/`_free_r`/`_calloc_r`/`_realloc_r`
and this reported "14 overlapping, clean" without seeing one of them.

Usage: check-libc-overlap.py [--arch x64|aa64]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Names AXL must define STRONGLY even though newlib also defines them, each with
# the reason newlib's cannot serve under UEFI. A name here is asserted to be
# strong; a name NOT here is asserted to be weak. Both directions are checked,
# so this cannot be used to wave a symbol through -- moving one here forces
# whoever does it to write the reason down.
# THE PORTING LAYER IS NOT HERE ANY MORE, and its absence is the design.
#
# close/fstat/open/read/write/lseek/isatty/stat/unlink/getpid/kill/sbrk used to
# be MUST_WIN entries reading "AXL's porting layer, §4c.1". That was correct
# while AXL defined ONLY the plain names: letting newlib's win would have
# reached a syscall nobody implemented. AXL now defines the UNDERSCORE forms as
# the implementations and the plain names as WEAK aliases, so newlib's wrappers
# -- where they exist at all, which is aarch64 only -- call `_open_r` and land
# in ours. Both winners are correct, which is the point: the outcome no longer
# depends on link order.
#
# `rename` stays, and is the exception that shows the rule: newlib's falls back
# to link() + unlink() when the target has no _rename syscall, and AXL provides
# no link(). There the winner still matters.
MUST_WIN: dict[str, str] = {
    "time": (
        "mbedTLS calls time() to decide CERTIFICATE VALIDITY. AXL's reads the "
        "firmware RTC via axl_clock_gettime(AXL_CLOCK_REALTIME); newlib's "
        "reaches gettimeofday, which has no backend under UEFI -- so newlib's "
        "would silently make every certificate's notBefore/notAfter check "
        "meaningless. Became visible only when mbedTLS stopped being optional: "
        "axl-mbedtls-platform.c used to compile only under AXL_TLS, so this "
        "gate never saw the overlap"
    ),
    "__cxa_atexit": (
        "newlib registers into its own table, drained by exit(); nothing calls "
        "newlib's exit() under UEFI, so C++ static dtors would never run"
    ),
    "__stack_chk_fail": (
        "newlib's writes the diagnostic through write(), which is an AXL stub "
        "returning -1 -- the image would halt silently, naming no frame"
    ),
    "__stack_chk_guard": (
        "newlib's is BSS initialised by __stack_chk_init, which nothing calls "
        "under UEFI -- the canary would stay 0"
    ),
    # The other two symbols of newlib's stack_protector.o. AXL owned
    # __stack_chk_fail and __stack_chk_guard and NOT these, which left the
    # member pullable: an archive member comes in for ANY symbol it defines, so
    # one reference to either of these would have dragged newlib's version in
    # and multiply-defined the other two against ours. Owning the WHOLE member
    # is the rule; owning most of it is the bug.
    # libc_a-mstats.o, ALL FIVE of it. The pinned x86_64-elf newlib fills a
    # struct mallinfo of ten INT fields while its own <malloc.h> declares ten
    # size_t, so a caller reads pairs of 32-bit fields as one 64-bit and 40
    # bytes of stack after them -- fordblks came back as __stack_chk_guard.
    # AXL's mallinfo reads it through a matching declaration and widens.
    #
    # The other four are here because a member is all-or-nothing: taking only
    # mallinfo would leave a consumer calling malloc_stats to pull mstats.o and
    # multiply-define mallinfo against ours. Same rule as stack_protector.o
    # above -- owning MOST of a member is the bug.
    #
    # All five are compiled ONLY where a build-time probe says the toolchain's
    # layout is wrong (AXL_NEWLIB_MALLINFO_INT). ARM's newlib is correct, so on
    # aa64 AXL defines none of them and this gate sees no overlap at all.
    "mallinfo": (
        "newlib's fills int fields into a struct its own header declares as "
        "size_t; the numbers it returns are field-shifted garbage plus stack"
    ),
    "malloc_stats": (
        "same archive member as mallinfo (libc_a-mstats.o); leaving it to "
        "newlib makes that member pullable and multiply-defines mallinfo"
    ),
    "mallopt": (
        "same archive member as mallinfo (libc_a-mstats.o) -- see above"
    ),
    "mstats": (
        "same member; and newlib's prints through a stderr firmware never "
        "wires up, so it emitted nothing at all"
    ),
    "_mstats_r": (
        "same member; newlib's _mstats_r is defined inside mstats.o, so it "
        "cannot be forwarded to -- it has to be reimplemented with it"
    ),
    # stat(): overlaps on aa64 ONLY -- ARM's newlib ships a stat wrapper and
    # the x86_64-elf build does not, which is the same toolchain-build
    # asymmetry as the mallinfo layout. Either way AXL's must win: newlib's is
    # a wrapper over a _stat_r syscall that only AXL can service, since only
    # AXL knows what an EFI_FILE_PROTOCOL is.
    # rename(): newlib's calls _rename_r, which reaches a link/unlink pair AXL
    # does not implement -- it would fail at runtime rather than at link. AXL's
    # goes through axl_file_move, which is the only thing that can rename on a
    # FAT volume through the UEFI file protocol.
    "rename": (
        "newlib's routes through _rename_r to link()/unlink() syscalls AXL "
        "does not provide; ours goes through axl_file_move"
    ),
    "__stack_chk_fail_local": (
        "gcc's -fpic entry point for the same check; ours calls "
        "__stack_chk_fail so the diagnostic cannot drift out of step"
    ),
    "__stack_chk_init": (
        "a deliberate no-op: AXL's canary is a fixed compile-time value, "
        "already correct before any code runs. newlib's would seed it LATER, "
        "which is the ordering hazard axl-stack-guard.c documents -- frames "
        "already on the stack captured the old value"
    ),
    # The POSIX porting layer (§4c.1). These appear as overlaps on aa64 ONLY:
    # ARM's newlib ships plain<->reentrant adapters (`libc_a-syswrite.o` etc.)
    # while the x64 build ships none, so the same source is a 14-symbol overlap
    # on one arch and 26 on the other. They are must-win with a sharp
    # consequence rather than a preference: `syswrite.o`'s `write` calls
    # `_write_r`, and `writer.o`'s `_write_r` calls `write`. The platform is
    # expected to break that cycle. AXL supplies the plain side; letting
    # newlib's win would link two halves of a mutual recursion. They are also
    # the only versions that can reach UEFI at all.
    "getentropy": (
        "AXL's REFUSES rather than filling the buffer; newlib's aa64 version "
        "would answer from a source AXL cannot vouch for, and a caller asking "
        "for entropy must not receive something that merely looks like it"
    ),
    # sbrk is AXL's for a NEW reason since §2-DECISION: it is no longer a
    # fail-closed -1, it is the region newlib's dlmalloc grows into. newlib's
    # aa64 sbrk would hand dlmalloc a different heap than the one AXL manages.
}


# Leaf libc names AXL must NOT define at all (P3). These were
# `axl-str-compat.c` and `axl-intrinsics.c` -- a stand-in libc for links that
# carried none, back when a C link was `libaxl.a` and nothing else. `libc.a` is
# on every link now, so newlib owns them and a definition here would be a
# second provider competing on scan order for no reason.
#
# Distinct from MUST_WIN, and checked separately: MUST_WIN says "AXL's must
# win", this says "AXL must not have one". A name appearing here after being
# deleted means someone reintroduced a stand-in rather than fixing a missing
# link path.
FORBIDDEN: dict[str, str] = {
    n: "P3: newlib owns the leaf libc names; libc.a is on every link now"
    for n in (
        "memcpy", "memset", "memmove", "memcmp", "memchr",
        "strlen", "strcmp", "strncmp", "strchr", "strstr", "strncpy",
    )
}


def nm_defined(nm: str, archive: Path) -> dict[str, str]:
    """Map defined GLOBAL symbol -> nm type letter for @a archive.

    UPPERCASE only, and that is the whole correctness of this function. nm
    spells a local symbol with a lowercase letter, and locals do not participate
    in linking between objects at all -- the first draft accepted them and
    reported 95 "overlapping" symbols, 81 of which were `.LC0`-style string
    labels that both archives happen to name identically inside their own
    objects. A gate that loud about non-problems is one nobody reads.

    Later members win, which is irrelevant here: a name defined twice INSIDE one
    archive is a different defect, and `ar` would not have accepted it into one
    link anyway.
    """
    out = subprocess.run(
        [nm, "--defined-only", str(archive)],
        capture_output=True, text=True,
    )
    syms: dict[str, str] = {}
    for line in out.stdout.splitlines():
        # "<addr> T name" or, for a bss symbol, "<addr> B name".
        m = re.match(r"^[0-9a-fA-F]*\s+([A-Z])\s+(\S+)$", line)
        if m:
            syms[m.group(2)] = m.group(1)
    return syms


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="x64", choices=("x64", "aa64"))
    args = ap.parse_args(argv)

    # The archive a consumer actually links. Prefer the STAGED one; fall back to
    # the build tree, which `make` produces without staging.
    #
    # Absence is a FAILURE, not a skip. This gate exists to catch a link error
    # nobody sees until a consumer hits it, so a version of it that quietly
    # passes on a tree it could not read would be the exact "gate that cannot
    # see" shape it was written against -- and verify.sh does not stage, so the
    # skip would have been the NORMAL path on a fresh clone rather than an edge
    # case.
    prefix = subprocess.run(
        [str(REPO / "scripts" / "sdk-prefix.sh"), "--abs"],
        capture_output=True, text=True,
    ).stdout.strip()
    candidates = [Path(prefix) / "lib" / "axl" / args.arch / "libaxl.a"]
    built = subprocess.run(
        ["make", "-s", f"ARCH={args.arch}", "BUILD=RELEASE", "print-prefix"],
        cwd=REPO, capture_output=True, text=True,
    ).stdout.strip()
    if built:
        candidates.append(REPO / built / "lib" / "libaxl.a")
    candidates.append(REPO / "out" / f"native-{args.arch}" / "lib" / "libaxl.a")

    # The staged GLUE OBJECTS are checked alongside the archive, because the
    # allocator bridge lives there rather than in libaxl.a and is exactly the
    # two-provider case this gate exists for: `axl-cxxrt-alloc.o` defines
    # `malloc`/`free` and (since P1) `_malloc_r`/`_free_r`/`_calloc_r`/
    # `_realloc_r`, every one of which newlib also defines.
    #
    # Reading only libaxl.a made this gate blind to its own subject: P1 landed
    # four new overlapping symbols and the gate reported "14 overlapping, clean"
    # without seeing any of them.
    glue = sorted((Path(prefix) / "lib" / "axl" / args.arch).glob("axl-cxxrt-*.o"))

    libaxl = next((c for c in candidates if c.exists()), None)
    if libaxl is None:
        print(f"check-libc-overlap: FAIL ({args.arch}) — no libaxl.a to read.")
        print("    Looked in:")
        for c in candidates:
            print(f"      {c}")
        print("    Build or stage first, e.g. scripts/install.sh --arch all --cpp")
        return 1

    conf = (REPO / "scripts" / "axl-toolchains.conf").read_text()
    # The OVERRIDE first, then the default -- the same ladder every other
    # consumer uses (see test/integration/test-cxx-exceptions-qemu.sh). Reading
    # only the default would compare against a toolchain the tree is not
    # building with.
    stem = "AXL_X64_BINUTILS_PREFIX" if args.arch == "x64" \
        else "AXL_AA64_BINUTILS_PREFIX"
    prefix_tc = os.environ.get(stem)
    if not prefix_tc:
        m = re.search(rf'^{stem}_DEFAULT=\"?([^\"\n]+)\"?', conf, re.M)
        if not m:
            print(f"check-libc-overlap: FAIL — {stem}_DEFAULT not in "
                  "axl-toolchains.conf")
            return 1
        prefix_tc = m.group(1)

    nm = prefix_tc + "nm"
    gcc = prefix_tc + "gcc"
    # BOTH tools checked. Only nm was, and the gcc below is derived from the
    # BINUTILS prefix -- on a split or binutils-only install that call raised a
    # FileNotFoundError traceback instead of the skip it meant to take.
    for tool in (nm, gcc):
        if not shutil.which(tool):
            print(f"check-libc-overlap: SKIP — {tool} not installed")
            return 0

    # newlib, found the way the compiler itself would rather than by guessing a
    # path: the driver knows where its own sysroot is.
    libc = subprocess.run(
        [gcc, "-print-file-name=libc.a"], capture_output=True, text=True,
    ).stdout.strip()
    if not libc or not Path(libc).is_file():
        print(f"check-libc-overlap: SKIP — {gcc} cannot locate libc.a")
        return 0

    axl_syms = nm_defined(nm, libaxl)
    # Glue objects fold into the same map. An object's definitions displace an
    # archive member outright (that is the whole mechanism -- see axl-cc), so
    # for the purpose of "who provides this name" they belong in one set.
    for g in glue:
        axl_syms.update(nm_defined(nm, g))
    libc_syms = nm_defined(nm, Path(libc))
    overlap = sorted(set(axl_syms) & set(libc_syms))

    # A control: if the intersection is empty, something is wrong with the
    # reading, not with the tree. These two archives DO overlap by design.
    if not overlap:
        print("check-libc-overlap: FAIL — no overlap found at all, which means")
        print(f"    nm read nothing useful from {libaxl} or {libc}")
        return 1

    problems: list[str] = []

    # The FORBIDDEN set first: these must not be defined at all, weak or not.
    for sym in sorted(FORBIDDEN):
        if sym in axl_syms:
            problems.append(
                f"  {sym}: DEFINED by AXL ({axl_syms[sym]}), but newlib owns it.\n"
                f"      {FORBIDDEN[sym]}\n"
                f"      Delete the definition -- do not mark it weak."
            )

    for sym in overlap:
        if sym in FORBIDDEN:
            continue   # reported above; do not double-count as an overlap
        kind = axl_syms[sym]
        # W = weak function, V = weak object. Only these two, because
        # nm_defined already discarded every lowercase (local) letter.
        weak = kind in ("W", "V")
        if sym in MUST_WIN:
            if weak:
                problems.append(
                    f"  {sym}: is WEAK in libaxl.a, but must win — {MUST_WIN[sym]}"
                )
        elif not weak:
            problems.append(
                f"  {sym}: STRONG in libaxl.a and also defined by newlib.\n"
                f"      Mark it __attribute__((weak)) so newlib's wins where a\n"
                f"      real libc is on the link line, or add it to MUST_WIN in\n"
                f"      {Path(__file__).name} with the reason newlib's cannot serve."
            )

    if problems:
        print(f"check-libc-overlap: FAIL ({args.arch}) — "
              f"{len(problems)} of {len(overlap)} overlapping symbols")
        print("\n".join(problems))
        return 1

    print(f"check-libc-overlap: clean ({args.arch}) — {len(overlap)} overlapping "
          f"symbols, {len(MUST_WIN)} must-win, rest weak")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
