#!/bin/bash
# measure-printf-size.sh — what does a formatter COST, in bytes, on this target?
#
# Backs docs/AXL-Libc-Substrate-Design.md §4.1 and §4b. §4.1 answered "does
# newlib's printf reintroduce the Log -> Data cycle?" by linking each candidate
# and reading the result; §4b asks the same of picolibc, and llvm-libc is
# queued behind it. Three measurements of one shape, so: a script.
#
# THE RIG. Compile a probe that calls exactly one formatter, link it -nostdlib
# with `-e probe --gc-sections` so the formatter's own closure is the only
# thing retained, and report .text/.rodata/.bss plus which archive members the
# linker had to pull in. The member list is the real finding -- §4.1's verdict
# did not turn on newlib's printf being large, it turned on it dragging the
# allocator (and therefore AXL's allocator, and therefore AxlLog's own
# dependency) in behind it.
#
# WHY IT RE-MEASURES EVERY ROW instead of quoting §4.1's. That rig was never
# committed and does not fall out of the obvious reconstructions -- its
# AxlFormat figure (6,708 + 0 B, no members) cannot be reproduced here, and the
# "no members" column suggests it excluded the float path, which is not
# separable at -Os because `emit_float` inlines into `axl_vformat`. So every
# row is measured HERE, on one rig, and the table is internally consistent
# rather than half-inherited. The newlib rows land lower than §4.1's in
# absolute terms and identical in conclusion.
#
# FLAGS ARE THE CONFOUND TO WATCH. picolibc's meson default is `minsize`
# (-Os), AXL's RELEASE build is -Os and its dev build is -Og. Measuring one
# against the other flatters whichever got -Os, so this compiles the AXL
# sources ITSELF at $OPT rather than reusing out/*/build objects.
#
# Usage:
#   scripts/measure-printf-size.sh [--arch x64|aa64] [--opt -Os]
#                                  [--picolibc PREFIX] [candidate ...]
#
#   candidates: axl newlib-int newlib picolibc   (default: axl newlib-int newlib)
#   --picolibc PREFIX   picolibc install prefix (holds lib/libc.a + include/).
#   --label NAME        row label (default: the candidate name), so two
#                       picolibc prefixes do not both print as "picolibc"
#
# Building the picolibc prefixes -- run these IN A PICOLIBC CHECKOUT, the cross
# file is picolibc's own and does not exist in this repo. The triple matches
# our bare-metal cross exactly, so no custom cross file is needed:
#
#   PATH=/opt/x86_64-elf-gcc-<ver>/bin:$PATH
#   meson setup build-int --cross-file scripts/cross-coreboot-x86_64-elf.txt \
#       --prefix=<PREFIX-int> -Dformat-default=integer \
#       -Dthread-local-storage=false -Dnewlib-global-errno=true -Dtests=false
#   ninja -C build-int && ninja -C build-int install
#
#   ...and again with -Dformat-default=double into <PREFIX-dbl>.
#
# thread-local-storage=false is not optional: UEFI provides no TLS.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

ARCH="x64"
OPT="-Os"
PICOLIBC_PREFIX=""
LABEL=""
CANDIDATES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)     ARCH="$2"; shift 2 ;;
        --opt)      OPT="$2"; shift 2 ;;
        --picolibc) PICOLIBC_PREFIX="$2"; shift 2 ;;
        --label)    LABEL="$2"; shift 2 ;;
        -h|--help)  sed -n '2,45p' "$0"; exit 0 ;;
        *)          CANDIDATES+=("$1"); shift ;;
    esac
done
[[ ${#CANDIDATES[@]} -eq 0 ]] && CANDIDATES=(axl newlib-int newlib)

# The manifest, sourced — its dual sh/make KEY=VALUE form exists so scripts
# read the toolchain location instead of restating it.
# shellcheck disable=SC1091
. scripts/axl-toolchains.conf
case "$ARCH" in
    x64)  CC="${AXL_X64_GCC:-$AXL_X64_GCC_DEFAULT}" ;;
    aa64) CC="${AXL_AA64_GCC:-$AXL_AA64_GCC_DEFAULT}" ;;
    *)    echo "unknown arch: $ARCH" >&2; exit 2 ;;
esac
[[ -x "$CC" ]] || { echo "no C cross compiler at $CC — run ./scripts/install-toolchain.sh $ARCH" >&2; exit 2; }
SIZE="${CC%-gcc}-size"
READELF="${CC%-gcc}-readelf"
# Checked up front: without this, a missing binutils prints 0 0 0 for every row
# and exits 0 -- `${text:-0}` launders a tool failure into a measurement, which
# is the one thing a measurement script must never do.
for tool in "$SIZE" "$READELF"; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing $tool (binutils for $ARCH)" >&2; exit 2; }
done
GCCINC="$("$CC" -print-file-name=include)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The freestanding floor. Both spellings of each syscall: newlib's stdio
# reaches for the bare names through its *r wrappers, and needing `sbrk` AT ALL
# for the INTEGER-only entry point is §4.1's finding reproducing itself.
#
# The heaps are 16 bytes, not 4 KB. They land in .bss and are retained only by
# the rows whose libc drags sbrk, so a large array would be reported as that
# row's static state when it is really the rig's.
cat > "$WORK/stubs.c" <<'EOF'
static char heap_a[16], heap_b[16];
void _start(void) { }
void _exit(int c) { (void)c; for (;;) { } }
void *_sbrk(int i) { (void)i; return heap_a; }
void *sbrk(int i)  { (void)i; return heap_b; }
int  _close(int f) { (void)f; return -1; }
int  close(int f) { (void)f; return -1; }
int  _fstat(int f, void *s) { (void)f; (void)s; return -1; }
int  fstat(int f, void *s) { (void)f; (void)s; return -1; }
int  _isatty(int f) { (void)f; return 0; }
int  isatty(int f) { (void)f; return 0; }
int  _lseek(int f, int o, int w) { (void)f; (void)o; (void)w; return -1; }
int  lseek(int f, int o, int w) { (void)f; (void)o; (void)w; return -1; }
int  _read(int f, char *b, int n) { (void)f; (void)b; (void)n; return -1; }
int  read(int f, char *b, int n) { (void)f; (void)b; (void)n; return -1; }
int  _write(int f, const char *b, int n) { (void)f; (void)b; return n; }
int  write(int f, const char *b, int n) { (void)f; (void)b; return n; }
int  _kill(int p, int s) { (void)p; (void)s; return -1; }
int  kill(int p, int s) { (void)p; (void)s; return -1; }
int  _getpid(void) { return 1; }
int  getpid(void) { return 1; }
/* The tree builds -fstack-protector-strong -mstack-protector-guard=global, so
   every instrumented object references these. Supplying them here keeps the
   AXL row from dragging half of libaxl.a in to find their real definitions. */
unsigned long __stack_chk_guard = 0x1234;
void __stack_chk_fail(void) { for (;;) { } }
/* axl_sqrt defers to the platform's sqrt. Nothing on the format path calls it;
   this only satisfies the linker. */
double sqrt(double x) { return x; }
EOF
# -ffunction-sections/-fdata-sections here too: without them a row that needs
# ONE stub retains all of them (newlib-int measured 64 B high).
"$CC" -ffreestanding "$OPT" -ffunction-sections -fdata-sections \
    -c "$WORK/stubs.c" -o "$WORK/stubs.o"

# AxlFormat's true closure. axl-format.c alone does not link: the float path
# calls axl_isnan/axl_isinf (axl-math.c) and axl_dtoa (axl-dtoa.c), and at -Os
# emit_float inlines into axl_vformat, so --gc-sections cannot drop it. There
# is therefore no integer-only AxlFormat to measure -- which is itself a
# finding when the comparison is against a libc that offers one as a switch.
# axl-str.c is here for axl_vsnprintf itself -- the public "format into a
# buffer" entry the libc rows are measured at. -ffunction-sections plus
# --gc-sections keep only it and its callees, not the rest of the string module.
AXL_SOURCES=(src/format/axl-format.c src/math/axl-math.c src/format/axl-dtoa.c
             src/data/axl-str.c)

probe_src() {
    case "$1" in
        axl) cat <<'EOF'
/* axl_vsnprintf, not axl_vformat: every libc row measures "format into a
   buffer", and axl_vformat is one layer below that. Measuring the engine
   against their full entry point understated AXL by the adapter (109 B). */
#include <stdarg.h>
#include <stddef.h>
#include <axl/axl-str.h>
char probe_buf[256];
int probe(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = axl_vsnprintf(probe_buf, sizeof(probe_buf), fmt, ap); va_end(ap); return n; }
EOF
            ;;
        newlib-int) cat <<'EOF'
#include <stdarg.h>
#include <stdio.h>
char probe_buf[256];
int probe(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vsniprintf(probe_buf, sizeof(probe_buf), fmt, ap); va_end(ap); return n; }
EOF
            ;;
        newlib|picolibc) cat <<'EOF'
#include <stdarg.h>
#include <stdio.h>
char probe_buf[256];
int probe(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vsnprintf(probe_buf, sizeof(probe_buf), fmt, ap); va_end(ap); return n; }
EOF
            ;;
        *) echo "unknown candidate: $1" >&2; exit 2 ;;
    esac
}

measure() {
    local name="$1"
    # Two statements, not `local name=.. out=..$name`: bash expands every
    # word of a `local` before performing any assignment, so $name is still
    # unset there and `set -u` aborts.
    local out="$WORK/$name"
    mkdir -p "$out"
    probe_src "$name" > "$out/probe.c"

    # -ffunction-sections/-fdata-sections because the REAL build uses them and
    # --gc-sections below is only as fine-grained as they make it. Without
    # them the AXL row retained all of axl-math.o (6,632 B) to satisfy two
    # predicates, overstating AxlFormat by half. The libc rows are archives
    # whose members are already per-function, so this only moves the AXL row.
    local cflags=(-ffreestanding "$OPT" -fno-builtin -ffunction-sections -fdata-sections)
    local objs=() libs=()
    case "$name" in
        axl)
            # The ABI/codegen half of CFLAGS_BASE (Makefile). AXL pays for these
            # in every shipped object, so a row measured without them is not
            # measuring what AXL costs: -fpic, the frame pointer and the stack
            # protector together add ~160 B here. The libc rows cannot be given
            # the same treatment -- they are prebuilt archives carrying whatever
            # their own build chose -- and that asymmetry is inherent to
            # comparing our source against their binary.
            cflags+=(-std=gnu2x -DAXL_ALLOW_UEFI -DAXL_BACKEND_NATIVE
                     -fshort-wchar -fno-math-errno -fno-trapping-math
                     -fno-omit-frame-pointer -fpic
                     -fstack-protector-strong -mstack-protector-guard=global
                     -I include -I src/backend)
            local src
            for src in "${AXL_SOURCES[@]}"; do
                "$CC" "${cflags[@]}" -c "$src" -o "$out/$(basename "$src" .c).o"
                objs+=("$out/$(basename "$src" .c).o")
            done
            ;;
        picolibc)
            [[ -n "$PICOLIBC_PREFIX" ]] || { echo "  picolibc: needs --picolibc PREFIX" >&2; return 1; }
            # -nostdinc + the compiler's OWN include dir: gcc has no
            # -nostdlibinc (that is clang's), and plain -nostdinc would take
            # stdarg.h/stddef.h away too.
            cflags+=(-nostdinc "-isystem$GCCINC" "-isystem$PICOLIBC_PREFIX/include")
            libs=("-L$PICOLIBC_PREFIX/lib" -lc)
            ;;
        *)  libs=(-lc) ;;
    esac

    "$CC" "${cflags[@]}" -c "$out/probe.c" -o "$out/probe.o" 2>"$out/cc.err" || {
        echo "  $name: COMPILE FAILED"; sed 's/^/      /' "$out/cc.err" | head -5; return 1; }

    # -e probe: nothing references the probe, so without making it the entry
    # symbol --gc-sections collects the whole formatter and reports ~160 bytes
    # of stubs as the answer.
    "$CC" -ffreestanding -nostdlib -Wl,--gc-sections -Wl,-e,probe \
        "$out/probe.o" "$WORK/stubs.o" ${objs[@]+"${objs[@]}"} ${libs[@]+"${libs[@]}"} \
        -o "$out/probe.elf" -Wl,-Map="$out/map.txt" 2>"$out/ld.err" || {
        echo "  $name: LINK FAILED"; sed 's/^/      /' "$out/ld.err" | head -8; return 1; }

    local text rodata data bss image members alloc
    text=$("$SIZE" -A "$out/probe.elf" | awk '$1==".text"{print $2}')
    rodata=$("$SIZE" -A "$out/probe.elf" | awk '$1==".rodata"{print $2}')
    data=$("$SIZE" -A "$out/probe.elf" | awk '$1==".data"{print $2}')
    bss=$("$SIZE" -A "$out/probe.elf" | awk '$1==".bss"{print $2}')
    # IMAGE = every SHF_ALLOC section that occupies file bytes (NOBITS, i.e.
    # .bss, excluded and reported separately). Three hand-picked section names
    # missed newlib-int's .data (2,480 B) and .eh_frame (1,136 B) entirely, and
    # the AXL-vs-picolibc verdict turns on a margin far smaller than that. §4.1
    # counted a data column for the same reason.
    image=$("$READELF" -S -W "$out/probe.elf" | python3 -c "
import sys, re
total = 0
for line in sys.stdin:
    m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S*)', line)
    if not m:
        continue
    name, typ, _addr, _off, size, flags = m.groups()
    if typ == 'NOBITS' or 'A' not in flags:
        continue
    total += int(size, 16)
print(total)
")

    # Members pulled from an ARCHIVE, per §4.1. The verdict reads MEMBER NAMES,
    # not the symbol table: the rig's own stubs define _sbrk/sbrk, and an
    # nm-based check calls that "the libc dragged an allocator in".
    #
    # dtoa/mprec are deliberately NOT in the pattern. They are newlib's
    # allocating float path, but picolibc's dtoa is Ryu, which allocates
    # nothing -- matching on the name reported picolibc as YES when nm shows no
    # malloc-family symbol anywhere in its image. Name the allocator, not its
    # neighbours.
    # Collected ONCE into a variable, then tested. Piping straight into
    # `grep -q` under `set -o pipefail` reports the opposite of the truth:
    # grep -q exits at the first match, the upstream grep dies of SIGPIPE, and
    # the pipeline's non-zero status turns a HIT into "no allocator".
    local member_list
    member_list=$(grep -oE 'libc\.a\(([^)]+)\)' "$out/map.txt" | sort -u || true)
    members=$(printf '%s' "$member_list" | grep -c . || true)
    if printf '%s' "$member_list" | grep -qiE 'malloc|calloc|realloc|free|sbrk|reent|impure'; then
        alloc="YES"
    else
        alloc="no"
    fi

    printf '  %-22s %7s %8s %6s %6s %8s %5s   %s\n' \
        "${LABEL:-$name}" "${text:-0}" "${rodata:-0}" "${data:-0}" "${bss:-0}" \
        "${image:-0}" "$members" "$alloc"
}

echo "=== formatter size on $ARCH at $OPT ($(basename "$CC")) ==="
printf '  %-22s %7s %8s %6s %6s %8s %5s   %s\n' \
    "formatter" ".text" ".rodata" ".data" ".bss" "IMAGE" "libc" "allocator"
rc=0
for c in "${CANDIDATES[@]}"; do
    measure "$c" || rc=1
done
exit $rc
