#!/bin/bash
# test-meta: arch=both needs= est=6 local-only=0
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
# ($(LOG_ENGINE_PULL) in the Makefile, `--minimal-runtime=log` in axl-cc).
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

# The seam symbol. Named once: a rename that reaches the Makefile and axl-cc
# but not this test would leave every assertion below passing vacuously,
# because `nm | grep <old name>` finds nothing in an image that HAS an engine
# just as surely as in one that does not -- which is why the positive controls
# assert PRESENCE rather than only absence.
SEAM=_axl_log_vdispatch

for arch in "${ARCHES[@]}"; do
    echo "=== log engine is opt-in at link time ($arch, RELEASE) ==="

    if ! make ARCH="$arch" BUILD=RELEASE all >"$WORK/build-$arch.log" 2>&1; then
        fail "$arch: library build failed"
        tail -20 "$WORK/build-$arch.log" | sed 's/^/      /'
        continue
    fi

    # Read the real build variables rather than restating them: a probe that
    # hardcodes its own flags stops measuring the shipped link the first time
    # one changes.
    mapfile -t V < <(make -s ARCH="$arch" BUILD=RELEASE \
        print-CC print-CFLAGS print-INCLUDES print-LD_ELF print-LDFLAGS_EFI \
        print-EFI_LDS print-GCC_CRT0 print-RELOC_OBJ print-DEBUG_INFO_OBJ \
        print-CRT0_MINIMAL_OBJ print-LINK_CRT0_CMD print-LINK_LIBS \
        print-LOG_ENGINE_PULL print-CROSS print-OBJCOPY print-OBJCOPY_SECTIONS \
        print-OBJCOPY_STRIP print-PE_TARGET)
    CC=${V[0]}; CFLAGS=${V[1]}; INCLUDES=${V[2]}; LD=${V[3]}; LDFLAGS=${V[4]}
    LDS=${V[5]}; ACRT0=${V[6]}; RELOC=${V[7]}; DBGI=${V[8]}; MINCRT0=${V[9]}
    FULLCRT0=${V[10]}; LIBS=${V[11]}; PULL=${V[12]}; CROSS=${V[13]}
    OBJCOPY=${V[14]}; OCSECS=${V[15]}; OCSTRIP=${V[16]}; PETGT=${V[17]}
    NM="${CROSS}nm"

    if [[ -z "$PULL" ]]; then
        fail "$arch: \$(LOG_ENGINE_PULL) is empty — the Makefile has no pull to omit"
        continue
    fi

    echo 'int main(void) { return 0; }' > "$WORK/nop.c"
    if ! $CC $CFLAGS $INCLUDES -c "$WORK/nop.c" -o "$WORK/nop-$arch.o" \
            >"$WORK/cc-$arch.log" 2>&1; then
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

    link "min-nolog-$arch" "$ACRT0 $RELOC $DBGI $MINCRT0" ""       || \
        { fail "$arch: minimal link (no pull) failed"; sed 's/^/      /' "$WORK/min-nolog-$arch.ldlog"; continue; }
    link "min-log-$arch"   "$ACRT0 $RELOC $DBGI $MINCRT0" "$PULL"  || \
        { fail "$arch: minimal link (with pull) failed"; sed 's/^/      /' "$WORK/min-log-$arch.ldlog"; continue; }
    link "full-$arch"      "$FULLCRT0"                    "$PULL"  || \
        { fail "$arch: full-runtime link failed"; sed 's/^/      /' "$WORK/full-$arch.ldlog"; continue; }

    # Defined (T/t/W/w with an address) vs undefined (U/w with none). Localized
    # by efi-localize.ver, so a definition reads as lowercase `t`.
    engine_defined() {
        $NM "$1" 2>/dev/null | grep -qE "^[0-9a-f]+ [TtWV] $SEAM\$"
    }
    sym_present() {
        $NM "$1" 2>/dev/null | grep -qE "^[0-9a-f]+ [TtWV] $2\$"
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

    if engine_defined "$WORK/min-nolog-$arch.so"; then
        fail "$arch: --minimal-runtime still links $SEAM — the seam is not weak,
      or something in libaxl references the engine strongly"
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

    # ld pulls axl-log.o only for the engine, so its presence in the map is an
    # independent reading of the same fact -- one that a `nm` filter typo
    # cannot fake.
    members() {
        sed -n '/^Archive member included/,/^Discarded input sections/p' "$1" \
            | grep -o 'libaxl\.a([^)]*)' | sed 's/.*(//;s/)//' | sort -u
    }
    if members "$WORK/min-nolog-$arch.map" | grep -qx 'axl-log.o'; then
        fail "$arch: axl-log.o is still an archive member of the minimal image"
    else
        pass "$arch: axl-log.o is not pulled into the minimal image"
    fi
    if members "$WORK/min-log-$arch.map" | grep -qx 'axl-log.o'; then
        pass "$arch: axl-log.o IS pulled by --minimal-runtime=log"
    else
        fail "$arch: --minimal-runtime=log did not pull axl-log.o"
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
