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

# The artifact under test is the STAGED copy, and nothing in the suite re-stages
# it: run-integration.sh's prebuild builds the library but never runs
# install.sh. So an edit to scripts/axl-cc that was never installed leaves this
# test silently exercising the PREVIOUS driver and reporting on it.
#
# That is not hypothetical. A sabotage run staged its own broken axl-cc; the
# restore put the SOURCE back byte-identically (and said so), the staged copy
# kept the defect, and the next suite run failed on a "regression" that existed
# only in out/bin. Same shape as a stale .efi surviving a libaxl.a-only rebuild.
# install.sh copies this file verbatim, so a plain compare is the right check.
if ! cmp -s "$PROJECT_DIR/scripts/axl-cc" "$AXL_CC"; then
    echo "ERROR: staged $AXL_CC is STALE — it differs from scripts/axl-cc." >&2
    echo "       This test would report on the staged copy, not your edit." >&2
    echo "       Run 'scripts/install.sh --arch x64' first." >&2
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

# 4b. The axl-c++ alias names ITSELF, not axl-cc. It `exec`s axl-cc, which
# leaves $0 as axl-cc's path, so the name has to be passed through the
# environment — easy to drop, and the symptom (help telling you to run a
# different command than the one you ran) is quiet. All text-only, so no C++
# toolchain is required and these run unconditionally.
#
# Also pins that axl-c++ ships alongside axl-cc UNCONDITIONALLY: it is a
# dependency-free wrapper, and withholding it on a C-only install replaces
# axl-cc's precise "libaxl-cxx.a is missing, run ./scripts/install.sh --cpp"
# with the shell's bare "command not found".
CXX_DRV="$(dirname "$AXL_CC")/axl-c++"
[[ -x "$CXX_DRV" ]]
check "$?" "axl-c++ is installed alongside axl-cc (unconditional wrapper)"

if [[ -x "$CXX_DRV" ]]; then
    # Capture to files and grep those, never `--help | grep -q`: help is
    # produced by a `cat | sed` pipeline, so a short-circuiting grep -q can
    # SIGPIPE the writer and pipefail then reports the whole pipeline failed.
    # That races on output size and produced an intermittent red here.
    "$CXX_DRV" --help    >"$WORK/cxxhelp.txt" 2>&1
    "$CXX_DRV" --version >"$WORK/cxxver.txt"  2>&1
    "$AXL_CC"  --help    >"$WORK/cchelp.txt"  2>&1
    "$CXX_DRV" --bogus-option x.cpp >"$WORK/cxxbogus.log" 2>&1

    [[ "$(head -1 "$WORK/cxxhelp.txt")" == axl-c++\ * ]]
    check "$?" "axl-c++ --help identifies itself as axl-c++"
    grep -q '^Usage: axl-c++ ' "$WORK/cxxhelp.txt"
    check "$?" "axl-c++ --help Usage line names axl-c++"
    grep -q '^axl-c++ ' "$WORK/cxxver.txt"
    check "$?" "axl-c++ --version names axl-c++"
    grep -q "run 'axl-c++ --help'" "$WORK/cxxbogus.log"
    check "$?" "axl-c++ unknown-option error names axl-c++"
    # ...and axl-cc still names itself, i.e. the substitution is per-invocation
    # rather than hardcoded the other way. The alias note must NOT leak into it.
    [[ "$(head -1 "$WORK/cchelp.txt")" == axl-cc\ * ]] \
        && ! grep -q 'axl-c++ is axl-cc with' "$WORK/cchelp.txt" \
        && ! grep -q '@PROG@' "$WORK/cchelp.txt" "$WORK/cxxhelp.txt"
    check "$?" "axl-cc --help names axl-cc, omits the alias note, no stray @PROG@"
else
    # Balanced SKIP count (5) — see feedback_balancer_count.
    for _m in "axl-c++ --help identifies itself as axl-c++" \
              "axl-c++ --help Usage line names axl-c++" \
              "axl-c++ --version names axl-c++" \
              "axl-c++ unknown-option error names axl-c++" \
              "axl-cc --help names axl-cc, omits the alias note, no stray @PROG@"; do
        echo "SKIP: $_m (axl-c++ not installed)"
    done
fi

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

# 5b. Why --depfile exists at all: pin what the two FORWARDED gcc flags do with
# SDK headers, because that is the reason --help gives a consumer for choosing
# between them, and a silent drift in it would send them the wrong way.
#
# The SDK arrives via -isystem, so:
#   -MMD skips system headers => it lists NO axl-sdk header whatsoever (the
#        consumer gets zero SDK dependency tracking, not merely awkward paths);
#   -MD  lists them, absolute, while the relative source stays relative => a
#        MIXED file, which is what CMake's DEPFILE cannot rebase.
#
# Host-independent by construction: it asserts a COUNT (zero vs non-zero) and
# the coexistence of one relative and one absolute prerequisite, never a
# particular path — so it does not depend on the host gcc's include layout.
# The relative side is the source name itself, which is relative because the
# compile runs from its own directory.
SDW="$WORK/depsys"
mkdir -p "$SDW"
printf '#include <axl.h>\nint sdkdep(void){ return 0; }\n' > "$SDW/s.c"

( cd "$SDW" && "$AXL_CC" -c -MMD -MF s-mmd.d s.c -o s-mmd.o ) >"$SDW/mmd.log" 2>&1
mmd_rc=$?
( cd "$SDW" && "$AXL_CC" -c -MD  -MF s-md.d  s.c -o s-md.o  ) >"$SDW/md.log" 2>&1
md_rc=$?
[[ "$mmd_rc" -eq 0 && "$md_rc" -eq 0 ]]
check "$?" "forwarded -MMD and -MD both compile an SDK TU (rc=$mmd_rc/$md_rc)"

dep_tokens() { tr -d '\\' < "$1" | tr ' \t' '\n' | grep -vxE '.*:|' || true; }

mmd_sdk=$(dep_tokens "$SDW/s-mmd.d" | grep -c 'include/axl-sdk/')
[[ "$mmd_rc" -eq 0 && -s "$SDW/s-mmd.d" && "$mmd_sdk" -eq 0 ]]
check "$?" "forwarded -MMD lists NO SDK headers — they come via -isystem (found $mmd_sdk)"

md_sdk=$(dep_tokens "$SDW/s-md.d" | grep -c 'include/axl-sdk/')
[[ "$md_sdk" -gt 0 ]]
check "$?" "forwarded -MD does list SDK headers (found $md_sdk)"

# Mixed: at least one relative and at least one absolute prerequisite, same file.
md_rel=$(dep_tokens "$SDW/s-md.d" | grep -cvE '^/')
md_abs=$(dep_tokens "$SDW/s-md.d" | grep -cE '^/')
[[ "$md_rel" -gt 0 && "$md_abs" -gt 0 ]]
check "$?" "forwarded -MD yields a MIXED depfile ($md_rel relative + $md_abs absolute)"

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

# 9. -std is routed to the LANGUAGE IT NAMES, not into the shared extras.
#
# The extras are expanded after the baked-in flags on both compile lines, so a
# C standard used to land on the g++ line too:
# `g++ -std=c++23 ... -std=gnu2x` draws "valid for C/ObjC but not for C++",
# which -Werror (also forwarded) turns into a hard error. Splitting it per
# language fixes that but opens two silent failures instead -- a dropped -std
# in the --service re-invocation, and a C standard parked where an `-x c++`
# build never reads it. All three are pinned here.
SD="$DW/std"; mkdir -p "$SD"
printf '#include <axl.h>\nint axl_c_side(void){ return 0; }\n' > "$SD/s.c"
printf 'int axl_cpp_side(){ return 0; }\n'                     > "$SD/s.cpp"

# The C++ cases need a working C++ toolchain (host g++ for x64, ARM's
# bare-metal cross for aa64). Probe once by compiling, rather than guessing
# from a path: a staged-but-broken toolchain would otherwise turn these into
# confusing failures instead of honest skips.
HAVE_CXX_TOOLCHAIN=false
if ( cd "$SD" && "$AXL_CC" -c s.cpp -o probe.o ) >"$SD/probe.log" 2>&1; then
    HAVE_CXX_TOOLCHAIN=true
fi

# 9a. A C standard with -Werror must not reach the C++ compiler. The fixture
# MUST contain a C++ source: with only .c input the g++ line never runs, a
# leaked flag has nothing to break, and the check passes against the very bug
# it is written for (confirmed -- the first version of this test did not fail
# under a sabotage that put -std back into the shared extras). Grep for the
# exact diagnostic, because "the build failed" is a different claim from
# "the flag leaked".
if [[ "$HAVE_CXX_TOOLCHAIN" == "true" ]]; then
    # cd + no -o: multi-source -c wants a directory destination (see case 8).
    ( cd "$SD" && "$AXL_CC" -std=gnu2x -Werror -c s.c s.cpp ) >"$SD/c.log" 2>&1
    std_c_rc=$?
    [[ "$std_c_rc" -eq 0 ]] && ! grep -q "valid for C/ObjC but not for C++" "$SD/c.log"
    check "$?" "-std=<C> in a mixed C/C++ build never reaches g++ (rc=$std_c_rc)"
else
    check 0 "SKIP: no C++ toolchain — mixed-build -std leak check not run"
fi

# 9d. The same silent drop reachable by extension dispatch rather than -x:
# a C standard with only .cpp sources can never be applied.
if [[ "$HAVE_CXX_TOOLCHAIN" == "true" ]]; then
    "$AXL_CC" -std=gnu2x -c "$SD/s.cpp" -o "$SD/s3.o" >"$SD/only.log" 2>&1
    only_rc=$?
    [[ "$only_rc" -ne 0 ]] && grep -q "names a C standard" "$SD/only.log"
    check "$?" "-std=<C> with only C++ sources errors instead of dropping (rc=$only_rc)"
else
    check 0 "SKIP: no C++ toolchain — extension-dispatch -std check not run"
fi

# 9b. axl-c++ forces -x c++ on EVERY source, so a C standard can never be
# applied. It must say so: before the split g++ rejected it with a clear
# message, and a silent build at the wrong standard is strictly worse.
if [[ -x "$CXX_DRV" ]]; then
    "$CXX_DRV" -std=gnu17 -c "$SD/s.cpp" -o "$SD/s2.o" >"$SD/x.log" 2>&1
    xmm_rc=$?
    [[ "$xmm_rc" -ne 0 ]] && grep -q "names a C standard" "$SD/x.log"
    check "$?" "axl-c++ -std=<C> errors clearly instead of dropping it (rc=$xmm_rc)"
else
    # Balancer: the populated path above contributes one check, so the skip
    # path must too, or the ratchet drifts by arch.
    check 0 "SKIP: axl-c++ absent — -std=<C> mismatch check not run"
fi

# 9c. --service re-invokes axl-cc TWICE (driver, then launcher app); -std lives
# outside CFLAGS_EXTRA now, so it must be forwarded explicitly or both passes
# silently fall back to the default. --verbose echoes each compile, so grep the
# command lines rather than trusting that a successful build used the flag.
#
# Uses the real service example, not a stub: a fixture with no AXL_SERVICE_DRIVER
# fails pass 1 on an undefined DriverEntry, pass 2 never runs, and the check
# then proves only half of what its name claims. Two occurrences is the
# assertion — one per pass.
#
# cd into the work dir: --service writes NAME.efi / NAME-dxe.efi (and .so) to
# the CWD under fixed names, which is the repo root when the suite runs.
# The service NAME is load-bearing, not arbitrary: --service NAME generates the
# embed symbols axl_embedded_NAME{,_end}, and service-demo.c references
# axl_embedded_service_demo by hand. A mismatched name fails pass 1 on those
# undefined symbols — which looks like an -std problem and is not.
SVC_SRC="$PROJECT_DIR/sdk/examples/service-demo.c"
if [[ -f "$SVC_SRC" ]]; then
    ( cd "$SD" && "$AXL_CC" --verbose --service service_demo -std=gnu2x "$SVC_SRC" ) \
        >"$SD/svc.log" 2>&1
    svc_rc=$?
    svc_n=$(grep -c -- "-std=gnu2x" "$SD/svc.log")
    [[ "$svc_rc" -eq 0 && "$svc_n" -ge 2 ]]
    check "$?" "--service forwards -std to BOTH sub-builds (rc=$svc_rc, seen ${svc_n}x)"
else
    check 0 "SKIP: sdk/examples/service-demo.c absent — --service -std check not run"
fi

echo "--- results ---"
echo "axl-cc flag passthrough: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
