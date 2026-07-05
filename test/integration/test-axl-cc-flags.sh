#!/bin/bash
# test-meta: arch=x64 needs= est=5 local-only=0
# test-axl-cc-flags.sh — axl-cc forwards compiler/linker flags it doesn't
# consume itself, instead of mistaking them for source files.
#
#   - gcc dependency-generation flags (-MMD -MF <f> -MT <t> ...) reach the
#     compile, so a .d is emitted.
#   - a generic single-dash compile flag (-O2) is forwarded, not treated as a
#     source ("source file not found: -O2" was the old failure).
#   - linker flags via -Wl,<opt> / -Xlinker <opt> reach the ld link.
#   - an unrecognized --long option is a clear axl-cc error, not a silent
#     forward or a "source not found".
#
# Requires scripts/install.sh --arch x64 to have staged out/bin/axl-cc + libs
# (the artifact under test). Exits 2 if the staged SDK isn't present.
#
# Usage: ./test/integration/test-axl-cc-flags.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

test_setup
# This test deliberately drives axl-cc failure paths (unknown option) and
# captures each rc, so disable errexit for the body.
set +e

AXL_CC="$PROJECT_DIR/out/bin/axl-cc"
LIB_DIR="$PROJECT_DIR/out/lib/axl/x64"
if [[ ! -x "$AXL_CC" || ! -d "$LIB_DIR" ]]; then
    echo "ERROR: staged SDK missing — run 'scripts/install.sh --arch x64' first" >&2
    exit 2
fi

SRC="$PROJECT_DIR/sdk/examples/hello.c"
[[ -f "$SRC" ]] || { echo "FAIL: source missing: $SRC"; exit 1; }

WORK="$TEST_TMPDIR/axl-cc-flags"
mkdir -p "$WORK"

pass=0
fail=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "PASS: $2"; pass=$((pass+1))
    else echo "FAIL: $2"; fail=$((fail+1)); fi
}

# 1. Dependency file: -MMD -MF <f> -MT <t> reaches the compile. Use a -MT
# target DISTINCT from -o so the .d rule text proves -MT was honoured (gcc's
# default target derives from -o, which would mask a dropped -MT).
rm -f "$WORK/hello.o" "$WORK/hello.d"
"$AXL_CC" -c -MMD -MF "$WORK/hello.d" -MT "DEPTARGET.o" \
    "$SRC" -o "$WORK/hello.o" >"$WORK/dep.log" 2>&1
dep_rc=$?
check "$dep_rc" "-c -MMD -MF -MT compiles (rc=$dep_rc)"
[[ -f "$WORK/hello.d" ]] && grep -q "^DEPTARGET.o:" "$WORK/hello.d" && grep -q "hello.c" "$WORK/hello.d"
check "$?" ".d lists the -MT target (DEPTARGET.o:) and the source (hello.c)"

# 2. A generic single-dash compile flag is forwarded to the compiler command
# line -- NOT merely swallowed (a swallowed -O2 would still compile rc=0). Prove
# it reached gcc by grepping the -v-echoed command.
rm -f "$WORK/o2.o"
"$AXL_CC" -v -c -O2 "$SRC" -o "$WORK/o2.o" >"$WORK/o2.log" 2>&1
o2_rc=$?
[[ "$o2_rc" -eq 0 ]] && grep -q -- "-O2" "$WORK/o2.log"
check "$?" "-O2 reaches the compiler command line (forwarded, not swallowed) (rc=$o2_rc)"

# 3. Linker flag via -Wl, reaches the ld link (ld -Map emits a map file).
rm -f "$WORK/hello.efi" "$WORK/link.map"
"$AXL_CC" "$SRC" -Wl,-Map="$WORK/link.map" -o "$WORK/hello.efi" \
    >"$WORK/link.log" 2>&1
link_rc=$?
check "$link_rc" "-Wl,-Map link succeeds (rc=$link_rc)"
[[ -s "$WORK/link.map" ]]
check "$?" "-Wl, reached ld (a non-empty link map was produced)"

# 3b. Same passthrough on the C++ driver (axl-c++ = axl-cc -x c++): a C++ TU
# emits a .d and honours a forwarded compile flag.
AXL_CXX="$PROJECT_DIR/out/bin/axl-c++"
CXX_SRC="$PROJECT_DIR/sdk/examples/hello.cpp"
if [[ -x "$AXL_CXX" && -f "$CXX_SRC" ]]; then
    rm -f "$WORK/hpp.o" "$WORK/hpp.d"
    "$AXL_CXX" -c -O2 -MMD -MF "$WORK/hpp.d" -MT "$WORK/hpp.o" \
        "$CXX_SRC" -o "$WORK/hpp.o" >"$WORK/cxx.log" 2>&1
    cxx_rc=$?
    [[ "$cxx_rc" -eq 0 && -f "$WORK/hpp.d" ]] && grep -q "hello.cpp" "$WORK/hpp.d"
    check "$?" "axl-c++ forwards -O2/-MMD on a C++ TU (.d produced) (rc=$cxx_rc)"
else
    echo "SKIP: axl-c++ or hello.cpp missing"
fi

# 4. An unrecognized --long option is a clear axl-cc error.
"$AXL_CC" --bogus-option "$SRC" -o "$WORK/x.efi" >"$WORK/bogus.log" 2>&1
bogus_rc=$?
[[ "$bogus_rc" -ne 0 ]] && grep -qi "unknown option" "$WORK/bogus.log"
check "$?" "unknown --long option errors clearly (rc=$bogus_rc)"

# 5. --depfile: emit ABSOLUTE dependency paths while compiling the BARE source
# (object must stay bit-identical to a no-depfile compile — Makefile<->CMake
# bit-parity). Use a local header so the .d has a relative dep to absolutize,
# and compile with a bare source name from the source dir (the real scenario).
DW="$WORK/dep"
mkdir -p "$DW"
echo 'int helper(void);' > "$DW/t.h"
printf '#include "t.h"\nint helper(void){ return 0; }\n' > "$DW/t.c"

( cd "$DW" && "$AXL_CC" -c --depfile t.d t.c -o t.o ) >"$DW/dep.log" 2>&1
df_rc=$?
check "$df_rc" "-c --depfile compiles (rc=$df_rc)"

# target line present (relative, as the -o value); every DEPENDENCY absolute.
grep -q '^t.o:' "$DW/t.d" 2>/dev/null
check "$?" "depfile has the target line (t.o:)"
# Flatten: strip continuations, split, drop trailing colon. Everything except
# the target object 't.o' and blanks must be an absolute path. Guard on a
# non-empty .d so a missing/empty file can't pass vacuously.
if [[ -s "$DW/t.d" ]]; then
    bad=$(tr -d '\\' < "$DW/t.d" | tr ' \t' '\n' | sed 's/:$//' \
          | grep -vxE '(/.*|t\.o|)' || true)
    [[ -z "$bad" ]]
else
    bad="(depfile empty/missing)"; false
fi
check "$?" "every dependency path in the depfile is absolute (stray: ${bad:-none})"
# The local header is listed by its absolute, existing path.
grep -qF "$DW/t.h" "$DW/t.d" 2>/dev/null && [[ -f "$DW/t.h" ]]
check "$?" "local header t.h is listed as an absolute existing path"

# 6. Bit-parity: --depfile must NOT perturb the object vs a bare compile.
( cd "$DW" && "$AXL_CC" -c t.c -o t-nodep.o ) >"$DW/nodep.log" 2>&1
cmp -s "$DW/t.o" "$DW/t-nodep.o"
check "$?" "--depfile leaves the object bit-identical to a bare compile"

# 7. --depfile requires -c (a full-link build would name a scratch object).
"$AXL_CC" --depfile "$DW/nc.d" "$DW/t.c" -o "$WORK/nc.efi" >"$DW/nc.log" 2>&1
nc_rc=$?
[[ "$nc_rc" -ne 0 ]] && grep -qi "requires -c" "$DW/nc.log"
check "$?" "--depfile without -c errors clearly (rc=$nc_rc)"

# 8. Multi-source: DEST is a directory; each object gets its own absolute .d.
MD="$DW/multi"; mkdir -p "$MD" "$MD/out"
printf '#include "t.h"\nint a(void){return 0;}\n' > "$MD/a.c"
printf '#include "t.h"\nint b(void){return 0;}\n' > "$MD/b.c"
cp "$DW/t.h" "$MD/t.h"
( cd "$MD" && "$AXL_CC" -c --depfile out a.c b.c ) >"$MD/multi.log" 2>&1
ms_rc=$?
[[ "$ms_rc" -eq 0 && -f "$MD/out/a.d" && -f "$MD/out/b.d" ]] \
    && grep -qF "$MD/t.h" "$MD/out/a.d" && grep -qF "$MD/t.h" "$MD/out/b.d"
check "$?" "multi-source --depfile <dir> writes per-object absolute .d files (rc=$ms_rc)"

echo "--- results ---"
echo "axl-cc flag passthrough: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
