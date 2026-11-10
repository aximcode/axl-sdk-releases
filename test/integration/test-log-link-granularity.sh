#!/bin/bash
# test-meta: arch=both needs=jq est=6 local-only=0 builds=BUILD=RELEASE,all
# test-log-link-granularity.sh — an image that never asks for logging must not
# link the log engine, and one that does must link it identically.
#
# WHY THIS EXISTS. `--minimal-runtime` measured ~0 bytes saved against the full
# runtime for as long as it existed, and the reason was a single link edge that
# no compile gate can see: 27 of the 51 archive members in a do-nothing image
# carry a strong reference to axl_log_full, so the log layer -- and through it
# the printf engine (axl_vformat, axl_dtoa, kCachedPowers) -- landed in every
# image ever produced. The `ld --cref` map named the BACKEND as the puller,
# which is true and misleading: it was merely first in link order, and
# weak-linking it would only have promoted axl-mem.o behind it.
#
# The fix is a seam inside the log layer: axl_log_full / axl_log are
# trampolines in axl-log-emit.o that forward to _axl_log_vdispatch, declared
# WEAK there and defined in axl-log.o. Nothing pulls the engine implicitly;
# every non-minimal link asks for it with `-u _axl_log_vdispatch`
# (AXL_LOG_ENGINE_PULL in cmake/AxlImages.cmake, `--minimal-runtime=log` in
# axl-cc).
#
# So the property under test is a LINKAGE property, and behaviour tests cannot
# see it -- a full-runtime image behaves identically whether the seam exists or
# not. Three links, one source, one library:
#
#   1. minimal CRT0, no pull   -> engine ABSENT, and materially smaller
#   2. minimal CRT0, with pull -> engine PRESENT  (`--minimal-runtime=log`)
#   3. full CRT0 + pull        -> engine PRESENT  (every ordinary image)
#
# (2) and (3) are the positive controls: they prove the check can SEE an engine
# when there is one, so (1)'s absence is evidence rather than a broken probe.
# And all three must define axl_log_full, because an app that calls axl_error
# with no engine linked must no-op, not fail to link.
#
# RELEASE, because that is the build a size claim belongs to: install.sh stages
# RELEASE, and the numbers in docs/AXL-Minimal-Image-Notes.md are RELEASE. The
# LINKAGE assertions hold in either mode; only the byte count needs pinning.
#
# Ratchet-exempt (link-property scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-log-link-granularity.sh [--arch X64|AARCH64|both]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
cd "$PROJECT_DIR"

export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(x64) ;;
    AARCH64) ARCHES=(aa64) ;;
    both)    ARCHES=(x64 aa64) ;;
    *) echo "usage: $0 [--arch X64|AARCH64|both]" >&2; exit 2 ;;
esac

WORK="$(mktemp -d -t axl-logseam.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# The seam symbol. Named once: a rename that reaches the build and axl-cc
# but not this test would leave every assertion below passing vacuously,
# because `nm | grep <old name>` finds nothing in an image that HAS an engine
# just as surely as in one that does not -- which is why the positive controls
# assert PRESENCE rather than only absence.
SEAM=_axl_log_vdispatch

for arch in "${ARCHES[@]}"; do
    echo "=== log engine is opt-in at link time ($arch, RELEASE) ==="

    # The CMake RELEASE tree, and ITS link pieces: axl-link-pieces.sh is
    # generated from the variables and objects every image's link is made of
    # (cmake/AxlImages.cmake), and compile_commands.json holds every TU's
    # compile -- so the probe is linked and compiled as a shipped image is,
    # never with flags restated here.
    if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$arch" BUILD=RELEASE all >"$WORK/build-$arch.log" 2>&1; then
        fail "$arch: library build failed"
        tail -20 "$WORK/build-$arch.log" | sed 's/^/      /'
        continue
    fi
    TREE="$("$PROJECT_DIR/scripts/axl-build.sh" --print-prefix --abs ARCH="$arch" BUILD=RELEASE)"
    PIECES="$TREE/axl-link-pieces.sh"
    if [[ ! -f "$PIECES" ]]; then
        fail "$arch: no $PIECES -- this test reads the CMake tree's link pieces"
        continue
    fi
    unset AXL_LP_LD AXL_LP_LDFLAGS AXL_LP_LDS AXL_LP_CRT0_APP AXL_LP_CRT0_MINIMAL \
          AXL_LP_LIBS AXL_LP_LOG_PULL AXL_LP_NM AXL_LP_OBJCOPY \
          AXL_LP_OBJCOPY_SECTIONS AXL_LP_OBJCOPY_STRIP AXL_LP_PE_TARGET
    # shellcheck source=/dev/null
    source "$PIECES"
    LD=$AXL_LP_LD; LDFLAGS=$AXL_LP_LDFLAGS; LDS=$AXL_LP_LDS
    MINCRT0=$AXL_LP_CRT0_MINIMAL; FULLCRT0=$AXL_LP_CRT0_APP; LIBS=$AXL_LP_LIBS
    PULL=$AXL_LP_LOG_PULL; NM=$AXL_LP_NM; OBJCOPY=$AXL_LP_OBJCOPY
    OCSECS=$AXL_LP_OBJCOPY_SECTIONS; OCSTRIP=$AXL_LP_OBJCOPY_STRIP; PETGT=$AXL_LP_PE_TARGET
    missing=""
    for v in LD LDFLAGS LDS MINCRT0 FULLCRT0 LIBS PULL NM OBJCOPY OCSECS OCSTRIP PETGT; do
        [[ -n "${!v}" ]] || missing+=" $v"
    done
    if [[ -n "$missing" ]]; then
        fail "$arch: $PIECES left empty:$missing"
        continue
    fi
    # hello.c's compile (an application TU of this tree), with the probe as
    # its source and output. Read as a whole command: the tree escaped it for
    # a shell, and it runs from the tree's directory as ninja runs it.
    CC_CMD="$(jq -r '.[] | select(.file | endswith("/sdk/examples/hello.c")) | .command' \
              "$TREE/compile_commands.json")"
    if [[ -z "$CC_CMD" || "$CC_CMD" != *" -c "* || "$CC_CMD" != *" -o "* ]]; then
        fail "$arch: no compile of sdk/examples/hello.c in $TREE/compile_commands.json"
        continue
    fi
    CC_CMD="$(sed -E "s# -o [^ ]+# -o $WORK/nop-$arch.o#; s# -c [^ ]+# -c $WORK/nop.c#" <<< "$CC_CMD")"

    # CONTROL: the pieces ARE the shipped link. hello.c's own object, linked
    # from them exactly as the build links hello.efi's .so, must reproduce the
    # tree's hello.so byte for byte -- otherwise every reading below would be
    # of some other link than the one images get.
    # The database's `output` is relative to the build directory under CMake
    # 3.31 and ABSOLUTE under 4.2 (CI's pin) -- the spec allows either, so
    # take it as given when absolute. Prefixing it unconditionally linked
    # "$TREE//$TREE/..." on CI and failed only there.
    HELLO_O="$(jq -r '.[] | select(.file | endswith("/sdk/examples/hello.c")) | .output' \
              "$TREE/compile_commands.json")"
    [[ "$HELLO_O" == /* ]] || HELLO_O="$TREE/$HELLO_O"
    # shellcheck disable=SC2086
    if $LD $LDFLAGS -T "$LDS" $PULL -o "$WORK/hello-$arch.so" $FULLCRT0 "$HELLO_O" $LIBS \
            >"$WORK/hello-$arch.ldlog" 2>&1 \
       && cmp -s "$WORK/hello-$arch.so" "$TREE/hello.so"; then
        pass "$arch: the link pieces reproduce the tree's hello.so byte for byte"
    else
        fail "$arch: the link pieces do NOT reproduce $TREE/hello.so -- they are not the shipped link"
        sed 's/^/      /' "$WORK/hello-$arch.ldlog"
        continue
    fi

    echo 'int main(void) { return 0; }' > "$WORK/nop.c"
    if ! (cd "$TREE" && bash -c "$CC_CMD") >"$WORK/cc-$arch.log" 2>&1; then
        fail "$arch: probe compile failed"
        sed 's/^/      /' "$WORK/cc-$arch.log"
        continue
    fi

    # $1 tag, $2 crt0 object list, $3 extra ld args
    link() {
        local tag="$1" crt0="$2" extra="$3"
        # shellcheck disable=SC2086
        $LD $LDFLAGS -T "$LDS" $extra -o "$WORK/$tag.so" \
            $crt0 "$WORK/nop-$arch.o" $LIBS -Map "$WORK/$tag.map" --cref \
            >"$WORK/$tag.ldlog" 2>&1 || return 1
        # The size claim belongs to the SHIPPED artifact. A .so delta is mostly
        # DWARF -- it moves with -g, not with the code that was dropped.
        # shellcheck disable=SC2086
        $OBJCOPY $OCSECS $OCSTRIP --output-target="$PETGT" --subsystem=10 \
            "$WORK/$tag.so" "$WORK/$tag.efi" >>"$WORK/$tag.ldlog" 2>&1
    }

    link "min-nolog-$arch" "$MINCRT0"                    ""       || \
        { fail "$arch: minimal link (no pull) failed"; sed 's/^/      /' "$WORK/min-nolog-$arch.ldlog"; continue; }
    link "min-log-$arch"   "$MINCRT0"                    "$PULL"  || \
        { fail "$arch: minimal link (with pull) failed"; sed 's/^/      /' "$WORK/min-log-$arch.ldlog"; continue; }
    link "full-$arch"      "$FULLCRT0"                    "$PULL"  || \
        { fail "$arch: full-runtime link failed"; sed 's/^/      /' "$WORK/full-$arch.ldlog"; continue; }

    # Defined (T/t/W/w with an address) vs undefined (U/w with none). Localized
    # by efi-localize.ver, so a definition reads as lowercase `t`.
    # 0 defined, 1 not defined, 2 nm could not read the image (sym_present).
    engine_defined() { sym_present "$1" "$SEAM"; }
    # CAPTURE, then grep -- never `$NM | grep -q` under pipefail: grep -q
    # exits at the first match, and an nm still writing then dies of
    # SIGPIPE, failing the pipeline exactly when the symbol IS there (nm
    # does exit 141 on an early-closed pipe). An aa64 "axl_log_full is not
    # defined" failed once and passed on every rerun (2026-11-04); this
    # trap is the plausible cause, not a reproduced one. Capturing also
    # turns an nm that could not RUN into a failure of its own instead of
    # "symbol absent".
    syms_of() {
        local out
        out=$($NM "$1" 2>&1) || { echo "nm failed on $1: $out" >&2; return 2; }
        printf '%s\n' "$out"
    }
    sym_present() {
        local syms
        syms=$(syms_of "$1") || return 2
        grep -qE "^[0-9a-f]+ [TtWV] $2\$" <<< "$syms"
    }

    if engine_defined "$WORK/min-log-$arch.so"; then
        pass "$arch: --minimal-runtime=log links the engine"
    else
        fail "$arch: --minimal-runtime=log did NOT link $SEAM — the pull does not work,
      so the absence check below proves nothing"
    fi

    if engine_defined "$WORK/full-$arch.so"; then
        pass "$arch: a full-runtime image links the engine"
    else
        fail "$arch: a full-runtime image lost $SEAM — ordinary images stopped logging"
    fi

    # The ABSENCE check, so "nm could not read it" (2) must not read as
    # "not linked" (1): the two are the same silence and opposite facts.
    engine_defined "$WORK/min-nolog-$arch.so"; rc=$?
    if [[ $rc -eq 0 ]]; then
        fail "$arch: --minimal-runtime still links $SEAM — the seam is not weak,
      or something in libaxl references the engine strongly"
    elif [[ $rc -eq 2 ]]; then
        fail "$arch: nm could not read the minimal image — the absence of $SEAM is unjudged"
    else
        pass "$arch: --minimal-runtime does not link the engine"
    fi

    # The trampoline is unconditional: axl_error from an app with no engine
    # must no-op, not fail to link and not fault.
    for img in min-nolog min-log full; do
        if sym_present "$WORK/$img-$arch.so" axl_log_full; then
            pass "$arch/$img: axl_log_full is still defined"
        else
            fail "$arch/$img: axl_log_full is not defined — a caller cannot link"
        fi
    done

    # ld pulls axl-log's member only for the engine, so its presence in the
    # map is an independent reading of the same fact -- one that a `nm`
    # filter typo cannot fake. CMake's archive names it axl-log.c.obj.
    members() {
        sed -n '/^Archive member included/,/^Discarded input sections/p' "$1" \
            | grep -o 'libaxl\.a([^)]*)' | sed 's/.*(//;s/)//' | sort -u
    }
    if members "$WORK/min-nolog-$arch.map" | grep -qx 'axl-log.c.obj'; then
        fail "$arch: axl-log.c.obj is still an archive member of the minimal image"
    else
        pass "$arch: axl-log.c.obj is not pulled into the minimal image"
    fi
    if members "$WORK/min-log-$arch.map" | grep -qx 'axl-log.c.obj'; then
        pass "$arch: axl-log.c.obj IS pulled by --minimal-runtime=log"
    else
        fail "$arch: --minimal-runtime=log did not pull axl-log.c.obj"
    fi

    # The bytes are the whole point of the exercise, so assert them. 4096 is a
    # regression bound rather than a target: the measured saving at the time of
    # writing is 6,144 on x64 (36,864 -> 30,720 for a do-nothing image).
    sz_off=$(stat -c%s "$WORK/min-nolog-$arch.efi")
    sz_on=$(stat -c%s "$WORK/min-log-$arch.efi")
    delta=$((sz_on - sz_off))
    if [[ "$delta" -ge 4096 ]]; then
        pass "$arch: dropping the engine saves $delta bytes ($sz_on -> $sz_off)"
    else
        fail "$arch: dropping the engine saved only $delta bytes (want >= 4096) —
      the engine is gone but something else now roots the printf machinery"
    fi
done

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]]
